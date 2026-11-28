// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where a relation walk on macOS or a BSD leaves a mount, and what that step skipped: one
/// implementation per OS, chosen by CMake (`darwin/MountExit.cpp`, `bsd/MountExit.cpp`). Private to
/// the fs module.

#include <core/fs/posix/MountBoundaries.hpp>

#include <sys/mount.h>
#include <sys/param.h>

namespace core::fs::detail
{

/// @param child A directory whose `..` is on another mount.
/// @param mount The mount it is on (fstatfs).
/// @param view What the walk knows of that mount.
/// @return The crossing: `AtRoot` only where @p child is known to be @p mount's root; below it, the
///         directories the step skipped, as far as they can be read (posix/MountBoundaries.hpp).
[[nodiscard]] MountCrossing crossingFrom(int child, struct ::statfs const& mount, MountView view);

} // namespace core::fs::detail
