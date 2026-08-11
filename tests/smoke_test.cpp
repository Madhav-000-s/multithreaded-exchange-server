// Phase 0 smoke test.
//
// Proves the harness before anything depends on it: that GoogleTest is
// fetched, compiled, linked and discovered by CTest, and that the toolchain
// actually selected the language level the build claims. A test suite whose
// plumbing is first exercised by a real failing test cannot distinguish a bug
// in the code from a bug in the harness.

#include <gtest/gtest.h>

namespace {

TEST(BuildHarness, GoogleTestIsWiredAndDiscovered) {
    SUCCEED() << "reaching this point proves fetch, compile, link and discovery";
}

TEST(BuildHarness, ToolchainSelectedCxx20) {
    // Compile-time: the standard is wrong before the test ever runs.
    static_assert(__cplusplus >= 202002L, "the build must select C++20 or later");

    // Run-time: guards against a stale object surviving a flag change.
    EXPECT_GE(__cplusplus, 202002L);
}

TEST(BuildHarness, CompilerExtensionsAreDisabled) {
    // CMAKE_CXX_EXTENSIONS OFF means -std=c++20, not -std=gnu++20. GCC and
    // Clang both define __STRICT_ANSI__ only in the conforming mode, so this
    // catches a silent regression to the extended dialect.
    static_assert(sizeof(int) > 0); // keep the body non-empty under -Wall
#ifdef __STRICT_ANSI__
    SUCCEED();
#else
    FAIL() << "compiler extensions are enabled; expected -std=c++20";
#endif
}

} // namespace
