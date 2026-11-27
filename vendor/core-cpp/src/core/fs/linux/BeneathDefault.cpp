// SPDX-License-Identifier: Apache-2.0
//
// What BeneathStrategy::Best is on Linux: openat2(RESOLVE_BENEATH) where the kernel has it, probed
// once per file system, and the walk where it does not. The walk opens each directory as a path
// handle (O_PATH), so a directory the process may search but not read is walked through. CMake
// compiles exactly one BeneathDefault.cpp, so no `#ifdef` chooses either.
#include <core/fs/detail/ResolveBeneath.hpp>
#include <core/fs/linux/Openat2.hpp>
#include <core/fs/posix/PosixPathWalk.hpp>

#include <memory>

#include <fcntl.h>

namespace core::fs::detail
{

namespace
{

    constexpr int WalkDirectoryOpenFlags = O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

} // namespace

std::unique_ptr<IBeneathOpener const> makeBestBeneathOpener()
{
    return makeOpenat2Opener(systemOpenat2());
}

int walkDirectoryOpenFlags() noexcept
{
    return WalkDirectoryOpenFlags;
}

} // namespace core::fs::detail
