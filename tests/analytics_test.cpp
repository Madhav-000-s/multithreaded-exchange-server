// The analytics layer. Every function under test is written with standard
// algorithms; that the layer contains no hand-written loops is checked
// separately by the AnalyticsHasNoRawLoops CTest case.

#include "analytics/book_metrics.hpp"
#include "analytics/side_stats.hpp"
#include "core/book.hpp"
#include "core/order_book.hpp"
#include "core/types.hpp"

#include "order_factory.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace exchange::analytics {
namespace {

using test::kAlice;
using test::kBob;
using test::kCarol;

constexpr double kTolerance = 1e-9;

// ---------------------------------------------------------------------------
// Depth
// ---------------------------------------------------------------------------

TEST(Analytics, RestingQtyIncludesHiddenSize) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 100, 10, kAlice));
    bids.insert(test::iceberg(2, Side::Buy, 99, 100, 10, kBob));

    EXPECT_EQ(restingQty(bids), 110u);
    EXPECT_EQ(visibleQty(bids), 20u) << "an L2 feed publishes only the tranche";
}

TEST(Analytics, RestingQtyOfAnEmptySideIsZero) {
    const BidBook bids;

    EXPECT_EQ(restingQty(bids), 0u);
    EXPECT_EQ(visibleQty(bids), 0u);
}

TEST(Analytics, DepthQtyIsBoundedByTheLevelCount) {
    AskBook asks;
    asks.insert(test::limit(1, Side::Sell, 100, 10, kAlice));
    asks.insert(test::limit(2, Side::Sell, 101, 20, kBob));
    asks.insert(test::limit(3, Side::Sell, 102, 30, kCarol));

    EXPECT_EQ(depthQty(asks, 0), 0u);
    EXPECT_EQ(depthQty(asks, 1), 10u);
    EXPECT_EQ(depthQty(asks, 2), 30u);
    EXPECT_EQ(depthQty(asks, 3), 60u);
    EXPECT_EQ(depthQty(asks, 99), 60u) << "ranges::next clamps at end rather than running past";
}

TEST(Analytics, QtyThroughAPriceStopsAtTheBoundOnTheAskSide) {
    AskBook asks;
    asks.insert(test::limit(1, Side::Sell, 100, 10, kAlice));
    asks.insert(test::limit(2, Side::Sell, 101, 20, kBob));
    asks.insert(test::limit(3, Side::Sell, 102, 30, kCarol));

    EXPECT_EQ(qtyThrough(asks, 99), 0u) << "nothing is offered that cheaply";
    EXPECT_EQ(qtyThrough(asks, 100), 10u);
    EXPECT_EQ(qtyThrough(asks, 101), 30u) << "inclusive of the bound";
    EXPECT_EQ(qtyThrough(asks, 500), 60u);
}

TEST(Analytics, QtyThroughAPriceRunsTheOtherWayOnTheBidSide) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 100, 10, kAlice));
    bids.insert(test::limit(2, Side::Buy, 99, 20, kBob));
    bids.insert(test::limit(3, Side::Buy, 98, 30, kCarol));

    // Same call, opposite direction. The comparator carries the asymmetry, so
    // there is no branch on side anywhere in the implementation.
    EXPECT_EQ(qtyThrough(bids, 101), 0u);
    EXPECT_EQ(qtyThrough(bids, 100), 10u);
    EXPECT_EQ(qtyThrough(bids, 99), 30u);
    EXPECT_EQ(qtyThrough(bids, 0), 60u);
}

// ---------------------------------------------------------------------------
// VWAP
// ---------------------------------------------------------------------------

TEST(Analytics, VwapOfAnEmptySideIsAbsent) {
    const BidBook bids;

    EXPECT_FALSE(vwap(bids).has_value());
}

TEST(Analytics, VwapWeightsPriceByQuantity) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 100, 30, kAlice));
    bids.insert(test::limit(2, Side::Buy, 90, 10, kBob));

    // (100*30 + 90*10) / 40 = 97.5 -- pulled toward the larger resting size,
    // and deliberately not a representable tick.
    const auto value = vwap(bids);
    ASSERT_TRUE(value.has_value());
    EXPECT_NEAR(*value, 97.5, kTolerance);
}

TEST(Analytics, VwapOfASingleLevelIsThatPrice) {
    AskBook asks;
    asks.insert(test::limit(1, Side::Sell, 250, 7, kAlice));

    const auto value = vwap(asks);
    ASSERT_TRUE(value.has_value());
    EXPECT_NEAR(*value, 250.0, kTolerance);
}

