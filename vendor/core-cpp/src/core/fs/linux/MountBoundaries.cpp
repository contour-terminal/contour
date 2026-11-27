// SPDX-License-Identifier: Apache-2.0
//
// Mount boundaries on Linux. statx names the mount each directory is on (STATX_MNT_ID); a mount is
// looked up in this process's mountinfo, read through a handle on /proc/self opened when the file
// system was built, for its file system's device and its root: `/` shows the whole file system;
// anything else (a bind mount of a subdirectory, a btrfs subvolume) shows a subtree whose real
// parents the kernel's `..` skips (posix/MountBoundaries.hpp decides what they may hide). A mount
// missing from the table, or a line that does not parse, is known by its id alone (fail closed).
#include <core/fs/posix/MountBoundaries.hpp>

#include <core/fs/FsError.hpp>
#include <core/fs/detail/NativeSupport.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/linux/MountInfo.hpp>
#include <core/fs/posix/PosixSystem.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <memory>
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

    /// The file system types that may show another file system's directories as their own: a union
    /// or overlay of other directories, any FUSE file system (bindfs, sshfs, a passthrough), and a
    /// network share, which may be a loopback export of this machine's own.
    constexpr auto ReexposingTypes = std::to_array<ReexposingType>({
        { .name = "overlay", .match = TypeMatch::Exact },
        { .name = "aufs", .match = TypeMatch::Exact },
        { .name = "shiftfs", .match = TypeMatch::Exact },
        { .name = "fuse", .match = TypeMatch::Family },
        { .name = "nfs", .match = TypeMatch::Family },
        { .name = "cifs", .match = TypeMatch::Exact },
        { .name = "smb3", .match = TypeMatch::Exact },
    });

    /// How much of mountinfo is read at most: a mount table larger than this is refused, not
    /// half-read.
    constexpr auto MaxMountInfoBytes = std::size_t { 16 } << 20U;

    [[nodiscard]] std::unexpected<FsError> hidden(char const* why)
    {
        return std::unexpected { FsError {
            .code = FsErrorCode::Unsupported, .systemCode = 0, .context = why } };
    }

    [[nodiscard]] FsResult<std::uint64_t> mountIdOf(int handle)
    {
        struct ::statx status {};
        if (::statx(handle, "", AT_EMPTY_PATH, STATX_MNT_ID, &status) != 0)
            return errnoError("statx");
        if ((status.stx_mask & STATX_MNT_ID) == 0)
            return hidden("statx names no mount here");
        return std::uint64_t { status.stx_mnt_id };
    }

    class LinuxMountBoundaries final: public IMountBoundaries
    {
      public:
        explicit LinuxMountBoundaries(UniqueHandle processDirectory):
            _processDirectory { std::move(processDirectory) }
        {
        }

        [[nodiscard]] FsResult<std::optional<MountCrossing>> crossing(int child, int parent) const override
        {
            auto const childMount = mountIdOf(child);
            if (!childMount)
                return std::unexpected { childMount.error() };
            auto const parentMount = mountIdOf(parent);
            if (!parentMount)
                return std::unexpected { parentMount.error() };
            if (*childMount == *parentMount)
                return std::nullopt;
            // Linux's `..` leaves a mount only at its root (follow_dotdot); below it, `..` stays on
            // the mount.
            return viewOf(*childMount).transform([](MountView view) {
                return std::optional { MountCrossing { .mount = std::move(view),
                                                       .exit = MountExit::AtRoot,
                                                       .skipped = {},
                                                       .skippedSet = SkippedSet::Unknown } };
            });
        }

        [[nodiscard]] FsResult<MountView> mountOf(int handle) const override
        {
            return mountIdOf(handle).and_then([&](std::uint64_t mount) { return viewOf(mount); });
        }

        [[nodiscard]] FsResult<std::vector<MountTableRow>> mountTable() const override
        {
            return readMountInfo().and_then(
                [](std::string const& table) -> FsResult<std::vector<MountTableRow>> {
                    auto places = mountPlacesOf(table);
                    if (!places)
                        return hidden("mountinfo holds a line that does not parse");
                    auto rows = std::vector<MountTableRow> {};
                    rows.reserve(places->size());
                    for (auto& place: *places)
                        rows.push_back(MountTableRow { .path = std::move(place.mountPoint),
                                                       .mount = std::to_string(place.id) });
                    return rows;
                });
        }

      private:
        [[nodiscard]] FsResult<MountView> viewOf(std::uint64_t mount) const
        {
            return readMountInfo().transform([&](std::string const& table) {
                auto view = MountView { .mount = std::to_string(mount),
                                        .fileSystem = {},
                                        .root = {},
                                        .reexposure = Reexposure::Possible };
                if (auto entry = mountEntryOf(table, mount))
                {
                    view.fileSystem = entry->device;
                    view.root = std::move(entry->root);
                    view.reexposure = reexposureOf(ReexposingTypes, entry->type);
                }
                return view;
            });
        }

        [[nodiscard]] FsResult<std::string> readMountInfo() const
        {
            auto const file = UniqueHandle { retryOnInterrupt(
                [&] { return ::openat(_processDirectory.get(), "mountinfo", O_RDONLY | O_CLOEXEC); }) };
            if (file.get() == platform::InvalidHandle)
                return errnoError("openat(mountinfo)");
            auto text = std::string {};
            auto buffer = std::array<char, 8192> {};
            while (true)
            {
                auto const got =
                    retryOnInterrupt([&] { return ::read(file.get(), buffer.data(), buffer.size()); });
                if (got < 0)
                    return errnoError("read(mountinfo)");
                if (got == 0)
                    return text;
                if (text.size() + static_cast<std::size_t>(got) > MaxMountInfoBytes)
                    return hidden("mountinfo is larger than is read");
                text.append(buffer.data(), static_cast<std::size_t>(got));
            }
        }

        UniqueHandle _processDirectory;
    };

    /// @return @p text with mountinfo's octal escapes (`\040` for a space) decoded; nullopt for a bad
    ///         escape, or one that decodes to NUL, which no path holds.
    [[nodiscard]] std::optional<std::string> unescaped(std::string_view text)
    {
        auto decoded = std::string {};
        while (!text.empty())
        {
            if (text.front() != '\\')
            {
                decoded += text.front();
                text.remove_prefix(1);
                continue;
            }
            if (text.size() < 4)
                return std::nullopt;
            auto value = 0U;
            for (auto const digit: text.substr(1, 3))
            {
                if (digit < '0' || digit > '7')
                    return std::nullopt;
                value = (value * 8U) + static_cast<unsigned>(digit - '0');
            }
            if (value == 0 || value > 0xFFU)
                return std::nullopt;
            decoded += static_cast<char>(value);
            text.remove_prefix(4);
        }
        return decoded;
    }

    /// @return @p text as a decimal number; nullopt where it is empty, holds anything but digits,
    ///         or overflows.
    [[nodiscard]] std::optional<std::uint64_t> decimal(std::string_view text)
    {
        if (text.empty() || text.size() > 19
            || !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; }))
            return std::nullopt;
        auto value = std::uint64_t { 0 };
        for (auto const digit: text)
            value = (value * 10U) + static_cast<std::uint64_t>(digit - '0');
        return value;
    }

    /// @return mountinfo's `major:minor` as `major << 32 | minor`; nullopt where it is not one.
    [[nodiscard]] std::optional<std::uint64_t> deviceOf(std::string_view text)
    {
        auto const colon = text.find(':');
        if (colon == std::string_view::npos)
            return std::nullopt;
        auto const major = decimal(text.substr(0, colon));
        auto const minor = decimal(text.substr(colon + 1));
        if (!major || !minor || *major > 0xFFFF'FFFFU || *minor > 0xFFFF'FFFFU)
            return std::nullopt;
        return (*major << 32U) | *minor;
    }

} // namespace

