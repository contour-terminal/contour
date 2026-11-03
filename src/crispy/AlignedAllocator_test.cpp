// SPDX-License-Identifier: Apache-2.0
#include <crispy/AlignedAllocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <numeric>
#include <ranges>

namespace
{
struct alignas(16) Wide
{
    std::array<std::byte, 48> bytes;
};

template <typename T>
void checkAlignedAllocations()
{
    auto allocator = crispy::AlignedAllocator<T> {};
    for (auto const count: std::array<std::size_t, 6> { 1, 2, 7, 64, 100, 4097 })
    {
        auto* const ptr = allocator.allocate(count);
        REQUIRE(ptr != nullptr);
        CHECK(reinterpret_cast<std::uintptr_t>(ptr) % crispy::AlignedAllocator<T>::Alignment == 0);
        allocator.deallocate(ptr, count);
    }
}
} // namespace

TEST_CASE("AlignedAllocator.allocate_is_cache_line_aligned")
{
    checkAlignedAllocations<char>();
    checkAlignedAllocations<std::uint32_t>();
    checkAlignedAllocations<Wide>();
}

TEST_CASE("AlignedAllocator.zero_elements_yield_nullptr")
{
    auto allocator = crispy::AlignedAllocator<int> {};
    CHECK(allocator.allocate(0) == nullptr);
    allocator.deallocate(nullptr, 0); // Must be a no-op.
}

TEST_CASE("AlignedAllocator.overflowing_request_throws")
{
    auto allocator = crispy::AlignedAllocator<std::uint64_t> {};
    CHECK_THROWS_AS(allocator.allocate(std::numeric_limits<std::size_t>::max()), std::bad_alloc);

    // A byte count whose round-up to the alignment would wrap to a tiny size.
    auto bytes = crispy::AlignedAllocator<char> {};
    CHECK_THROWS_AS(bytes.allocate(std::numeric_limits<std::size_t>::max() - 1), std::bad_alloc);
    CHECK_THROWS_AS(bytes.allocate(std::numeric_limits<std::size_t>::max()), std::bad_alloc);
}

TEST_CASE("AlignedAllocator.vector_storage_is_aligned")
{
    auto values = crispy::AlignedVector<int>(1000);
    std::ranges::copy(std::views::iota(0, 1000), values.begin());
    CHECK(reinterpret_cast<std::uintptr_t>(values.data()) % crispy::AlignedAllocator<int>::Alignment == 0);
    CHECK(std::accumulate(values.begin(), values.end(), 0) == 499500);
}
