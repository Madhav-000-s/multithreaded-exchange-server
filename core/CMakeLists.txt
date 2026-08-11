# core -- Order hierarchy, OrderBook<PriceCompare>, MatchingStrategy,
#         BookIterator. The only component permitted to mutate book state.
add_library(exchange_core STATIC
    fill.cpp
    order.cpp
    price_level.cpp
    price_time_strategy.cpp
    book.cpp)

add_library(exchange::core ALIAS exchange_core)

# PUBLIC rather than PRIVATE: the flags target also carries the include root,
# and core's headers are part of every dependent's interface.
target_link_libraries(exchange_core PUBLIC exchange::flags)
