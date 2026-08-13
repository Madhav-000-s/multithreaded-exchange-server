#pragma once

#include "core/book.hpp"
#include "core/order.hpp"
#include "core/order_book.hpp"
#include "core/types.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <ostream>
#include <vector>

namespace exchange::test {

/// Everything observable about one resting order.
///
/// The strong guarantee is a statement about observable state, so the
/// comparison has to cover every field a client could ever see -- including
/// the sequence number, since a rolled-back submit that silently advanced
/// queue priority would be a real defect that a quantity-only comparison
/// would miss.
struct OrderSnapshot {
    OrderId id{};
    Price price{};
    Quantity remaining{};
    Quantity visible{};
    AccountId account{};
    Sequence sequence{};

    [[nodiscard]] friend bool operator==(const OrderSnapshot&, const OrderSnapshot&) = default;
};

inline std::ostream& operator<<(std::ostream& os, const OrderSnapshot& order) {
    return os << "{id=" << order.id << " px=" << order.price << " rem=" << order.remaining
              << " vis=" << order.visible << " acct=" << order.account << " seq=" << order.sequence
              << '}';
}

/// A complete, order-by-order picture of both sides.
///
/// Built with the Phase 2 book iterator, so the snapshot walks the book in
/// exactly the order a client would: best price first, queue order within a
/// price. Position in these vectors therefore carries queue priority, and a
/// rollback that restored the right orders in the wrong order would still fail
/// the comparison.
struct BookSnapshot {
    std::vector<OrderSnapshot> bids;
    std::vector<OrderSnapshot> asks;

    [[nodiscard]] friend bool operator==(const BookSnapshot&, const BookSnapshot&) = default;
};

inline std::ostream& operator<<(std::ostream& os, const BookSnapshot& snapshot) {
    os << "BookSnapshot{bids=[";
    std::ranges::copy(snapshot.bids, std::ostream_iterator<OrderSnapshot>(os, " "));
    os << "] asks=[";
    std::ranges::copy(snapshot.asks, std::ostream_iterator<OrderSnapshot>(os, " "));
    return os << "]}";
}

template <PriceComparator Cmp>
[[nodiscard]] inline std::vector<OrderSnapshot> snapshotSide(const OrderBook<Cmp>& side) {
    std::vector<OrderSnapshot> orders;
    orders.reserve(side.orderCount());

    // The iterator yields orders but not their price, so the level price is
    // read from the iterator itself rather than unwrapping the order optional.
    for (auto it = side.begin(); it != side.end(); ++it) {
        orders.push_back(OrderSnapshot{.id = it->id(),
                                       .price = it.price(),
                                       .remaining = it->remaining(),
                                       .visible = it->visibleQty(),
                                       .account = it->account(),
                                       .sequence = it->sequence()});
    }
    return orders;
}

[[nodiscard]] inline BookSnapshot snapshot(const Book& book) {
    return BookSnapshot{.bids = snapshotSide(book.bids()), .asks = snapshotSide(book.asks())};
}

} // namespace exchange::test
