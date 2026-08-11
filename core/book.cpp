#include "core/book.hpp"

#include "core/price_time_strategy.hpp"

#include <cassert>
#include <utility>

namespace exchange {

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

template <typename OppositeBook>
void Book::matchAgainst(OppositeBook& opposite, Order& aggressor, SubmitResult& result) {
    // Walk the opposite side from the touch outward. `bestPrice()` is the map's
    // first key on either side -- that is what templating on the comparator
    // buys, and it is why this loop is written once rather than twice.
    while (aggressor.remaining() > 0) {
        const std::optional<Price> best = opposite.bestPrice();
        if (!best.has_value()) {
            break; // opposite side exhausted
        }

        // The price test lives here, not in the strategy: whether a level is
        // eligible at all is the book's business, while how the quantity is
        // distributed within an eligible level is the strategy's. Keeping the
        // split there is what lets pro-rata be a drop-in.
        if (!aggressor.crosses(*best)) {
            break;
        }

        PriceLevel* level = opposite.bestLevel();
        assert(level != nullptr);

        MatchResult matched = strategy_->match(aggressor, *level);

        for (const std::unique_ptr<Order>& consumed : matched.filled) {
            opposite.unindex(consumed->id());
        }

        for (const Fill& fill : matched.fills) {
            result.filledQty += fill.quantity;
        }
        result.fills.insert(result.fills.end(), matched.fills.begin(), matched.fills.end());

        opposite.dropBestLevelIfEmpty();

        if (matched.selfMatchBlocked) {
            result.status = SubmitStatus::SelfMatchBlocked;
            return;
        }

        // Nothing traded and nothing was blocked: the level could not satisfy
        // the aggressor and never will. Break rather than spin.
        if (matched.fills.empty()) {
            break;
        }
    }
}

SubmitResult Book::submit(std::unique_ptr<Order> order) {
    assert(order != nullptr);

    SubmitResult result;
    order->assignSequence(nextSequence_++);

    const Side side = order->side();
    if (side == Side::Buy) {
        matchAgainst(asks_, *order, result);
    } else {
        matchAgainst(bids_, *order, result);
    }

    if (order->isFilled()) {
        result.status = SubmitStatus::Filled;
        return result; // order destroyed here; its lifetime ended at the fill
    }

    if (result.status == SubmitStatus::SelfMatchBlocked) {
        // Deliberately not rested. The order crosses its own quote by
        // definition, so resting it would leave the book crossed.
        return result;
    }

    const std::optional<Price> restingPrice = order->restingPrice();
    if (!restingPrice.has_value()) {
        // A market order with quantity left: the opposite side ran out. There
        // is no price at which it could wait, so the remainder is cancelled.
        result.status = SubmitStatus::CancelledRemainder;
        return result;
    }

    result.restingQty = order->remaining();
    result.status =
        result.fills.empty() ? SubmitStatus::Resting : SubmitStatus::PartiallyFilledResting;

    if (side == Side::Buy) {
        bids_.insert(std::move(order));
    } else {
        asks_.insert(std::move(order));
    }

    // A resting order can only have been priced through the opposite side if
    // matching stopped early, which would be a matching bug rather than a
    // legal state.
    assert(!isCrossed() && "book left crossed after submit");
    return result;
}

std::unique_ptr<Order> Book::cancel(OrderId id) {
    if (std::unique_ptr<Order> order = bids_.cancel(id); order != nullptr) {
        return order;
    }
    return asks_.cancel(id);
}

ModifyResult Book::modify(OrderId id, Quantity newQty, std::optional<Price> newPrice) {
    ModifyResult result;

    // A zero-quantity amendment is a cancel wearing a disguise. Rejecting it
    // keeps the two operations distinct, so a client cannot cancel by
    // accident and the audit trail says which one was intended.
    if (newQty == 0) {
        result.status = ModifyStatus::Rejected;
        return result;
    }

    const bool onBid = bids_.find(id) != nullptr;
    const bool onAsk = asks_.find(id) != nullptr;
    if (!onBid && !onAsk) {
        result.status = ModifyStatus::NotFound;
        return result;
    }

    const auto attempt = [&](auto& side) -> ModifyResult {
        ModifyResult out;
        const auto located = side.locate(id);
        assert(located.has_value());

        Order& order = **located->position;
        const std::optional<Price> currentPrice = order.restingPrice();
        assert(currentPrice.has_value() && "a resting order always has a price");

        const bool priceMoves = newPrice.has_value() && *newPrice != *currentPrice;
        const bool sizeGrows = newQty > order.remaining();

        if (!priceMoves && !sizeGrows) {
            located->level->reduceQuantity(located->position, newQty);
            out.status = ModifyStatus::AmendedInPlace;
            out.restingQty = newQty;
            return out;
        }

        // Priority is forfeit, so the order leaves the book entirely and comes
        // back in through submit(). Reusing the submit path is deliberate: a
        // repriced order that now crosses must trade exactly as a fresh order
        // would, and a second implementation of that would eventually disagree
        // with the first.
        std::unique_ptr<Order> detached = side.cancel(id);
        assert(detached != nullptr);
        detached->amend(newQty, newPrice.value_or(*currentPrice));

        SubmitResult resubmit = submit(std::move(detached));
        out.status = ModifyStatus::Requeued;
        out.fills = std::move(resubmit.fills);
        out.restingQty = resubmit.restingQty;
        return out;
    };

    result = onBid ? attempt(bids_) : attempt(asks_);
    return result;
}

} // namespace exchange
