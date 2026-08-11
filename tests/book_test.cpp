// Book: submit, cancel and modify against both sides. This file carries the
// scenarios that define whether the matcher is correct at all.

#include "core/book.hpp"
#include "core/fill.hpp"
#include "core/types.hpp"

#include "order_factory.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace exchange {
namespace {

using test::kAlice;
using test::kBob;
using test::kCarol;

[[nodiscard]] std::vector<OrderId> restingIds(const std::vector<Fill>& fills) {
    std::vector<OrderId> ids;
    ids.reserve(fills.size());
    for (const Fill& fill : fills) {
        ids.push_back(fill.restingId);
    }
    return ids;
}

[[nodiscard]] std::vector<Price> fillPrices(const std::vector<Fill>& fills) {
    std::vector<Price> prices;
    prices.reserve(fills.size());
    for (const Fill& fill : fills) {
        prices.push_back(fill.price);
    }
    return prices;
}

// ---------------------------------------------------------------------------
// Resting
// ---------------------------------------------------------------------------

TEST(BookSubmit, NonCrossingOrderRests) {
    Book book;

    const SubmitResult result = book.submit(test::limit(1, Side::Buy, 100, 10, kAlice));

    EXPECT_EQ(result.status, SubmitStatus::Resting);
    EXPECT_TRUE(result.fills.empty());
    EXPECT_EQ(result.restingQty, 10u);
    EXPECT_EQ(*book.bestBid(), 100);
    EXPECT_FALSE(book.bestAsk().has_value());
}

TEST(BookSubmit, TwoSidesQuoteWithoutTrading) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 99, 10, kAlice));
    book.submit(test::limit(2, Side::Sell, 101, 10, kBob));

    EXPECT_EQ(*book.bestBid(), 99);
    EXPECT_EQ(*book.bestAsk(), 101);
    ASSERT_TRUE(book.spread().has_value());
    EXPECT_EQ(*book.spread(), 2);
    EXPECT_FALSE(book.isCrossed());
}

// ---------------------------------------------------------------------------
// Fills
// ---------------------------------------------------------------------------

TEST(BookMatching, PartialFillLeavesTheRestingOrderInPlace) {
    Book book;
    book.submit(test::limit(1, Side::Sell, 100, 10, kAlice));

    const SubmitResult result = book.submit(test::limit(2, Side::Buy, 100, 4, kBob));

    EXPECT_EQ(result.status, SubmitStatus::Filled) << "the aggressor got everything it asked for";
    EXPECT_EQ(result.filledQty, 4u);
    ASSERT_EQ(result.fills.size(), 1u);
    EXPECT_EQ(result.fills[0].quantity, 4u);

    // The resting order is the one partially filled.
    EXPECT_EQ(book.asks().qtyAt(100), 6u);
    ASSERT_NE(book.find(1), nullptr);
    EXPECT_EQ(book.find(1)->remaining(), 6u);
}

TEST(BookMatching, AggressorPartiallyFilledThenRests) {
    Book book;
    book.submit(test::limit(1, Side::Sell, 100, 4, kAlice));

    const SubmitResult result = book.submit(test::limit(2, Side::Buy, 100, 10, kBob));

    EXPECT_EQ(result.status, SubmitStatus::PartiallyFilledResting);
    EXPECT_EQ(result.filledQty, 4u);
    EXPECT_EQ(result.restingQty, 6u);
    EXPECT_TRUE(book.asks().empty());
    EXPECT_EQ(book.bids().qtyAt(100), 6u);
}

TEST(BookMatching, FullyFilledRestingOrderLeavesTheBook) {
    Book book;
    book.submit(test::limit(1, Side::Sell, 100, 10, kAlice));

    book.submit(test::limit(2, Side::Buy, 100, 10, kBob));

    EXPECT_TRUE(book.asks().empty());
    EXPECT_TRUE(book.bids().empty());
    EXPECT_EQ(book.find(1), nullptr) << "the index entry must go with the order";
    EXPECT_EQ(book.find(2), nullptr);
}

TEST(BookMatching, SweepsThreeLevels) {
    Book book;
    book.submit(test::limit(1, Side::Sell, 100, 10, kAlice));
    book.submit(test::limit(2, Side::Sell, 101, 10, kAlice));
    book.submit(test::limit(3, Side::Sell, 102, 10, kAlice));

    const SubmitResult result = book.submit(test::limit(4, Side::Buy, 102, 30, kBob));

    EXPECT_EQ(result.status, SubmitStatus::Filled);
    EXPECT_EQ(result.filledQty, 30u);
    ASSERT_EQ(result.fills.size(), 3u);

    // Best price first, outward from the touch. That ordering is the whole
    // reason the ask side is instantiated with std::less.
    EXPECT_EQ(fillPrices(result.fills), (std::vector<Price>{100, 101, 102}));
    EXPECT_TRUE(book.asks().empty());
}

