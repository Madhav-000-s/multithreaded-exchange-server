#include "core/engine.hpp"

#include "core/command.hpp"
#include "core/exceptions.hpp"

#include <optional>
#include <utility>
#include <variant>

namespace exchange {
namespace {

/// The overload-set idiom: assembles several lambdas into one callable so
/// std::visit can dispatch on the alternative. Adding a Command alternative
/// without a matching lambda is then a compile error, not a run-time
/// fall-through.
template <typename... Handlers>
struct Overloaded : Handlers... {
    using Handlers::operator()...;
};

template <typename... Handlers>
Overloaded(Handlers...) -> Overloaded<Handlers...>;

} // namespace

Engine::Engine(std::size_t capacity) : queue_(capacity) {}

Engine::~Engine() {
    // A joinable std::thread destroyed is std::terminate, so this cannot be
    // left to the caller.
    stop();
}

void Engine::onFill(FillHandler handler) {
    onFill_ = std::move(handler);
}

void Engine::start() {
    if (started_.exchange(true)) {
        return;
    }
    thread_ = std::thread([this] { run(); });
}

void Engine::stop() noexcept {
    // exchange() rather than load-then-store: two threads calling stop()
    // concurrently must not both reach join(), because joining an already
    // joined thread is undefined.
    if (stopping_.exchange(true)) {
        return;
    }

    // Closing wakes the engine if it is blocked in pop() and releases any
    // producer blocked in push(). Accepted work is still drained -- close()
    // refuses new commands, it does not discard queued ones.
    queue_.close();

    if (thread_.joinable()) {
        thread_.join();
    }
}

bool Engine::submit(Command command) {
    return queue_.push(std::move(command));
}

bool Engine::trySubmit(Command command) {
    return queue_.tryPush(std::move(command));
}

void Engine::run() noexcept {
    running_.store(true, std::memory_order_release);

    // pop() returns nullopt only when the queue is closed *and* drained, so
    // this loop is the whole termination condition. No polled flag, no
    // sentinel value, and no window in which a command is accepted but never
    // applied.
    while (std::optional<Command> command = queue_.pop()) {
        try {
            apply(std::move(*command));
        } catch (const ExchangeError&) {
            // A rejected order is a normal outcome, not an engine failure.
            rejected_.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            // An exception escaping a thread entry point is std::terminate.
            // The engine surviving matters more here than in the worker pool:
            // it is the only thread that owns the book, so losing it loses the
            // exchange. The book is left untouched by a throw, which is
            // exactly what Phase 3 bought.
            rejected_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    running_.store(false, std::memory_order_release);
}

void Engine::apply(Command&& command) {
    std::visit(Overloaded{
                   [this](SubmitCommand&& submit) {
                       SubmitResult result = book_.submit(std::move(submit.order));
                       for (const Fill& fill : result.fills) {
                           fills_.fetch_add(1, std::memory_order_relaxed);
                           if (onFill_) {
                               onFill_(fill);
                           }
                       }
                       processed_.fetch_add(1, std::memory_order_relaxed);
                   },
                   [this](const CancelCommand& cancel) {
                       const std::unique_ptr<Order> removed = book_.cancel(cancel.id);
                       if (removed == nullptr) {
                           rejected_.fetch_add(1, std::memory_order_relaxed);
                           return;
                       }
                       processed_.fetch_add(1, std::memory_order_relaxed);
                   },
                   [this](const ModifyCommand& modify) {
                       const ModifyResult result =
                           book_.modify(modify.id, modify.quantity, modify.price);
                       for (const Fill& fill : result.fills) {
                           fills_.fetch_add(1, std::memory_order_relaxed);
                           if (onFill_) {
                               onFill_(fill);
                           }
                       }
                       if (result.status == ModifyStatus::NotFound ||
                           result.status == ModifyStatus::Rejected) {
                           rejected_.fetch_add(1, std::memory_order_relaxed);
                           return;
                       }
                       processed_.fetch_add(1, std::memory_order_relaxed);
                   },
               },
               std::move(command));
}

} // namespace exchange
