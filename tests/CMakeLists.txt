include(FetchContent)

# Pinned to a release tag rather than a branch: a build that reproduces is
# worth more than one that is automatically current, and an unpinned
# dependency can turn a green suite red without a local commit.
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
    # SYSTEM demotes GoogleTest's headers to -isystem, so its diagnostics do
    # not reach -Werror. Without it the project's warning set would be
    # hostage to a third party's style.
    SYSTEM)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

add_executable(exchange_tests
    smoke_test.cpp
    order_test.cpp
    price_level_test.cpp
    order_book_test.cpp
    book_test.cpp
    iceberg_test.cpp
    book_iterator_test.cpp
    analytics_test.cpp
    exception_safety_test.cpp
    ownership_sweep_test.cpp
    thread_safe_queue_test.cpp
    thread_pool_test.cpp
    engine_test.cpp
    protocol_test.cpp
    net_integration_test.cpp
    alloc_injection.cpp)

# Test-local headers (order_factory.hpp) are included unqualified; everything
# reaching into the project proper is spelled "core/...".
target_include_directories(exchange_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(exchange_tests PRIVATE
    exchange::flags
    exchange::core
    exchange::concurrent
    exchange::net
    exchange::store
    exchange::analytics
    GTest::gtest_main)

if(EXCHANGE_TEST_LAUNCHER)
    # gtest_discover_tests both enumerates and runs the binary through
    # CROSSCOMPILING_EMULATOR, so one property covers build-time discovery and
    # `ctest` alike. See the ThreadSanitizer note in the top-level CMakeLists.
    set_target_properties(exchange_tests PROPERTIES
        CROSSCOMPILING_EMULATOR "${EXCHANGE_TEST_LAUNCHER}")
endif()

include(GoogleTest)

# Registers each TEST() with CTest individually rather than as one opaque
# binary, so a failure names the case and -j actually parallelises.
gtest_discover_tests(exchange_tests
    DISCOVERY_TIMEOUT 30)

# Phase 2's exit criterion is a property of the source, not of a running
# binary, so it is checked as a source-level test rather than asserted in a
# comment. A rule nothing enforces decays on the next commit.
add_test(NAME AnalyticsHasNoRawLoops
    COMMAND "${CMAKE_COMMAND}"
        -D "ANALYTICS_DIR=${PROJECT_SOURCE_DIR}/analytics"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_no_raw_loops.cmake")

# The ownership sweep from ARCHITECTURE section 5, checked rather than trusted.
# The library must reach every allocation through a smart pointer.
add_test(NAME LibraryHasNoRawNewOrDelete
    COMMAND "${CMAKE_COMMAND}"
        -D "SOURCE_DIRS=${PROJECT_SOURCE_DIR}/core;${PROJECT_SOURCE_DIR}/analytics"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_no_raw_memory.cmake")
