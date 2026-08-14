#pragma once

#include "analytics/side_stats.hpp"
#include "core/book.hpp"
#include "core/types.hpp"

#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace exchange::analytics {

/// A published view of the book at one instant.
struct L2Snapshot {
    std::vector<L2Level> bids;
    std::vector<L2Level> asks;

    /// Increments on every publication. A reader that sees the same value
    /// twice knows nothing changed, and one that sees it jump knows how many
    /// updates it missed -- which is the difference between a feed a client
    /// can trust and one it can only hope about.
    Sequence sequence{0};

    [[nodiscard]] friend bool operator==(const L2Snapshot&, const L2Snapshot&) = default;
};

/// The market-data snapshot: written by the engine, read by everyone else.
///
/// **This is the one place a reader-writer lock is honestly justified.** The
/// access pattern is genuinely asymmetric -- one writer publishing after each
/// batch against many readers polling continuously -- which is the only
/// situation where `std::shared_mutex` beats a plain `std::mutex`. It is
/// otherwise the wrong default: a shared_mutex is a larger, slower object with
/// more complex fairness behaviour, and under low contention or a balanced
/// read/write mix it loses to the mutex it replaced.
///
/// The snapshot is copied out under the shared lock rather than handed out by
/// reference. Returning a reference would let a reader hold the lock for as
/// long as it liked -- or, worse, read the vectors after releasing it, while
/// the writer is reallocating them.
///
/// A lock-free alternative exists and was considered: publish
/// `shared_ptr<const L2Snapshot>` and swap it atomically, so readers take no
/// lock at all. It is genuinely better for a hot feed. It is not used here
/// because it would make the reader-writer lock -- which the design is
/// supposed to demonstrate -- disappear from the codebase, and because the
/// atomic shared_ptr operations have their own costs that only a benchmark
/// could settle. Phase 7 is where that gets measured rather than argued.
class L2Publisher {
public:
    explicit L2Publisher(std::size_t depth = 5) : depth_(depth) {}

    L2Publisher(const L2Publisher&) = delete;
    L2Publisher& operator=(const L2Publisher&) = delete;
    L2Publisher(L2Publisher&&) = delete;
    L2Publisher& operator=(L2Publisher&&) = delete;
    ~L2Publisher() = default;

    /// Called by the engine thread, under a unique lock.
    ///
    /// Both sides are rebuilt from the book *before* the lock is taken, so the
    /// exclusive section is two vector moves and an increment. Building them
    /// inside the lock would block every reader for the duration of a
    /// traversal, which is precisely the stall a reader-writer lock is meant
    /// to avoid.
    void publish(const Book& book) {
        std::vector<L2Level> bids = l2Snapshot(book.bids(), depth_);
        std::vector<L2Level> asks = l2Snapshot(book.asks(), depth_);

        const std::unique_lock<std::shared_mutex> lock(mutex_);
        current_.bids = std::move(bids);
        current_.asks = std::move(asks);
        ++current_.sequence;
    }

    /// Called by any number of readers, concurrently.
    [[nodiscard]] L2Snapshot read() const {
        const std::shared_lock<std::shared_mutex> lock(mutex_);
        return current_;
    }

    /// The publication counter alone, for a reader that only wants to know
    /// whether anything changed.
    [[nodiscard]] Sequence sequence() const {
        const std::shared_lock<std::shared_mutex> lock(mutex_);
        return current_.sequence;
    }

    [[nodiscard]] std::size_t depth() const noexcept { return depth_; }

private:
    mutable std::shared_mutex mutex_;
    L2Snapshot current_;
    std::size_t depth_;
};

} // namespace exchange::analytics
