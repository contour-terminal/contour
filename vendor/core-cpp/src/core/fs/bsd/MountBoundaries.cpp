// SPDX-License-Identifier: Apache-2.0
//
// Mount boundaries on macOS and the BSDs. fstatfs names the mount each directory is on: where it is
// mounted (f_mntonname) and its file system id. These systems say nowhere which directory of its
// file system a mount shows, so every mount left is taken to show a subtree, and an `Unrelated`
// stands only where the other directory is on that same mount or on another file system
// (posix/MountBoundaries.hpp). The file system is the directory's `st_dev`; a type that may show
// another file system's directories as its own (nullfs, unionfs, FUSE, a network share) is marked
// so, and only the same mount decides there. Whether a step leaves from the mount's root, and what
// it skipped where not, is bsd/MountExit.hpp's to say: below it on a macOS firmlink. The mount
// table, for the mounts beneath a directory, is getfsstat's.
#include <core/fs/posix/MountBoundaries.hpp>

#include <core/fs/FsError.hpp>
#include <core/fs/bsd/MountExit.hpp>
#include <core/fs/posix/PosixSystem.hpp>

#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace core::fs::detail
{

namespace
{

    /// The file system types that may show another file system's directories as their own: a null
    /// or union mount, which also shows them under an id of its own; any FUSE file system (macFUSE,
    /// fuse-t, bindfs, sshfs); and a network share, which may be a loopback export of this machine
    /// (fuse-t is one: it reports `nfs`).
    constexpr auto ReexposingTypes = std::to_array<ReexposingType>({
        { .name = "nullfs", .match = TypeMatch::Exact },
        { .name = "unionfs", .match = TypeMatch::Exact },
        { .name = "fuse", .match = TypeMatch::Family },
        { .name = "macfuse", .match = TypeMatch::Exact },
        { .name = "osxfuse", .match = TypeMatch::Exact },
        { .name = "bindfs", .match = TypeMatch::Exact },
        { .name = "nfs", .match = TypeMatch::Family },
        { .name = "smbfs", .match = TypeMatch::Exact },
        { .name = "afpfs", .match = TypeMatch::Exact },
        { .name = "webdav", .match = TypeMatch::Exact },
    });

    /// @return The mount @p handle is on.
    [[nodiscard]] FsResult<struct ::statfs> statfsOf(int handle)
    {
        struct ::statfs status {};
        if (::fstatfs(handle, &status) != 0)
            return errnoError("fstatfs");
        return status;
    }

    /// @return A name for the mount @p status describes: where it is mounted, and its id, so a mount
    ///         made over another at one place is not taken for it.
    [[nodiscard]] std::string mountNameOf(struct ::statfs const& status)
    {
        return std::format("{}:{}:{}",
                           static_cast<std::int64_t>(status.f_fsid.val[0]),
                           static_cast<std::int64_t>(status.f_fsid.val[1]),
                           std::string_view { status.f_mntonname });
    }

    class BsdMountBoundaries final: public IMountBoundaries
    {
      public:
        [[nodiscard]] FsResult<std::optional<MountCrossing>> crossing(int child, int parent) const override
        {
            auto const childMount = statfsOf(child);
            if (!childMount)
                return std::unexpected { childMount.error() };
            auto const parentMount = statfsOf(parent);
            if (!parentMount)
                return std::unexpected { parentMount.error() };
            if (mountNameOf(*childMount) == mountNameOf(*parentMount))
                return std::nullopt;
            return viewOf(child, *childMount).transform([&](MountView view) {
                return std::optional { crossingFrom(child, *childMount, std::move(view)) };
            });
        }

        [[nodiscard]] FsResult<MountView> mountOf(int handle) const override
        {
            return statfsOf(handle).and_then(
                [&](struct ::statfs const& status) { return viewOf(handle, status); });
        }

        [[nodiscard]] FsResult<std::vector<MountTableRow>> mountTable() const override
        {
            // getfsstat rather than getmntinfo, whose buffer is shared by every thread. The count is
            // asked first, and the table read again where a mount arrived in between: a buffer that
            // came back full may have dropped one, and nothing may be missing.
            for ([[maybe_unused]] auto const attempt: std::views::iota(0, MaxTableReads))
            {
                auto const counted = ::getfsstat(nullptr, 0, MNT_NOWAIT);
                if (counted < 0)
                    return errnoError("getfsstat");
                auto mounts = std::vector<struct ::statfs>(static_cast<std::size_t>(counted) + SpareRows);
                auto const read = ::getfsstat(mounts.data(), byteSizeOf(mounts), MNT_NOWAIT);
                if (read < 0)
                    return errnoError("getfsstat");
                if (static_cast<std::size_t>(read) == mounts.size())
                    continue;
                mounts.resize(static_cast<std::size_t>(read));
                auto rows = std::vector<MountTableRow> {};
                rows.reserve(mounts.size());
                for (auto const& mount: mounts)
                    rows.push_back(MountTableRow { .path = std::string { mount.f_mntonname },
                                                   .mount = mountNameOf(mount) });
                return rows;
            }
            return std::unexpected { FsError { .code = FsErrorCode::Busy,
                                               .systemCode = 0,
                                               .context =
                                                   "the mount table kept growing while it was read" } };
        }

      private:
        /// How many times the table is read before a table that keeps growing is refused.
        static constexpr auto MaxTableReads = 4;
        /// Rows beyond the count asked for, so a mount made in between is seen as a full buffer.
        static constexpr auto SpareRows = std::size_t { 8 };

        /// @return @p mounts' size in bytes, as getfsstat takes it: an `int`, which macOS's parameter
        ///         is and FreeBSD's `long` widens from; a mount table is far below either's limit.
        [[nodiscard]] static int byteSizeOf(std::vector<struct ::statfs> const& mounts) noexcept
        {
            return static_cast<int>(mounts.size() * sizeof(struct ::statfs));
        }

      private:
        [[nodiscard]] static FsResult<MountView> viewOf(int handle, struct ::statfs const& mount)
        {
            struct ::stat status {};
            if (::fstat(handle, &status) != 0)
                return errnoError("fstat");
            return MountView { .mount = mountNameOf(mount),
                               .fileSystem = static_cast<std::uint64_t>(status.st_dev),
                               .root = std::nullopt,
                               .reexposure =
                                   reexposureOf(ReexposingTypes, std::string_view { mount.f_fstypename }) };
        }
    };

} // namespace

std::shared_ptr<IMountBoundaries const> makeMountBoundaries()
{
    return std::make_shared<BsdMountBoundaries const>();
}

} // namespace core::fs::detail
