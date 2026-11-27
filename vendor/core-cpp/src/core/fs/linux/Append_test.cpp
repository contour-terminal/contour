// SPDX-License-Identifier: Apache-2.0
//
// Linux's append where RWF_APPEND is refused: it writes at the end through the descriptor's own
// handle, so a file that is read-only by now (chmod after open) still takes the append, and the
// handle's integrity flags stay what they were.
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/linux/Append.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace
{

std::span<std::byte const> bytesOf(std::string const& text)
{
    return std::as_bytes(std::span { text });
}

} // namespace

TEST_CASE("linux append: without RWF_APPEND, the end is written through the handle itself", "[fs][native]")
{
    auto const directory = core::testing::ScopedTempDir { "core-cpp-fs-append" };
    auto const path = directory.path() / "f";
    auto const file = core::fs::detail::UniqueHandle { ::open(
        path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_DSYNC | O_CLOEXEC, 0644) };
    REQUIRE(file.get() >= 0);
    REQUIRE(::write(file.get(), "abc", 3) == 3);
    // Read-only from here on: a reopen for writing would be refused.
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_read | std::filesystem::perms::group_read
                                     | std::filesystem::perms::others_read);
    auto lock = std::mutex {};

    auto const first = std::string { "de" };
    auto const second = std::string { "f!" };
    CHECK(core::fs::detail::appendAtEnd(file.get(), lock, bytesOf(first)) == 2U);
    CHECK(core::fs::detail::appendAtEnd(file.get(), lock, bytesOf(second)) == 2U);

    auto text = std::array<char, 16> {};
    auto const length = ::pread(file.get(), text.data(), text.size(), 0);
    CHECK(std::string(text.data(), static_cast<std::size_t>(length)) == "abcdef!");
    CHECK((::fcntl(file.get(), F_GETFL) & O_DSYNC) == O_DSYNC);
}
