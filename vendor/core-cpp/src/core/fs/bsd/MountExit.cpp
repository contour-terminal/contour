// SPDX-License-Identifier: Apache-2.0
//
// Mount exits on the BSDs but macOS: their `..` leaves a mount only at its root (the lookup crosses
// to the covered directory only from a vnode flagged as its mount's root), so every step that
// changes mount leaves from the root and skips nothing but what the mount's root column would name.
// macOS has firmlinks, which do not: darwin/MountExit.cpp.
#include <core/fs/bsd/MountExit.hpp>

#include <utility>

namespace core::fs::detail
{

MountCrossing crossingFrom(int /*child*/, struct ::statfs const& /*mount*/, MountView view)
{
    return MountCrossing {
        .mount = std::move(view), .exit = MountExit::AtRoot, .skipped = {}, .skippedSet = SkippedSet::Unknown
    };
}

} // namespace core::fs::detail
