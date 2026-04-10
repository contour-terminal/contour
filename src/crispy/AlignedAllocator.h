// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>
#include <vector>

namespace crispy
{

/// STL-compatible allocator that returns 64-byte aligned memory.
/// 64 bytes = one cache line = sufficient for SSE (16B), AVX2 (32B), and AVX-512 (64B).
template <typename T>
class aligned_allocator
{
  public:
    using value_type = T;

    static constexpr std::size_t Alignment = 64;

    constexpr aligned_allocator() noexcept = default;

    template <typename U>
    constexpr aligned_allocator(aligned_allocator<U> const&) noexcept
    {
    }

    [[nodiscard]] T* allocate(std::size_t n)
    {
        if (n == 0)
            return nullptr;

        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
            throw std::bad_alloc();

        auto const byteCount = n * sizeof(T);
        // Round up to a multiple of Alignment (required by aligned_alloc).
        auto const alignedSize = (byteCount + Alignment - 1) & ~(Alignment - 1);

#if defined(_MSC_VER)
        auto* ptr = static_cast<T*>(_aligned_malloc(alignedSize, Alignment));
#else
        auto* ptr = static_cast<T*>(std::aligned_alloc(Alignment, alignedSize));
#endif
        if (!ptr)
            throw std::bad_alloc();
        return ptr;
    }

    void deallocate(T* p, [[maybe_unused]] std::size_t n) noexcept
    {
        if (!p)
            return;
#if defined(_MSC_VER)
        _aligned_free(p);
#else
        std::free(p);
#endif
    }

    template <typename U>
    [[nodiscard]] constexpr bool operator==(aligned_allocator<U> const&) const noexcept
    {
        return true;
    }

    template <typename U>
    [[nodiscard]] constexpr bool operator!=(aligned_allocator<U> const&) const noexcept
    {
        return false;
    }
};

/// Convenience alias: std::vector with 64-byte cache-line aligned allocation.
template <typename T>
using aligned_vector = std::vector<T, aligned_allocator<T>>;

} // namespace crispy
