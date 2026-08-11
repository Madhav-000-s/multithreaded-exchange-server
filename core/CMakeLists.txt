# core -- Order hierarchy, OrderBook<PriceCompare>, MatchingStrategy,
#         BookIterator. The only component permitted to mutate book state.
#
# INTERFACE until Phase 1 adds translation units, at which point this becomes
# add_library(exchange_core STATIC <sources>) and the link keyword below
# becomes PUBLIC. An INTERFACE library is legal with zero sources; a STATIC one
# is not, so the skeleton stays configurable from the first commit.
add_library(exchange_core INTERFACE)
add_library(exchange::core ALIAS exchange_core)

target_link_libraries(exchange_core INTERFACE exchange::flags)
