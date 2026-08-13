#pragma once

#include "core/book_iterator.hpp"
#include "core/order.hpp"
#include "core/precondition.hpp"
#include "core/price_level.hpp"
#include "core/types.hpp"

#include <cassert>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace exchange {

/// Constrains OrderBook's parameter to something that actually orders prices.
///
/// A C++20 concept rather than an unconstrained template: instantiating with a
/// comparator of the wrong shape otherwise fails deep inside std::map with a
/// page of instantiation backtrace. With the constraint, the error names the
/// requirement that was not met, at the point of use.
template <typename Cmp>
concept PriceComparator = std::strict_weak_order<Cmp, Price, Price>;

/// One side of the book: every resting order, grouped by price.
///
/// Templated on the ordering so bids and asks are the *same* class. With
/// `std::greater` the highest price sorts first; with `std::less` the lowest
/// does. Either way `levels_.begin()` is the best price, so "walk outward from
/// the touch" is written once and both sides get it. Writing a BidBook and an
/// AskBook separately would duplicate every traversal and guarantee that a fix
/// eventually lands in only one of them.
///
/// @tparam PriceCompare BidOrdering (std::greater) or AskOrdering (std::less).
template <PriceComparator PriceCompare>
class OrderBook {
public:
    using Levels = std::map<Price, PriceLevel, PriceCompare>;
    // The commit phase claims that installing a level node and erasing a level
    // cannot throw. Both reduce to comparisons, so that claim is only true if
    // the comparator is nothrow. Check rather than trust.
    static_assert(std::is_nothrow_invocable_r_v<bool, PriceCompare, Price, Price>,
                  "the price comparator must not throw");

    /// Where a resting order lives. Storing the price rather than a pointer to
    /// the PriceLevel keeps this valid across level creation and destruction,
    /// and keeps it meaningful for Phase 7's flat-array book, where levels are
    /// array slots and pointers into them would not survive a resize.
    struct Locator {
        Price price{};
        PriceLevel::Iterator position{};
    };

    /// Flattened order-level traversal, best price first and in queue order
    /// within a price. See core/book_iterator.hpp for why this is a forward
    /// iterator and not something stronger.
    using iterator = BookIteratorImpl<Levels, false>;
    using const_iterator = BookIteratorImpl<Levels, true>;

