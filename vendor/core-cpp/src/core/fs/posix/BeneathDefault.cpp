// SPDX-License-Identifier: Apache-2.0
//
// What BeneathStrategy::Best is on every POSIX system but Linux: the walk, one component at a
// time, since no kernel here resolves a path beneath a directory for the caller. Each directory is
// opened as a path handle where the system has O_PATH (FreeBSD 13), so a directory the process
// may search but not read is walked through; elsewhere (macOS) it is opened for reading, and
// O_NONBLOCK keeps a FIFO swapped in for a directory from holding the open. CMake compiles exactly
// one BeneathDefault.cpp, so no `#ifdef` chooses the strategy; the one below picks a flag inside
// the data, as the errno tables do.
#include <core/fs/detail/ResolveBeneath.hpp>
#include <core/fs/posix/PosixPathWalk.hpp>

#include <memory>

#include <fcntl.h>

namespace core::fs::detail
{

namespace
{

#ifdef O_PATH
    constexpr int WalkAccess = O_PATH;
#else
    constexpr int WalkAccess = O_RDONLY | O_NONBLOCK;
#endif

    constexpr int WalkDirectoryOpenFlags = WalkAccess | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

} // namespace

std::unique_ptr<IBeneathOpener const> makeBestBeneathOpener()
{
    return nullptr;
}

int walkDirectoryOpenFlags() noexcept
{
    return WalkDirectoryOpenFlags;
}

} // namespace core::fs::detail
