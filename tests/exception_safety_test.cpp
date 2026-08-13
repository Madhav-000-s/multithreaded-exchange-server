// The strong exception guarantee, verified rather than asserted.
//
// Each test runs the same operation repeatedly, failing the first allocation,
// then the second, and so on until the operation has no allocations left to
// fail. After every failure the book is compared field-by-field against a
// snapshot taken beforehand. Passing means there is no ordinal at which a
// failure leaves the book changed -- which is a stronger statement than any
// number of hand-placed throw sites could make, because the enumeration
// includes allocation points inside libstdc++ that no hand-written injection
// would reach.

#include "core/book.hpp"
#include "core/exceptions.hpp"
#include "core/order.hpp"
#include "core/types.hpp"

#include "alloc_injection.hpp"
#include "book_snapshot.hpp"
#include "order_factory.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace exchange {
namespace {

using test::BookSnapshot;
using test::kAlice;
using test::kBob;
using test::kCarol;
using test::snapshot;
using test::ThrowOnAllocation;

/// Upper bound on injection ordinals. Well past what any operation here
/// allocates; the loop stops early once an ordinal produces no failure.
constexpr std::size_t kMaxInjectionOrdinal = 200;

/// Runs `operation` against a book built by `build`, failing allocation `nth`
/// on each pass, and asserts the book is unchanged whenever the operation
/// threw.
///
/// Returns the number of ordinals that actually produced a failure, so a test
/// can assert the operation had throw sites at all -- a harness that silently
/// injected nothing would otherwise pass vacuously.
[[nodiscard]] std::size_t forEachAllocationFailure(const std::function<void(Book&)>& build,
                                                   const std::function<void(Book&)>& operation) {
    std::size_t failures = 0;

    for (std::size_t nth = 0; nth < kMaxInjectionOrdinal; ++nth) {
        Book book;
        build(book);
        const BookSnapshot before = snapshot(book);

        bool threw = false;
        bool injected = false;
        {
            // Armed for the narrowest possible window. The snapshot above and
            // the comparison below allocate freely without being counted.
            const ThrowOnAllocation guard{nth};
            try {
                operation(book);
            } catch (const std::bad_alloc&) {
                threw = true;
            } catch (const ExchangeError&) {
                threw = true;
            }
            injected = ThrowOnAllocation::fired();
        }

        if (!injected) {
            // The operation ran out of allocations before reaching this
            // ordinal, so every throw site has now been covered.
            break;
        }
        ++failures;

        if (threw) {
            EXPECT_EQ(before, snapshot(book))
                << "failing allocation #" << nth << " left the book modified";
        }
    }

    return failures;
}

// ---------------------------------------------------------------------------
// submit
// ---------------------------------------------------------------------------

TEST(ExceptionSafety, SubmitOfARestingOrderIsAtomic) {
    const auto build = [](Book& book) {
        book.submit(test::limit(1, Side::Buy, 100, 10, kAlice));
        book.submit(test::limit(2, Side::Sell, 110, 10, kBob));
    };
    const auto operation = [](Book& book) {
        // Does not cross, so this exercises the resting path: level node,
        // index node, list node, hash reserve.
        book.submit(test::limit(3, Side::Buy, 99, 25, kCarol));
    };

    EXPECT_GT(forEachAllocationFailure(build, operation), 0u)
        << "the harness injected nothing, so it proved nothing";
}

TEST(ExceptionSafety, SubmitThatSweepsSeveralLevelsIsAtomic) {
    const auto build = [](Book& book) {
        book.submit(test::limit(1, Side::Sell, 100, 10, kAlice));
        book.submit(test::limit(2, Side::Sell, 101, 10, kAlice));
        book.submit(test::limit(3, Side::Sell, 102, 10, kBob));
    };
    const auto operation = [](Book& book) {
        // The case the old mutate-as-you-go matcher could not survive: a
        // failure part way through would have left some fills applied and
        // some orders extracted.
        book.submit(test::limit(4, Side::Buy, 102, 25, kCarol));
    };

    EXPECT_GT(forEachAllocationFailure(build, operation), 0u);
}

TEST(ExceptionSafety, SubmitThatFullyConsumesTheOppositeSideIsAtomic) {
    const auto build = [](Book& book) {
        book.submit(test::limit(1, Side::Sell, 100, 10, kAlice));
        book.submit(test::limit(2, Side::Sell, 101, 10, kAlice));
    };
    const auto operation = [](Book& book) {
        book.submit(test::limit(3, Side::Buy, 105, 20, kBob));
    };

    EXPECT_GT(forEachAllocationFailure(build, operation), 0u);
}

TEST(ExceptionSafety, SubmitAgainstAnIcebergIsAtomic) {
    const auto build = [](Book& book) {
        book.submit(test::iceberg(1, Side::Sell, 100, 100, 10, kAlice));
        book.submit(test::limit(2, Side::Sell, 100, 10, kBob));
    };
    const auto operation = [](Book& book) {
        // Several tranches and a requeue, so the plan holds repeated entries
        // for the same resting order and the commit relies on list iterators
        // surviving a splice.
        book.submit(test::limit(3, Side::Buy, 100, 45, kCarol));
    };

    EXPECT_GT(forEachAllocationFailure(build, operation), 0u);
}

TEST(ExceptionSafety, MarketOrderSubmitIsAtomic) {
    const auto build = [](Book& book) { book.submit(test::limit(1, Side::Sell, 100, 6, kAlice)); };
    const auto operation = [](Book& book) { book.submit(test::market(2, Side::Buy, 10, kBob)); };

    EXPECT_GT(forEachAllocationFailure(build, operation), 0u);
}

TEST(ExceptionSafety, SelfMatchBlockedSubmitIsAtomic) {
    const auto build = [](Book& book) { book.submit(test::limit(1, Side::Sell, 100, 10, kAlice)); };
    const auto operation = [](Book& book) {
        book.submit(test::limit(2, Side::Buy, 100, 10, kAlice));
    };

    EXPECT_GT(forEachAllocationFailure(build, operation), 0u);
}

// ---------------------------------------------------------------------------
// modify
// ---------------------------------------------------------------------------

TEST(ExceptionSafety, ModifyThatRequeuesIsAtomic) {
    const auto build = [](Book& book) {
        book.submit(test::limit(1, Side::Buy, 100, 10, kAlice));
        book.submit(test::limit(2, Side::Buy, 100, 10, kBob));
    };
    const auto operation = [](Book& book) {
        // Size increase: priority is forfeit, so the order is cloned, planned
        // and provisioned before the original is withdrawn.
        book.modify(1, 50);
    };

    EXPECT_GT(forEachAllocationFailure(build, operation), 0u);
}

TEST(ExceptionSafety, ModifyThatRepricesIntoACrossIsAtomic) {
    const auto build = [](Book& book) {
        book.submit(test::limit(1, Side::Sell, 105, 10, kAlice));
        book.submit(test::limit(2, Side::Buy, 100, 10, kBob));
    };
    const auto operation = [](Book& book) {
        // The hardest case: withdraw, match, and rest the remainder, all of
        // which has to be atomic together.
        book.modify(2, 25, 105);
    };

    EXPECT_GT(forEachAllocationFailure(build, operation), 0u);
}

TEST(ExceptionSafety, ModifyInPlaceNeverAllocates) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 100, 10, kAlice));
    const BookSnapshot before = snapshot(book);

    {
        // A pure size reduction keeps queue position and rewrites two
        // counters. If it allocated at all, this would throw.
        const ThrowOnAllocation guard{0};
        book.modify(1, 4);
        EXPECT_FALSE(ThrowOnAllocation::fired()) << "in-place amendment must not allocate";
    }

    EXPECT_EQ(book.bids().qtyAt(100), 4u);
    EXPECT_NE(before, snapshot(book)) << "and it must actually have done something";
}

