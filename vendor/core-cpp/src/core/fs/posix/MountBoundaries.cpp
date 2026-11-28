// SPDX-License-Identifier: Apache-2.0
//
// Mount boundaries on a POSIX system with no way to tell what a mount shows (neither Linux's
// mountinfo nor the BSDs' statfs): every step that changes device leaves a mount, which may show a
// subtree, from below its root, of a type that may show another file system's directories; no
// mount can be named, so every `Unrelated` past a change of device is Unsupported (fail closed). Nor
// can the mounts be listed, so no `Unrelated` stands at all: one beneath either directory could show
// the other.
#include <core/fs/posix/MountBoundaries.hpp>

#include <core/fs/FsError.hpp>
#include <core/fs/posix/PosixSystem.hpp>

#include <sys/stat.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <vector>

namespace core::fs::detail
{

namespace
{

    [[nodiscard]] FsResult<std::uint64_t> deviceOf(int handle)
    {
        struct ::stat status {};
        if (::fstat(handle, &status) != 0)
            return errnoError("fstat");
        return static_cast<std::uint64_t>(status.st_dev);
    }

    [[nodiscard]] MountView viewOn(std::uint64_t device)
    {
        // Nor what type of file system it is: one that may show another's (fail closed).
        return MountView { .mount = std::nullopt,
                           .fileSystem = device,
                           .root = std::nullopt,
                           .reexposure = Reexposure::Possible };
    }

    class DeviceMountBoundaries final: public IMountBoundaries
    {
      public:
        [[nodiscard]] FsResult<std::optional<MountCrossing>> crossing(int child, int parent) const override
        {
            auto const childDevice = deviceOf(child);
            if (!childDevice)
                return std::unexpected { childDevice.error() };
            auto const parentDevice = deviceOf(parent);
            if (!parentDevice)
                return std::unexpected { parentDevice.error() };
            if (*childDevice == *parentDevice)
                return std::nullopt;
            // Nothing here says whether the step left from the mount's root: below it (fail closed).
            return std::optional { MountCrossing { .mount = viewOn(*childDevice),
                                                   .exit = MountExit::BelowRoot,
                                                   .skipped = {},
                                                   .skippedSet = SkippedSet::Unknown } };
        }

        [[nodiscard]] FsResult<MountView> mountOf(int handle) const override
        {
            return deviceOf(handle).transform(viewOn);
        }

        [[nodiscard]] FsResult<std::vector<MountTableRow>> mountTable() const override
        {
            // No table to read: a mount beneath either directory cannot be ruled out (fail closed).
            return std::unexpected { FsError { .code = FsErrorCode::Unsupported,
                                               .systemCode = 0,
                                               .context = "this system lists no mounts" } };
        }
    };

} // namespace

std::shared_ptr<IMountBoundaries const> makeMountBoundaries()
{
    return std::make_shared<DeviceMountBoundaries const>();
}

} // namespace core::fs::detail
