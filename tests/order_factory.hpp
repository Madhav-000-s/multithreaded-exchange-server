#pragma once

#include "core/order.hpp"
#include "core/types.hpp"

#include <memory>

namespace exchange::test {

// Distinct accounts by default. Self-match prevention is a real behaviour of
// this book, so tests that do not care about accounts still have to avoid
// accidentally triggering it -- naming the participants is cheaper than
// debugging why a fill did not happen.
inline constexpr AccountId kAlice = 1;
inline constexpr AccountId kBob = 2;
inline constexpr AccountId kCarol = 3;

// The factories take (id, side, price, qty, account) rather than the
// constructor's (id, side, account, qty, price). Three adjacent integers in
// the constructor are trivially transposable; putting price next to side and
// account last reads closer to how an order is spoken about.

[[nodiscard]] inline std::unique_ptr<LimitOrder> limit(OrderId id, Side side, Price price,
                                                       Quantity qty, AccountId account) {
    return std::make_unique<LimitOrder>(id, side, account, qty, price);
}

[[nodiscard]] inline std::unique_ptr<MarketOrder> market(OrderId id, Side side, Quantity qty,
                                                         AccountId account) {
    return std::make_unique<MarketOrder>(id, side, account, qty);
}

[[nodiscard]] inline std::unique_ptr<IcebergOrder> iceberg(OrderId id, Side side, Price price,
                                                           Quantity totalQty, Quantity displaySize,
                                                           AccountId account) {
    return std::make_unique<IcebergOrder>(id, side, account, totalQty, price, displaySize);
}

} // namespace exchange::test