TEST(ExceptionSafety, CancelNeverAllocates) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 100, 10, kAlice));
    book.submit(test::limit(2, Side::Buy, 99, 10, kBob));

    {
        const ThrowOnAllocation guard{0};
        const std::unique_ptr<Order> cancelled = book.cancel(1);
        EXPECT_FALSE(ThrowOnAllocation::fired()) << "cancel only destroys; it must not allocate";
        EXPECT_NE(cancelled, nullptr);
    }

    EXPECT_EQ(book.bids().orderCount(), 1u);
}

// ---------------------------------------------------------------------------
// Validation rejects before touching anything
// ---------------------------------------------------------------------------

TEST(ExceptionSafety, ZeroQuantityOrderIsRejected) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 100, 10, kAlice));
    const BookSnapshot before = snapshot(book);

    EXPECT_THROW((void)book.submit(test::limit(2, Side::Buy, 100, 0, kBob)), InvalidOrderError);
    EXPECT_EQ(before, snapshot(book));
}

TEST(ExceptionSafety, DuplicateOrderIdIsRejected) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 100, 10, kAlice));
    const BookSnapshot before = snapshot(book);

    EXPECT_THROW((void)book.submit(test::limit(1, Side::Buy, 99, 5, kBob)), InvalidOrderError);
    EXPECT_EQ(before, snapshot(book)) << "a rejected duplicate must not disturb the original";
}

TEST(ExceptionSafety, NullOrderIsRejected) {
    Book book;

    EXPECT_THROW((void)book.submit(nullptr), InvalidOrderError);
}

// ---------------------------------------------------------------------------
// The hierarchy itself
// ---------------------------------------------------------------------------

