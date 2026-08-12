// BookIteratorImpl: that it walks the book in the right order, that its traits
// are honest, and that const and non-const behave as a caller expects.

#include "core/order_book.hpp"
#include "core/types.hpp"

#include "order_factory.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>
#include <ranges>
#include <type_traits>
#include <vector>

namespace exchange {
namespace {

using test::kAlice;
using test::kBob;
using test::kCarol;

[[nodiscard]] std::vector<OrderId> idsOf(const auto& side) {
    std::vector<OrderId> ids;
    std::ranges::transform(side, std::back_inserter(ids), &Order::id);
    return ids;
}

// ---------------------------------------------------------------------------
// Traits
// ---------------------------------------------------------------------------

TEST(BookIterator, PublishesTheFullSetOfTraits) {
    using It = BidBook::const_iterator;
    using Traits = std::iterator_traits<It>;

    static_assert(std::is_same_v<Traits::iterator_category, std::forward_iterator_tag>);
    static_assert(std::is_same_v<Traits::value_type, Order>);
    static_assert(std::is_same_v<Traits::reference, const Order&>);
    static_assert(std::is_same_v<Traits::pointer, const Order*>);
    static_assert(std::is_same_v<Traits::difference_type, std::ptrdiff_t>);
    SUCCEED();
}

TEST(BookIterator, ClaimsForwardAndNoMore) {
    // Forward is the strongest category the storage can honour. Claiming
    // bidirectional or random access would still compile and would silently
    // change which algorithm the library selects.
    static_assert(std::forward_iterator<BidBook::iterator>);
    static_assert(!std::bidirectional_iterator<BidBook::iterator>);
    static_assert(!std::random_access_iterator<BidBook::iterator>);
    SUCCEED();
}

TEST(BookIterator, IsDefaultConstructibleAsForwardIteratorRequires) {
    // std::forward_iterator subsumes std::semiregular, so a singular iterator
    // must exist and must compare equal to another singular one.
    const BidBook::iterator first;
    const BidBook::iterator second;

    EXPECT_EQ(first, second);
}

TEST(BookIterator, NonConstBookYieldsMutableOrders) {
    static_assert(std::is_same_v<std::iter_reference_t<BidBook::iterator>, Order&>);
    static_assert(std::is_same_v<std::iter_reference_t<BidBook::const_iterator>, const Order&>);
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Traversal
// ---------------------------------------------------------------------------

TEST(BookIterator, EmptySideBeginEqualsEnd) {
    const BidBook bids;

    EXPECT_EQ(bids.begin(), bids.end());
    EXPECT_EQ(bids.cbegin(), bids.cend());
    EXPECT_EQ(std::ranges::distance(bids), 0);
}

TEST(BookIterator, WalksBidsFromHighestPriceDown) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 100, 10, kAlice));
    bids.insert(test::limit(2, Side::Buy, 102, 10, kBob));
    bids.insert(test::limit(3, Side::Buy, 101, 10, kCarol));

    EXPECT_EQ(idsOf(bids), (std::vector<OrderId>{2, 3, 1})) << "best price first";
}

TEST(BookIterator, WalksAsksFromLowestPriceUp) {
    AskBook asks;
    asks.insert(test::limit(1, Side::Sell, 100, 10, kAlice));
    asks.insert(test::limit(2, Side::Sell, 102, 10, kBob));
    asks.insert(test::limit(3, Side::Sell, 101, 10, kCarol));

    // Same iterator, opposite order -- the comparator supplies the direction.
    EXPECT_EQ(idsOf(asks), (std::vector<OrderId>{1, 3, 2}));
}

TEST(BookIterator, PreservesQueueOrderWithinALevel) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 100, 10, kAlice));
    bids.insert(test::limit(2, Side::Buy, 100, 10, kBob));
    bids.insert(test::limit(3, Side::Buy, 101, 10, kCarol));

    EXPECT_EQ(idsOf(bids), (std::vector<OrderId>{3, 1, 2}));
}

TEST(BookIterator, FlattensAcrossLevelBoundaries) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 101, 10, kAlice));
    bids.insert(test::limit(2, Side::Buy, 101, 10, kBob));
    bids.insert(test::limit(3, Side::Buy, 100, 10, kCarol));
    bids.insert(test::limit(4, Side::Buy, 100, 10, kAlice));

    EXPECT_EQ(std::ranges::distance(bids), 4);
    EXPECT_EQ(idsOf(bids), (std::vector<OrderId>{1, 2, 3, 4}));
}

TEST(BookIterator, ExposesTheLevelPriceItSitsOn) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 100, 10, kAlice));
    bids.insert(test::limit(2, Side::Buy, 102, 10, kBob));

    auto it = bids.begin();
    EXPECT_EQ(it.price(), 102);
    ++it;
    EXPECT_EQ(it.price(), 100);
}

