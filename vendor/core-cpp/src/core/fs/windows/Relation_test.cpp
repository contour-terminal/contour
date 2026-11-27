// SPDX-License-Identifier: Apache-2.0
//
// What the Windows relation trusts an identity from: the file systems of StableFileIdRows, never a
// zero id, and nothing on a FAT volume, whose ids move with a directory entry.
#include <core/fs/Descriptor.hpp>
#include <core/fs/DescriptorTestSupport.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/windows/StableFileIdRows.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

#include <windows.h>

#include "VirtualVolumeTestSupport.hpp"

using core::fs::DescriptorFlags;
using core::fs::FsErrorCode;
using core::fs::detail::FileIdStability;
using core::fs::detail::fileIdStabilityOf;
using core::fs::detail::stableIdentityOf;
using core::fs::test::DescriptorFixture;
using core::fs::test::requireElevationOrSkip;
using core::fs::test::VirtualVolume;

namespace
{

/// A file id of 16 bytes, each @p fill.
[[nodiscard]] std::array<std::byte, 16> fileIdOf(std::byte fill)
{
    auto id = std::array<std::byte, 16> {};
    id.fill(fill);
    return id;
}

} // namespace

TEST_CASE("windows: only the file systems of StableFileIdRows give identities", "[fs][relation]")
{
    struct Row
    {
        std::wstring_view name;
        FileIdStability stability = FileIdStability::Unstable;
    };
    auto const row = GENERATE(Row { L"NTFS", FileIdStability::Stable },
                              Row { L"ReFS", FileIdStability::Stable },
                              Row { L"FAT", FileIdStability::Unstable },
                              Row { L"FAT32", FileIdStability::Unstable },
                              Row { L"exFAT", FileIdStability::Unstable },
                              Row { L"CSVFS", FileIdStability::Unstable },
                              Row { L"ntfs", FileIdStability::Unstable },
                              Row { L"", FileIdStability::Unstable });
    CHECK(fileIdStabilityOf(row.name) == row.stability);
    auto const id = fileIdOf(std::byte { 7 });
    auto const identity = stableIdentityOf(42, id, row.name);
    CHECK(identity.has_value() == (row.stability == FileIdStability::Stable));
}

TEST_CASE("windows: a zero file id is no identity", "[fs][relation]")
{
    auto const zero = fileIdOf(std::byte { 0 });
    auto const identity = stableIdentityOf(42, zero, L"NTFS");
    REQUIRE_FALSE(identity.has_value());
    CHECK(identity.error().code == FsErrorCode::Unsupported);
    // Either half alone is an id.
    auto low = zero;
    low.front() = std::byte { 1 };
    CHECK(stableIdentityOf(42, low, L"NTFS").has_value());
    auto high = zero;
    high.back() = std::byte { 1 };
    CHECK(stableIdentityOf(42, high, L"NTFS").has_value());
}

TEST_CASE("relation on FAT is Unsupported", "[fs][native][relation][windows-native]")
{
    auto const scratch = core::testing::ScopedTempDir { "core-cpp-fs-fat" };
    auto volume = VirtualVolume { scratch.path(), "fat32" };
    auto const root = volume.attach();
    if (!root)
        requireElevationOrSkip(
            "no FAT volume could be attached here: diskpart needs an administrator, and wine has none");
    std::filesystem::create_directories(*root / "a" / "b");
    auto fixture = DescriptorFixture::createNative();
    auto top = fixture.run(fixture.fileSystem().openPreopen(*root / "a", DescriptorFlags::Read));
    auto inner = fixture.run(fixture.fileSystem().openPreopen(*root / "a" / "b", DescriptorFlags::Read));
    INFO("open a: " << (top ? std::string { "ok" } : top.error().toString()));
    INFO("open a/b: " << (inner ? std::string { "ok" } : inner.error().toString()));
    REQUIRE(top.has_value());
    REQUIRE(inner.has_value());
    auto const relation = fixture.run(fixture.fileSystem().relation(**top, **inner));
    REQUIRE_FALSE(relation.has_value());
    CHECK(relation.error().code == FsErrorCode::Unsupported);
}

TEST_CASE("relation crosses a volume mounted in a folder", "[fs][native][relation][windows-native]")
{
    // An NTFS volume mounted at outer/mnt, with no drive letter: outer contains what is on it, though
    // no path by volume GUID of it passes through outer.
    auto const scratch = core::testing::ScopedTempDir { "core-cpp-fs-mount" };
    std::filesystem::create_directories(scratch.path() / "outer" / "mnt");
    std::filesystem::create_directories(scratch.path() / "beside");
    auto volume = VirtualVolume { scratch.path(), "ntfs" };
    auto const root = volume.attach(scratch.path() / "outer" / "mnt");
    if (!root)
        requireElevationOrSkip(
            "no volume could be mounted in a folder here: diskpart needs an administrator, and "
            "wine has none");
    std::filesystem::create_directories(*root / "key");
    auto fixture = DescriptorFixture::createNative();
    auto const open = [&](std::filesystem::path const& path) {
        auto opened = fixture.run(fixture.fileSystem().openPreopen(path, DescriptorFlags::Read));
        INFO("open " << path.string() << ": " << (opened ? std::string { "ok" } : opened.error().toString()));
        REQUIRE(opened.has_value());
        return std::move(*opened);
    };
    auto const outer = open(scratch.path() / "outer");
    auto const beside = open(scratch.path() / "beside");
    auto const mounted = open(*root);
    auto const key = open(*root / "key");
    auto const relationOf = [&](core::fs::IDescriptor const& first, core::fs::IDescriptor const& second) {
        return fixture.run(fixture.fileSystem().relation(first, second));
    };
    auto const outerKey = relationOf(*outer, *key);
    INFO("outer, key: " << (outerKey ? std::string { "ok" } : outerKey.error().toString()));
    REQUIRE(outerKey.has_value());
    CHECK(*outerKey == core::fs::Relation::FirstContainsSecond);
    auto const outerMounted = relationOf(*outer, *mounted);
    REQUIRE(outerMounted.has_value());
    CHECK(*outerMounted == core::fs::Relation::FirstContainsSecond);
    auto const besideKey = relationOf(*beside, *key);
    REQUIRE(besideKey.has_value());
    CHECK(*besideKey == core::fs::Relation::Unrelated);
}
