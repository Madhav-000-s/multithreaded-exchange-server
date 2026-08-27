#include "core/apply.hpp"

#include "core/book.hpp"
#include "core/command.hpp"

#include <memory>
#include <utility>
#include <variant>

namespace exchange {
namespace {

template <typename... Handlers>
struct Overloaded : Handlers... {
    using Handlers::operator()...;
};

template <typename... Handlers>
Overloaded(Handlers...) -> Overloaded<Handlers...>;

} // namespace

ApplyOutcome applyCommand(Book& book, Command&& command) {
    ApplyOutcome outcome;

    std::visit(Overloaded{
                   [&](SubmitCommand&& submit) {
                       SubmitResult result = book.submit(std::move(submit.order));
                       outcome.fills = std::move(result.fills);
                       outcome.restingQty = result.restingQty;
                       outcome.accepted = true;
                   },
                   [&](const CancelCommand& cancel) {
                       const std::unique_ptr<Order> removed = book.cancel(cancel.id);
                       outcome.accepted = removed != nullptr;
                   },
                   [&](const ModifyCommand& modify) {
                       ModifyResult result = book.modify(modify.id, modify.quantity, modify.price);
                       outcome.fills = std::move(result.fills);
                       outcome.restingQty = result.restingQty;
                       outcome.accepted = result.status != ModifyStatus::NotFound &&
                                          result.status != ModifyStatus::Rejected;
                   },
               },
               std::move(command));

    return outcome;
}

} // namespace exchange
