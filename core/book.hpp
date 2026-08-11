#pragma once

#include "core/fill.hpp"
#include "core/matching_strategy.hpp"
#include "core/order.hpp"
#include "core/order_book.hpp"
#include "core/types.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace exchange {

enum class SubmitStatus : std::uint8_t {
    /// Did not trade; the whole order rests.
    Resting,
    /// Traded in part; the remainder rests.
    PartiallyFilledResting,
    /// Traded in full; nothing rests.
    Filled,
    /// Traded in part or not at all, and the remainder could not rest --
    /// a market order that exhausted the opposite side.
    CancelledRemainder,
    /// Stopped on the submitter's own resting order. The remainder is
    /// cancelled rather than rested, so the book cannot end up crossed.
    SelfMatchBlocked,
};

struct SubmitResult {
    SubmitStatus status{SubmitStatus::Resting};
    std::vector<Fill> fills;
    Quantity filledQty{0};
    Quantity restingQty{0};
};

enum class ModifyStatus : std::uint8_t {
    /// No such resting order -- unknown id, or already filled or cancelled.
    NotFound,
    /// Rejected without effect; the original order is untouched.
    Rejected,
    /// Size reduced where it stood. Queue priority retained.
    AmendedInPlace,
    /// Reinserted at the back of its level. Queue priority lost, and the
    /// order may have traded on the way in if the new price crosses.
    Requeued,
};

struct ModifyResult {
    ModifyStatus status{ModifyStatus::NotFound};
    std::vector<Fill> fills;
    Quantity restingQty{0};

    [[nodiscard]] bool priorityRetained() const noexcept {
        return status == ModifyStatus::AmendedInPlace;
    }
};

/// The order book for one instrument: both sides, plus the operations that
/// move orders between them.
///
/// Single-threaded by construction and deliberately unlocked. From Phase 4 a
/// single engine thread owns an instance exclusively and every other thread
/// reaches it by message passing, so there is no mutex here to forget to take.
class Book {
public:
    /// @param strategy the fill rule; price-time when not specified.
    explicit Book(std::unique_ptr<MatchingStrategy> strategy = nullptr);

    Book(const Book&) = delete;
    Book& operator=(const Book&) = delete;
    Book(Book&&) = delete;
    Book& operator=(Book&&) = delete;
    ~Book() = default;

    /// Matches an incoming order against the opposite side, then rests any
    /// remainder that is entitled to rest.
    ///
    /// Takes ownership unconditionally: whether the order ends up resting,
    /// fully filled, or cancelled, the caller is not left holding a pointer
    /// whose lifetime depends on which of those happened.
    SubmitResult submit(std::unique_ptr<Order> order);

    /// Removes a resting order and returns it, or nullptr if unknown.
    /// Ownership comes back to the caller rather than being dropped, so a
    /// cancel can be reported on with the order's final state intact.
    [[nodiscard]] std::unique_ptr<Order> cancel(OrderId id);

    /// Amends a resting order's size and optionally its price.
    ///
    /// Queue position is retained only for a pure size *reduction* at the same
    /// price. Raising size or moving price forfeits priority, because both
    /// would otherwise let a participant hold a queue position earned under
    /// terms they are no longer offering -- quote small to get to the front,
    /// then size up once there. Reducing takes nothing from anyone behind, so
    /// there is no reason to charge for it.
    ///
    /// @param newPrice nullopt leaves the price unchanged.
    ModifyResult modify(OrderId id, Quantity newQty, std::optional<Price> newPrice = std::nullopt);

    [[nodiscard]] std::optional<Price> bestBid() const noexcept { return bids_.bestPrice(); }

    [[nodiscard]] std::optional<Price> bestAsk() const noexcept { return asks_.bestPrice(); }

    /// Best ask minus best bid; nullopt unless both sides are populated.
    [[nodiscard]] std::optional<Price> spread() const noexcept;

    /// True when the best bid is at or above the best ask -- a state this book
    /// must never be observed in, and which the tests assert against.
    [[nodiscard]] bool isCrossed() const noexcept;

    [[nodiscard]] const BidBook& bids() const noexcept { return bids_; }

    [[nodiscard]] const AskBook& asks() const noexcept { return asks_; }

    [[nodiscard]] const Order* find(OrderId id) const noexcept;

    [[nodiscard]] const MatchingStrategy& strategy() const noexcept { return *strategy_; }

private:
    /// Runs the aggressor down the opposite side, level by level, best first.
    template <typename OppositeBook>
    void matchAgainst(OppositeBook& opposite, Order& aggressor, SubmitResult& result);

    BidBook bids_;
    AskBook asks_;
    std::unique_ptr<MatchingStrategy> strategy_;

    /// Stamped onto each order as it is accepted. Starts at 1 so that 0 reads
    /// as "never queued".
    Sequence nextSequence_{1};
};

} // namespace exchange