    OrderBook() = default;
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) noexcept = default;
    OrderBook& operator=(OrderBook&&) noexcept = default;
    ~OrderBook() = default;

    [[nodiscard]] iterator begin() noexcept { return iterator{levels_.begin(), levels_.end()}; }

    [[nodiscard]] iterator end() noexcept { return iterator{levels_.end(), levels_.end()}; }

    [[nodiscard]] const_iterator begin() const noexcept {
        return const_iterator{levels_.begin(), levels_.end()};
    }

    [[nodiscard]] const_iterator end() const noexcept {
        return const_iterator{levels_.end(), levels_.end()};
    }

    [[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }

    [[nodiscard]] const_iterator cend() const noexcept { return end(); }

    [[nodiscard]] bool empty() const noexcept { return levels_.empty(); }

    [[nodiscard]] std::size_t levelCount() const noexcept { return levels_.size(); }

    [[nodiscard]] std::size_t orderCount() const noexcept { return index_.size(); }

    /// Best price on this side, or nullopt when the side is empty.
    [[nodiscard]] std::optional<Price> bestPrice() const noexcept {
        if (levels_.empty()) {
            return std::nullopt;
        }
        return levels_.begin()->first;
    }

    [[nodiscard]] PriceLevel* bestLevel() noexcept {
        return levels_.empty() ? nullptr : &levels_.begin()->second;
    }

    [[nodiscard]] const PriceLevel* levelAt(Price price) const noexcept {
        const auto it = levels_.find(price);
        return it == levels_.end() ? nullptr : &it->second;
    }

    /// Resting quantity at one price, hidden size included. Zero if no level.
    [[nodiscard]] Quantity qtyAt(Price price) const noexcept {
        const PriceLevel* level = levelAt(price);
        return level == nullptr ? Quantity{0} : level->totalQty();
    }

    [[nodiscard]] const Order* find(OrderId id) const noexcept {
        const auto it = index_.find(id);
        return it == index_.end() ? nullptr : it->second.position->get();
    }

    [[nodiscard]] const Levels& levels() const noexcept { return levels_; }

    // -----------------------------------------------------------------------
    // Primitives for the two-phase submit path.
    //
    // Split apart precisely along the line that matters for exception safety:
    // ensureLevel and indexOrder allocate and may throw, adopt and eraseLevel
    // cannot. Book::submit performs every throwing step while the book is
    // still untouched, then commits through the non-throwing ones. Fusing them
    // back into a single insert() is what made the old code merely basic.
    // -----------------------------------------------------------------------

    /// Returns the level at `price`, creating it if absent. May throw; on
    /// failure the side is unchanged.
    PriceLevel& ensureLevel(Price price) { return levels_.try_emplace(price, price).first->second; }

    /// Drops a level. Used to undo an ensureLevel that a later throwing step
    /// made pointless, and to tidy up after matching.
    void eraseLevel(Price price) noexcept { levels_.erase(price); }

    /// Records where a resting order lives. May throw; on failure the index is
    /// unchanged.
    void indexOrder(OrderId id, Price price, PriceLevel::Iterator position) {
        EXCHANGE_PRECONDITION(index_.find(id) == index_.end());
        index_.emplace(id, Locator{.price = price, .position = position});
    }

    /// Grows the hash table ahead of `additional` insertions, so the insertion
    /// itself cannot rehash.
    void reserveIndex(std::size_t additional) { index_.reserve(index_.size() + additional); }

    /// Discards levels emptied by matching.
    ///
    /// Only from the front: an aggressor consumes the book outward from the
    /// touch, so any level it emptied is at the head. Scanning the whole map
    /// would be O(levels) on every submit for no benefit.
    void dropEmptyLevelsFromFront() noexcept {
        while (!levels_.empty() && levels_.begin()->second.empty()) {
            levels_.erase(levels_.begin());
        }
    }

    /// Mutable level access, for the planner and the commit phase.
    [[nodiscard]] Levels& levels() noexcept { return levels_; }

    // -----------------------------------------------------------------------
    // Pre-allocated commit tokens.
    //
    // The remaining obstacle to a non-throwing commit is that both containers
    // are node-based: every insertion allocates. C++17 node handles split that
    // allocation away from the insertion, so the node can be built while a
    // throw is still free and installed once it must not be.
    //
    // insert(node_type&&) allocates nothing. For the map it only compares, and
    // the comparator is asserted nothrow below. For the hash table it can
    // still rehash, which reserveIndex() is called beforehand to rule out.
    // -----------------------------------------------------------------------

    using LevelNode = typename Levels::node_type;
    using IndexNode = typename std::unordered_map<OrderId, Locator>::node_type;

    /// Builds a detached level node. May throw; touches nothing.
    [[nodiscard]] static LevelNode makeLevelNode(Price price) {
        Levels scratch;
        scratch.try_emplace(price, price);
        return scratch.extract(scratch.begin());
    }

    /// Builds a detached index node. May throw; touches nothing.
    [[nodiscard]] static IndexNode makeIndexNode(OrderId id) {
        std::unordered_map<OrderId, Locator> scratch;
        scratch.try_emplace(id, Locator{});
        return scratch.extract(scratch.begin());
    }

    /// Installs a pre-built level node, or discards it if the price already
    /// has a level. Returns the level either way.
    [[nodiscard]] PriceLevel& installLevel(LevelNode&& node) noexcept {
        auto result = levels_.insert(std::move(node));
        return result.position->second;
    }

    /// Points a pre-built index node at `position` and installs it.
    /// Precondition: reserveIndex has made room, so this cannot rehash.
    void installIndex(IndexNode&& node, Price price, PriceLevel::Iterator position) noexcept {
        node.mapped() = Locator{.price = price, .position = position};
        index_.insert(std::move(node));
    }

    /// Rests an order, creating its price level if needed.
    /// Precondition: order->restingPrice() has a value.
    void insert(std::unique_ptr<Order> order) {
        assert(order != nullptr);
        const std::optional<Price> price = order->restingPrice();
        assert(price.has_value() && "a non-resting order must never reach insert()");

        const OrderId id = order->id();
        assert(index_.find(id) == index_.end() && "duplicate order id");

        // try_emplace constructs the level in place only when absent, so an
        // existing queue is never disturbed by a rebuild.
        const auto levelIt = levels_.try_emplace(*price, *price).first;
        const PriceLevel::Iterator position = levelIt->second.enqueue(std::move(order));

        index_.emplace(id, Locator{.price = *price, .position = position});
    }

    /// Removes a resting order and hands back ownership; nullptr if unknown.
    ///
    /// O(1) in the number of *orders*, which is the property that matters: the
    /// hash index goes straight to the list node, and unlinking it is three
    /// pointer writes. There is no scan of the level. The residual cost is the
    /// O(log levels) map lookup to reach the level -- levels number in the
    /// hundreds where orders number in the millions, and Phase 7's flat array
    /// removes even that.
    [[nodiscard]] std::unique_ptr<Order> cancel(OrderId id) noexcept {
        const auto indexIt = index_.find(id);
        if (indexIt == index_.end()) {
            return nullptr;
        }

        const Locator locator = indexIt->second;
        index_.erase(indexIt);

        const auto levelIt = levels_.find(locator.price);
        assert(levelIt != levels_.end() && "index names a level that does not exist");

        std::unique_ptr<Order> order = levelIt->second.extract(locator.position);

        // An empty level is dropped so that bestPrice() cannot report a price
        // with nothing behind it, and so the map does not accumulate every
        // price ever touched.
        if (levelIt->second.empty()) {
            levels_.erase(levelIt);
        }
        return order;
    }

    /// Drops an index entry for an order the matcher has already unlinked.
    ///
    /// The strategy owns level surgery but cannot see the index, so the book
    /// reconciles afterwards. Phase 3 inverts this -- the strategy will return
    /// a plan and the book will perform every mutation itself -- which is what
    /// makes the strong guarantee achievable.
    void unindex(OrderId id) noexcept { index_.erase(id); }

    /// Discards the best level once matching has emptied it.
    void dropBestLevelIfEmpty() noexcept {
        if (!levels_.empty() && levels_.begin()->second.empty()) {
            levels_.erase(levels_.begin());
        }
    }

    /// Resting order plus the handle needed to mutate it in place.
    struct Located {
        PriceLevel* level{nullptr};
        PriceLevel::Iterator position{};
    };

    [[nodiscard]] std::optional<Located> locate(OrderId id) noexcept {
        const auto indexIt = index_.find(id);
        if (indexIt == index_.end()) {
            return std::nullopt;
        }
        const auto levelIt = levels_.find(indexIt->second.price);
        assert(levelIt != levels_.end());
        return Located{.level = &levelIt->second, .position = indexIt->second.position};
    }

    /// Total resting quantity across every level on this side.
    [[nodiscard]] Quantity totalQty() const noexcept {
        Quantity total = 0;
        for (const auto& entry : levels_) {
            total += entry.second.totalQty();
        }
        return total;
    }

private:
    Levels levels_;

    /// OrderId to location. Per side rather than shared, so this class is
    /// self-contained and testable on its own; Book pays two hash lookups on a
    /// cancel instead of one, which is still O(1).
    std::unordered_map<OrderId, Locator> index_;
};

// The iterator's traits are a promise to the standard library, so verify the
// promise rather than assume it. If iterator_category, the reference type or
// the equality operator were wrong, every algorithm would still compile and
// some would quietly do the wrong thing; these fail the build instead.
static_assert(std::forward_iterator<OrderBook<BidOrdering>::iterator>);
static_assert(std::forward_iterator<OrderBook<BidOrdering>::const_iterator>);
static_assert(std::ranges::forward_range<OrderBook<BidOrdering>>);
static_assert(std::ranges::forward_range<const OrderBook<BidOrdering>>);

// begin() and end() share a type, so the book is a common_range and the
// classic two-iterator algorithms work on it, not only the ranges ones.
static_assert(std::ranges::common_range<OrderBook<BidOrdering>>);

// Const-correctness of the traversal: a const book must not hand out a
// mutable reference to a resting order.
static_assert(
    std::is_same_v<std::iter_reference_t<OrderBook<BidOrdering>::const_iterator>, const Order&>);
static_assert(std::is_same_v<std::iter_reference_t<OrderBook<BidOrdering>::iterator>, Order&>);

using BidBook = OrderBook<BidOrdering>; // best bid = highest price
using AskBook = OrderBook<AskOrdering>; // best ask = lowest price

} // namespace exchange
