#include "alloc_injection.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>

namespace {

// Plain globals, not atomics: see the thread-safety note in the header.
bool g_armed = false;
std::size_t g_remaining = 0;
std::size_t g_observed = 0;
bool g_fired = false;

/// The one place that decides whether an allocation should fail.
[[nodiscard]] bool shouldFail() noexcept {
    if (!g_armed) {
        return false;
    }
    ++g_observed;
    if (g_remaining == 0) {
        g_fired = true;
        // Disarm on firing. Otherwise every subsequent allocation on the
        // unwind path would also throw, and an exception escaping a
        // destructor during unwinding is std::terminate -- which would
        // report as a crash rather than as the failure being tested.
        g_armed = false;
        return true;
    }
    --g_remaining;
    return false;
}

[[nodiscard]] void* allocate(std::size_t size) {
    if (shouldFail()) {
        throw std::bad_alloc();
    }
    // Zero-sized allocations must still return a distinct pointer.
    void* memory = std::malloc(size == 0 ? 1 : size);
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    return memory;
}

} // namespace

// Replacing the global allocation functions. The whole set is provided
// together: mixing a replaced operator new with a library operator delete is
// undefined, and the sized and array forms are what the containers under test
// actually call.

void* operator new(std::size_t size) {
    return allocate(size);
}

void* operator new[](std::size_t size) {
    return allocate(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (shouldFail()) {
        return nullptr;
    }
    return std::malloc(size == 0 ? 1 : size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    if (shouldFail()) {
        return nullptr;
    }
    return std::malloc(size == 0 ? 1 : size);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete(void* memory, const std::nothrow_t&) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept {
    std::free(memory);
}

namespace exchange::test {

ThrowOnAllocation::ThrowOnAllocation(std::size_t nth) noexcept {
    g_remaining = nth;
    g_observed = 0;
    g_fired = false;
    g_armed = true;
}

ThrowOnAllocation::~ThrowOnAllocation() {
    g_armed = false;
}

std::size_t ThrowOnAllocation::observed() noexcept {
    return g_observed;
}

bool ThrowOnAllocation::fired() noexcept {
    return g_fired;
}

} // namespace exchange::test