TEST(BookMatching, StopsAtTheLimitPriceMidSweep) {
    Book book;
    book.submit(test::limit(1, Side::Sell, 100, 10, kAlice));
    book.submit(test::limit(2, Side::Sell, 101, 10, kAlice));
    book.submit(test::limit(3, Side::Sell, 102, 10, kAlice));

    const SubmitResult result = book.submit(test::limit(4, Side::Buy, 101, 30, kBob));

    EXPECT_EQ(result.status, SubmitStatus::PartiallyFilledResting);
    EXPECT_EQ(result.filledQty, 20u);
    EXPECT_EQ(result.restingQty, 10u);
    EXPECT_EQ(*book.bestAsk(), 102) << "the 102 level is untouched";
    EXPECT_EQ(*book.bestBid(), 101);
    EXPECT_FALSE(book.isCrossed());
}

TEST(BookMatching, ExecutesAtTheRestingPriceNotTheAggressorLimit) {
    Book book;
    book.submit(test::limit(1, Side::Sell, 100, 10, kAlice));

    const SubmitResult result = book.submit(test::limit(2, Side::Buy, 105, 10, kBob));

    ASSERT_EQ(result.fills.size(), 1u);
    // The 5 ticks of improvement belong to the aggressor. Charging its own
    // limit would mean the book paid out less than it took in.
    EXPECT_EQ(result.fills[0].price, 100);
}

TEST(BookMatching, HonoursTimePriorityWithinALevel) {
    Book book;
    book.submit(test::limit(1, Side::Sell, 100, 10, kBob));
    book.submit(test::limit(2, Side::Sell, 100, 10, kCarol));
    book.submit(test::limit(3, Side::Sell, 100, 10, kBob));

    const SubmitResult result = book.submit(test::limit(4, Side::Buy, 100, 25, kAlice));

    ASSERT_EQ(result.fills.size(), 3u);
    EXPECT_EQ(restingIds(result.fills), (std::vector<OrderId>{1, 2, 3}));
    EXPECT_EQ(result.fills[2].quantity, 5u) << "the last in the queue gets the remainder";
}

TEST(BookMatching, CrossingLimitBecomesTheNewBestBid) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 100, 10, kAlice));
    book.submit(test::limit(2, Side::Sell, 105, 10, kBob));

    const SubmitResult result = book.submit(test::limit(3, Side::Buy, 106, 25, kCarol));

    EXPECT_EQ(result.status, SubmitStatus::PartiallyFilledResting);
    EXPECT_EQ(result.filledQty, 10u);
    EXPECT_EQ(*book.bestBid(), 106) << "the unfilled remainder sets a new touch";
    EXPECT_FALSE(book.bestAsk().has_value());
    EXPECT_FALSE(book.isCrossed());
}

TEST(BookMatching, NeverLeavesTheBookCrossed) {
    Book book;
    book.submit(test::limit(1, Side::Sell, 100, 10, kAlice));
    book.submit(test::limit(2, Side::Sell, 101, 10, kAlice));
    book.submit(test::limit(3, Side::Buy, 99, 10, kBob));

    // A buy priced far through the offer cannot rest above a live ask: it
    // keeps consuming until either it or the eligible levels are exhausted.
    book.submit(test::limit(4, Side::Buy, 200, 15, kCarol));

    EXPECT_FALSE(book.isCrossed());
    EXPECT_EQ(*book.bestBid(), 99) << "the aggressor was fully filled and never rested";
    EXPECT_EQ(*book.bestAsk(), 101);
    EXPECT_EQ(book.asks().qtyAt(101), 5u);
}

// ---------------------------------------------------------------------------
// Market orders
// ---------------------------------------------------------------------------

TEST(BookMarketOrder, IntoAnEmptyBookIsCancelled) {
    Book book;

    const SubmitResult result = book.submit(test::market(1, Side::Buy, 10, kAlice));

    EXPECT_EQ(result.status, SubmitStatus::CancelledRemainder);
    EXPECT_TRUE(result.fills.empty());
    EXPECT_EQ(result.filledQty, 0u);
    EXPECT_EQ(result.restingQty, 0u);
    EXPECT_TRUE(book.bids().empty()) << "a market order has no price at which to wait";
    EXPECT_TRUE(book.asks().empty());
}

TEST(BookMarketOrder, TakesEveryPriceUntilExhausted) {
    Book book;
    book.submit(test::limit(1, Side::Sell, 100, 10, kAlice));
    book.submit(test::limit(2, Side::Sell, 500, 10, kAlice));

    const SubmitResult result = book.submit(test::market(3, Side::Buy, 20, kBob));

    EXPECT_EQ(result.status, SubmitStatus::Filled);
    EXPECT_EQ(fillPrices(result.fills), (std::vector<Price>{100, 500}))
        << "a market order declines no price";
}

