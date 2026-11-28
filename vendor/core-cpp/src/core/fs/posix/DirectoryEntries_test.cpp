// SPDX-License-Identifier: Apache-2.0
//
// What a POSIX directory entry is, decided by entryOf over a real directory: d_type where the file
// system reports one, and a stat of the name that never follows a link where it does not (DT_UNKNOWN,
// which ext4, tmpfs and btrfs never report, so the streams over them never reach it).
#include <core/fs/Descriptor.hpp>
#include <core/fs/DescriptorTestSupport.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/posix/DirectoryRead.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <optional>

#include <dirent.h>
#include <fcntl.h>

using core::fs::DescriptorType;
using core::fs::FsErrorCode;
using core::fs::detail::entryOf;
using core::fs::detail::UniqueHandle;
using core::fs::test::outcomeOf;

namespace
{

/// @return What entryOf says @p name in @p directory is, when the file system reports @p entryType.
std::optional<DescriptorType> typeOf(UniqueHandle const& directory, char const* name, unsigned char entryType)
{
    auto const entry = entryOf(directory.get(), name, entryType);
    REQUIRE(entry.has_value());
    REQUIRE(entry->has_value());
    CHECK((*entry)->name == name);
    return (*entry)->type;
}

} // namespace

TEST_CASE("posix entries: DT_UNKNOWN is stat'ed, never following a link", "[fs][native][directory]")
{
    auto const temp = core::testing::ScopedTempDir { "core-cpp-fs-entries" };
    std::ofstream { temp.path() / "file" } << "x";
    std::filesystem::create_directory(temp.path() / "dir");
    std::filesystem::create_directory_symlink("dir", temp.path() / "link-to-dir");
    std::filesystem::create_symlink("file", temp.path() / "link-to-file");
    auto const directory = UniqueHandle { ::open(temp.path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC) };
    REQUIRE(directory.get() >= 0);

    CHECK(typeOf(directory, "file", DT_UNKNOWN) == DescriptorType::RegularFile);
    CHECK(typeOf(directory, "dir", DT_UNKNOWN) == DescriptorType::Directory);
    CHECK(typeOf(directory, "link-to-dir", DT_UNKNOWN) == DescriptorType::SymbolicLink);
    CHECK(typeOf(directory, "link-to-file", DT_UNKNOWN) == DescriptorType::SymbolicLink);
    // Gone by the time it is stat'ed: reported, but as what it can no longer be said to be.
    CHECK(typeOf(directory, "gone", DT_UNKNOWN) == DescriptorType::Unknown);
    // A d_type that is known is taken as it is, without a stat.
    CHECK(typeOf(directory, "gone", DT_REG) == DescriptorType::RegularFile);
    CHECK(typeOf(directory, "file", DT_LNK) == DescriptorType::SymbolicLink);
}

TEST_CASE("posix entries: . and .. are no entries, and a name no file system gives is refused",
          "[fs][native][directory]")
{
    CHECK(entryOf(-1, ".", DT_DIR) == std::nullopt);
    CHECK(entryOf(-1, "..", DT_DIR) == std::nullopt);
    auto const nonUtf8 = entryOf(-1, "bad-\xff", DT_REG);
    REQUIRE(nonUtf8.has_value());
    CHECK(outcomeOf(*nonUtf8) == FsErrorCode::IllegalByteSequence);
    for (auto const* name: { "", "a/b", "/" })
    {
        CAPTURE(name);
        auto const entry = entryOf(-1, name, DT_REG);
        REQUIRE(entry.has_value());
        CHECK(outcomeOf(*entry) == FsErrorCode::Io);
    }
}
