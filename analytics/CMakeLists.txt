# analytics -- VWAP, depth, imbalance, L2 snapshot.
#
# Built entirely from standard algorithms over the book's iterators, which is
# why it depends on core but core never depends on it. The no-raw-loops rule is
# checked by a CTest case, not left to discipline; see
# tests/check_no_raw_loops.cmake.
#
# side_stats.hpp is header-only because it is templated on the book's price
# comparator; book_metrics operates on Book, which is concrete, so it compiles.
add_library(exchange_analytics STATIC
    book_metrics.cpp)

add_library(exchange::analytics ALIAS exchange_analytics)

target_link_libraries(exchange_analytics PUBLIC
    exchange::flags
    exchange::core)
