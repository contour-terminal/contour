// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>
#include <vector>

#ifdef _WIN32
    #include <malloc.h>
#endif

namespace crispy
{

/// STL-compatible allocator that returns 64-byte aligned memory.
/// 64 bytes = one cache line = sufficient for SSE (16B), AVX2 (32B), and AVX-512 (64B).
template <typename T>
class AlignedAllocator
{
  public:
    // NOLINTNEXTLINE(readability-identifier-naming): standard iterator/container trait spelling.
    using value_type = T;

    static constexpr std::size_t Alignment = 64;

    constexpr AlignedAllocator() noexcept = default;

    template <typename U>
    constexpr AlignedAllocator(AlignedAllocator<U> const&) noexcept
    {
    }

    [[nodiscard]] T* allocate(std::size_t n)
    {
        if (n == 0)
            return nullptr;

        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
            throw std::bad_alloc();

        auto const byteCount = n * sizeof(T);
        if (byteCount > std::numeric_limits<std::size_t>::max() - (Alignment - 1))
            throw std::bad_alloc(); // rounding up would wrap
        // Round up to a multiple of Alignment (keeps the block size a whole number of cache lines).
        auto const alignedSize = (byteCount + Alignment - 1) & ~(Alignment - 1);

#ifdef _WIN32
        auto* ptr = static_cast<T*>(_aligned_malloc(alignedSize, Alignment));
#else
        // posix_memalign is available on every POSIX system (std::aligned_alloc is not, e.g. older macOS).
        void* raw = nullptr;
        if (posix_memalign(&raw, Alignment, alignedSize) != 0)
            raw = nullptr;
        auto* ptr = static_cast<T*>(raw);
#endif
        if (!ptr)
            throw std::bad_alloc();
        return ptr;
    }

    void deallocate(T* p, [[maybe_unused]] std::size_t n) noexcept
    {
        if (!p)
            return;
#ifdef _WIN32
        _aligned_free(p);
#else
        std::free(p);
#endif
    }

    template <typename U>
    [[nodiscard]] constexpr bool operator==(AlignedAllocator<U> const&) const noexcept
    {
        return true;
    }

    template <typename U>
    [[nodiscard]] constexpr bool operator!=(AlignedAllocator<U> const&) const noexcept
    {
        return false;
    }
};

/// Convenience alias: std::vector with 64-byte cache-line aligned allocation.
template <typename T>
using AlignedVector = std::vector<T, AlignedAllocator<T>>;

} // namespace crispy
