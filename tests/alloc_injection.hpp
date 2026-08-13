#pragma once

#include <cstddef>

namespace exchange::test {

/// Makes the Nth allocation inside a scope fail.
///
/// The exception-safety claim is that a throw *anywhere* in submit() leaves
/// the book untouched. Verifying that by hand-placing throws would only test
/// the places someone thought of, which are exactly the places already
/// handled. Failing allocations by ordinal instead enumerates every throw site
/// the operation actually has -- including the ones inside libstdc++, in
/// std::map node construction and hash-table rehashing, that no hand-written
/// injection point would reach.
///
/// Implemented by replacing the global operator new, so the counter is armed
/// for the narrowest possible window: constructed immediately before the call
/// under test and destroyed immediately after, including during stack
/// unwinding. Anything the test framework allocates outside that window is
/// neither counted nor failed.
///
/// Not thread-safe, and deliberately so -- the counters are plain globals.
/// Arming this from more than one thread at once would be meaningless anyway,
/// since allocation ordinals are only reproducible in a serial execution.
class ThrowOnAllocation {
public:
    /// @param nth zero-based ordinal of the allocation to fail. 0 fails the
    ///        very first allocation attempted inside the scope.
    explicit ThrowOnAllocation(std::size_t nth) noexcept;

    ThrowOnAllocation(const ThrowOnAllocation&) = delete;
    ThrowOnAllocation& operator=(const ThrowOnAllocation&) = delete;
    ThrowOnAllocation(ThrowOnAllocation&&) = delete;
    ThrowOnAllocation& operator=(ThrowOnAllocation&&) = delete;

    ~ThrowOnAllocation();

    /// Allocations seen while armed, whether or not one was failed. Used to
    /// discover how many throw sites an operation has, so the test can stop
    /// once it has covered all of them.
    [[nodiscard]] static std::size_t observed() noexcept;

    /// True if this scope actually failed an allocation, i.e. `nth` was within
    /// range. Lets a test tell "survived because it is exception-safe" apart
    /// from "survived because nothing was injected".
    [[nodiscard]] static bool fired() noexcept;
};

} // namespace exchange::test
