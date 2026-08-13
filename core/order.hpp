#pragma once

#include "core/types.hpp"

#include <memory>
#include <optional>

namespace exchange {

/// Base of the order hierarchy.
///
/// The book owns orders through `std::unique_ptr<Order>` and destroys them
/// through that base pointer, so the virtual destructor is a correctness
/// requirement rather than a convention -- without it, destroying a
/// LimitOrder through an Order* is undefined behaviour. `-Wnon-virtual-dtor`
/// makes the compiler enforce it.
///
/// The constructor is protected, not public: an Order is an abstract concept
/// with no meaningful standalone instance, and the access modifier says so at
/// compile time rather than relying on the pure virtuals to make it so.
class Order {
public:
    // An order is a unique identity in the book. Copying one would duplicate
    // an OrderId and give the index two entries claiming the same order;
    // moving one would leave a husk still reachable through a Locator. Both
    // are deleted so neither can happen by accident -- lifetime is managed
    // exclusively by transferring the owning unique_ptr.
    Order(const Order&) = delete;
    Order& operator=(const Order&) = delete;
    Order(Order&&) = delete;
    Order& operator=(Order&&) = delete;

    virtual ~Order() = default;

    [[nodiscard]] OrderId id() const noexcept { return id_; }

    [[nodiscard]] Side side() const noexcept { return side_; }

    [[nodiscard]] AccountId account() const noexcept { return account_; }

    /// Quantity not yet executed, including any hidden portion.
    [[nodiscard]] Quantity remaining() const noexcept { return remaining_; }

    [[nodiscard]] bool isFilled() const noexcept { return remaining_ == 0; }

    /// Queue position within a price level. Assigned by the book on
    /// acceptance, and reassigned when an amendment forfeits priority.
    [[nodiscard]] Sequence sequence() const noexcept { return sequence_; }

    void assignSequence(Sequence sequence) noexcept { sequence_ = sequence; }

    /// Would this order trade against a resting order priced at `best`?
    ///
    /// `best` is the best price on the *opposite* side. Pure virtual because
    /// this is precisely what distinguishes the order types: a limit compares,
    /// a market always says yes.
    [[nodiscard]] virtual bool crosses(Price best) const noexcept = 0;

    /// The price this order rests at once it stops trading, or nullopt if it
    /// never rests. A market order returns nullopt, which is what makes
    /// "unfilled market remainder is cancelled" fall out of the submit path
    /// rather than needing a type test.
    [[nodiscard]] virtual std::optional<Price> restingPrice() const noexcept = 0;

    /// Quantity currently exposed to the market and therefore available to
    /// trade right now. Equal to remaining() for everything except an iceberg,
    /// which shows only its display tranche.
    [[nodiscard]] virtual Quantity visibleQty() const noexcept { return remaining_; }

    /// The quantity this order puts back on display when its tranche is
    /// exhausted. Equal to remaining() for an order that hides nothing.
    ///
    /// Exists so the matching planner can simulate replenishment without a
    /// dynamic_cast. The planner must predict the whole sweep before mutating
    /// anything, and asking the order how much it would re-display keeps that
    /// prediction inside the polymorphic interface rather than special-casing
    /// iceberg in the strategy.
    [[nodiscard]] virtual Quantity displaySize() const noexcept { return remaining_; }

    /// Applies an execution of `qty`. Precondition: qty <= visibleQty().
    virtual void onPartialFill(Quantity qty) noexcept;

    /// True when the order still has quantity but nothing left on display, so
    /// it must refresh its tranche and go to the back of the queue.
    ///
    /// Non-virtual, and derived from two virtuals: only an iceberg can ever be
    /// in this state, but nothing in the base has to know that. The book asks
    /// this question without a type test.
    [[nodiscard]] bool needsReplenish() const noexcept {
        return remaining_ > 0 && visibleQty() == 0;
    }

    /// Refreshes the display tranche. No-op unless the order hides quantity.
    virtual void replenish() noexcept {}

    /// Rewrites size and resting price wholesale, re-cutting any display
    /// tranche against the new size. Used on the path where an amendment has
    /// already forfeited queue priority, so there is nothing to preserve.
    ///
    /// Precondition: the order is *not* currently linked into a PriceLevel --
    /// it maintains no aggregate totals and no index entry, so amending a
    /// resting order directly would silently desynchronise both. Book::modify
    /// extracts first, or routes through PriceLevel, which does maintain them.
    virtual void amend(Quantity newQty, Price newPrice) noexcept;

