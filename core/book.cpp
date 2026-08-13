#include "core/book.hpp"

#include "core/exceptions.hpp"
#include "core/precondition.hpp"
#include "core/price_time_strategy.hpp"

#include <string>
#include <utility>

namespace exchange {
namespace {

/// Rewrites a resting order size without moving it. Factored out because the
/// two sides are different types, so a ternary over them will not compile --
/// the same reason every other cross-side operation here is a template.
template <typename OwnSide>
void reduceInPlace(OwnSide& side, OrderId id, Quantity newQty) noexcept {
    const auto located = side.locate(id);
    EXCHANGE_PRECONDITION(located.has_value());
    located->level->reduceQuantity(located->position, newQty);
}

} // namespace

Book::Book(std::unique_ptr<MatchingStrategy> strategy)
    : strategy_(strategy ? std::move(strategy) : std::make_unique<PriceTimeStrategy>()) {}

std::optional<Price> Book::spread() const noexcept {
    const std::optional<Price> bid = bids_.bestPrice();
    const std::optional<Price> ask = asks_.bestPrice();
    if (!bid.has_value() || !ask.has_value()) {
        return std::nullopt;
    }
    return *ask - *bid;
}

bool Book::isCrossed() const noexcept {
    const std::optional<Price> bid = bids_.bestPrice();
    const std::optional<Price> ask = asks_.bestPrice();
    return bid.has_value() && ask.has_value() && *bid >= *ask;
}

const Order* Book::find(OrderId id) const noexcept {
    if (const Order* order = bids_.find(id); order != nullptr) {
        return order;
    }
    return asks_.find(id);
}

void Book::validate(const Order& order) const {
    if (order.remaining() == 0) {
        throw InvalidOrderError("order " + std::to_string(order.id()) + " has zero quantity");
    }
    if (find(order.id()) != nullptr) {
        throw InvalidOrderError("duplicate order id " + std::to_string(order.id()));
    }
    // Deliberately no check that the price is positive: negative prices are
    // legitimate, which is why Price is signed. See core/types.hpp.
}

// ---------------------------------------------------------------------------
// Plan
// ---------------------------------------------------------------------------

template <typename OppositeSide>
void Book::planAgainst(OppositeSide& opposite, const Order& aggressor, MatchPlan& plan) const {
    Quantity available = aggressor.remaining();

    // Walk from the touch outward. bestPrice() is the first key on either
    // side -- that is what templating on the comparator buys, and why this
    // loop is written once rather than twice.
    for (auto level = opposite.levels().begin(); level != opposite.levels().end() && available > 0;
         ++level) {
        // Whether a level is eligible at all is the book's business; how
        // quantity is distributed within an eligible level is the strategy's.
        // Keeping the split there is what lets pro-rata be a drop-in.
        if (!aggressor.crosses(level->first)) {
            break;
        }

        const std::size_t before = plan.fills.size();
        strategy_->plan(aggressor, available, level->second, plan);

        Quantity planned = 0;
        for (std::size_t i = before; i < plan.fills.size(); ++i) {
            planned += plan.fills[i].quantity;
        }
        available -= planned;

        if (plan.selfMatchBlocked) {
            break;
        }
        // Nothing traded and nothing was blocked: this level cannot satisfy
        // the aggressor and never will. Break rather than spin.
        if (planned == 0) {
            break;
        }
    }

    plan.aggressorFilled = aggressor.remaining() - available;
}

// ---------------------------------------------------------------------------
// Prepare -- everything that can throw, before anything has changed
// ---------------------------------------------------------------------------

template <typename OwnSide, typename OppositeSide>
PreparedSubmit<OwnSide> Book::prepareSubmit(OwnSide& own, OppositeSide& opposite,
                                            std::unique_ptr<Order> order) {
    EXCHANGE_PRECONDITION(order != nullptr);

    PreparedSubmit<OwnSide> prepared;

    // The order is parked in a one-element list rather than kept as a bare
    // unique_ptr. That buys the list node up front, so resting it later is a
    // splice instead of an allocation.
    prepared.holding.push_back(std::move(order));
    prepared.position = prepared.holding.begin();

    const Order& aggressor = **prepared.position;
    planAgainst(opposite, aggressor, prepared.plan);

    prepared.fills.reserve(prepared.plan.fills.size());

    const Quantity restingQty = aggressor.remaining() - prepared.plan.aggressorFilled;
    const std::optional<Price> restingPrice = aggressor.restingPrice();

    // A market order returns nullopt here, which is what makes "cancel the
    // unfilled remainder" fall out without a type test.
    prepared.willRest =
        restingQty > 0 && !prepared.plan.selfMatchBlocked && restingPrice.has_value();

    if (prepared.willRest) {
        prepared.restPrice = *restingPrice;
        prepared.levelNode = OwnSide::makeLevelNode(prepared.restPrice);
        prepared.indexNode = OwnSide::makeIndexNode(aggressor.id());
        // Grows the bucket array now so installing the index node cannot
        // rehash later. This changes capacity, never contents, so the book
        // observably still holds exactly what it did.
        own.reserveIndex(1);
    }

    return prepared;
}

// ---------------------------------------------------------------------------
// Commit -- nothing here may throw
// ---------------------------------------------------------------------------

template <typename OwnSide, typename OppositeSide>
SubmitResult Book::commitSubmit(OwnSide& own, OppositeSide& opposite,
                                PreparedSubmit<OwnSide>&& prepared) noexcept {
    SubmitResult result;
    result.fills = std::move(prepared.fills); // moves the reserved buffer

    Order& aggressor = prepared.aggressor();
    aggressor.assignSequence(nextSequence_++);

    for (const PlannedFill& planned : prepared.plan.fills) {
        // Every iterator in the plan is still valid: the level queue is a
        // std::list, so filling, extracting a neighbour, or splicing a
        // replenished iceberg to the back leaves other nodes untouched.
        planned.level->applyFill(planned.resting, planned.quantity);
        aggressor.onPartialFill(planned.quantity);

        // Capacity was reserved during preparation, so this cannot allocate.
        result.fills.push_back(Fill{.aggressorId = aggressor.id(),
                                    .restingId = planned.restingId,
                                    .price = planned.price,
                                    .quantity = planned.quantity,
                                    .aggressorAccount = aggressor.account(),
                                    .restingAccount = planned.restingAccount,
                                    .aggressorSide = aggressor.side()});

        Order& resting = **planned.resting;
        if (resting.isFilled()) {
            opposite.unindex(planned.restingId);
            const std::unique_ptr<Order> consumed = planned.level->extract(planned.resting);
        } else if (resting.needsReplenish()) {
            planned.level->replenishAndRequeue(planned.resting);
        }
    }

    result.filledQty = prepared.plan.aggressorFilled;
    opposite.dropEmptyLevelsFromFront();

    if (prepared.willRest) {
        PriceLevel& destination = own.installLevel(std::move(prepared.levelNode));
        own.installIndex(std::move(prepared.indexNode), prepared.restPrice, prepared.position);
        destination.adopt(prepared.holding, prepared.position);

        result.restingQty = aggressor.remaining();
        result.status =
            result.fills.empty() ? SubmitStatus::Resting : SubmitStatus::PartiallyFilledResting;
        return result;
    }

    // Not resting: the order is destroyed with `prepared.holding`.
    if (aggressor.isFilled()) {
        result.status = SubmitStatus::Filled;
    } else if (prepared.plan.selfMatchBlocked) {
        // Deliberately not rested. The order crosses its own quote by
        // definition, so resting it would leave the book crossed.
        result.status = SubmitStatus::SelfMatchBlocked;
    } else {
        // A market order with quantity left: the opposite side ran out and
        // there is no price at which it could wait.
        result.status = SubmitStatus::CancelledRemainder;
    }
    return result;
}

// ---------------------------------------------------------------------------

SubmitResult Book::submit(std::unique_ptr<Order> order) {
    if (order == nullptr) {
        throw InvalidOrderError("submit called with a null order");
    }
    validate(*order);

    if (order->side() == Side::Buy) {
        PreparedSubmit<BidBook> prepared = prepareSubmit(bids_, asks_, std::move(order));
        SubmitResult result = commitSubmit(bids_, asks_, std::move(prepared));
        EXCHANGE_PRECONDITION(!isCrossed());
        return result;
    }

    PreparedSubmit<AskBook> prepared = prepareSubmit(asks_, bids_, std::move(order));
    SubmitResult result = commitSubmit(asks_, bids_, std::move(prepared));
    EXCHANGE_PRECONDITION(!isCrossed());
    return result;
}

std::unique_ptr<Order> Book::cancel(OrderId id) noexcept {
    if (std::unique_ptr<Order> order = bids_.cancel(id); order != nullptr) {
        return order;
    }
    return asks_.cancel(id);
}

// ---------------------------------------------------------------------------
// Modify
// ---------------------------------------------------------------------------

template <typename OwnSide, typename OppositeSide>
ModifyResult Book::requeue(OwnSide& own, OppositeSide& opposite, OrderId id, Quantity newQty,
                           Price targetPrice) {
    const Order* resting = own.find(id);
    EXCHANGE_PRECONDITION(resting != nullptr);

    // A detached copy carrying the new terms. Cloning rather than amending in
    // place is what makes this strong: the original stays resting and intact
    // while everything that can fail is done against the replacement.
    std::unique_ptr<Order> replacement = resting->cloneAmended(newQty, targetPrice);

    // Plans against the opposite side and buys every commit resource. The
    // original order sits on `own`, so nothing here is disturbed by it still
    // being there -- and if any of it throws, it still is.
    PreparedSubmit<OwnSide> prepared = prepareSubmit(own, opposite, std::move(replacement));

    // ---- nothing below may throw ----

    // Withdrawing frees an index slot before the prepared node claims one, so
    // the hash table cannot need to grow.
    const std::unique_ptr<Order> withdrawn = own.cancel(id);
    EXCHANGE_PRECONDITION(withdrawn != nullptr);

    SubmitResult submitted = commitSubmit(own, opposite, std::move(prepared));

    ModifyResult result;
    result.status = ModifyStatus::Requeued;
    result.fills = std::move(submitted.fills);
    result.restingQty = submitted.restingQty;
    return result;
}

ModifyResult Book::modify(OrderId id, Quantity newQty, std::optional<Price> newPrice) {
    ModifyResult result;

    // A zero-quantity amendment is a cancel wearing a disguise. Rejecting it
    // keeps the two operations distinct, so a client cannot cancel by accident
    // and the audit trail says which one was intended.
    if (newQty == 0) {
        result.status = ModifyStatus::Rejected;
        return result;
    }

    // Looked up once and held, rather than re-queried per branch. Repeating
    // find() would cost two extra hash lookups and, more importantly, would
    // leave the compiler unable to connect the null check to the dereference.
    const Order* resting = bids_.find(id);
    const bool onBid = resting != nullptr;
    if (!onBid) {
        resting = asks_.find(id);
    }
    if (resting == nullptr) {
        result.status = ModifyStatus::NotFound;
        return result;
    }

    const std::optional<Price> currentPrice = resting->restingPrice();
    EXCHANGE_PRECONDITION(currentPrice.has_value());

    const bool priceMoves = newPrice.has_value() && *newPrice != *currentPrice;
    const bool sizeGrows = newQty > resting->remaining();

    if (!priceMoves && !sizeGrows) {
        // Reducing takes nothing from anyone behind, so it keeps its place.
        // Nothrow: the level rewrites a quantity and adjusts two counters.
        if (onBid) {
            reduceInPlace(bids_, id, newQty);
        } else {
            reduceInPlace(asks_, id, newQty);
        }

        result.status = ModifyStatus::AmendedInPlace;
        result.restingQty = newQty;
        return result;
    }

    // Priority is forfeit, so the order leaves the book and comes back in
    // through the normal submit path. Reusing that path is deliberate: a
    // repriced order that now crosses must trade exactly as a fresh order
    // would, and a second implementation of that rule would eventually
    // disagree with the first.
    const Price target = newPrice.value_or(*currentPrice);
    return onBid ? requeue(bids_, asks_, id, newQty, target)
                 : requeue(asks_, bids_, id, newQty, target);
}

} // namespace exchange
