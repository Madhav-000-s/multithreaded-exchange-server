#pragma once

#include "core/order.hpp"
#include "core/order_book.hpp"
#include "core/price_level.hpp"
#include "core/types.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <numeric>
#include <optional>
#include <ranges>
#include <vector>

/// Statistics over one side of a book.
///
/// Every function here is expressed with standard algorithms. That is not
/// stylistic: the algorithms name their intent, they cannot get the loop
/// bounds wrong, and each one is a place where the book's iterator has to
/// actually satisfy the contract it advertises. A hand-written loop would work
/// against the containers directly and prove nothing about the iterator.
///
/// The rule is enforced by a CTest case rather than by discipline -- see
/// tests/check_no_raw_loops.cmake.
namespace exchange::analytics {

/// One rung of an L2 market-data snapshot.
struct L2Level {
    Price price{};
    /// What a public feed would publish -- hidden iceberg size excluded.
    Quantity visibleQty{};
    /// What is actually available to an aggressor, hidden size included.
    Quantity totalQty{};
    std::size_t orderCount{};

    [[nodiscard]] friend bool operator==(const L2Level&, const L2Level&) = default;
};

/// Resting orders split by a predicate. Pointers rather than copies: Order is
/// deliberately non-copyable, and a caller filtering the book wants to
/// identify orders, not snapshot them.
struct OrderPartition {
    std::vector<const Order*> matching;
    std::vector<const Order*> rest;
};

namespace detail {

inline constexpr auto addTotalQty = [](Quantity running, const auto& entry) {
    return running + entry.second.totalQty();
};

inline constexpr auto addVisibleQty = [](Quantity running, const auto& entry) {
    return running + entry.second.visibleQty();
};

} // namespace detail

/// Total resting quantity on this side, hidden iceberg size included.
template <PriceComparator Cmp>
[[nodiscard]] Quantity restingQty(const OrderBook<Cmp>& side) {
    return std::accumulate(side.levels().begin(), side.levels().end(), Quantity{0},
                           detail::addTotalQty);
}

/// Total displayed quantity -- what the sum of an L2 feed's rungs would show.
template <PriceComparator Cmp>
[[nodiscard]] Quantity visibleQty(const OrderBook<Cmp>& side) {
    return std::accumulate(side.levels().begin(), side.levels().end(), Quantity{0},
                           detail::addVisibleQty);
}

/// Quantity an aggressor would find at prices no worse than `worst`.
///
/// ARCHITECTURE names `views::take_while` for this. `upper_bound` is used
/// instead because the map is already sorted best-first by Cmp, so the
/// eligible levels are a prefix and its boundary is O(log levels); take_while
/// would walk them linearly to discover the same point. The comparator does
/// the side-specific reasoning either way -- for bids `upper_bound` stops
/// below `worst`, for asks above it, with no branch on side anywhere.
template <PriceComparator Cmp>
[[nodiscard]] Quantity qtyThrough(const OrderBook<Cmp>& side, Price worst) {
    const auto last = side.levels().upper_bound(worst);
    return std::accumulate(side.levels().begin(), last, Quantity{0}, detail::addTotalQty);
}

/// Quantity resting within the best `depth` price levels.
template <PriceComparator Cmp>
[[nodiscard]] Quantity depthQty(const OrderBook<Cmp>& side, std::size_t depth) {
    // ranges::next with a bound clamps at end() instead of running past it,
    // which is the whole reason to prefer it over std::next here.
    const auto last = std::ranges::next(side.levels().begin(), static_cast<std::ptrdiff_t>(depth),
                                        side.levels().end());
    return std::accumulate(side.levels().begin(), last, Quantity{0}, detail::addTotalQty);
}

/// Volume-weighted average price of everything resting on this side.
///
/// Returns a double where the rest of the system uses integer ticks. The
/// distinction is deliberate: a price that has to compare exactly and index a
/// map must be integral, but VWAP is a derived statistic that is *already* not
/// a representable price, so rounding it into one would be inventing
/// precision. Notional is accumulated as double for the same reason -- an
/// integer numerator would overflow at realistic size.
template <PriceComparator Cmp>
[[nodiscard]] std::optional<double> vwap(const OrderBook<Cmp>& side) {
    struct Weighted {
        double notional;
        Quantity qty;
    };

    // transform_reduce, not accumulate: the per-level work (price x quantity)
    // is a transform and the combination is a reduction, and saying so keeps
    // them separable. It also states that the reduction is associative, which
    // is what a parallel execution policy would need.
    const Weighted total = std::transform_reduce(
        side.levels().begin(), side.levels().end(), Weighted{0.0, 0},
        [](const Weighted& lhs, const Weighted& rhs) {
            return Weighted{lhs.notional + rhs.notional, lhs.qty + rhs.qty};
        },
        [](const auto& entry) {
            const Quantity qty = entry.second.totalQty();
            return Weighted{static_cast<double>(entry.first) * static_cast<double>(qty), qty};
        });

    if (total.qty == 0) {
        return std::nullopt;
    }
    return total.notional / static_cast<double>(total.qty);
}

/// The best `depth` rungs, best price first.
template <PriceComparator Cmp>
[[nodiscard]] std::vector<L2Level> l2Snapshot(const OrderBook<Cmp>& side, std::size_t depth) {
    std::vector<L2Level> rungs;
    rungs.reserve(std::min(depth, side.levelCount()));

    std::ranges::transform(side.levels() | std::views::take(depth), std::back_inserter(rungs),
                           [](const auto& entry) {
                               const PriceLevel& level = entry.second;
                               return L2Level{.price = entry.first,
                                              .visibleQty = level.visibleQty(),
                                              .totalQty = level.totalQty(),
                                              .orderCount = level.orderCount()};
                           });
    return rungs;
}

/// Splits resting orders by a predicate, preserving book order in both halves.
template <PriceComparator Cmp, typename Predicate>
[[nodiscard]] OrderPartition partitionOrders(const OrderBook<Cmp>& side, Predicate predicate) {
    OrderPartition split;

    // The ranges overload rather than the classic one: `addresses` is a
    // transform_view whose end may be a sentinel rather than an iterator, and
    // the ranges algorithms accept that natively.
    auto addresses = side | std::views::transform([](const Order& order) { return &order; });

    std::ranges::partition_copy(addresses, std::back_inserter(split.matching),
                                std::back_inserter(split.rest),
                                [&predicate](const Order* order) { return predicate(*order); });
    return split;
}

/// Resting orders satisfying `predicate`, across every level.
template <PriceComparator Cmp, typename Predicate>
[[nodiscard]] std::size_t countOrders(const OrderBook<Cmp>& side, Predicate predicate) {
    return static_cast<std::size_t>(std::ranges::count_if(side, predicate));
}

/// The single largest resting order by remaining quantity, or nullptr.
template <PriceComparator Cmp>
[[nodiscard]] const Order* largestOrder(const OrderBook<Cmp>& side) {
    // The third argument is a projection: max_element compares
    // order.remaining() while still yielding an iterator to the order itself.
    // Without projections this needs a comparator that reaches into both
    // operands, which states the same thing less directly.
    const auto largest = std::ranges::max_element(side, std::less<>{}, &Order::remaining);
    return largest == std::ranges::end(side) ? nullptr : &*largest;
}

} // namespace exchange::analytics
