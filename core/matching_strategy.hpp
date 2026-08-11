#pragma once

#include "core/fill.hpp"
#include "core/order.hpp"
#include "core/price_level.hpp"

#include <memory>
#include <vector>

namespace exchange {

/// What one pass of matching against a single price level produced.
struct MatchResult {
    std::vector<Fill> fills;

    /// Resting orders fully consumed, already unlinked from the level.
    ///
    /// Ownership is handed back rather than dropped on the floor: the book
    /// still has index entries naming these orders and must erase them, and
    /// returning the unique_ptr makes that obligation visible in the type
    /// instead of leaving it as a comment. It also keeps Phase 3's rollback
    /// possible -- an order that has been moved out can be put back, one that
    /// has been destroyed cannot.
    std::vector<std::unique_ptr<Order>> filled;

    /// Set when the aggressor reached its own resting order. Matching stops
    /// there and the caller cancels the remainder; see PriceTimeStrategy for
    /// why that policy was chosen over the alternatives.
    bool selfMatchBlocked{false};
};

/// The fill rule: given an aggressor and one price level, decide who trades
/// and for how much.
///
/// A strategy object rather than a branch inside the book. The payoff is that
/// the book is provably agnostic to the rule -- price-time and pro-rata differ
/// only in how they walk the queue, and neither can reach the book's index or
/// its level map to take a shortcut. Swapping the rule is a constructor
/// argument, and adding one touches no existing file.
///
/// The cost, stated honestly, is an indirect call per level per order. It is
/// nowhere near the hot path's dominant term -- the map lookup and the list
/// pointer chase are -- so the extensibility is worth paying for here. That is
/// not true everywhere, and the Order hierarchy makes the same trade for the
/// same reason.
class MatchingStrategy {
public:
    MatchingStrategy() = default;
    MatchingStrategy(const MatchingStrategy&) = delete;
    MatchingStrategy& operator=(const MatchingStrategy&) = delete;
    MatchingStrategy(MatchingStrategy&&) = delete;
    MatchingStrategy& operator=(MatchingStrategy&&) = delete;
    virtual ~MatchingStrategy() = default;

    /// Trades `aggressor` against `level` until one of them is exhausted.
    ///
    /// The caller has already established that the aggressor crosses this
    /// level's price; the strategy decides only the distribution within it.
    virtual MatchResult match(Order& aggressor, PriceLevel& level) = 0;

    /// For diagnostics and for the README's claim that the rule is pluggable.
    [[nodiscard]] virtual const char* name() const noexcept = 0;
};

} // namespace exchange
