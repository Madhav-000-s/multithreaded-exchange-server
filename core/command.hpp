#pragma once

#include "core/order.hpp"
#include "core/types.hpp"

#include <memory>
#include <optional>
#include <variant>

namespace exchange {

struct SubmitCommand {
    std::unique_ptr<Order> order;
};

struct CancelCommand {
    OrderId id{};
};

struct ModifyCommand {
    OrderId id{};
    Quantity quantity{};
    std::optional<Price> price;
};

/// One unit of work for the engine thread.
///
/// A `std::variant` here, where the Order hierarchy uses inheritance -- and the
/// contrast is the point. The two choices answer different questions:
///
///   - The set of *order types* is open. A venue adds stop orders, pegged
///     orders, fill-or-kill. Inheritance lets that happen without touching the
///     matcher, which is worth a vtable and an allocation.
///   - The set of *commands* is closed by the protocol. Submit, cancel, modify
///     is the whole vocabulary, and adding one is a protocol version change
///     that every handler must be updated for anyway. A variant makes the
///     compiler enforce that: `std::visit` over an overload set fails to
///     compile the moment an alternative is unhandled, whereas a missed
///     `dynamic_cast` branch would just fall through at run time.
///
/// The variant is also move-only, because SubmitCommand owns its order. That
/// is exactly why the queue was written not to require copyability.
using Command = std::variant<SubmitCommand, CancelCommand, ModifyCommand>;

} // namespace exchange
