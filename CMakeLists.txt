# 3.25 is the floor because FetchContent_Declare(SYSTEM) lands there, and the
# test harness relies on it to keep third-party warnings out of -Werror.
cmake_minimum_required(VERSION 3.25)

project(exchange
    VERSION 0.1.0
    DESCRIPTION "Multi-threaded TCP exchange server"
    HOMEPAGE_URL "https://github.com/Madhav-000-s/multithreaded-exchange-server"
    LANGUAGES CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not permitted. Configure with, for example:\n"
        "  cmake -S . -B build")
endif()

# The engine is built against POSIX interfaces (epoll, fsync, socket options)
# from Phase 5 onward. Fail loudly and early rather than at the first
# <sys/epoll.h> include several thousand lines from now.
if(NOT UNIX)
    message(FATAL_ERROR
        "This project targets Linux only. Build under WSL or a Linux host.")
endif()

# ---------------------------------------------------------------------------
# Language level
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# -std=c++20 rather than -std=gnu++20: compiler extensions are disabled so
# that a non-portable construct is a compile error here, not a surprise later.
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

get_property(_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _is_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
        "Build type (Debug|RelWithDebInfo|Release)" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(EXCHANGE_BUILD_TESTS "Build the GoogleTest suite" ON)
option(EXCHANGE_BUILD_BENCH "Build the replay benchmark driver" OFF)

set(EXCHANGE_SANITIZER "none" CACHE STRING
    "Sanitizer to instrument the build with (none|address|thread|undefined)")
set_property(CACHE EXCHANGE_SANITIZER PROPERTY STRINGS
    none address thread undefined)

# ---------------------------------------------------------------------------
# exchange::flags -- the single source of truth for how every target compiles
#
# An INTERFACE library rather than directory-scoped add_compile_options: the
# settings propagate through target_link_libraries, so a target either opts in
# explicitly or is not built to project standards. Fetched dependencies never
# inherit these flags, which is what keeps -Werror survivable.
# ---------------------------------------------------------------------------
add_library(exchange_flags INTERFACE)
add_library(exchange::flags ALIAS exchange_flags)

target_compile_features(exchange_flags INTERFACE cxx_std_20)

# Component headers are included as "core/order.hpp" -- rooted at the repo, so
# an include line always states which component it crosses into.
target_include_directories(exchange_flags INTERFACE "${PROJECT_SOURCE_DIR}")

target_compile_options(exchange_flags INTERFACE
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    # The book destroys orders through unique_ptr<Order>; a missing virtual
    # destructor must be a build failure, not a code-review catch.
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    # Silent narrowing of a quantity or a price is a correctness bug in an
    # exchange, so every conversion is made to be written down deliberately.
    -Wconversion
    -Wsign-conversion
    -Wdouble-promotion
    -Wshadow
    -Wold-style-cast
    -Wcast-qual
    -Wcast-align
    -Wnull-dereference
    -Wformat=2
    -Wundef
    -Wimplicit-fallthrough)

find_package(Threads REQUIRED)
target_link_libraries(exchange_flags INTERFACE Threads::Threads)

# ---------------------------------------------------------------------------
# Sanitizers
#
# Carried on the same INTERFACE target so instrumentation reaches compile and
# link steps together -- a sanitized object linked without -fsanitize produces
# undefined symbols, which is the usual way this is got wrong.
# ---------------------------------------------------------------------------
if(NOT EXCHANGE_SANITIZER STREQUAL "none")
    message(STATUS "Sanitizer enabled: ${EXCHANGE_SANITIZER}")
    target_compile_options(exchange_flags INTERFACE
        -fsanitize=${EXCHANGE_SANITIZER}
        -fno-omit-frame-pointer
        -fno-optimize-sibling-calls
        -g)
    target_link_options(exchange_flags INTERFACE
        -fsanitize=${EXCHANGE_SANITIZER})

    if(EXCHANGE_SANITIZER STREQUAL "undefined")
        # Without this UBSan prints and continues, so a test can "pass" while
        # having executed undefined behaviour.
        target_compile_options(exchange_flags INTERFACE
            -fno-sanitize-recover=all)
    endif()

    if(EXCHANGE_SANITIZER STREQUAL "thread")
        # ThreadSanitizer reserves fixed regions of the address space for its
        # shadow memory. Kernels from 6.x ship vm.mmap_rnd_bits=32, whose ASLR
        # range overlaps that reservation, and TSan aborts at startup with
        # "unexpected memory mapping" before main() runs.
        #
        # The usual fix is `sysctl vm.mmap_rnd_bits=28`, which is a privileged
        # global change to the host. Disabling randomisation for this process
        # alone is narrower and needs no privileges, and randomised mappings
        # are of no value to a race detector anyway.
        find_program(EXCHANGE_SETARCH setarch REQUIRED)
        set(EXCHANGE_TEST_LAUNCHER
            "${EXCHANGE_SETARCH}" "${CMAKE_SYSTEM_PROCESSOR}" "-R"
            CACHE INTERNAL "wrapper applied to test binaries")
        message(STATUS "  tsan launch: setarch ${CMAKE_SYSTEM_PROCESSOR} -R")
    endif()
endif()

# ---------------------------------------------------------------------------
# Components
# ---------------------------------------------------------------------------
# concurrent first: core now depends on it for the engine command queue, and a
# namespaced target must be defined before the directory that links it.
add_subdirectory(concurrent)
add_subdirectory(core)
add_subdirectory(net)
add_subdirectory(store)
add_subdirectory(analytics)

if(EXCHANGE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(EXCHANGE_BUILD_BENCH)
    add_subdirectory(bench)
endif()

# ---------------------------------------------------------------------------
message(STATUS "exchange ${PROJECT_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  standard   : C++${CMAKE_CXX_STANDARD} (extensions off)")
message(STATUS "  sanitizer  : ${EXCHANGE_SANITIZER}")
message(STATUS "  tests      : ${EXCHANGE_BUILD_TESTS}")
