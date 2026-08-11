// The order hierarchy in isolation: what each type does with a price, and the
// iceberg tranche mechanics that the book relies on but does not implement.

#include "core/order.hpp"
#include "core/types.hpp"

#include "order_factory.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <type_traits>

namespace exchange {
namespace {

using test::kAlice;

TEST(LimitOrder, BuyCrossesAtOrBelowItsLimit) {
    const auto order = test::limit(1, Side::Buy, 100, 10, kAlice);

    EXPECT_TRUE(order->crosses(99)) << "an ask below the limit is a better price";
    EXPECT_TRUE(order->crosses(100)) << "an ask exactly at the limit does trade";
    EXPECT_FALSE(order->crosses(101)) << "paying above the limit is the whole point of a limit";
}

TEST(LimitOrder, SellCrossesAtOrAboveItsLimit) {
    const auto order = test::limit(1, Side::Sell, 100, 10, kAlice);

    EXPECT_TRUE(order->crosses(101));
    EXPECT_TRUE(order->crosses(100));
    EXPECT_FALSE(order->crosses(99));
}

TEST(LimitOrder, RestsAtItsLimitPrice) {
    const auto order = test::limit(1, Side::Buy, 100, 10, kAlice);

    ASSERT_TRUE(order->restingPrice().has_value());
    EXPECT_EQ(*order->restingPrice(), 100);
}

TEST(LimitOrder, ShowsItsEntireSize) {
    const auto order = test::limit(1, Side::Buy, 100, 10, kAlice);

    EXPECT_EQ(order->visibleQty(), order->remaining());
}

TEST(MarketOrder, CrossesAtAnyPrice) {
    const auto order = test::market(1, Side::Buy, 10, kAlice);

    EXPECT_TRUE(order->crosses(1));
    EXPECT_TRUE(order->crosses(1'000'000));
}

TEST(MarketOrder, NeverRests) {
    const auto order = test::market(1, Side::Buy, 10, kAlice);

    // nullopt is what makes "cancel the unfilled remainder" fall out of the
    // submit path without a type test.
    EXPECT_FALSE(order->restingPrice().has_value());
}

TEST(Order, PartialFillReducesRemaining) {
    const auto order = test::limit(1, Side::Buy, 100, 10, kAlice);

    order->onPartialFill(4);

    EXPECT_EQ(order->remaining(), 6u);
    EXPECT_FALSE(order->isFilled());
}

TEST(Order, FullFillMarksTheOrderFilled) {
    const auto order = test::limit(1, Side::Buy, 100, 10, kAlice);

    order->onPartialFill(10);

    EXPECT_EQ(order->remaining(), 0u);
    EXPECT_TRUE(order->isFilled());
}

TEST(Order, PlainOrderNeverNeedsReplenishing) {
    const auto order = test::limit(1, Side::Buy, 100, 10, kAlice);

    EXPECT_FALSE(order->needsReplenish());
    order->onPartialFill(9);
    EXPECT_FALSE(order->needsReplenish()) << "a visible order always shows what remains";
    order->onPartialFill(1);
    EXPECT_FALSE(order->needsReplenish()) << "a filled order has nothing to replenish";
}

// ---------------------------------------------------------------------------
// Iceberg
// ---------------------------------------------------------------------------

TEST(IcebergOrder, ShowsOnlyItsDisplayTranche) {
    const auto order = test::iceberg(1, Side::Buy, 100, 100, 10, kAlice);

    EXPECT_EQ(order->remaining(), 100u);
    EXPECT_EQ(order->visibleQty(), 10u);
    EXPECT_EQ(order->hiddenQty(), 90u);
}

TEST(IcebergOrder, RequiresReplenishOnlyOnceTheTrancheIsSpent) {
    const auto order = test::iceberg(1, Side::Buy, 100, 100, 10, kAlice);

    order->onPartialFill(9);
    EXPECT_EQ(order->visibleQty(), 1u);
    EXPECT_FALSE(order->needsReplenish());

    order->onPartialFill(1);
    EXPECT_EQ(order->visibleQty(), 0u);
    EXPECT_TRUE(order->needsReplenish()) << "quantity remains but nothing is on display";
}

TEST(IcebergOrder, ReplenishCutsAFreshTranche) {
    const auto order = test::iceberg(1, Side::Buy, 100, 100, 10, kAlice);

    order->onPartialFill(10);
    order->replenish();

    EXPECT_EQ(order->visibleQty(), 10u);
    EXPECT_EQ(order->remaining(), 90u);
    EXPECT_EQ(order->hiddenQty(), 80u);
    EXPECT_FALSE(order->needsReplenish());
}

TEST(IcebergOrder, FinalTrancheIsTheRemainderNotTheDisplaySize) {
    const auto order = test::iceberg(1, Side::Buy, 100, 25, 10, kAlice);

    order->onPartialFill(10);
    order->replenish();
    order->onPartialFill(10);
    order->replenish();

    EXPECT_EQ(order->remaining(), 5u);
    EXPECT_EQ(order->visibleQty(), 5u) << "the last tranche cannot show more than is left";
    EXPECT_EQ(order->hiddenQty(), 0u);
}

TEST(IcebergOrder, DisplaySizeIsClampedToTotalQuantity) {
    const auto order = test::iceberg(1, Side::Buy, 100, 10, 50, kAlice);

    EXPECT_EQ(order->displaySize(), 10u);
    EXPECT_EQ(order->visibleQty(), 10u);
}

TEST(IcebergOrder, ZeroDisplaySizeFallsBackToShowingEverything) {
    // A tranche of zero would expose nothing and never trade -- a live-lock
    // rather than an error the matcher could detect.
    const auto order = test::iceberg(1, Side::Buy, 100, 40, 0, kAlice);

    EXPECT_EQ(order->visibleQty(), 40u);
    EXPECT_FALSE(order->needsReplenish());
}

TEST(IcebergOrder, ReduceToCannotIncreaseDisplayedSize) {
    const auto order = test::iceberg(1, Side::Buy, 100, 100, 10, kAlice);
    order->onPartialFill(7);
    ASSERT_EQ(order->visibleQty(), 3u);

    // Shrinking while holding queue position must not buy exposure: this is
    // the in-place amendment path, where priority is retained.
    order->reduceTo(50);

    EXPECT_EQ(order->remaining(), 50u);
    EXPECT_EQ(order->visibleQty(), 3u) << "displayed size may only fall on an in-place reduction";
}

TEST(IcebergOrder, AmendRecutsTheTranche) {
    const auto order = test::iceberg(1, Side::Buy, 100, 100, 10, kAlice);
    order->onPartialFill(7);

    // amend() is the path taken after the order has already left the book and
    // forfeited priority, so there is nothing left to protect.
    order->amend(50, 101);

    EXPECT_EQ(order->remaining(), 50u);
    EXPECT_EQ(order->visibleQty(), 10u);
    EXPECT_EQ(*order->restingPrice(), 101);
}

TEST(IcebergOrder, IsMatchedAsALimitOrder) {
    const auto order = test::iceberg(1, Side::Buy, 100, 100, 10, kAlice);

    // Inherits LimitOrder's price behaviour unchanged; only visibility differs.
    EXPECT_TRUE(order->crosses(100));
    EXPECT_FALSE(order->crosses(101));
    EXPECT_EQ(*order->restingPrice(), 100);
}

TEST(Order, DestroysCorrectlyThroughABasePointer) {
    // The book owns orders as unique_ptr<Order> and destroys them through it.
    // Without a virtual destructor this leaks the derived members silently.
    std::unique_ptr<Order> order = test::iceberg(1, Side::Buy, 100, 100, 10, kAlice);

    EXPECT_EQ(order->visibleQty(), 10u) << "virtual dispatch through the base works";
    EXPECT_TRUE(std::has_virtual_destructor_v<Order>);
}

} // namespace
} // namespace exchange
