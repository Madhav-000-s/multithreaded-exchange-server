#pragma once

#include "core/order.hpp"
#include "core/types.hpp"

#include <cstddef>
#include <list>
#include <memory>
#include <type_traits>

namespace exchange {

/// The FIFO queue of orders resting at one price.
///
/// Storage is `std::list<std::unique_ptr<Order>>`, chosen for one property:
/// **iterator stability**. The book's O(1) cancel works by storing an iterator
/// into this queue in a hash index, so that iterator has to survive arbitrary
/// insertions and removals elsewhere in the queue. `std::vector` invalidates
/// everything from the erased position onward and `std::deque` invalidates all
/// iterators on any middle erase, which would leave every index entry at the
/// level dangling. A list node is independent: erasing one leaves every other
/// iterator valid, and `splice` moves a node between positions without even
/// touching the element -- again keeping iterators valid, which is what makes
/// an iceberg requeue free.
///
/// The cost is a pointer chase per order and an allocation per insert. That is
/// the trade this design accepts, and Phase 7's flat-array variant is where it
/// gets revisited with a measurement rather than an opinion.
///
/// This class is the sole owner of the mutation vocabulary for a queue: every
/// operation that changes an order's quantity goes through it, because it also
/// maintains the cached aggregates. Letting a caller call
/// `order.onPartialFill()` directly would silently desynchronise them.
class PriceLevel {
public:
    using Queue = std::list<std::unique_ptr<Order>>;
    using Iterator = Queue::iterator;
    using ConstIterator = Queue::const_iterator;

    explicit PriceLevel(Price price) noexcept : price_(price) {}

    // Holds unique_ptrs, so copying is meaningless; moving is required because
    // std::map may relocate the mapped value.
    PriceLevel(const PriceLevel&) = delete;
    PriceLevel& operator=(const PriceLevel&) = delete;
    PriceLevel(PriceLevel&&) noexcept = default;
    PriceLevel& operator=(PriceLevel&&) noexcept = default;
    ~PriceLevel() = default;

    [[nodiscard]] Price price() const noexcept { return price_; }

    [[nodiscard]] bool empty() const noexcept { return orders_.empty(); }

    [[nodiscard]] std::size_t orderCount() const noexcept { return orders_.size(); }

    /// Sum of remaining() across the queue, including hidden iceberg size.
    /// Maintained incrementally so a depth query is O(1) per level rather than
    /// O(orders) -- the difference matters because analytics polls it.
    [[nodiscard]] Quantity totalQty() const noexcept { return totalQty_; }

    /// Sum of visibleQty(), i.e. what an L2 market-data feed would publish.
    [[nodiscard]] Quantity visibleQty() const noexcept { return visibleQty_; }

    [[nodiscard]] Iterator begin() noexcept { return orders_.begin(); }

    [[nodiscard]] Iterator end() noexcept { return orders_.end(); }

    [[nodiscard]] ConstIterator begin() const noexcept { return orders_.begin(); }

    [[nodiscard]] ConstIterator end() const noexcept { return orders_.end(); }

    /// Appends to the back of the queue -- the only way in, which is what
    /// makes time priority a property of the container rather than something
    /// the matcher has to sort by. Returns the iterator the book indexes.
    Iterator enqueue(std::unique_ptr<Order> order);

    /// Unlinks and hands back ownership. O(1) and noexcept: list::erase on a
    /// single node neither allocates nor compares.
    [[nodiscard]] std::unique_ptr<Order> extract(Iterator where) noexcept;

    /// Executes `qty` against the order at `where`, keeping the aggregates in
    /// step. Precondition: qty <= that order's visibleQty().
    void applyFill(Iterator where, Quantity qty) noexcept;

    /// Refreshes an exhausted iceberg tranche and sends it to the back of the
    /// queue. `where` stays valid: splice relinks the node, it does not copy
    /// or reseat the element, so the book's index needs no update at all.
    void replenishAndRequeue(Iterator where) noexcept;

    /// Rewrites the quantity of a resting order without moving it, preserving
    /// queue position. Only legal as a reduction -- growing an order while it
    /// holds its place would let a participant claim priority it never queued
    /// for.
    void reduceQuantity(Iterator where, Quantity newQty) noexcept;

    /// Takes the order at `position` in `from` and appends it to this queue.
    ///
    /// noexcept, and that is the point: splice relinks an existing node rather
    /// than allocating a new one, so an order can be pre-allocated into a
    /// scratch list while throwing is still harmless and then committed here
    /// once it must not be. `position` stays valid across the move, so the
    /// book's index entry can be built before the commit.
    void adopt(Queue& from, Iterator position) noexcept;

private:
    Queue orders_;
    Quantity totalQty_{0};
    Quantity visibleQty_{0};
    Price price_;
};

// std::map relocates mapped values; a throwing move would break the strong
// guarantee the book claims in Phase 3, so verify rather than assume.
static_assert(std::is_nothrow_move_constructible_v<PriceLevel>,
              "PriceLevel must move without throwing");

} // namespace exchange
