// Phase 3's sweep, expressed as compile-time assertions.
//
// Each of these would still compile if it were false; that is precisely why
// they are worth writing down. A missing virtual destructor, a throwing move,
// or a result type that copies instead of moving are all silent until they are
// expensive or undefined.

#include "core/book.hpp"
#include "core/exceptions.hpp"
#include "core/fill.hpp"
#include "core/matching_strategy.hpp"
#include "core/order.hpp"
#include "core/order_book.hpp"
#include "core/price_level.hpp"
#include "core/price_time_strategy.hpp"
#include "core/types.hpp"

#include "order_factory.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <type_traits>
#include <vector>

namespace exchange {
namespace {

TEST(OwnershipSweep, EveryPolymorphicBaseHasAVirtualDestructor) {
    // The book destroys orders through unique_ptr<Order>, and Book destroys
    // its strategy through unique_ptr<MatchingStrategy>. Without a virtual
    // destructor both are undefined behaviour that happens to work until a
    // derived class gains a member that owns something.
    static_assert(std::has_virtual_destructor_v<Order>);
    static_assert(std::has_virtual_destructor_v<MatchingStrategy>);
    static_assert(std::has_virtual_destructor_v<ExchangeError>);
    SUCCEED();
}

TEST(OwnershipSweep, OrdersCannotBeCopiedOrMoved) {
    // An order is a unique identity in the book. Copying would duplicate an
    // OrderId and give the index two entries for one order; moving would leave
    // a husk still reachable through a Locator.
    static_assert(!std::is_copy_constructible_v<Order>);
    static_assert(!std::is_copy_assignable_v<Order>);
    static_assert(!std::is_move_constructible_v<Order>);
    static_assert(!std::is_move_assignable_v<Order>);
    static_assert(!std::is_copy_constructible_v<LimitOrder>);
    static_assert(!std::is_copy_constructible_v<IcebergOrder>);
    SUCCEED();
}

TEST(OwnershipSweep, MoveOperationsAreNoexcept) {
    // std::vector reallocation uses move_if_noexcept: a throwing move forces
    // it to copy instead, which for these types would be a silent performance
    // cliff -- and for the ones holding unique_ptr, a compile error deferred
    // until the vector happens to grow.
    static_assert(std::is_nothrow_move_constructible_v<Fill>);
    static_assert(std::is_nothrow_move_assignable_v<Fill>);
    static_assert(std::is_nothrow_move_constructible_v<PriceLevel>);
    static_assert(std::is_nothrow_move_assignable_v<PriceLevel>);
    static_assert(std::is_nothrow_move_constructible_v<PlannedFill>);
    static_assert(std::is_nothrow_move_constructible_v<MatchPlan>);
    static_assert(std::is_nothrow_move_constructible_v<SubmitResult>);
    static_assert(std::is_nothrow_move_constructible_v<ModifyResult>);
    static_assert(std::is_nothrow_move_constructible_v<BidBook>);
    static_assert(std::is_nothrow_move_constructible_v<AskBook>);
    SUCCEED();
}

TEST(OwnershipSweep, FillIsTriviallyCopyableSoTheFillVectorIsCheap) {
    // The commit phase appends to a pre-reserved vector<Fill> and must not be
    // able to throw. Trivial copyability is what makes that true regardless of
    // how the vector was reached.
    static_assert(std::is_trivially_copyable_v<Fill>);
    static_assert(std::is_nothrow_default_constructible_v<Fill>);
    SUCCEED();
}

TEST(OwnershipSweep, TheBookIsNeitherCopiedNorMoved) {
    // From Phase 4 exactly one thread owns a Book. Making it immovable means
    // that ownership cannot be transferred by accident.
    static_assert(!std::is_copy_constructible_v<Book>);
    static_assert(!std::is_move_constructible_v<Book>);
    static_assert(!std::is_copy_constructible_v<MatchingStrategy>);
    static_assert(!std::is_move_constructible_v<MatchingStrategy>);
    SUCCEED();
}

TEST(OwnershipSweep, CancelAndTheCommitPathAreDeclaredNoexcept) {
    // Declared, not merely believed. If the reasoning behind either is ever
    // wrong, the program terminates at the point of the mistake instead of
    // continuing with a half-mutated book.
    Book book;
    static_assert(noexcept(book.cancel(OrderId{1})));

    PriceLevel level(100);
    PriceLevel::Iterator position{};
    static_assert(noexcept(level.extract(position)));
    static_assert(noexcept(level.applyFill(position, Quantity{1})));
    static_assert(noexcept(level.replenishAndRequeue(position)));
    static_assert(noexcept(level.reduceQuantity(position, Quantity{1})));
    static_assert(noexcept(level.adopt(std::declval<PriceLevel::Queue&>(), position)));
    SUCCEED();
}

TEST(OwnershipSweep, TheBookOwnsOrdersExclusively) {
    // A behavioural check to go with the static ones: the order handed to
    // submit is owned by the book afterwards, and comes back out on cancel.
    Book book;
    auto order = test::limit(1, Side::Buy, 100, 10, test::kAlice);
    const Order* raw = order.get();

    book.submit(std::move(order));
    EXPECT_EQ(order, nullptr) << "submit takes ownership unconditionally";
    EXPECT_EQ(book.find(1), raw) << "and the book holds the same object, not a copy";

    std::unique_ptr<Order> returned = book.cancel(1);
    ASSERT_NE(returned, nullptr);
    EXPECT_EQ(returned.get(), raw) << "cancel hands the same object back";
    EXPECT_EQ(book.find(1), nullptr);
}

TEST(OwnershipSweep, AFilledOrderIsDestroyedByTheBook) {
    Book book;
    book.submit(test::limit(1, Side::Sell, 100, 10, test::kAlice));

    book.submit(test::limit(2, Side::Buy, 100, 10, test::kBob));

    // Nothing leaks and nothing dangles: ASan and the sanitizer builds would
    // catch either. The observable part is that the index no longer names it.
    EXPECT_EQ(book.find(1), nullptr);
    EXPECT_EQ(book.find(2), nullptr);
    EXPECT_TRUE(book.bids().empty());
    EXPECT_TRUE(book.asks().empty());
}

} // namespace
} // namespace exchange
