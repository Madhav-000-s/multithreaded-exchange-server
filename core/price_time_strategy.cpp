#include "core/price_time_strategy.hpp"

#include "core/precondition.hpp"

#include <algorithm>
#include <deque>

namespace exchange {
namespace {

/// A resting order's simulated state during planning.
///
/// The plan has to predict a whole sweep without touching the book, and an
/// iceberg's behaviour depends on quantities that change as the sweep
/// proceeds. Mirroring those quantities locally is what lets the prediction be
/// exact while the real orders stay untouched.
struct Pending {
    PriceLevel::Iterator position;
    AccountId account;
    OrderId id;
    Quantity remaining;
    Quantity visible;
    Quantity display;
};

} // namespace

void PriceTimeStrategy::plan(const Order& aggressor, Quantity available, PriceLevel& level,
                             MatchPlan& into) const {
    // A working copy of the queue. Allocating here is deliberate and safe: it
    // happens during planning, where a throw costs nothing because the book
    // has not been touched.
    std::deque<Pending> queue;
    std::transform(level.begin(), level.end(), std::back_inserter(queue),
                   [](const std::unique_ptr<Order>& resting) {
                       return Pending{.position = {},
                                      .account = resting->account(),
                                      .id = resting->id(),
                                      .remaining = resting->remaining(),
                                      .visible = resting->visibleQty(),
                                      .display = resting->displaySize()};
                   });

    // std::transform cannot hand the lambda the iterator itself, so the
    // positions are stitched on afterwards. Both sequences are the level's
    // queue in the same order, so they line up element for element.
    auto position = level.begin();
    std::for_each(queue.begin(), queue.end(), [&position](Pending& pending) {
        pending.position = position;
        ++position;
    });

    // Work from the head, exactly as the commit phase will. An iceberg that
    // replenishes goes to the *back* of this same level, so a forward walk
    // would step past it and stop early with quantity left on both sides.
    while (available > 0 && !queue.empty()) {
        Pending& front = queue.front();

        if (front.account == aggressor.account()) {
            // Self-match prevention, "cancel newest": the aggressor stops here
            // and its remainder is cancelled by the caller.
            //
            // Rejected: cancelling the *resting* order instead, which is CME's
            // default. It keeps the aggressor working, but lets anyone vacate
            // their own queue position by sending a crossing order -- a cheap
            // reprice that modify() is careful to forbid. Rejected also:
            // decrementing both, which is the most even-handed but prints a
            // trade at a real price where no ownership changed, polluting the
            // tape and any VWAP drawn from it.
            into.selfMatchBlocked = true;
            return;
        }

        // Only displayed quantity is available. For everything but an iceberg
        // that is the whole order; for an iceberg it is the current tranche,
        // and capping here is what forces the requeue below.
        const Quantity qty = std::min(available, front.visible);
        EXCHANGE_PRECONDITION(qty > 0);

        into.fills.push_back(PlannedFill{.level = &level,
                                         .resting = front.position,
                                         // The resting order's price, never the
                                         // aggressor's -- see fill.hpp.
                                         .price = level.price(),
                                         .quantity = qty,
                                         .restingId = front.id,
                                         .restingAccount = front.account});

        front.remaining -= qty;
        front.visible -= qty;
        available -= qty;

        if (front.remaining == 0) {
            queue.pop_front();
        } else if (front.visible == 0) {
            // Hidden size remains but the tranche is spent. It refreshes and
            // goes to the back: hiding size has to cost priority, or every
            // order would be an iceberg.
            front.visible = std::min(front.display, front.remaining);
            const Pending refreshed = front;
            queue.pop_front();
            queue.push_back(refreshed);
        }
        // Otherwise the resting order still shows quantity, which can only
        // mean the aggressor is exhausted -- the loop condition ends it.
    }
}

} // namespace exchange
