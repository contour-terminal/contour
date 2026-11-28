// SPDX-License-Identifier: Apache-2.0
//
// Mount exits on macOS, where a firmlink (`/Users`, `/private`, ... on the Data volume) has a `..`
// on the system volume: a walk leaves the Data volume there, below its root, and skips the Data
// volume's directories above the firmlink and every directory on its mount point's path
// (`/System`, `/System/Volumes`, `/System/Volumes/Data`).
//
// A directory is its mount's root where its identity is that of what the mount's own path names:
// stat of f_mntonname resolves to the root of the mount on top there, never to a covered directory
// and never to a directory below a root. ATTR_DIR_MOUNTSTATUS is asked first: xnu sets
// DIR_MNTSTATUS_MNTPOINT for a mount's root vnode or a covered one (vfs_attrlist.c), so a
// directory it does not flag is below its root without a path being looked at.
//
// Below the root, the skipped directories are read from the root handle down the mount point's
// path, one component at a time with O_NOFOLLOW, each by identity; the mount's root is the last.
// Where the directory left from is a child of that root -- its name (F_GETPATH) opened beneath the
// root has its identity -- that is all the step skipped (Exactly); otherwise the mount's own
// directories between are unknown (AtLeast). Anything unreadable is Unknown (fail closed).
#include <core/fs/bsd/MountExit.hpp>

#include <core/fs/detail/Ancestry.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/posix/PosixSystem.hpp>

#include <sys/attr.h>
#include <sys/stat.h>

#include <array>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace core::fs::detail
{

namespace
{

    constexpr int DirectoryOpenFlags = O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

    /// What fgetattrlist returns for ATTR_DIR_MOUNTSTATUS alone.
    struct MountStatusReply
    {
        std::uint32_t length = 0;
        std::uint32_t status = 0;
    };

    [[nodiscard]] bool isFlaggedMountPoint(int handle)
    {
        auto request = attrlist {};
        request.bitmapcount = ATTR_BIT_MAP_COUNT;
        request.dirattr = ATTR_DIR_MOUNTSTATUS;
        auto reply = MountStatusReply {};
        if (::fgetattrlist(handle, &request, &reply, sizeof(reply), 0) != 0)
            return false;
        return (reply.status & DIR_MNTSTATUS_MNTPOINT) != 0;
    }

    [[nodiscard]] std::optional<ObjectIdentity> identityOf(int handle)
    {
        struct ::stat status {};
        if (::fstat(handle, &status) != 0)
            return std::nullopt;
        return ObjectIdentity { .device = static_cast<std::uint64_t>(status.st_dev),
                                .idLow = static_cast<std::uint64_t>(status.st_ino),
                                .idHigh = 0 };
    }

    [[nodiscard]] bool isWhatTheMountPathNames(ObjectIdentity const& child, struct ::statfs const& mount)
    {
        struct ::stat named {};
        if (::stat(mount.f_mntonname, &named) != 0)
            return false;
        return child.device == static_cast<std::uint64_t>(named.st_dev)
               && child.idLow == static_cast<std::uint64_t>(named.st_ino);
    }

    /// The directories on a mount point's path below `/`, and a handle on the last of them.
    struct MountPath
    {
        std::vector<ObjectIdentity> directories;
        UniqueHandle root;
    };

    /// @return Each directory on @p mountPoint below `/`, opened from the root one component at a
    ///         time without following a link; nullopt where one cannot be.
    [[nodiscard]] std::optional<MountPath> mountPathOf(std::string_view mountPoint)
    {
        if (!mountPoint.starts_with('/'))
            return std::nullopt;
        auto current = UniqueHandle { retryOnInterrupt([] { return ::open("/", DirectoryOpenFlags); }) };
        if (current.get() == platform::InvalidHandle)
            return std::nullopt;
        auto path = MountPath { .directories = {}, .root = UniqueHandle { platform::InvalidHandle } };
        for (auto const componentRange: mountPoint | std::views::split('/'))
        {
            auto const component = std::string { std::string_view { componentRange } };
            if (component.empty())
                continue;
            if (component == "." || component == "..")
                return std::nullopt;
            auto next = UniqueHandle { retryOnInterrupt(
                [&] { return ::openat(current.get(), component.c_str(), DirectoryOpenFlags); }) };
            if (next.get() == platform::InvalidHandle)
                return std::nullopt;
            auto const identity = identityOf(next.get());
            if (!identity)
                return std::nullopt;
            path.directories.push_back(*identity);
            current = std::move(next);
        }
        if (path.directories.empty())
            return std::nullopt;
        path.root = std::move(current);
        return path;
    }

    /// @return Whether @p child, @p identity, is a child of @p root: its own name opened beneath
    ///         @p root has its identity.
    [[nodiscard]] bool isChildOf(int root, int child, ObjectIdentity const& identity)
    {
        auto path = std::array<char, MAXPATHLEN> {};
        if (::fcntl(child, F_GETPATH, path.data()) == -1)
            return false;
        auto const full = std::string_view { path.data() };
        auto const slash = full.rfind('/');
        if (slash == std::string_view::npos || slash + 1 == full.size())
            return false;
        auto const name = std::string { full.substr(slash + 1) };
        auto const opened = UniqueHandle { retryOnInterrupt(
            [&] { return ::openat(root, name.c_str(), DirectoryOpenFlags); }) };
        if (opened.get() == platform::InvalidHandle)
            return false;
        return identityOf(opened.get()) == identity;
    }

} // namespace

MountCrossing crossingFrom(int child, struct ::statfs const& mount, MountView view)
{
    auto crossing = MountCrossing { .mount = std::move(view),
                                    .exit = MountExit::BelowRoot,
                                    .skipped = {},
                                    .skippedSet = SkippedSet::Unknown };
    auto const self = identityOf(child);
    if (!self)
        return crossing;
    if (isFlaggedMountPoint(child) && isWhatTheMountPathNames(*self, mount))
    {
        crossing.exit = MountExit::AtRoot;
        return crossing;
    }
    auto path = mountPathOf(std::string_view { mount.f_mntonname });
    if (!path)
        return crossing;
    crossing.skippedSet =
        isChildOf(path->root.get(), child, *self) ? SkippedSet::Exactly : SkippedSet::AtLeast;
    crossing.skipped = std::move(path->directories);
    return crossing;
}

} // namespace core::fs::detail
