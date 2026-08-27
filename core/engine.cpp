#include "core/engine.hpp"

#include "core/apply.hpp"
#include "core/command.hpp"
#include "core/exceptions.hpp"

#include <optional>
#include <utility>

namespace exchange {

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

    // Flush after the engine has drained, so a clean shutdown loses nothing
    // even under a batching or non-syncing policy. Best effort: stop() is
    // noexcept and there is nobody left to report to.
    if (log_ != nullptr) {
        try {
            log_->flush();
        } catch (const ExchangeError&) {
        }
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
        } catch (const StorageError&) {
            // The command could not be made durable, so it was deliberately
            // not applied. Counted separately from a rejection because the
            // meanings differ: a rejection is the book declining, this is the
            // exchange being unable to promise anything.
            storageFailures_.fetch_add(1, std::memory_order_relaxed);
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
    // ---- Write ahead. ----
    //
    // The record is made durable *before* the mutation is visible. A crash
    // between these two lines loses nothing that was acknowledged: recovery
    // replays the record and reaches the state the client was told about.
    //
    // The reverse order is write-behind, and it loses exactly the orders
    // acknowledged just before the crash -- the ones a participant is most
    // certain they placed. The ordering is the whole point of the name.
    //
    // If the append throws, this function exits before touching the book, so
    // the log and the book cannot disagree.
    if (log_ != nullptr) {
        (void)log_->append(command);
    }

    // ---- Then mutate. ----
    //
    // applyCommand is the same function recovery replays through, which is
    // what makes a recovered book identical to the one that was logged rather
    // than merely similar to it.
    const ApplyOutcome outcome = applyCommand(book_, std::move(command));

    for (const Fill& fill : outcome.fills) {
        fills_.fetch_add(1, std::memory_order_relaxed);
        if (onFill_) {
            onFill_(fill);
        }
    }

    if (outcome.accepted) {
        processed_.fetch_add(1, std::memory_order_relaxed);
    } else {
        rejected_.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace exchange
