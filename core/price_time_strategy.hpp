#pragma once

#include "core/matching_strategy.hpp"
#include "core/order.hpp"
#include "core/price_level.hpp"
#include "core/types.hpp"

namespace exchange {

/// Price-time priority: within a level, the order that arrived first trades
/// first, in full, before the next one gets anything.
///
/// Chosen as the default because it is what the overwhelming majority of
/// equity and futures venues run, and because the incentive it creates is the
/// one an exchange wants -- being early is the only way to gain priority, so
/// participants compete by quoting sooner and tighter. Pro-rata allocates by
/// size instead, which rewards quoting *bigger*; that suits deeply fungible
/// contracts like short-term rate futures, where it discourages the queue from
/// being monopolised by whoever has the fastest connection, but it invites
/// participants to inflate size they do not intend to trade.
///
/// Time priority is not stored or sorted on: it is the queue order in the
/// underlying std::list, established by append-only insertion. Sorting by a
/// sequence number would be the same answer computed more expensively.
class PriceTimeStrategy final : public MatchingStrategy {
public:
    void plan(const Order& aggressor, Quantity available, PriceLevel& level,
              MatchPlan& into) const override;

    [[nodiscard]] const char* name() const noexcept override { return "price-time"; }
};

} // namespace exchange
