#pragma once

#include "core/book.hpp"
#include "core/types.hpp"

#include <optional>

namespace exchange::analytics {

/// Top-of-book quantities on each side.
struct TouchQty {
    Quantity bid{};
    Quantity ask{};
};

/// Best ask minus best bid, in ticks. nullopt unless both sides are quoted.
[[nodiscard]] std::optional<Price> spreadTicks(const Book& book);

/// Resting quantity at the best price on each side, hidden size included.
[[nodiscard]] TouchQty touchQty(const Book& book);

/// The arithmetic midpoint of the touch.
///
/// Fractional by construction on an odd spread, which is why it is a double
/// rather than a Price -- rounding it to a tick would invent a price that
/// nobody is quoting.
[[nodiscard]] std::optional<double> midPrice(const Book& book);

/// Size-weighted midpoint: (ask x bidQty + bid x askQty) / (bidQty + askQty).
///
/// Note the crossed weighting -- the bid quantity multiplies the *ask* price.
/// A large resting bid means buyers are queued and sellers are scarce, so fair
/// value sits nearer the offer. The plain midpoint ignores that entirely and
/// is the worse predictor of the next trade for exactly this reason.
[[nodiscard]] std::optional<double> microPrice(const Book& book);

/// Order-book imbalance in [-1, +1]: (bidQty - askQty) / (bidQty + askQty).
/// Positive means more size resting on the bid.
[[nodiscard]] std::optional<double> imbalance(const Book& book);

} // namespace exchange::analytics
