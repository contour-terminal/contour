// SPDX-License-Identifier: Apache-2.0
//
// The open made again after a lease answered EWOULDBLOCK: it opens only a regular file blocking. A
// FIFO renamed over the name between the non-blocking open and this one is what the case below
// stands for, deterministically: the name is a FIFO when the retry looks, and the retry answers
// WouldBlock at once rather than waiting for a writer that never comes. Then the whole way, through
// openAt: a real lease the open must wait for, released by a thread that sees it being broken.
#include <core/fs/Descriptor.hpp>
#include <core/fs/DescriptorTestSupport.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/posix/PosixSystem.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <sys/stat.h>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <thread>
#include <tuple>

#include <fcntl.h>
#include <unistd.h>

using core::fs::DescriptorFlags;
using core::fs::FsErrorCode;
using core::fs::OpenFlags;
using core::fs::SymlinkPolicy;
using core::fs::detail::openAfterWouldBlock;
using core::fs::detail::UniqueHandle;
using core::fs::test::DescriptorFixture;

TEST_CASE("lease retry: only a regular file is opened again, waiting", "[fs][native]")
{
    auto const directory = core::testing::ScopedTempDir { "core-cpp-fs-lease" };
    {
        auto file = std::ofstream { directory.path() / "f" };
        file << "abc";
    }
    REQUIRE(::mkfifo((directory.path() / "fifo").c_str(), 0600) == 0);
    auto const held = UniqueHandle { ::open(directory.path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC) };
    REQUIRE(held.get() >= 0);

    auto const fifo = openAfterWouldBlock(held.get(), "fifo", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    REQUIRE_FALSE(fifo.has_value());
    CHECK(fifo.error().code == FsErrorCode::WouldBlock);

    auto const file = openAfterWouldBlock(held.get(), "f", O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    REQUIRE(file.has_value());
    CHECK((::fcntl(file->get(), F_GETFL) & (O_ACCMODE | O_NONBLOCK)) == O_RDWR);
    auto text = std::array<char, 8> {};
    CHECK(::pread(file->get(), text.data(), text.size(), 0) == 3);
}

TEST_CASE("lease retry: openAt waits for a lease rather than answering WouldBlock", "[fs][native]")
{
    auto fixture = DescriptorFixture::createNative();
    {
        auto file = std::ofstream { fixture.hostDirectory() / "leased" };
        file << "abc";
    }
    // A read lease, broken by any open for writing, this process's own included. The break is
    // signalled with SIGURG, whose default is to be ignored, rather than SIGIO, which would end the
    // process; the kernel makes this process the lease's owner on F_SETLEASE.
    auto const holder =
        UniqueHandle { ::open((fixture.hostDirectory() / "leased").c_str(), O_RDONLY | O_CLOEXEC) };
    REQUIRE(holder.get() >= 0);
    REQUIRE(::fcntl(holder.get(), F_SETSIG, SIGURG) == 0);
    if (::fcntl(holder.get(), F_SETLEASE, F_RDLCK) != 0)
        SKIP("this file system grants no lease");

    // Releases the lease once its break has begun (F_GETLEASE then reports the lease it is being
    // broken down to), within a bound, so a regression fails rather than hangs.
    auto broken = std::atomic<bool> { false };
    auto releaser = std::thread { [&] {
        for ([[maybe_unused]] auto const attempt: std::views::iota(0, 10'000))
        {
            if (::fcntl(holder.get(), F_GETLEASE) == F_UNLCK)
            {
                broken.store(true);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds { 1 });
        }
        std::ignore = ::fcntl(holder.get(), F_SETLEASE, F_UNLCK);
    } };

    auto const opened = fixture.run(
        fixture.root().openAt(SymlinkPolicy::NoFollow, "leased", OpenFlags::None, DescriptorFlags::Write));
    releaser.join();
    CHECK(broken.load());
    REQUIRE(opened.has_value());
    CHECK(fixture.run((*opened)->write(core::fs::test::bytesOf("x"), 0)).value() == 1);
}
