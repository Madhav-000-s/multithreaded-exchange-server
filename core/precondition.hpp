#pragma once

#include <cassert>

/// States a precondition to the reader, the test build, and the optimiser.
///
/// `assert` alone addresses only the first two: with NDEBUG it vanishes
/// entirely, so an optimising build is left unable to rule out the very paths
/// the precondition forbids. That matters under -Wnull-dereference, where GCC
/// reports a "potential null dereference" on a branch that the caller is
/// contractually forbidden from taking -- correct analysis of a path that
/// cannot occur.
///
/// Violating a documented precondition is already undefined behaviour, so
/// telling the optimiser as much states nothing new; it only makes the
/// existing contract visible to the compiler. Debug builds still trap, which
/// is where a violation actually gets caught. C++23 spells this `[[assume]]`.
///
/// Use only where the condition is a genuine caller obligation. It is not an
/// error-handling mechanism, and it must never guard input from the network.
#ifdef NDEBUG
#if defined(__GNUC__) || defined(__clang__)
#define EXCHANGE_PRECONDITION(condition)                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            __builtin_unreachable();                                                               \
        }                                                                                          \
    } while (false)
#else
#define EXCHANGE_PRECONDITION(condition) ((void)0)
#endif
#else
#define EXCHANGE_PRECONDITION(condition) assert(condition)
#endif
