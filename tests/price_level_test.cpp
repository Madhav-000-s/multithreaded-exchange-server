// PriceLevel: FIFO discipline, the cached aggregates, and the iterator
// stability the book's O(1) cancel depends on.

#include "core/price_level.hpp"
#include "core/types.hpp"

#include "order_factory.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace exchange {
namespace {

using test::kAlice;
using test::kBob;
using test::kCarol;

TEST(PriceLevel, EnqueuePreservesArrivalOrder) {
    PriceLevel level(100);
    level.enqueue(test::limit(1, Side::Buy, 100, 10, kAlice));
    level.enqueue(test::limit(2, Side::Buy, 100, 10, kBob));
    level.enqueue(test::limit(3, Side::Buy, 100, 10, kCarol));

    std::vector<OrderId> order;
    for (const auto& resting : level) {
        order.push_back(resting->id());
    }

    // Time priority is not sorted for -- it is the container's order, produced
    // by append-only insertion.
    EXPECT_EQ(order, (std::vector<OrderId>{1, 2, 3}));
}

TEST(PriceLevel, TracksTotalAndVisibleQuantity) {
    PriceLevel level(100);
    level.enqueue(test::limit(1, Side::Buy, 100, 10, kAlice));
    level.enqueue(test::limit(2, Side::Buy, 100, 25, kBob));

    EXPECT_EQ(level.totalQty(), 35u);
    EXPECT_EQ(level.visibleQty(), 35u);
    EXPECT_EQ(level.orderCount(), 2u);
}

TEST(PriceLevel, VisibleQuantityExcludesHiddenIcebergSize) {
    PriceLevel level(100);
    level.enqueue(test::limit(1, Side::Buy, 100, 10, kAlice));
    level.enqueue(test::iceberg(2, Side::Buy, 100, 100, 10, kBob));

    EXPECT_EQ(level.totalQty(), 110u) << "depth counts hidden size";
    EXPECT_EQ(level.visibleQty(), 20u) << "an L2 feed would publish only what is shown";
}

TEST(PriceLevel, ExtractReturnsOwnershipAndUpdatesAggregates) {
    PriceLevel level(100);
    const auto first = level.enqueue(test::limit(1, Side::Buy, 100, 10, kAlice));
    level.enqueue(test::limit(2, Side::Buy, 100, 25, kBob));

    std::unique_ptr<Order> removed = level.extract(first);

    ASSERT_NE(removed, nullptr);
    EXPECT_EQ(removed->id(), 1u);
    EXPECT_EQ(level.totalQty(), 25u);
    EXPECT_EQ(level.visibleQty(), 25u);
    EXPECT_EQ(level.orderCount(), 1u);
}

TEST(PriceLevel, ExtractLeavesOtherIteratorsValid) {
    PriceLevel level(100);
    const auto a = level.enqueue(test::limit(1, Side::Buy, 100, 10, kAlice));
    const auto b = level.enqueue(test::limit(2, Side::Buy, 100, 10, kBob));
    const auto c = level.enqueue(test::limit(3, Side::Buy, 100, 10, kCarol));

    const std::unique_ptr<Order> discarded = level.extract(a);

    // This is the entire reason storage is a std::list. The book keeps these
    // iterators in a hash index; a vector or deque would have invalidated both
    // of the survivors here and left every index entry dangling.
    EXPECT_EQ((*b)->id(), 2u);
    EXPECT_EQ((*c)->id(), 3u);
}

TEST(PriceLevel, ApplyFillReducesBothAggregates) {
    PriceLevel level(100);
    const auto first = level.enqueue(test::limit(1, Side::Buy, 100, 10, kAlice));

    level.applyFill(first, 4);

    EXPECT_EQ(level.totalQty(), 6u);
    EXPECT_EQ(level.visibleQty(), 6u);
    EXPECT_EQ((*first)->remaining(), 6u);
}

TEST(PriceLevel, ReplenishSendsTheIcebergToTheBack) {
    PriceLevel level(100);
    const auto ice = level.enqueue(test::iceberg(1, Side::Buy, 100, 100, 10, kAlice));
    level.enqueue(test::limit(2, Side::Buy, 100, 10, kBob));

    level.applyFill(ice, 10);
    ASSERT_TRUE((*ice)->needsReplenish());

    level.replenishAndRequeue(ice);

    std::vector<OrderId> order;
    for (const auto& resting : level) {
        order.push_back(resting->id());
    }
    EXPECT_EQ(order, (std::vector<OrderId>{2, 1})) << "hiding size costs priority";
    EXPECT_EQ(level.visibleQty(), 20u) << "a fresh tranche is back on display";
}

TEST(PriceLevel, RequeueKeepsTheIteratorValid) {
    PriceLevel level(100);
    const auto ice = level.enqueue(test::iceberg(1, Side::Buy, 100, 100, 10, kAlice));
    level.enqueue(test::limit(2, Side::Buy, 100, 10, kBob));

    level.applyFill(ice, 10);
    level.replenishAndRequeue(ice);

    // splice relinks the node without touching the element, so the book's
    // index entry needs no update at all when an iceberg loses priority.
    EXPECT_EQ((*ice)->id(), 1u);
    EXPECT_EQ((*ice)->remaining(), 90u);
}

TEST(PriceLevel, ReduceQuantityKeepsQueuePosition) {
    PriceLevel level(100);
    const auto a = level.enqueue(test::limit(1, Side::Buy, 100, 10, kAlice));
    level.enqueue(test::limit(2, Side::Buy, 100, 10, kBob));

    level.reduceQuantity(a, 4);

    EXPECT_EQ(level.begin()->get()->id(), 1u) << "reducing takes nothing from those behind";
    EXPECT_EQ(level.totalQty(), 14u);
    EXPECT_EQ(level.visibleQty(), 14u);
}

TEST(PriceLevel, EmptyLevelReportsZeroes) {
    const PriceLevel level(100);

    EXPECT_TRUE(level.empty());
    EXPECT_EQ(level.orderCount(), 0u);
    EXPECT_EQ(level.totalQty(), 0u);
    EXPECT_EQ(level.price(), 100);
}

} // namespace
} // namespace exchange
