#include "core/order.hpp"

#include <algorithm>
#include <cassert>

namespace exchange {

Order::Order(OrderId id, Side side, AccountId account, Quantity qty) noexcept
    : id_(id), remaining_(qty), account_(account), side_(side) {}

void Order::onPartialFill(Quantity qty) noexcept {
    assert(qty <= remaining_ && "fill exceeds remaining quantity");
    remaining_ -= qty;
}

void Order::amend(Quantity newQty, Price) noexcept {
    remaining_ = newQty;
}

void Order::reduceTo(Quantity newQty) noexcept {
    assert(newQty <= remaining_ && "reduceTo cannot grow an order");
    remaining_ = newQty;
}

// ---------------------------------------------------------------------------

LimitOrder::LimitOrder(OrderId id, Side side, AccountId account, Quantity qty, Price limit) noexcept
    : Order(id, side, account, qty), limit_(limit) {}

bool LimitOrder::crosses(Price best) const noexcept {
    // A buy is willing to pay up to its limit, so it trades against any ask at
    // or below it. A sell is the mirror image. Note the >= and <=: an order
    // priced exactly at the touch does trade.
    return side() == Side::Buy ? limit_ >= best : limit_ <= best;
}

void LimitOrder::amend(Quantity newQty, Price newPrice) noexcept {
    Order::amend(newQty, newPrice);
    limit_ = newPrice;
}

// ---------------------------------------------------------------------------

MarketOrder::MarketOrder(OrderId id, Side side, AccountId account, Quantity qty) noexcept
    : Order(id, side, account, qty) {}

// ---------------------------------------------------------------------------

IcebergOrder::IcebergOrder(OrderId id, Side side, AccountId account, Quantity totalQty, Price limit,
                           Quantity displaySize) noexcept
    : LimitOrder(id, side, account, totalQty, limit),
      // A display size of zero would expose nothing and never trade, which is
      // a live-lock rather than an error the matcher could detect. Clamping
      // keeps the invariant "a resting order always has visible quantity"
      // total. Rejecting it properly belongs with the rest of validation in
      // Phase 3.
      display_(displaySize == 0 ? totalQty : std::min(displaySize, totalQty)),
      visible_(display_) {}

void IcebergOrder::onPartialFill(Quantity qty) noexcept {
    assert(qty <= visible_ && "fill exceeds the visible tranche");
    LimitOrder::onPartialFill(qty);
    visible_ -= qty;
}

void IcebergOrder::replenish() noexcept {
    visible_ = std::min(display_, remaining());
}

void IcebergOrder::amend(Quantity newQty, Price newPrice) noexcept {
    LimitOrder::amend(newQty, newPrice);
    // The tranche is re-cut against the new size rather than carried over: an
    // amendment already forfeits priority, so there is nothing to preserve,
    // and a stale visible_ larger than remaining() would break the
    // qty <= visibleQty() precondition on the next fill.
    display_ = std::min(display_, newQty);
    visible_ = std::min(display_, newQty);
}

void IcebergOrder::reduceTo(Quantity newQty) noexcept {
    LimitOrder::reduceTo(newQty);
    // Note the asymmetry with amend(): the tranche is clamped, never re-cut.
    // Displayed size can only fall, so shrinking cannot buy exposure.
    visible_ = std::min(visible_, newQty);
    display_ = std::min(display_, newQty);
}

} // namespace exchange
