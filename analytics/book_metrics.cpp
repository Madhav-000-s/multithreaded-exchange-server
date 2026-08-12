#include "analytics/book_metrics.hpp"

#include "core/book.hpp"
#include "core/types.hpp"

#include <optional>

namespace exchange::analytics {
namespace {

/// The touch on both sides, or nullopt if either is unquoted. Every metric
/// below needs exactly this, and each would otherwise repeat the two optional
/// checks and get to disagree about the edge case.
struct Touch {
    Price bid;
    Price ask;
    double bidQty;
    double askQty;
};

[[nodiscard]] std::optional<Touch> touch(const Book& book) {
    const std::optional<Price> bid = book.bestBid();
    const std::optional<Price> ask = book.bestAsk();
    if (!bid.has_value() || !ask.has_value()) {
        return std::nullopt;
    }
    return Touch{.bid = *bid,
                 .ask = *ask,
                 .bidQty = static_cast<double>(book.bids().qtyAt(*bid)),
                 .askQty = static_cast<double>(book.asks().qtyAt(*ask))};
}

} // namespace

std::optional<Price> spreadTicks(const Book& book) {
    return book.spread();
}

TouchQty touchQty(const Book& book) {
    const std::optional<Price> bid = book.bestBid();
    const std::optional<Price> ask = book.bestAsk();
    return TouchQty{.bid = bid.has_value() ? book.bids().qtyAt(*bid) : Quantity{0},
                    .ask = ask.has_value() ? book.asks().qtyAt(*ask) : Quantity{0}};
}

std::optional<double> midPrice(const Book& book) {
    const std::optional<Touch> quote = touch(book);
    if (!quote.has_value()) {
        return std::nullopt;
    }
    // Halve after summing, in double: (bid + ask) / 2 in integer arithmetic
    // would silently floor a half-tick mid.
    return (static_cast<double>(quote->bid) + static_cast<double>(quote->ask)) / 2.0;
}

std::optional<double> microPrice(const Book& book) {
    const std::optional<Touch> quote = touch(book);
    if (!quote.has_value()) {
        return std::nullopt;
    }
    const double totalQty = quote->bidQty + quote->askQty;
    if (totalQty <= 0.0) {
        return std::nullopt;
    }
    return (static_cast<double>(quote->ask) * quote->bidQty +
            static_cast<double>(quote->bid) * quote->askQty) /
           totalQty;
}

std::optional<double> imbalance(const Book& book) {
    const std::optional<Touch> quote = touch(book);
    if (!quote.has_value()) {
        return std::nullopt;
    }
    const double totalQty = quote->bidQty + quote->askQty;
    if (totalQty <= 0.0) {
        return std::nullopt;
    }
    return (quote->bidQty - quote->askQty) / totalQty;
}

} // namespace exchange::analytics
