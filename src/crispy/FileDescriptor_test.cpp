// SPDX-License-Identifier: Apache-2.0
#include <crispy/FileDescriptor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <utility>

#include <unistd.h>

using crispy::FileDescriptor;

namespace
{

/// Opens a pipe and hands back both ends as owned descriptors.
std::pair<FileDescriptor, FileDescriptor> makePipe()
{
    auto fds = std::array<int, 2> {};
    REQUIRE(::pipe(fds.data()) == 0);
    return { FileDescriptor::fromNative(fds[0]), FileDescriptor::fromNative(fds[1]) };
}

} // namespace

TEST_CASE("FileDescriptor.close.is_noexcept", "[crispy][FileDescriptor]")
{
    // The destructor and the move assignment are noexcept and both close; a close() that may throw
    // would terminate the process from either of them.
    STATIC_REQUIRE(noexcept(std::declval<FileDescriptor&>().close()));
}

TEST_CASE("FileDescriptor.close.failure_does_not_throw", "[crispy][FileDescriptor]")
{
    auto [reader, writer] = makePipe();

    // Close the descriptor behind the handle's back, so that its own close() fails with EBADF.
    REQUIRE(::close(reader.get()) == 0);

    REQUIRE_NOTHROW(reader.close());
    CHECK(reader.isClosed());
}

TEST_CASE("FileDescriptor.close.failure_on_move_assignment", "[crispy][FileDescriptor]")
{
    auto [reader, writer] = makePipe();
    auto [otherReader, otherWriter] = makePipe();

    REQUIRE(::close(reader.get()) == 0);

    // Move-assigning over the handle closes its (already gone) descriptor first.
    reader = std::move(otherReader);
    CHECK(reader.isOpen());
}