TEST(BookMarketOrder, CancelsTheRemainderWhenLiquidityRunsOut) {
    Book book;
    book.submit(test::limit(1, Side::Sell, 100, 6, kAlice));

    const SubmitResult result = book.submit(test::market(2, Side::Buy, 10, kBob));

    EXPECT_EQ(result.status, SubmitStatus::CancelledRemainder);
    EXPECT_EQ(result.filledQty, 6u);
    EXPECT_EQ(result.restingQty, 0u);
    EXPECT_TRUE(book.bids().empty());
}

// ---------------------------------------------------------------------------
// Self-match prevention
// ---------------------------------------------------------------------------

TEST(BookSelfMatch, AggressorIsBlockedAgainstItsOwnRestingOrder) {
    Book book;
    book.submit(test::limit(1, Side::Sell, 100, 10, kAlice));

    const SubmitResult result = book.submit(test::limit(2, Side::Buy, 100, 10, kAlice));

    EXPECT_EQ(result.status, SubmitStatus::SelfMatchBlocked);
    EXPECT_TRUE(result.fills.empty()) << "no trade is printed to the tape";
    EXPECT_EQ(result.restingQty, 0u) << "resting it would leave the book crossed";
    EXPECT_EQ(book.asks().qtyAt(100), 10u) << "the resting order keeps its priority";
    EXPECT_FALSE(book.isCrossed());
}

TEST(BookSelfMatch, TradesWithOthersBeforeBeingBlocked) {
    Book book;
    book.submit(test::limit(1, Side::Sell, 100, 10, kBob));
    book.submit(test::limit(2, Side::Sell, 100, 10, kAlice));

    const SubmitResult result = book.submit(test::limit(3, Side::Buy, 100, 20, kAlice));

    EXPECT_EQ(result.status, SubmitStatus::SelfMatchBlocked);
    ASSERT_EQ(result.fills.size(), 1u);
    EXPECT_EQ(result.fills[0].restingId, 1u) << "Bob's order was ahead and does trade";
    EXPECT_EQ(book.asks().qtyAt(100), 10u) << "Alice's own order is left alone";
}

TEST(BookSelfMatch, DifferentAccountsTradeNormally) {
    Book book;
    book.submit(test::limit(1, Side::Sell, 100, 10, kAlice));

    const SubmitResult result = book.submit(test::limit(2, Side::Buy, 100, 10, kBob));

    EXPECT_EQ(result.status, SubmitStatus::Filled);
}

// ---------------------------------------------------------------------------
// Cancel
// ---------------------------------------------------------------------------

TEST(BookCancel, ReturnsOwnershipOfARestingOrder) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 100, 10, kAlice));

    std::unique_ptr<Order> cancelled = book.cancel(1);

    ASSERT_NE(cancelled, nullptr);
    EXPECT_EQ(cancelled->id(), 1u);
    EXPECT_EQ(cancelled->remaining(), 10u);
    EXPECT_TRUE(book.bids().empty());
}

TEST(BookCancel, OfAPartiallyFilledOrderReturnsTheRemainder) {
    Book book;
    book.submit(test::limit(1, Side::Sell, 100, 10, kAlice));
    book.submit(test::market(2, Side::Buy, 4, kBob));

    std::unique_ptr<Order> cancelled = book.cancel(1);

    ASSERT_NE(cancelled, nullptr);
    EXPECT_EQ(cancelled->remaining(), 6u) << "only the unexecuted part can be withdrawn";
    EXPECT_TRUE(book.asks().empty());
    EXPECT_EQ(book.find(1), nullptr);
}

TEST(BookCancel, OfAnUnknownOrderReturnsNull) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 100, 10, kAlice));

    EXPECT_EQ(book.cancel(999), nullptr);
    EXPECT_EQ(book.bids().orderCount(), 1u);
}

TEST(BookCancel, OfAFilledOrderReturnsNull) {
    Book book;
    book.submit(test::limit(1, Side::Sell, 100, 10, kAlice));
    book.submit(test::limit(2, Side::Buy, 100, 10, kBob));

    EXPECT_EQ(book.cancel(1), nullptr) << "the order's lifetime ended at the fill";
}

TEST(BookCancel, FindsOrdersOnEitherSide) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 99, 10, kAlice));
    book.submit(test::limit(2, Side::Sell, 101, 10, kBob));

    EXPECT_NE(book.cancel(2), nullptr);
    EXPECT_NE(book.cancel(1), nullptr);
    EXPECT_TRUE(book.bids().empty());
    EXPECT_TRUE(book.asks().empty());
}

