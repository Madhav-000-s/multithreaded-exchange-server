#include "core/price_time_strategy.hpp"

#include <algorithm>
#include <cassert>
#include <utility>

namespace exchange {

MatchResult PriceTimeStrategy::match(Order& aggressor, PriceLevel& level) {
    MatchResult result;

    // Always re-read the front rather than advancing an iterator. An iceberg
    // that replenishes is spliced to the *back* of this same level, so a
    // forward walk would step past it and never come back -- a large aggressor
    // would stop early with quantity left and quantity available. Working from
    // the head instead means a requeued order is naturally reachable again
    // once everything ahead of it is consumed.
    while (aggressor.remaining() > 0 && !level.empty()) {
        const PriceLevel::Iterator front = level.begin();
        Order& resting = **front;

        if (resting.account() == aggressor.account()) {
            // Self-match prevention, "cancel newest": the aggressor stops
            // here and its remainder is cancelled by the caller.
            //
            // Rejected: cancelling the *resting* order instead, which is what
            // CME defaults to. It keeps the aggressor working, but it lets
            // anyone remove their own queue position by sending a crossing
            // order -- a cheap way to jump the queue on the reprice. Rejected
            // also: decrementing both, which is the most "fair" but produces
            // a phantom trade at a real price on the public tape.
            //
            // Cancelling the aggressor is the conservative choice: no trade is
            // printed, no resting priority is disturbed, and the participant
            // who created the situation is the one who absorbs it. It also
            // cannot leave the book crossed, because the aggressor does not
            // rest afterwards.
            result.selfMatchBlocked = true;
            break;
        }

        // Only displayed quantity is available. For everything but an iceberg
        // that is the whole order; for an iceberg it is the current tranche,
        // and capping here is what forces the requeue below.
        const Quantity qty = std::min(aggressor.remaining(), resting.visibleQty());
        assert(qty > 0 && "a resting order must always show quantity");

        result.fills.push_back(Fill{
            .aggressorId = aggressor.id(),
            .restingId = resting.id(),
            // The resting order's price, not the aggressor's -- see fill.hpp.
            .price = level.price(),
            .quantity = qty,
            .aggressorAccount = aggressor.account(),
            .restingAccount = resting.account(),
            .aggressorSide = aggressor.side(),
        });

        level.applyFill(front, qty);
        aggressor.onPartialFill(qty);

        if (resting.isFilled()) {
            result.filled.push_back(level.extract(front));
        } else if (resting.needsReplenish()) {
            // Hidden size remains but the tranche is spent. Refresh and go to
            // the back: hiding size has to cost priority, or every order would
            // be an iceberg.
            level.replenishAndRequeue(front);
        }
        // Otherwise the resting order still shows quantity, which can only
        // mean the aggressor is exhausted -- the loop condition ends it.
    }

    return result;
}

} // namespace exchange
