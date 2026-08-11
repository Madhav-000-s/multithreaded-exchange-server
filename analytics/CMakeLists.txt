# analytics -- VWAP, depth, imbalance, L2 snapshot.
#
# Built entirely from standard algorithms over the book's iterators, which is
# why it depends on core but core never depends on it.
add_library(exchange_analytics INTERFACE)
add_library(exchange::analytics ALIAS exchange_analytics)

target_link_libraries(exchange_analytics INTERFACE
    exchange::flags
    exchange::core)