// ---------------------------------------------------------------------------
// Modify
// ---------------------------------------------------------------------------

TEST(BookModify, SizeReductionKeepsQueuePriority) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 100, 10, kAlice));
    book.submit(test::limit(2, Side::Buy, 100, 10, kBob));

    const ModifyResult modified = book.modify(1, 4);

    EXPECT_EQ(modified.status, ModifyStatus::AmendedInPlace);
    EXPECT_TRUE(modified.priorityRetained());

    // Reducing takes nothing from those behind, so there is no reason to
    // charge for it -- order 1 still trades first.
    const SubmitResult result = book.submit(test::limit(3, Side::Sell, 100, 14, kCarol));
    EXPECT_EQ(restingIds(result.fills), (std::vector<OrderId>{1, 2}));
    EXPECT_EQ(result.fills[0].quantity, 4u);
}

TEST(BookModify, SizeIncreaseLosesTimePriority) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 100, 10, kAlice));
    book.submit(test::limit(2, Side::Buy, 100, 10, kBob));

    const ModifyResult modified = book.modify(1, 20);

    EXPECT_EQ(modified.status, ModifyStatus::Requeued);
    EXPECT_FALSE(modified.priorityRetained());
    EXPECT_EQ(modified.restingQty, 20u);

    // Otherwise a participant could quote small to reach the front, then size
    // up once there.
    const SubmitResult result = book.submit(test::limit(3, Side::Sell, 100, 30, kCarol));
    EXPECT_EQ(restingIds(result.fills), (std::vector<OrderId>{2, 1}));
}

TEST(BookModify, PriceChangeLosesTimePriority) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 100, 10, kAlice));
    book.submit(test::limit(2, Side::Buy, 100, 10, kBob));

    // Move away and back: the round trip must not restore the original place.
    ASSERT_EQ(book.modify(1, 10, 99).status, ModifyStatus::Requeued);
    ASSERT_EQ(book.modify(1, 10, 100).status, ModifyStatus::Requeued);

    const SubmitResult result = book.submit(test::limit(3, Side::Sell, 100, 20, kCarol));
    EXPECT_EQ(restingIds(result.fills), (std::vector<OrderId>{2, 1}));
}

TEST(BookModify, RepricedOrderTradesIfItNowCrosses) {
    Book book;
    book.submit(test::limit(1, Side::Sell, 105, 10, kAlice));
    book.submit(test::limit(2, Side::Buy, 100, 10, kBob));

    const ModifyResult modified = book.modify(2, 10, 105);

    // Routed back through submit() deliberately: a repriced order that crosses
    // must trade exactly as a fresh one would.
    EXPECT_EQ(modified.status, ModifyStatus::Requeued);
    ASSERT_EQ(modified.fills.size(), 1u);
    EXPECT_EQ(modified.fills[0].price, 105);
    EXPECT_EQ(modified.restingQty, 0u);
    EXPECT_TRUE(book.asks().empty());
    EXPECT_TRUE(book.bids().empty());
}

TEST(BookModify, MovingThePriceRelocatesTheLevel) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 100, 10, kAlice));

    book.modify(1, 10, 98);

    EXPECT_EQ(book.bids().qtyAt(100), 0u);
    EXPECT_EQ(book.bids().qtyAt(98), 10u);
    EXPECT_EQ(book.bids().levelCount(), 1u) << "the emptied level is dropped";
    EXPECT_EQ(*book.bestBid(), 98);
}

TEST(BookModify, OfAnUnknownOrderReportsNotFound) {
    Book book;

    EXPECT_EQ(book.modify(999, 10).status, ModifyStatus::NotFound);
}

TEST(BookModify, ToZeroQuantityIsRejectedRatherThanTreatedAsCancel) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 100, 10, kAlice));

    const ModifyResult modified = book.modify(1, 0);

    EXPECT_EQ(modified.status, ModifyStatus::Rejected);
    EXPECT_EQ(book.bids().qtyAt(100), 10u) << "a rejected amendment changes nothing";
}

TEST(BookModify, WorksOnTheAskSideToo) {
    Book book;
    book.submit(test::limit(1, Side::Sell, 100, 10, kAlice));

    EXPECT_EQ(book.modify(1, 4).status, ModifyStatus::AmendedInPlace);
    EXPECT_EQ(book.asks().qtyAt(100), 4u);
}

// ---------------------------------------------------------------------------
// Strategy
// ---------------------------------------------------------------------------

TEST(BookStrategy, DefaultsToPriceTime) {
    const Book book;

    EXPECT_STREQ(book.strategy().name(), "price-time");
}

} // namespace
} // namespace exchange
