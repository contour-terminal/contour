// SPDX-License-Identifier: Apache-2.0
#include <vtpty/UnixUtils.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <sys/wait.h>

#include <algorithm>
#include <array>

#include <fcntl.h>
#include <unistd.h>

using vtpty::util::DescriptorSweep;

namespace
{

/// The descriptor a child keeps, below the cut, and those it opens above it.
constexpr auto KeptFd = 50;
constexpr auto Cut = 55;
constexpr auto ClosedFds = std::array { 60, 70 };

/// One above the highest descriptor the bounded sweep tries, when @p sweep is tried first: above every
/// descriptor the child opens for the bounded sweep itself and off Linux; just above the cut for the ways
/// Linux has before it, so that one falling back to the bounded sweep unannounced leaves what is above the
/// cut open, and fails its row.
/// @param sweep The way tried first.
/// @return The limit the child passes.
[[nodiscard]] constexpr int limitFor([[maybe_unused]] DescriptorSweep sweep) noexcept
{
    constexpr auto AboveEveryOpened = 128;
#ifdef __linux__
    if (sweep != DescriptorSweep::Bounded)
        return Cut + 1;
#endif
    return AboveEveryOpened;
}

/// Forks a child that opens descriptors on both sides of the cut and closes those above it with @p sweep
/// tried first, as a child does between fork() and exec().
/// @return How the child exited: 0 when exactly the descriptors above the cut were closed.
int closeInChild(DescriptorSweep sweep)
{
    auto const pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0)
    {
        // Nothing here allocates or throws: this is a child of a process that may have other threads.
        auto const devNull = ::open("/dev/null", O_RDONLY);
        if (devNull < 0)
            ::_exit(2);
        ::dup2(devNull, KeptFd);
        for (auto const fd: ClosedFds)
            ::dup2(devNull, fd);
        vtpty::util::closeDescriptorsAbove(Cut, limitFor(sweep), sweep);
        auto const isOpen = [](int fd) {
            return ::fcntl(fd, F_GETFD) != -1;
        };
        ::_exit(isOpen(KeptFd) && std::ranges::none_of(ClosedFds, isOpen) ? 0 : 1);
    }
    auto status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

} // namespace

TEST_CASE("UnixUtils.closeDescriptorsAbove closes what is above the cut, whichever way is tried first",
          "[process]")
{
    // Where a way is missing -- close_range() before Linux 5.9, /proc/self/fd off Linux -- the next one is
    // tried, so each row forces the ways below it and passes off Linux too. On Linux the limit for the ways
    // before the bounded sweep is just above the cut (limitFor), so a Linux without /proc mounted fails the
    // Listing row: by design, as falling back there unannounced is what the row is to catch.
    auto const sweep = GENERATE(DescriptorSweep::Kernel, DescriptorSweep::Listing, DescriptorSweep::Bounded);
    INFO("first: " << static_cast<int>(sweep));
    CHECK(closeInChild(sweep) == 0);
}
