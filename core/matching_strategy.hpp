#pragma once

#include "core/fill.hpp"
#include "core/order.hpp"
#include "core/price_level.hpp"
#include "core/types.hpp"

#include <cstddef>
#include <vector>

namespace exchange {

/// One intended execution against one resting order.
///
/// A description, not an effect. Nothing in the book has changed when this is
/// produced, and applying it is a separate, non-throwing step.
///
/// `resting` stays valid from planning through commit because the level's
/// queue is a std::list: nothing the commit does -- filling, extracting a
/// neighbour, splicing a replenished iceberg to the back -- invalidates an
/// iterator to a different node. That property is the reason the whole
/// plan-then-commit split is affordable here at all; with a vector-backed
/// level every plan entry would dangle the moment the first one was applied.
struct PlannedFill {
    PriceLevel* level{nullptr};
    PriceLevel::Iterator resting{};
    Price price{};
    Quantity quantity{};
    OrderId restingId{};
    AccountId restingAccount{};
};

/// The complete result of matching an incoming order, before any of it has
/// happened. An iceberg appears more than once when the sweep consumes
/// several of its tranches.
struct MatchPlan {
    std::vector<PlannedFill> fills;
    Quantity aggressorFilled{0};

    /// The aggressor reached its own resting order. Planning stops there and
    /// the caller cancels the remainder.
    bool selfMatchBlocked{false};
};

/// The fill rule: given an aggressor and one price level, decide who would
/// trade and for how much.
///
/// **The strategy computes; it does not mutate.** That split is what makes the
/// strong exception guarantee reachable. Every allocation -- growing the plan
/// vector, building the simulation queue -- happens while the book is still
/// untouched, so a throw at any point leaves it bit-for-bit as it was. The
/// caller then applies the plan through operations that cannot throw.
///
/// The earlier design had the strategy mutate the level as it walked, which
/// meant a std::bad_alloc while appending the fourth fill left three fills
/// applied, two orders extracted, and an index naming rows that no longer
/// existed. That is a basic guarantee at best and, for a book that is about to
/// be written to a WAL, unrecoverable.
///
/// A strategy object rather than a branch inside the book: the book is thereby
/// provably agnostic to the rule, since the strategy can reach neither the
/// index nor the level map to take a shortcut. The cost is an indirect call
/// per level, nowhere near the dominant term next to the map lookup and the
/// list pointer chase.
class MatchingStrategy {
public:
    MatchingStrategy() = default;
    MatchingStrategy(const MatchingStrategy&) = delete;
    MatchingStrategy& operator=(const MatchingStrategy&) = delete;
    MatchingStrategy(MatchingStrategy&&) = delete;
    MatchingStrategy& operator=(MatchingStrategy&&) = delete;
    virtual ~MatchingStrategy() = default;

    /// Appends to `plan` the executions that `available` quantity of
    /// `aggressor` would produce against `level`.
    ///
    /// The caller has already established that the aggressor crosses this
    /// level's price; the strategy decides only the distribution within it.
    ///
    /// `level` is taken by non-const reference solely so the plan can carry
    /// mutable iterators for the commit phase. Implementations must not modify
    /// it -- the whole exception-safety argument rests on that.
    virtual void plan(const Order& aggressor, Quantity available, PriceLevel& level,
                      MatchPlan& into) const = 0;

    /// For diagnostics and for the README's claim that the rule is pluggable.
    [[nodiscard]] virtual const char* name() const noexcept = 0;
};

} // namespace exchange