    /// Shrinks the order while it keeps its place in the queue.
    ///
    /// Distinct from amend() precisely because priority is retained here.
    /// Displayed quantity may only fall: an iceberg that reset its tranche on
    /// a size reduction would show more size than before while still holding
    /// the queue position it earned with the old size, which is priority
    /// gaming rather than an amendment.
    ///
    /// Precondition: newQty <= remaining().
    virtual void reduceTo(Quantity newQty) noexcept;

    /// A detached copy carrying new terms, keeping id, side and account.
    ///
    /// Exists for the requeue path of Book::modify. Amending the resting order
    /// in place would mutate the book before the replacement is known to be
    /// admissible; cloning lets the whole entry be planned and provisioned
    /// while the original is still untouched, which is what makes modify
    /// strong rather than merely basic.
    ///
    /// Pure virtual: only the concrete type knows which fields to carry over.
    [[nodiscard]] virtual std::unique_ptr<Order> cloneAmended(Quantity newQty,
                                                              Price newPrice) const = 0;

protected:
    Order(OrderId id, Side side, AccountId account, Quantity qty) noexcept;

    void setRemaining(Quantity qty) noexcept { remaining_ = qty; }

private:
    OrderId id_;
    Quantity remaining_;
    Sequence sequence_{0};
    AccountId account_;
    Side side_;
};

/// Trades at its limit price or better, and rests at that price if it cannot
/// trade immediately.
class LimitOrder : public Order {
public:
    LimitOrder(OrderId id, Side side, AccountId account, Quantity qty, Price limit) noexcept;

    [[nodiscard]] Price limitPrice() const noexcept { return limit_; }

    [[nodiscard]] bool crosses(Price best) const noexcept override;

    [[nodiscard]] std::optional<Price> restingPrice() const noexcept override { return limit_; }

    void amend(Quantity newQty, Price newPrice) noexcept override;

    [[nodiscard]] std::unique_ptr<Order> cloneAmended(Quantity newQty,
                                                      Price newPrice) const override;

private:
    Price limit_;
};

/// Trades at whatever price the book offers and never rests; any quantity left
/// once the opposite side is exhausted is cancelled.
class MarketOrder : public Order {
public:
    MarketOrder(OrderId id, Side side, AccountId account, Quantity qty) noexcept;

    /// Unconditionally true -- a market order has no price at which it would
    /// decline to trade. This is the entire behavioural difference from
    /// LimitOrder, and it is why the base declares crosses() pure virtual
    /// instead of storing a price and comparing.
    [[nodiscard]] bool crosses(Price) const noexcept override { return true; }

    [[nodiscard]] std::optional<Price> restingPrice() const noexcept override {
        return std::nullopt;
    }

    [[nodiscard]] std::unique_ptr<Order> cloneAmended(Quantity newQty,
                                                      Price newPrice) const override;
};

/// A limit order that exposes only part of its size at a time.
///
/// Inherits from LimitOrder rather than Order because it *is* a limit order in
/// every respect that matters to matching -- it crosses on the same
/// comparison and rests at the same price. It overrides only how much of
/// itself is visible, which is the narrowest possible extension point and the
/// case where an inheritance hierarchy genuinely pays for its vtable.
///
/// When the visible tranche is exhausted the order refreshes and surrenders
/// queue priority. That is the real exchange semantic: hiding size has to cost
/// something, or every order would be an iceberg.
class IcebergOrder : public LimitOrder {
public:
    IcebergOrder(OrderId id, Side side, AccountId account, Quantity totalQty, Price limit,
                 Quantity displaySize) noexcept;

    [[nodiscard]] Quantity displaySize() const noexcept override { return display_; }

    /// Only the current tranche. An aggressor can never consume more than
    /// this in one execution, which is what forces the requeue.
    [[nodiscard]] Quantity visibleQty() const noexcept override { return visible_; }

    /// Hidden quantity waiting behind the display tranche.
    [[nodiscard]] Quantity hiddenQty() const noexcept { return remaining() - visible_; }

    void onPartialFill(Quantity qty) noexcept override;

    void replenish() noexcept override;

    void amend(Quantity newQty, Price newPrice) noexcept override;

    void reduceTo(Quantity newQty) noexcept override;

    [[nodiscard]] std::unique_ptr<Order> cloneAmended(Quantity newQty,
                                                      Price newPrice) const override;

private:
    Quantity display_;
    Quantity visible_;
};

} // namespace exchange
