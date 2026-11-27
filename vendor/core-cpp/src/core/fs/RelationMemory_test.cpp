// SPDX-License-Identifier: Apache-2.0
//
// What one IDescriptorFileSystem::relation holds while it walks: the parents it walked, never what
// maxAncestorDepth would allow. This binary replaces the global allocation functions to count the
// bytes alive, which is why it is a binary of its own: a replacement reaches every test linked
// beside it.
#include <core/fs/Descriptor.hpp>
#include <core/fs/DescriptorTestSupport.hpp>
#include <core/fs/NativeFileSystem.hpp>
#include <core/testing/ReplacedGlobalAllocation.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <new>
#include <string>

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

TEST_CASE("relation holds the parents it walks, not the cap", "[fs][relation]")
{
    using core::fs::test::DescriptorFixture;
    using core::fs::test::FixtureKind;
    /// A cap whose reservation would show: 24 MiB of identities, where a walk of a few dozen
    /// parents holds a few KiB.
    constexpr auto MaxAncestorDepth = std::size_t { 1 } << 20U;
    constexpr auto Budget = std::size_t { 64 } << 10U;

    auto fixture =
        DescriptorFixture::create(GENERATE(FixtureKind::InMemory, FixtureKind::Native),
                                  core::fs::NativeFileSystemOptions { .maxAncestorDepth = MaxAncestorDepth });
    auto path = std::string {};
    for (auto const name: { "d1", "d2", "d3", "d4", "d5", "d6", "d7", "d8", "d9" })
    {
        path += (path.empty() ? "" : "/") + std::string { name };
        REQUIRE(fixture.run(fixture.root().createDirectoryAt(path, core::fs::CreationAccess::Default))
                    .has_value());
    }
    auto deep = fixture.run(fixture.root().openAt(core::fs::SymlinkPolicy::NoFollow,
                                                  path,
                                                  core::fs::OpenFlags::Directory,
                                                  core::fs::DescriptorFlags::Read));
    REQUIRE(deep.has_value());

    auto const before = liveBytes.load();
    peakBytes.store(before);
    auto const relation = fixture.run(fixture.fileSystem().relation(fixture.root(), **deep));
    auto const held = peakBytes.load() - before;
    REQUIRE(relation.has_value());
    CHECK(*relation == core::fs::Relation::FirstContainsSecond);
    CAPTURE(held);
    CHECK(held < Budget);
}
