// SPDX-License-Identifier: Apache-2.0
//
// What the last owner of a handle does when the close it queues cannot be allocated: it closes the
// handle there and then, rather than ending the process from a destructor. This binary replaces the
// global allocation functions so a case can fail exactly the next one, which is why it is a binary
// of its own: a replacement reaches every test linked beside it.
#include <core/async/testing/ManualExecutor.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/testing/ReplacedGlobalAllocation.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace
{

/// Whether the next allocation on this thread fails.
thread_local bool failNextAllocation = false;

} // namespace

void* core::testing::replacedAllocate(std::size_t size)
{
    if (std::exchange(failNextAllocation, false))
        throw std::bad_alloc {};
    if (auto* const storage = std::malloc(size == 0 ? 1 : size))
        return storage;
    throw std::bad_alloc {};
}

void core::testing::replacedRelease(void* storage) noexcept
{
    std::free(storage);
}

TEST_CASE("an owned handle whose queued close cannot be allocated closes inline", "[fs][native]")
{
    auto closer = core::async::testing::ManualExecutor {};
    auto pipe = std::array<int, 2> {};
    REQUIRE(::pipe(pipe.data()) == 0);
    auto const writeEnd = core::fs::detail::UniqueHandle { pipe[1] };
    {
        auto const handle =
            core::fs::detail::OwnedHandle { core::fs::detail::UniqueHandle { pipe[0] }, closer };
        failNextAllocation = true;
    }
    CHECK_FALSE(failNextAllocation);
    CHECK(closer.pending() == 0);
    errno = 0;
    CHECK(::fcntl(pipe[0], F_GETFD) == -1);
    CHECK(errno == EBADF);
}
