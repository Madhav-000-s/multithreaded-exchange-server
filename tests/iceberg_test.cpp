// Iceberg orders inside the book. The interesting behaviour is not the hiding
// itself but what it costs: a refreshed tranche goes to the back of the queue,
// so an iceberg interleaves with the orders behind it instead of consuming the
// level in one go.

#include "core/book.hpp"
#include "core/types.hpp"

#include "order_factory.hpp"

#include <gtest/gtest.h>

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

TEST(BookIceberg, PublishesOnlyItsDisplayTranche) {
    Book book;
    book.submit(test::iceberg(1, Side::Sell, 100, 100, 10, kAlice));

    const PriceLevel* level = book.asks().levelAt(100);
    ASSERT_NE(level, nullptr);
    EXPECT_EQ(level->visibleQty(), 10u) << "what an L2 feed would show";
    EXPECT_EQ(book.asks().qtyAt(100), 100u) << "what is actually available";
}

TEST(BookIceberg, SmallAggressorSeesOnlyTheTranche) {
    Book book;
    book.submit(test::iceberg(1, Side::Sell, 100, 100, 10, kAlice));

    const SubmitResult result = book.submit(test::limit(2, Side::Buy, 100, 6, kBob));

    EXPECT_EQ(result.filledQty, 6u);
    const PriceLevel* level = book.asks().levelAt(100);
    ASSERT_NE(level, nullptr);
    EXPECT_EQ(level->visibleQty(), 4u);
    EXPECT_EQ(book.asks().qtyAt(100), 94u);
}

TEST(BookIceberg, RefreshesAndKeepsTradingWhenNothingIsBehindIt) {
    Book book;
    book.submit(test::iceberg(1, Side::Sell, 100, 100, 10, kAlice));

    // Nothing else rests at this price, so the iceberg is requeued behind
    // itself and the aggressor keeps consuming tranche after tranche.
    const SubmitResult result = book.submit(test::limit(2, Side::Buy, 100, 35, kBob));

    EXPECT_EQ(result.status, SubmitStatus::Filled);
    EXPECT_EQ(result.filledQty, 35u);
    EXPECT_EQ(book.asks().qtyAt(100), 65u);
}

TEST(BookIceberg, LosesPriorityToTheOrderBehindItOnRefresh) {
    Book book;
    book.submit(test::iceberg(1, Side::Sell, 100, 100, 10, kAlice));
    book.submit(test::limit(2, Side::Sell, 100, 10, kCarol));

    const SubmitResult result = book.submit(test::limit(3, Side::Buy, 100, 25, kBob));

    // 10 from the tranche, then the iceberg refreshes and goes to the back, so
    // the plain order behind it trades before the next tranche does.
    ASSERT_EQ(result.fills.size(), 3u);
    EXPECT_EQ(restingIds(result.fills), (std::vector<OrderId>{1, 2, 1}));
    EXPECT_EQ(result.fills[0].quantity, 10u);
    EXPECT_EQ(result.fills[1].quantity, 10u);
    EXPECT_EQ(result.fills[2].quantity, 5u);
}

TEST(BookIceberg, AVisibleOrderWouldNotHaveYieldedPriority) {
    // The control case for the test above: with the size fully displayed the
    // first order simply trades through, and the second gets the remainder.
    Book book;
    book.submit(test::limit(1, Side::Sell, 100, 100, kAlice));
    book.submit(test::limit(2, Side::Sell, 100, 10, kCarol));

    const SubmitResult result = book.submit(test::limit(3, Side::Buy, 100, 25, kBob));

    ASSERT_EQ(result.fills.size(), 1u);
    EXPECT_EQ(restingIds(result.fills), (std::vector<OrderId>{1}));
    EXPECT_EQ(result.fills[0].quantity, 25u);
}

TEST(BookIceberg, IsFullyConsumedByALargeEnoughAggressor) {
    Book book;
    book.submit(test::iceberg(1, Side::Sell, 100, 45, 10, kAlice));

    const SubmitResult result = book.submit(test::limit(2, Side::Buy, 100, 60, kBob));

    EXPECT_EQ(result.filledQty, 45u);
    EXPECT_EQ(result.status, SubmitStatus::PartiallyFilledResting);
    EXPECT_EQ(result.restingQty, 15u);
    EXPECT_TRUE(book.asks().empty()) << "the iceberg is gone, hidden size included";
    EXPECT_EQ(book.find(1), nullptr);
}

TEST(BookIceberg, FinalTrancheIsTheRemainder) {
    Book book;
    book.submit(test::iceberg(1, Side::Sell, 100, 25, 10, kAlice));

    const SubmitResult result = book.submit(test::limit(2, Side::Buy, 100, 25, kBob));

    // 10 + 10 + 5, not 10 + 10 + 10.
    ASSERT_EQ(result.fills.size(), 3u);
    EXPECT_EQ(result.fills[2].quantity, 5u);
    EXPECT_TRUE(book.asks().empty());
}

TEST(BookIceberg, CanBeCancelledLikeAnyRestingOrder) {
    Book book;
    book.submit(test::iceberg(1, Side::Sell, 100, 100, 10, kAlice));
    book.submit(test::limit(2, Side::Buy, 100, 15, kBob));

    std::unique_ptr<Order> cancelled = book.cancel(1);

    ASSERT_NE(cancelled, nullptr);
    EXPECT_EQ(cancelled->remaining(), 85u) << "hidden size comes back too";
    EXPECT_TRUE(book.asks().empty());
}

TEST(BookIceberg, RestsAtItsLimitWhenItCannotTrade) {
    Book book;

    const SubmitResult result = book.submit(test::iceberg(1, Side::Buy, 100, 100, 10, kAlice));

    EXPECT_EQ(result.status, SubmitStatus::Resting);
    EXPECT_EQ(result.restingQty, 100u) << "the resting quantity is the whole order";
    EXPECT_EQ(*book.bestBid(), 100);
}

TEST(BookIceberg, IsSubjectToSelfMatchPreventionLikeAnythingElse) {
    Book book;
    book.submit(test::iceberg(1, Side::Sell, 100, 100, 10, kAlice));

    const SubmitResult result = book.submit(test::limit(2, Side::Buy, 100, 10, kAlice));

    EXPECT_EQ(result.status, SubmitStatus::SelfMatchBlocked);
    EXPECT_EQ(book.asks().qtyAt(100), 100u);
}

} // namespace
} // namespace exchange