TEST(ExceptionHierarchy, EveryErrorIsCatchableAsStdException) {
    // A thread entry point or main() that catches std::exception is the last
    // line of defence. An exception hierarchy outside it turns a handled
    // failure into std::terminate.
    static_assert(std::is_base_of_v<std::runtime_error, ExchangeError>);
    static_assert(std::is_base_of_v<ExchangeError, ProtocolError>);
    static_assert(std::is_base_of_v<ExchangeError, InvalidOrderError>);
    static_assert(std::is_base_of_v<ExchangeError, InsufficientFundsError>);
    static_assert(std::is_base_of_v<ExchangeError, StorageError>);

    try {
        throw InvalidOrderError("bad order");
    } catch (const std::exception& error) {
        EXPECT_STREQ(error.what(), "bad order");
    }
}

TEST(ExceptionHierarchy, CallersCanCatchBroadlyOrNarrowly) {
    // The point of the hierarchy: a session handler catches ExchangeError and
    // rejects the message, while a funding-aware caller catches the specific
    // type and reads its fields.
    try {
        throw InsufficientFundsError(kAlice, 500, 120);
    } catch (const ProtocolError&) {
        FAIL() << "caught by an unrelated sibling";
    } catch (const ExchangeError& error) {
        const auto* funds = dynamic_cast<const InsufficientFundsError*>(&error);
        ASSERT_NE(funds, nullptr);
        EXPECT_EQ(funds->account(), kAlice);
        EXPECT_EQ(funds->required(), 500u);
        EXPECT_EQ(funds->available(), 120u);
    }
}

TEST(ExceptionHierarchy, InsufficientFundsCarriesStructuredFields) {
    // Carrying the numbers rather than only a message is the reason to define
    // the type at all -- the rejection goes back to the client as fields, and
    // re-parsing them out of a formatted string would be absurd.
    const InsufficientFundsError error(kBob, 1000, 999);

    EXPECT_EQ(error.account(), kBob);
    EXPECT_EQ(error.required(), 1000u);
    EXPECT_EQ(error.available(), 999u);
    EXPECT_NE(std::string(error.what()).find("1000"), std::string::npos);
}

// ---------------------------------------------------------------------------
// The harness itself
// ---------------------------------------------------------------------------

TEST(ExceptionSafetyHarness, DetectsAnOperationThatIsNotAtomic) {
    // A harness that cannot fail proves nothing, so here is an operation that
    // genuinely is not atomic: two independent submits. If the second throws,
    // the first has already been applied and the book differs from its
    // starting state -- which is exactly the condition the other tests in this
    // file assert never arises.
    std::size_t divergences = 0;

    for (std::size_t nth = 0; nth < kMaxInjectionOrdinal; ++nth) {
        Book book;
        book.submit(test::limit(1, Side::Buy, 100, 10, kAlice));
        const BookSnapshot before = snapshot(book);

        bool threw = false;
        bool injected = false;
        {
            const ThrowOnAllocation guard{nth};
            try {
                book.submit(test::limit(2, Side::Buy, 99, 10, kBob));
                book.submit(test::limit(3, Side::Buy, 98, 10, kCarol));
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            injected = ThrowOnAllocation::fired();
        }

        if (!injected) {
            break;
        }
        if (threw && before != snapshot(book)) {
            ++divergences;
        }
    }

    EXPECT_GT(divergences, 0u)
        << "the harness failed to notice a book that was left modified, so the "
           "atomicity results above would be meaningless";
}

TEST(ExceptionSafetyHarness, CountsEveryAllocationInTheOperation) {
    // Sanity check on the counter: a sweep across three levels allocates more
    // than a single non-crossing rest, so the harness explores strictly more
    // throw sites for it.
    Book simple;
    simple.submit(test::limit(1, Side::Buy, 100, 10, kAlice));
    std::size_t simpleAllocations = 0;
    {
        const ThrowOnAllocation guard{kMaxInjectionOrdinal};
        simple.submit(test::limit(2, Side::Buy, 99, 10, kBob));
        simpleAllocations = ThrowOnAllocation::observed();
    }

    Book sweeping;
    sweeping.submit(test::limit(1, Side::Sell, 100, 10, kAlice));
    sweeping.submit(test::limit(2, Side::Sell, 101, 10, kAlice));
    sweeping.submit(test::limit(3, Side::Sell, 102, 10, kAlice));
    std::size_t sweepAllocations = 0;
    {
        const ThrowOnAllocation guard{kMaxInjectionOrdinal};
        sweeping.submit(test::limit(4, Side::Buy, 102, 30, kBob));
        sweepAllocations = ThrowOnAllocation::observed();
    }

    EXPECT_GT(simpleAllocations, 0u) << "submit must allocate something to be worth testing";
    EXPECT_GT(sweepAllocations, simpleAllocations);
}

} // namespace
} // namespace exchange
