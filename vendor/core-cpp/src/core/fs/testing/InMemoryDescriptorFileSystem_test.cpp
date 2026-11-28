// SPDX-License-Identifier: Apache-2.0
//
// What the in-memory model owes the cases built on it beyond the contract: that it suspends where
// the native file system does, and that its setup helpers build what they say.
#include <core/async/testing/ManualExecutor.hpp>
#include <core/fs/Descriptor.hpp>
#include <core/fs/DescriptorTestSupport.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/testing/InMemoryDescriptorFileSystem.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <utility>

using core::async::testing::ManualExecutor;
using core::fs::DescriptorFlags;
using core::fs::DescriptorType;
using core::fs::FsErrorCode;
using core::fs::OpenFlags;
using core::fs::SymlinkPolicy;
using core::fs::test::outcomeOf;
using core::fs::test::textOf;
using core::fs::testing::InMemoryDescriptorFileSystem;
using core::platform::ManualWallClock;

TEST_CASE("in-memory: an operation runs on blocking and completes on home", "[fs][inmemory]")
{
    auto home = ManualExecutor {};
    auto blocking = ManualExecutor {};
    auto const clock = ManualWallClock {};
    auto fileSystem = InMemoryDescriptorFileSystem { home, blocking, clock };
    fileSystem.addFile("f", "parked");

    auto open = fileSystem.openPreopen(std::filesystem::path { "/" }, DescriptorFlags::Read);
    open.handle().resume();
    CHECK_FALSE(open.done());
    CHECK(blocking.pending() == 1);
    blocking.drain();
    CHECK_FALSE(open.done());
    CHECK(home.pending() == 1);
    home.drain();
    REQUIRE(open.done());
    auto root = open.result();
    REQUIRE(root.has_value());

    auto stat = (*root)->statAt(SymlinkPolicy::NoFollow, "f");
    stat.handle().resume();
    CHECK_FALSE(stat.done());
    CHECK(blocking.pending() == 1);
    blocking.drain();
    CHECK_FALSE(stat.done());
    home.drain();
    REQUIRE(stat.done());
    CHECK(stat.result()->size == 6);
}

TEST_CASE("in-memory: the setup helpers build directories, files and links", "[fs][inmemory]")
{
    auto home = ManualExecutor {};
    auto blocking = ManualExecutor {};
    auto const clock = ManualWallClock {};
    auto fileSystem = InMemoryDescriptorFileSystem { home, blocking, clock };
    fileSystem.addDirectory("a/b");
    fileSystem.addFile("a/b/file", "content");
    fileSystem.addSymlink("a/link", "b/file");
    CHECK_THROWS_AS(fileSystem.addFile("a/b/file", "again"), std::invalid_argument);
    CHECK_THROWS_AS(fileSystem.addDirectory("a/b/file/below"), std::invalid_argument);

    auto const drive = [&](auto task) {
        task.handle().resume();
        while (!task.done() && blocking.drain() + home.drain() != 0)
        {
        }
        REQUIRE(task.done());
        return task.result();
    };
    auto preopen = drive(fileSystem.openPreopen(std::filesystem::path { "a" }, DescriptorFlags::Read));
    REQUIRE(preopen.has_value());
    auto& dir = **preopen;
    CHECK(drive(dir.statAt(SymlinkPolicy::NoFollow, "b"))->type == DescriptorType::Directory);
    CHECK(drive(dir.statAt(SymlinkPolicy::NoFollow, "link"))->type == DescriptorType::SymbolicLink);
    auto file = drive(dir.openAt(SymlinkPolicy::Follow, "link", OpenFlags::None, DescriptorFlags::Read));
    REQUIRE(file.has_value());
    CHECK(textOf(drive((*file)->read(64, 0))->bytes) == "content");

    CHECK(
        outcomeOf(drive(fileSystem.openPreopen(std::filesystem::path { "a/b/file" }, DescriptorFlags::Read)))
        == FsErrorCode::NotDirectory);
    CHECK(outcomeOf(drive(fileSystem.openPreopen(std::filesystem::path { "missing" }, DescriptorFlags::Read)))
          == FsErrorCode::NoEntry);
    CHECK(outcomeOf(drive(fileSystem.openPreopen(std::filesystem::path { "a" },
                                                 DescriptorFlags::Read | DescriptorFlags::Write)))
          == FsErrorCode::IsDirectory);
}

TEST_CASE("in-memory: a guest-sized write or size is refused, never allocated", "[fs][inmemory]")
{
    auto fixture = core::fs::test::DescriptorFixture::create(core::fs::test::FixtureKind::InMemory);
    fixture.createFile("f", "content");
    auto file = fixture.run(fixture.root().openAt(
        SymlinkPolicy::NoFollow, "f", OpenFlags::None, DescriptorFlags::Read | DescriptorFlags::Write));
    REQUIRE(file.has_value());
    auto const tooLarge = [&](auto task) {
        return outcomeOf(fixture.run(std::move(task))) == FsErrorCode::FileTooLarge;
    };

    CHECK(tooLarge(
        (*file)->write(core::fs::test::bytesOf("wraps"), std::numeric_limits<std::uint64_t>::max() - 2)));
    CHECK(tooLarge((*file)->write(core::fs::test::bytesOf("far"), std::uint64_t { 1 } << 40U)));
    CHECK(tooLarge((*file)->setSize(std::uint64_t { 1 } << 40U)));
    CHECK(tooLarge((*file)->setSize(std::numeric_limits<std::uint64_t>::max())));
    CHECK(fixture.run((*file)->stat())->size == 7);
}