TEST(BookIterator, SkipsPastACancelledLevel) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 102, 10, kAlice));
    bids.insert(test::limit(2, Side::Buy, 101, 10, kBob));
    bids.insert(test::limit(3, Side::Buy, 100, 10, kCarol));

    const std::unique_ptr<Order> removed = bids.cancel(2);
    ASSERT_NE(removed, nullptr);

    EXPECT_EQ(idsOf(bids), (std::vector<OrderId>{1, 3}));
}

// ---------------------------------------------------------------------------
// Iterator mechanics
// ---------------------------------------------------------------------------

TEST(BookIterator, PostIncrementReturnsThePreviousPosition) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 101, 10, kAlice));
    bids.insert(test::limit(2, Side::Buy, 100, 10, kBob));

    auto it = bids.begin();
    const auto before = it++;

    EXPECT_EQ(before->id(), 1u);
    EXPECT_EQ(it->id(), 2u);
}

TEST(BookIterator, ArrowOperatorReachesTheOrder) {
    BidBook bids;
    bids.insert(test::limit(7, Side::Buy, 100, 42, kAlice));

    const auto it = bids.begin();

    EXPECT_EQ(it->id(), 7u);
    EXPECT_EQ(it->remaining(), 42u);
    EXPECT_EQ((*it).id(), 7u);
}

TEST(BookIterator, SatisfiesTheMultiPassGuarantee) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 101, 10, kAlice));
    bids.insert(test::limit(2, Side::Buy, 100, 10, kBob));

    // The property that separates forward from input: a copy taken before
    // advancing still reads the same element afterwards.
    const auto first = bids.begin();
    auto second = first;
    ++second;

    EXPECT_EQ(first->id(), 1u);
    EXPECT_EQ(second->id(), 2u);
    EXPECT_NE(first, second);
    EXPECT_EQ(idsOf(bids), (std::vector<OrderId>{1, 2})) << "and the range re-walks intact";
}

TEST(BookIterator, ConstIteratorConvertsFromMutableAndComparesAcross) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 100, 10, kAlice));

    const BidBook::iterator mutableIt = bids.begin();
    const BidBook::const_iterator constIt = mutableIt; // implicit, by design

    EXPECT_EQ(constIt, mutableIt) << "cbegin() must compare against a non-const end()";
    EXPECT_EQ(constIt->id(), 1u);
    static_assert(!std::is_convertible_v<BidBook::const_iterator, BidBook::iterator>,
                  "const must not silently decay to mutable");
}

TEST(BookIterator, MutableTraversalCanAmendInPlace) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 100, 10, kAlice));

    std::ranges::for_each(bids, [](Order& order) { order.onPartialFill(3); });

    EXPECT_EQ(bids.begin()->remaining(), 7u);
}

// ---------------------------------------------------------------------------
// Standard-library interoperation -- the reason the iterator exists
// ---------------------------------------------------------------------------

TEST(BookIterator, WorksInARangeBasedForLoop) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 101, 10, kAlice));
    bids.insert(test::limit(2, Side::Buy, 100, 25, kBob));

    Quantity total = 0;
    for (const Order& order : bids) {
        total += order.remaining();
    }

    EXPECT_EQ(total, 35u);
}

TEST(BookIterator, WorksWithStdForEach) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 101, 10, kAlice));
    bids.insert(test::limit(2, Side::Buy, 100, 25, kBob));

    Quantity total = 0;
    std::for_each(bids.begin(), bids.end(),
                  [&total](const Order& order) { total += order.remaining(); });

    EXPECT_EQ(total, 35u);
}

TEST(BookIterator, WorksWithClassicTwoIteratorAlgorithms) {
    // Possible only because begin() and end() share a type, making the book a
    // common_range. An iterator/sentinel pair would restrict callers to the
    // ranges overloads.
    static_assert(std::ranges::common_range<BidBook>);

    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 101, 10, kAlice));
    bids.insert(test::limit(2, Side::Buy, 100, 25, kBob));

    EXPECT_EQ(std::distance(bids.begin(), bids.end()), 2);
    EXPECT_EQ(std::count_if(bids.begin(), bids.end(),
                            [](const Order& order) { return order.remaining() > 20; }),
              1);
}

TEST(BookIterator, WorksWithRangesViewsAndProjections) {
    BidBook bids;
    bids.insert(test::limit(1, Side::Buy, 102, 5, kAlice));
    bids.insert(test::limit(2, Side::Buy, 101, 30, kBob));
    bids.insert(test::limit(3, Side::Buy, 100, 15, kCarol));

    std::vector<Quantity> large;
    std::ranges::copy(bids | std::views::filter([](const Order& o) { return o.remaining() > 10; }) |
                          std::views::transform(&Order::remaining),
                      std::back_inserter(large));

    EXPECT_EQ(large, (std::vector<Quantity>{30, 15}));
}

} // namespace
} // namespace exchange
