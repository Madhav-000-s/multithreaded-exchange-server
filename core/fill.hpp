#pragma once

#include "core/types.hpp"

#include <iosfwd>

namespace exchange {

/// One execution between an incoming aggressor and one resting order.
///
/// A single incoming order produces a vector of these -- one per resting order
/// it consumes, potentially across several price levels.
///
/// `price` is the *resting* order's price, never the aggressor's. A buy limit
/// at 105 hitting a resting ask at 103 executes at 103; the 2 ticks of price
/// improvement belong to the aggressor. Charging the aggressor its own limit
/// would mean the book paid out less than it took in, and would remove any
/// incentive to quote inside the spread.
///
/// Deliberately a flat aggregate of trivial types: trivially copyable, so
/// moving a std::vector<Fill> during reallocation cannot throw, which is what
/// the strong exception guarantee in Phase 3 leans on.
struct Fill {
    OrderId aggressorId{};
    OrderId restingId{};
    Price price{};
    Quantity quantity{};
    AccountId aggressorAccount{};
    AccountId restingAccount{};
    Side aggressorSide{};

    /// C++20 defaulted comparison: one line replaces the member-by-member
    /// operator== that would otherwise drift as fields are added.
    [[nodiscard]] friend bool operator==(const Fill&, const Fill&) = default;
};

/// Printed by GoogleTest on a failed comparison. Without it a mismatch reports
/// as an unhelpful byte dump.
std::ostream& operator<<(std::ostream& os, const Fill& fill);

} // namespace exchange
