// SPDX-License-Identifier: Apache-2.0
//
// What a directory stream holds while it reads a large directory: one batch, never the listing.
// This binary replaces the global allocation functions to count the bytes alive, which is why it is a
// binary of its own: a replacement reaches every test linked beside it. POSIX, where a directory of
// tens of thousands of entries is cheap to make.
#include <core/fs/Descriptor.hpp>
#include <core/fs/DescriptorTestSupport.hpp>
#include <core/testing/ReplacedGlobalAllocation.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <format>
#include <new>
#include <ranges>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace
{

/// What precedes each allocation: its size, and room to keep the storage after it aligned.
constexpr auto HeaderBytes = std::size_t { alignof(std::max_align_t) };

std::atomic<std::size_t> liveBytes { 0 };
std::atomic<std::size_t> peakBytes { 0 };

} // namespace

void* core::testing::replacedAllocate(std::size_t size)
{
    auto* const block = static_cast<std::byte*>(std::malloc(HeaderBytes + size));
    if (block == nullptr)
        throw std::bad_alloc {};
    std::memcpy(block, &size, sizeof(size));
    auto const live = liveBytes.fetch_add(size) + size;
    auto peak = peakBytes.load();
    while (live > peak && !peakBytes.compare_exchange_weak(peak, live))
    {
    }
    return block + HeaderBytes;
}

void core::testing::replacedRelease(void* storage) noexcept
{
    if (storage == nullptr)
        return;
    auto* const block = static_cast<std::byte*>(storage) - HeaderBytes;
    auto size = std::size_t { 0 };
    std::memcpy(&size, block, sizeof(size));
    liveBytes.fetch_sub(size);
    std::free(block);
}

TEST_CASE("a stream over a large directory holds a batch, not the listing", "[fs][native][directory]")
{
    /// What the stream may hold above what was alive before it was read: a batch's buffer and the
    /// entries of one batch, with room to spare.
    constexpr auto Budget = std::size_t { 1 } << 20U;
    // Derived from the entry's size rather than written down, because that size is the standard
    // library's: a `std::string` is 32 bytes in libstdc++ and 24 in libc++, so a fixed count that
    // outgrew the budget on Linux did not on macOS. Holding the listing takes more than twice the
    // budget in entries alone, before their names, on every library.
    constexpr auto Count = (2 * Budget / sizeof(core::fs::DirectoryEntry)) + 1;
    static_assert(Count * sizeof(core::fs::DirectoryEntry) > 2 * Budget);
    // Bounded, so the run stays capped and every name fits the fixed five-digit pattern below.
    static_assert(Count <= 99'999);

    auto fixture = core::fs::test::DescriptorFixture::createNative();
    for (auto const index: std::views::iota(std::size_t { 0 }, Count))
    {
        auto const path = fixture.hostDirectory() / std::format("entry-{:05}", index);
        auto const made = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        REQUIRE(made >= 0);
        ::close(made);
    }
    auto stream = fixture.run(fixture.root().readDirectory());
    REQUIRE(stream.has_value());

    auto const before = liveBytes.load();
    peakBytes.store(before);
    auto entries = std::size_t { 0 };
    while (entries <= Count)
    {
        auto const next = fixture.run((*stream)->next());
        REQUIRE(next.has_value());
        if (!*next)
            break;
        ++entries;
    }
    auto const held = peakBytes.load() - before;
    CAPTURE(held);
    CHECK(entries == Count);
    CHECK(held < Budget);
}
