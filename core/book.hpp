#pragma once

#include "core/fill.hpp"
#include "core/matching_strategy.hpp"
#include "core/order.hpp"
#include "core/order_book.hpp"
#include "core/price_level.hpp"
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

/// Everything an incoming order needs, assembled before the book is touched.
///
/// This type is the strong exception guarantee made concrete. Constructing it
/// allocates every resource the commit will need -- the plan, the fill vector,
/// the list node holding the order, the map node for its price level, the hash
/// node for its index entry -- while a throw still costs nothing, because the
/// book has not changed. Committing it then consists only of splices,
/// arithmetic and node installations, none of which can fail.
///
/// Templated on the side because a map node_type depends on its comparator, so
/// a bid level node and an ask level node are different types.
template <typename OwnSide>
struct PreparedSubmit {
    MatchPlan plan;

    /// Owns the incoming order until commit. A one-element list, so the order
    /// can later be spliced into a price level without allocating.
    PriceLevel::Queue holding;
    PriceLevel::Iterator position{};

    typename OwnSide::LevelNode levelNode;
    typename OwnSide::IndexNode indexNode;

    /// Capacity reserved during preparation, so appending during commit
    /// cannot allocate.
    std::vector<Fill> fills;

    Price restPrice{};
    bool willRest{false};

    [[nodiscard]] Order& aggressor() noexcept { return **position; }
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
    /// **Strong exception guarantee.** If this throws -- from validation, from
    /// an allocation while planning, or from an allocation while reserving the
    /// commit resources -- the book is bit-for-bit what it was, and the order
    /// is destroyed with the argument. There is no partially applied sweep and
    /// no index entry naming a row that does not exist.
    ///
    /// Takes ownership unconditionally: whether the order ends up resting,
    /// fully filled, or cancelled, the caller is not left holding a pointer
    /// whose lifetime depends on which of those happened.
    ///
    /// @throws InvalidOrderError on zero quantity or a duplicate order id.
    SubmitResult submit(std::unique_ptr<Order> order);

    /// Removes a resting order and returns it, or nullptr if unknown.
    ///
    /// **Nothrow.** Every step -- the hash lookup, unlinking the list node,
    /// erasing the level -- only destroys. Ownership comes back to the caller
    /// rather than being dropped, so a cancel can be reported on with the
    /// final state of the order intact.
    [[nodiscard]] std::unique_ptr<Order> cancel(OrderId id) noexcept;

    /// Amends a resting order size and optionally its price.
    ///
    /// Queue position is retained only for a pure size *reduction* at the same
    /// price. Raising size or moving price forfeits priority, because both
    /// would otherwise let a participant hold a queue position earned under
    /// terms they are no longer offering -- quote small to get to the front,
    /// then size up once there. Reducing takes nothing from anyone behind, so
    /// there is no reason to charge for it.
    ///
    /// **Strong exception guarantee**, including on the requeue path. The
    /// replacement is cloned, planned and fully provisioned before the
    /// original is withdrawn, so a failure cannot leave the amendment
    /// half-applied or the original order lost.
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
    /// Rejects an order the book will not accept. Throws before anything is
    /// touched, which is what makes a validation failure trivially strong.
    void validate(const Order& order) const;

    /// Walks the opposite side from the touch outward, accumulating what would
    /// trade. Mutates nothing.
    template <typename OppositeSide>
    void planAgainst(OppositeSide& opposite, const Order& aggressor, MatchPlan& plan) const;

    /// Assembles every resource the commit will need. May throw; on failure
    /// neither side has changed.
    template <typename OwnSide, typename OppositeSide>
    PreparedSubmit<OwnSide> prepareSubmit(OwnSide& own, OppositeSide& opposite,
                                          std::unique_ptr<Order> order);

    /// Applies a prepared submit. Cannot throw -- and is marked noexcept so
    /// that a mistake in that reasoning terminates loudly instead of leaving a
    /// half-mutated book behind.
    template <typename OwnSide, typename OppositeSide>
    SubmitResult commitSubmit(OwnSide& own, OppositeSide& opposite,
                              PreparedSubmit<OwnSide>&& prepared) noexcept;

    /// Withdraws a resting order and re-enters it with new terms.
    template <typename OwnSide, typename OppositeSide>
    ModifyResult requeue(OwnSide& own, OppositeSide& opposite, OrderId id, Quantity newQty,
                         Price targetPrice);

    BidBook bids_;
    AskBook asks_;
    std::unique_ptr<MatchingStrategy> strategy_;

    /// Stamped onto each order as it is accepted. Starts at 1 so that 0 reads
    /// as "never queued".
    Sequence nextSequence_{1};
};

} // namespace exchange