std::optional<std::vector<MountPlace>> mountPlacesOf(std::string_view mountInfo)
{
    auto places = std::vector<MountPlace> {};
    for (auto const lineRange: mountInfo | std::views::split('\n'))
    {
        auto const line = std::string_view { lineRange };
        if (line.empty())
            continue;
        // mount-id parent-id major:minor root mount-point ...
        auto fields = std::vector<std::string_view> {};
        for (auto const fieldRange: line | std::views::split(' '))
            fields.emplace_back(fieldRange);
        if (fields.size() < 5)
            return std::nullopt;
        auto const id = decimal(fields[0]);
        auto mountPoint = unescaped(fields[4]);
        if (!id || !mountPoint || !mountPoint->starts_with('/'))
            return std::nullopt;
        places.push_back(MountPlace { .id = *id, .mountPoint = std::move(*mountPoint) });
    }
    return places;
}

std::optional<MountEntry> mountEntryOf(std::string_view mountInfo, std::uint64_t mountId)
{
    for (auto const lineRange: mountInfo | std::views::split('\n'))
    {
        // mount-id parent-id major:minor root mount-point options [optional fields...] - type source
        // super-options
        auto fields = std::vector<std::string_view> {};
        for (auto const fieldRange: std::string_view { lineRange } | std::views::split(' '))
            fields.emplace_back(fieldRange);
        if (fields.size() < 4 || decimal(fields[0]) != mountId)
            continue;
        // The optional fields end at a lone `-`, never before the seventh column.
        auto const separator = std::ranges::find(fields | std::views::drop(6), std::string_view { "-" });
        auto device = deviceOf(fields[2]);
        auto root = unescaped(fields[3]);
        if (!device || !root || separator == fields.end() || std::next(separator) == fields.end())
            return std::nullopt;
        auto type = unescaped(*std::next(separator));
        if (!type || type->empty())
            return std::nullopt;
        return MountEntry { .device = *device, .root = std::move(*root), .type = std::move(*type) };
    }
    return std::nullopt;
}

std::shared_ptr<IMountBoundaries const> makeMountBoundaries()
{
    auto processDirectory = UniqueHandle { retryOnInterrupt(
        [] { return ::open("/proc/self", O_PATH | O_DIRECTORY | O_CLOEXEC); }) };
    if (processDirectory.get() == platform::InvalidHandle)
        return nullptr;
    return std::make_shared<LinuxMountBoundaries const>(std::move(processDirectory));
}

} // namespace core::fs::detail
