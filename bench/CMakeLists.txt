# bench -- replay driver and latency histogram (Phase 7).
#
# Reached only when -DEXCHANGE_BUILD_BENCH=ON. Both OrderBook implementations
# run through the identical driver so the std::map vs flat-array comparison is
# a genuine A/B rather than two different programs.
message(STATUS "  bench      : declared, sources arrive in Phase 7")
