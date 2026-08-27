// A process built to be killed.
//
// Applies orders through the real engine path -- WAL append, then book
// mutation -- and prints each order id to stdout *after* it is durable. The
// parent kills it at an arbitrary moment and then asserts that every id it saw
// printed is present in the recovered book.
//
// That is the durability claim stated as something falsifiable: **anything
// acknowledged is recoverable.** Nothing weaker is worth testing, and nothing
// stronger is true -- an order in flight when the process died was never
// acknowledged and is legitimately lost.
//
// A separate executable rather than fork() inside the test binary. Forking a
// process that already has threads leaves the child with one thread and any
// mutex the others held locked forever, so the child would deadlock on its
// first allocation. exec() gives a clean address space.

#include "core/apply.hpp"
#include "core/book.hpp"
#include "core/command.hpp"
#include "core/order.hpp"
#include "core/types.hpp"
#include "store/write_ahead_log.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

using namespace exchange;

[[nodiscard]] store::SyncPolicy parsePolicy(const std::string& name) {
    if (name == "always") {
        return store::SyncPolicy::Always;
    }
    if (name == "never") {
        return store::SyncPolicy::Never;
    }
    return store::SyncPolicy::Batch;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: crash_worker <wal-path> <policy> <order-count>\n");
        return 2;
    }

    const std::string walPath = argv[1];
    const store::SyncPolicy policy = parsePolicy(argv[2]);
    const auto orders = static_cast<unsigned long>(std::strtoul(argv[3], nullptr, 10));

    try {
        store::WriteAheadLog log(store::WalConfig{.path = walPath, .policy = policy});
        Book book;

        for (unsigned long i = 1; i <= orders; ++i) {
            const auto id = static_cast<OrderId>(i);

            // Alternating sides at one price, so roughly half the orders trade
            // and the book exercises matching rather than only resting.
            const Side side = (i % 2 == 0) ? Side::Buy : Side::Sell;
            const auto account = static_cast<AccountId>(1 + (i % 4));

            Command command =
                SubmitCommand{.order = std::make_unique<LimitOrder>(id, side, account, 10, 100)};

            // Write ahead: durable first.
            (void)log.append(command);

            // Then mutate, through the same function recovery replays with.
            (void)applyCommand(book, std::move(command));

            // Acknowledge only now. Under SyncPolicy::Always this line is
            // reached only once the record is on the platter, which is what
            // makes the parent's assertion meaningful.
            std::printf("%lu\n", i);
            std::fflush(stdout);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "crash_worker: %s\n", error.what());
        return 1;
    }

    return 0;
}