TEST(Analytics, VwapCountsHiddenIcebergSize) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 100, 10, kAlice));
    bids.insert(test::iceberg(2, Side::Buy, 50, 90, 5, kBob));

    // (100*10 + 50*90) / 100 = 55, not the 66.67 a visible-only view sees.
    const auto value = vwap(bids);
    ASSERT_TRUE(value.has_value());
    EXPECT_NEAR(*value, 55.0, kTolerance);
}

// ---------------------------------------------------------------------------
// L2 snapshot
// ---------------------------------------------------------------------------

TEST(Analytics, L2SnapshotReportsBestFirst) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 100, 10, kAlice));
    bids.insert(test::limit(2, Side::Buy, 102, 5, kBob));
    bids.insert(test::limit(3, Side::Buy, 102, 15, kCarol));

    const std::vector<L2Level> rungs = l2Snapshot(bids, 5);

    ASSERT_EQ(rungs.size(), 2u);
    EXPECT_EQ(rungs[0], (L2Level{.price = 102, .visibleQty = 20, .totalQty = 20, .orderCount = 2}));
    EXPECT_EQ(rungs[1].price, 100);
    EXPECT_EQ(rungs[1].orderCount, 1u);
}

TEST(Analytics, L2SnapshotIsTruncatedToTheRequestedDepth) {
    AskBook asks;
    asks.insert(test::limit(1, Side::Sell, 100, 10, kAlice));
    asks.insert(test::limit(2, Side::Sell, 101, 10, kBob));
    asks.insert(test::limit(3, Side::Sell, 102, 10, kCarol));

    EXPECT_EQ(l2Snapshot(asks, 2).size(), 2u);
    EXPECT_EQ(l2Snapshot(asks, 0).size(), 0u);
    EXPECT_EQ(l2Snapshot(asks, 99).size(), 3u);
}

TEST(Analytics, L2SnapshotSeparatesVisibleFromTotal) {
    AskBook asks;
    asks.insert(test::iceberg(1, Side::Sell, 100, 100, 10, kAlice));

    const std::vector<L2Level> rungs = l2Snapshot(asks, 1);

    ASSERT_EQ(rungs.size(), 1u);
    EXPECT_EQ(rungs[0].visibleQty, 10u) << "what the feed shows";
    EXPECT_EQ(rungs[0].totalQty, 100u) << "what is actually there";
}

// ---------------------------------------------------------------------------
// Filtering
// ---------------------------------------------------------------------------

TEST(Analytics, PartitionSplitsOrdersAndKeepsBookOrder) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 102, 10, kAlice));
    bids.insert(test::limit(2, Side::Buy, 101, 10, kBob));
    bids.insert(test::limit(3, Side::Buy, 100, 10, kAlice));

    const OrderPartition split =
        partitionOrders(bids, [](const Order& order) { return order.account() == kAlice; });

    ASSERT_EQ(split.matching.size(), 2u);
    ASSERT_EQ(split.rest.size(), 1u);
    EXPECT_EQ(split.matching[0]->id(), 1u) << "best price first, as the book walks";
    EXPECT_EQ(split.matching[1]->id(), 3u);
    EXPECT_EQ(split.rest[0]->id(), 2u);
}

TEST(Analytics, PartitionOfAnEmptySideYieldsTwoEmptyHalves) {
    const BidBook bids;

    const OrderPartition split = partitionOrders(bids, [](const Order&) { return true; });

    EXPECT_TRUE(split.matching.empty());
    EXPECT_TRUE(split.rest.empty());
}

TEST(Analytics, CountOrdersAppliesThePredicateAcrossLevels) {
    AskBook asks;
    asks.insert(test::limit(1, Side::Sell, 100, 5, kAlice));
    asks.insert(test::limit(2, Side::Sell, 100, 50, kBob));
    asks.insert(test::limit(3, Side::Sell, 101, 500, kCarol));

    EXPECT_EQ(countOrders(asks, [](const Order& o) { return o.remaining() >= 50; }), 2u);
    EXPECT_EQ(countOrders(asks, [](const Order&) { return false; }), 0u);
}

TEST(Analytics, LargestOrderUsesAProjectionOverRemainingQuantity) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 102, 10, kAlice));
    bids.insert(test::limit(2, Side::Buy, 100, 999, kBob));
    bids.insert(test::limit(3, Side::Buy, 101, 50, kCarol));

    const Order* largest = largestOrder(bids);

    ASSERT_NE(largest, nullptr);
    EXPECT_EQ(largest->id(), 2u) << "biggest by size, not by price";
}

