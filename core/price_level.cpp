#include "core/price_level.hpp"

#include "core/precondition.hpp"

#include <cassert>
#include <utility>

namespace exchange {

PriceLevel::Iterator PriceLevel::enqueue(std::unique_ptr<Order> order) {
    assert(order != nullptr);
    totalQty_ += order->remaining();
    visibleQty_ += order->visibleQty();
    orders_.push_back(std::move(order));
    return std::prev(orders_.end());
}

std::unique_ptr<Order> PriceLevel::extract(Iterator where) noexcept {
    assert(where != orders_.end());

    std::unique_ptr<Order> order = std::move(*where);
    orders_.erase(where);

    assert(totalQty_ >= order->remaining());
    assert(visibleQty_ >= order->visibleQty());
    totalQty_ -= order->remaining();
    visibleQty_ -= order->visibleQty();

    return order;
}

void PriceLevel::applyFill(Iterator where, Quantity qty) noexcept {
    assert(where != orders_.end());
    Order& order = **where;

    // An execution always consumes displayed quantity, so both aggregates move
    // by the same amount. Hidden size only becomes visible via replenish().
    assert(qty <= order.visibleQty());
    assert(totalQty_ >= qty && visibleQty_ >= qty);

    order.onPartialFill(qty);
    totalQty_ -= qty;
    visibleQty_ -= qty;
}

void PriceLevel::replenishAndRequeue(Iterator where) noexcept {
    assert(where != orders_.end());
    Order& order = **where;
    assert(order.needsReplenish());

    order.replenish();
    visibleQty_ += order.visibleQty(); // was zero before the refresh

    // splice() within the same list relinks the node in place. The element is
    // never moved or reassigned, so `where` -- and therefore the book's index
    // entry pointing at it -- remains valid. Losing priority costs nothing
    // beyond three pointer writes.
    orders_.splice(orders_.end(), orders_, where);
}

void PriceLevel::reduceQuantity(Iterator where, Quantity newQty) noexcept {
    assert(where != orders_.end());
    Order& order = **where;
    assert(newQty <= order.remaining() && "reduceQuantity cannot grow an order");

    const Quantity oldTotal = order.remaining();
    const Quantity oldVisible = order.visibleQty();

    order.reduceTo(newQty);

    totalQty_ = totalQty_ - oldTotal + order.remaining();
    visibleQty_ = visibleQty_ - oldVisible + order.visibleQty();
}

void PriceLevel::adopt(Queue& from, Iterator position) noexcept {
    EXCHANGE_PRECONDITION(position != from.end());
    const Order& order = **position;

    totalQty_ += order.remaining();
    visibleQty_ += order.visibleQty();

    // Relinks the node between lists. No allocation, no element move, and
    // `position` continues to name the same order afterwards, so the book's
    // index entry can be built before this runs.
    orders_.splice(orders_.end(), from, position);
}

} // namespace exchange
