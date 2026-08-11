// OrderBook<PriceCompare> -- one class, two orderings. These tests exist
// mostly to prove the template genuinely serves both sides rather than being
// a bid book with an unused parameter.

#include "core/order_book.hpp"
#include "core/types.hpp"

#include "order_factory.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <type_traits>

namespace exchange {
namespace {

using test::kAlice;
using test::kBob;

TEST(OrderBookTemplate, BidBookRanksHighestPriceBest) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 100, 10, kAlice));
    bids.insert(test::limit(2, Side::Buy, 102, 10, kBob));
    bids.insert(test::limit(3, Side::Buy, 101, 10, kAlice));

    ASSERT_TRUE(bids.bestPrice().has_value());
    EXPECT_EQ(*bids.bestPrice(), 102) << "the best bid is the most a buyer will pay";
}

TEST(OrderBookTemplate, AskBookRanksLowestPriceBest) {
    AskBook asks;
    asks.insert(test::limit(1, Side::Sell, 100, 10, kAlice));
    asks.insert(test::limit(2, Side::Sell, 102, 10, kBob));
    asks.insert(test::limit(3, Side::Sell, 101, 10, kAlice));

    ASSERT_TRUE(asks.bestPrice().has_value());
    EXPECT_EQ(*asks.bestPrice(), 100) << "the best ask is the least a seller will take";
}

TEST(OrderBookTemplate, BothSidesAreTheSameClass) {
    // The comparator is the only difference. If this ever needed two separate
    // classes, every traversal would exist twice and a fix would land in one.
    static_assert(std::is_same_v<BidBook, OrderBook<BidOrdering>>);
    static_assert(std::is_same_v<AskBook, OrderBook<AskOrdering>>);
    static_assert(!std::is_same_v<BidBook, AskBook>);
    SUCCEED();
}

TEST(OrderBookTemplate, EmptySideHasNoBestPrice) {
    const BidBook bids;

    EXPECT_TRUE(bids.empty());
    EXPECT_FALSE(bids.bestPrice().has_value());
    EXPECT_EQ(bids.levelCount(), 0u);
    EXPECT_EQ(bids.orderCount(), 0u);
}

TEST(OrderBookTemplate, InsertCreatesOneLevelPerPrice) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 100, 10, kAlice));
    bids.insert(test::limit(2, Side::Buy, 100, 15, kBob));
    bids.insert(test::limit(3, Side::Buy, 99, 10, kAlice));

    EXPECT_EQ(bids.levelCount(), 2u);
    EXPECT_EQ(bids.orderCount(), 3u);
    EXPECT_EQ(bids.qtyAt(100), 25u);
    EXPECT_EQ(bids.qtyAt(99), 10u);
    EXPECT_EQ(bids.qtyAt(98), 0u) << "a price with no level has no quantity";
}

TEST(OrderBookTemplate, CancelRemovesTheOrderAndReturnsIt) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 100, 10, kAlice));

    std::unique_ptr<Order> cancelled = bids.cancel(1);

    ASSERT_NE(cancelled, nullptr);
    EXPECT_EQ(cancelled->id(), 1u);
    EXPECT_TRUE(bids.empty());
    EXPECT_EQ(bids.orderCount(), 0u);
}

TEST(OrderBookTemplate, CancelDropsALevelOnceItIsEmpty) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 100, 10, kAlice));
    bids.insert(test::limit(2, Side::Buy, 99, 10, kBob));

    (void)bids.cancel(1);

    // Otherwise bestPrice() would report 100 with nothing behind it, and the
    // map would accumulate every price ever touched.
    EXPECT_EQ(bids.levelCount(), 1u);
    EXPECT_EQ(*bids.bestPrice(), 99);
}

TEST(OrderBookTemplate, CancelLeavesTheRestOfTheLevelIntact) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 100, 10, kAlice));
    bids.insert(test::limit(2, Side::Buy, 100, 10, kBob));
    bids.insert(test::limit(3, Side::Buy, 100, 10, kAlice));

    (void)bids.cancel(2);

    EXPECT_EQ(bids.qtyAt(100), 20u);
    EXPECT_EQ(bids.orderCount(), 2u);
    EXPECT_NE(bids.find(1), nullptr);
    EXPECT_EQ(bids.find(2), nullptr);
    EXPECT_NE(bids.find(3), nullptr);
}

TEST(OrderBookTemplate, CancelOfAnUnknownOrderIsNotAnError) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 100, 10, kAlice));

    EXPECT_EQ(bids.cancel(999), nullptr);
    EXPECT_EQ(bids.orderCount(), 1u) << "a miss must not disturb the book";
}

TEST(OrderBookTemplate, CancelIsResolvedByIndexRatherThanScan) {
    // Correctness proxy for the complexity claim: the order sits at the back
    // of a long queue, and cancelling it never inspects the ones in front. A
    // timing assertion would be flaky; this at least pins the structure.
    BidBook bids;
    constexpr OrderId kTarget = 500;
    for (OrderId id = 1; id <= 1000; ++id) {
        bids.insert(test::limit(id, Side::Buy, 100, 1, kAlice));
    }

    std::unique_ptr<Order> cancelled = bids.cancel(kTarget);

    ASSERT_NE(cancelled, nullptr);
    EXPECT_EQ(cancelled->id(), kTarget);
    EXPECT_EQ(bids.orderCount(), 999u);
    EXPECT_EQ(bids.qtyAt(100), 999u);
}

TEST(OrderBookTemplate, LocateFindsARestingOrderForInPlaceAmendment) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 100, 10, kAlice));

    const auto located = bids.locate(1);

    ASSERT_TRUE(located.has_value());
    ASSERT_NE(located->level, nullptr);
    EXPECT_EQ(located->level->price(), 100);
    EXPECT_EQ((*located->position)->id(), 1u);
    EXPECT_FALSE(bids.locate(999).has_value());
}

TEST(OrderBookTemplate, TotalQuantitySumsEveryLevel) {
    AskBook asks;
    asks.insert(test::limit(1, Side::Sell, 100, 10, kAlice));
    asks.insert(test::limit(2, Side::Sell, 101, 20, kBob));
    asks.insert(test::limit(3, Side::Sell, 102, 30, kAlice));

    EXPECT_EQ(asks.totalQty(), 60u);
    EXPECT_EQ(asks.levelCount(), 3u);
}

TEST(OrderBookTemplate, HiddenIcebergSizeCountsTowardDepth) {
    BidBook bids;
    bids.insert(test::iceberg(1, Side::Buy, 100, 100, 10, kAlice));

    EXPECT_EQ(bids.qtyAt(100), 100u);
    const PriceLevel* level = bids.levelAt(100);
    ASSERT_NE(level, nullptr);
    EXPECT_EQ(level->visibleQty(), 10u);
}

} // namespace
} // namespace exchange
