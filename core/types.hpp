#pragma once

#include <cstdint>
#include <functional>

namespace exchange {

/// Unique per order for the lifetime of the process. Assigned by the client
/// and rejected on collision (Phase 3); the book never invents one.
using OrderId = std::uint64_t;

/// The participant an order belongs to. Drives self-match prevention here and
/// balance checks once accounts exist.
using AccountId = std::uint32_t;

/// Order size in whole lots.
///
/// Unsigned deliberately: a negative quantity is then not a representable
/// state rather than a state that has to be validated against. The cost is
/// that underflow wraps instead of going negative, which is why every
/// subtraction of a Quantity in this codebase is guarded by a precondition
/// rather than by a sign check afterwards.
using Quantity = std::uint64_t;

/// Price in integer ticks.
///
/// Never a floating-point value. 0.1 is not representable in binary floating
/// point, so a price ladder built from doubles accumulates error, and two
/// prices that should compare equal do not. `std::map<Price, ...>` would then
/// grow duplicate levels for what is nominally the same price. Integer ticks
/// make equality exact and the ordering total.
///
/// Signed, not unsigned: negative prices are real (April 2020 WTI settled at
/// -$37), and price *differences* are needed for spread and imbalance
/// arithmetic, where an unsigned type would wrap.
using Price = std::int64_t;

/// Monotonic counter establishing time priority.
///
/// Not a wall-clock timestamp. A clock can step backwards under NTP, two
/// orders can land in the same tick at any realistic resolution, and neither
/// failure is acceptable when the value *is* the queue position. A sequence
/// number is what time priority actually needs; it is also reproducible,
/// which makes the matching tests deterministic.
using Sequence = std::uint64_t;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

[[nodiscard]] constexpr Side opposite(Side side) noexcept {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

/// The two orderings that instantiate OrderBook.
///
/// This is the whole point of templating the book: the best bid is the
/// *highest* price and the best ask is the *lowest*, so with the comparator as
/// a parameter, `levels_.begin()` is the best price on either side and every
/// algorithm that walks outward from the touch is written exactly once.
/// Declared noexcept, which std::greater and std::less are not.
///
/// The commit phase claims that installing a price level and erasing one
/// cannot throw. Both reduce to comparisons, so the claim only holds if the
/// comparator is nothrow -- and comparing two integers obviously cannot throw,
/// but std::greater<Price>::operator() is not *declared* noexcept, so the
/// compiler will not take that on trust. Spelling the comparators out here
/// turns an informal argument into one static_assert can check.
struct BidOrdering {
    [[nodiscard]] constexpr bool operator()(Price lhs, Price rhs) const noexcept {
        return lhs > rhs;
    }
};

struct AskOrdering {
    [[nodiscard]] constexpr bool operator()(Price lhs, Price rhs) const noexcept {
        return lhs < rhs;
    }
};

} // namespace exchange