TEST(Analytics, LargestOrderOfAnEmptySideIsNull) {
    const BidBook bids;

    EXPECT_EQ(largestOrder(bids), nullptr);
}

// ---------------------------------------------------------------------------
// Book-level metrics
// ---------------------------------------------------------------------------

TEST(Analytics, MetricsAreAbsentUntilBothSidesAreQuoted) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 100, 10, kAlice));

    EXPECT_FALSE(midPrice(book).has_value());
    EXPECT_FALSE(microPrice(book).has_value());
    EXPECT_FALSE(imbalance(book).has_value());
    EXPECT_FALSE(spreadTicks(book).has_value());
}

TEST(Analytics, MidPriceIsTheArithmeticMidpoint) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 100, 10, kAlice));
    book.submit(test::limit(2, Side::Sell, 104, 10, kBob));

    const auto mid = midPrice(book);
    ASSERT_TRUE(mid.has_value());
    EXPECT_NEAR(*mid, 102.0, kTolerance);
    EXPECT_EQ(*spreadTicks(book), 4);
}

TEST(Analytics, MidPriceCanFallBetweenTicks) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 100, 10, kAlice));
    book.submit(test::limit(2, Side::Sell, 101, 10, kBob));

    const auto mid = midPrice(book);
    ASSERT_TRUE(mid.has_value());
    EXPECT_NEAR(*mid, 100.5, kTolerance) << "integer arithmetic would have floored this";
}

TEST(Analytics, MicroPriceLeansTowardTheHeavierSide) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 100, 90, kAlice));
    book.submit(test::limit(2, Side::Sell, 110, 10, kBob));

    // (110*90 + 100*10) / 100 = 109. A large resting bid means buyers are
    // queued and sellers are scarce, so fair value sits near the offer.
    const auto micro = microPrice(book);
    ASSERT_TRUE(micro.has_value());
    EXPECT_NEAR(*micro, 109.0, kTolerance);
    EXPECT_GT(*micro, *midPrice(book));
}

TEST(Analytics, MicroPriceEqualsMidWhenSizeIsBalanced) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 100, 25, kAlice));
    book.submit(test::limit(2, Side::Sell, 110, 25, kBob));

    EXPECT_NEAR(*microPrice(book), *midPrice(book), kTolerance);
}

TEST(Analytics, ImbalanceIsSignedAndBounded) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 100, 75, kAlice));
    book.submit(test::limit(2, Side::Sell, 110, 25, kBob));

    const auto skew = imbalance(book);
    ASSERT_TRUE(skew.has_value());
    EXPECT_NEAR(*skew, 0.5, kTolerance) << "(75 - 25) / 100";
    EXPECT_GE(*skew, -1.0);
    EXPECT_LE(*skew, 1.0);
}

TEST(Analytics, ImbalanceIsZeroWhenTheTouchIsBalanced) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 100, 40, kAlice));
    book.submit(test::limit(2, Side::Sell, 110, 40, kBob));

    EXPECT_NEAR(*imbalance(book), 0.0, kTolerance);
}

TEST(Analytics, TouchQtyReadsOnlyTheBestLevel) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 100, 10, kAlice));
    book.submit(test::limit(2, Side::Buy, 99, 500, kAlice));
    book.submit(test::limit(3, Side::Sell, 110, 20, kBob));

    const TouchQty touch = touchQty(book);

    EXPECT_EQ(touch.bid, 10u) << "the 99 level is behind the touch";
    EXPECT_EQ(touch.ask, 20u);
}

TEST(Analytics, TouchQtyIsZeroOnAnUnquotedSide) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 100, 10, kAlice));

    const TouchQty touch = touchQty(book);

    EXPECT_EQ(touch.bid, 10u);
    EXPECT_EQ(touch.ask, 0u);
}

// ---------------------------------------------------------------------------
// Against a traded book, not just a constructed one
// ---------------------------------------------------------------------------

TEST(Analytics, ReflectTheBookAfterMatching) {
    Book book;
    book.submit(test::limit(1, Side::Sell, 100, 10, kAlice));
    book.submit(test::limit(2, Side::Sell, 101, 10, kAlice));
    book.submit(test::limit(3, Side::Buy, 100, 4, kBob));

    EXPECT_EQ(restingQty(book.asks()), 16u) << "6 left at 100, 10 at 101";
    EXPECT_EQ(qtyThrough(book.asks(), 100), 6u);

    const auto value = vwap(book.asks());
    ASSERT_TRUE(value.has_value());
    EXPECT_NEAR(*value, (100.0 * 6 + 101.0 * 10) / 16.0, kTolerance);
}

} // namespace
} // namespace exchange::analytics
