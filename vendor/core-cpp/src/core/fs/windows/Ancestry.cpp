// SPDX-License-Identifier: Apache-2.0
//
// The real parents of a directory on Windows. NtCreateFile has no `..` relative to a handle, so the
// walk is by name and the decision by identity: the handle's final path
// (`GetFinalPathNameByHandleW`, normalized, by volume GUID, every junction and link on the way
// resolved), then each ancestor prefix of it opened for its attributes only, without following a
// reparse point, and its `FILE_ID_INFO` (volume serial number and 128-bit file id) read. At a volume's
// root the walk goes on from each folder the volume is mounted on (`GetVolumePathNamesForVolumeNameW`),
// as POSIX's `..` leaves a mount for the directory it is mounted on; a drive letter is a root.
//
// A prefix renamed while it is walked can only lead to an object that is not the real ancestor, so
// every ancestor's handle is held, and once both walks are done the final paths of the two handles
// and of every ancestor are read again: a change is `Busy`, never a guess, and so is a failure on
// the way that a change could explain. A volume's mount folders are taken once, so a volume
// mounted inside itself ends; the walk keeps a list of paths to go on from, not a call stack. Any other
// failure -- a parent the user may not open, among them -- is the error it is, and the caller fails closed.
// Identities count only on the file systems of StableFileIdRows.cpp. These opens name absolute paths, outside
// every root: they read attributes and nothing else, and no handle they make leaves this file.
//
// CMake compiles this file on Windows, and posix/Ancestry.cpp everywhere else.
#include <core/fs/detail/Ancestry.hpp>

#include <core/fs/Descriptor.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/detail/NativeSupport.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/windows/StableFileIdRows.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>

namespace core::fs::detail
{

namespace
{

    /// The prefix of a path by volume GUID, before the GUID's closing `}\`.
    constexpr auto VerbatimPrefix = std::wstring_view { L"\\\\?\\" };

    /// The longest file-system name `GetVolumeInformationByHandleW` writes, with its NUL.
    constexpr auto FileSystemNameCapacity = std::size_t { MAX_PATH + 1 };

    [[nodiscard]] std::unexpected<FsError> lastError(char const* call)
    {
        return std::unexpected { fsErrorFromSystemCode(std::int64_t { ::GetLastError() }, call) };
    }

    /// @return @p handle's path by volume GUID, normalized: `\\?\Volume{...}\a\b`.
    [[nodiscard]] FsResult<std::wstring> finalPathOf(HANDLE handle)
    {
        auto path = std::wstring(MAX_PATH, L'\0');
        for ([[maybe_unused]] auto const attempt: { 0, 1, 2 })
        {
            auto const length = ::GetFinalPathNameByHandleW(handle,
                                                            path.data(),
                                                            static_cast<DWORD>(path.size()),
                                                            FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
            if (length == 0)
                return lastError("GetFinalPathNameByHandleW");
            if (length < path.size())
            {
                path.resize(length);
                return path;
            }
            path.resize(length); // the size it needs, its NUL included
        }
        return refuse(FsErrorCode::Busy);
    }

    /// @return The root of the volume @p path is on, `\\?\Volume{...}\`; empty where @p path is no
    ///         path by volume GUID.
    [[nodiscard]] std::wstring_view volumeRootOf(std::wstring_view path) noexcept
    {
        if (!path.starts_with(VerbatimPrefix))
            return {};
        auto const separator = path.find(L'\\', VerbatimPrefix.size());
        return separator == std::wstring_view::npos ? std::wstring_view {} : path.substr(0, separator + 1);
    }

    /// @return @p path without its last component; the volume's root stays as it is.
    [[nodiscard]] std::wstring_view parentPathOf(std::wstring_view path, std::wstring_view root) noexcept
    {
        auto const separator = path.find_last_of(L'\\');
        if (separator == std::wstring_view::npos || separator < root.size())
            return root;
        return path.substr(0, separator);
    }

    /// @return The identity of @p handle's object, where its file system gives stable ones.
    [[nodiscard]] FsResult<ObjectIdentity> identityOf(HANDLE handle)
    {
        // The file system first: FAT and others refuse FileIdInfo itself, which is no reason to give
        // than that they have no stable ids.
        auto fileSystem = std::array<wchar_t, FileSystemNameCapacity> {};
        if (::GetVolumeInformationByHandleW(handle,
                                            nullptr,
                                            0,
                                            nullptr,
                                            nullptr,
                                            nullptr,
                                            fileSystem.data(),
                                            static_cast<DWORD>(fileSystem.size()))
            == FALSE)
            return lastError("GetVolumeInformationByHandleW");
        auto const fileSystemName = std::wstring_view { fileSystem.data() };
        if (fileIdStabilityOf(fileSystemName) != FileIdStability::Stable)
            return refuse(FsErrorCode::Unsupported);
        // A share reports the file system of the server's volume, NTFS as often as not, but its ids
        // are the server's to keep: only a local volume's are trusted. The query answers only for a
        // remote file.
        auto remote = FILE_REMOTE_PROTOCOL_INFO {};
        if (::GetFileInformationByHandleEx(handle, FileRemoteProtocolInfo, &remote, sizeof(remote)) != FALSE)
            return refuse(FsErrorCode::Unsupported);
        auto id = FILE_ID_INFO {};
        if (::GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) == FALSE)
            return lastError("GetFileInformationByHandleEx(FileIdInfo)");
        return stableIdentityOf(
            id.VolumeSerialNumber, std::as_bytes(std::span { id.FileId.Identifier }), fileSystemName);
    }

    /// Opens @p path for its attributes alone, a reparse point as itself.
    [[nodiscard]] FsResult<UniqueHandle> openForIdentity(std::wstring const& path)
    {
        auto handle = UniqueHandle { ::CreateFileW(path.c_str(),
                                                   FILE_READ_ATTRIBUTES,
                                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                   nullptr,
                                                   OPEN_EXISTING,
                                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                                   nullptr) };
        if (handle.get() == platform::InvalidHandle)
            return lastError("CreateFileW");
        return handle;
    }

    /// @return The folders the volume @p root is mounted on, drive letters left out.
    [[nodiscard]] FsResult<std::vector<std::wstring>> mountFoldersOf(std::wstring const& root)
    {
        auto names = std::wstring(MAX_PATH, L'\0');
        auto needed = DWORD { 0 };
        while (::GetVolumePathNamesForVolumeNameW(
                   root.c_str(), names.data(), static_cast<DWORD>(names.size()), &needed)
               == FALSE)
        {
            if (::GetLastError() != ERROR_MORE_DATA || needed <= names.size())
                return lastError("GetVolumePathNamesForVolumeNameW");
            names.resize(needed);
        }
        auto folders = std::vector<std::wstring> {};
        auto rest = std::wstring_view { names };
        while (!rest.empty() && rest.front() != L'\0')
        {
            auto const name = rest.substr(0, rest.find(L'\0'));
            // `C:\` is a drive's root, above which there is nothing; `C:\mnt\v\` is a folder.
            if (name.size() > 3)
                folders.emplace_back(name);
            rest.remove_prefix(std::min(rest.size(), name.size() + 1));
        }
        return folders;
    }

    /// An ancestor the walk opened, held until the walk is checked, and the path it was opened by.
    struct Held
    {
        UniqueHandle handle;
        std::wstring path;
    };

    /// One walk's state: the identities met, every ancestor still held, the volumes whose mount
    /// folders were taken, and how many parents are left before `Loop`.
    struct Walk
    {
        std::vector<ObjectIdentity> chain;
        std::vector<Held> held;
        std::vector<std::wstring> volumesSeen;
        std::size_t parentsLeft = 0;
    };

    /// Records one ancestor: its identity, and its handle with the path it is expected at.
    [[nodiscard]] FsResult<void> record(Walk& walk, UniqueHandle handle, std::wstring path)
    {
        if (walk.parentsLeft == 0)
            return refuse(FsErrorCode::Loop);
        --walk.parentsLeft;
        auto const identity = identityOf(handle.get());
        if (!identity)
            return std::unexpected { identity.error() };
        walk.chain.push_back(*identity);
        walk.held.push_back(Held { .handle = std::move(handle), .path = std::move(path) });
        return {};
    }

    /// Records the parents of the object at @p start, which is already in the walk, up to its
    /// volume's root, then on from each folder that volume is mounted on, and so on, without
    /// recursion; a volume's folders are taken once.
    [[nodiscard]] FsResult<void> walkUp(Walk& walk, std::wstring start)
    {
        auto pending = std::vector<std::wstring> { std::move(start) };
        while (!pending.empty())
        {
            auto const path = std::move(pending.back());
            pending.pop_back();
            auto const root = std::wstring { volumeRootOf(path) };
            if (root.empty())
                return refuse(FsErrorCode::Unsupported);
            auto current = std::wstring_view { path };
            while (current.size() > root.size())
            {
                current = parentPathOf(current, root);
                auto prefix = std::wstring { current };
                auto opened = openForIdentity(prefix);
                if (!opened)
                    return std::unexpected { opened.error() };
                if (auto recorded = record(walk, std::move(*opened), std::move(prefix)); !recorded)
                    return recorded;
            }
            if (std::ranges::find(walk.volumesSeen, root) != walk.volumesSeen.end())
                continue;
            walk.volumesSeen.push_back(root);
            auto folders = mountFoldersOf(root);
            if (!folders)
                return std::unexpected { folders.error() };
            for (auto& folder: *folders)
            {
                // `C:\mnt\v\` with its separator names the root of the volume mounted there; without
                // it, and with OPEN_REPARSE_POINT, the folder itself, on the volume that holds it.
                while (folder.size() > 3 && folder.back() == L'\\')
                    folder.pop_back();
                auto opened = openForIdentity(folder);
                if (!opened)
                    return std::unexpected { opened.error() };
                auto folderPath = finalPathOf(opened->get());
                if (!folderPath)
                    return std::unexpected { folderPath.error() };
                pending.push_back(*folderPath);
                if (auto recorded = record(walk, std::move(*opened), std::move(*folderPath)); !recorded)
                    return recorded;
            }
        }
        return {};
    }

    /// @return Whether @p handle's final path is still @p path.
    [[nodiscard]] bool stillAt(HANDLE handle, std::wstring const& path)
    {
        auto const now = finalPathOf(handle);
        return now && *now == path;
    }

    /// @return Whether every ancestor @p walk holds is still at the path it was opened by: an
    ///         ancestor renamed away and another put in its place (ABA) shows here, through the
    ///         handle of the one that was met.
    [[nodiscard]] bool stillWhereMet(Walk const& walk)
    {
        return std::ranges::all_of(
            walk.held, [](Held const& ancestor) { return stillAt(ancestor.handle.get(), ancestor.path); });
    }

    /// @return @p error, or `Busy` for a name that is gone: an open directory the system cannot name
    ///         for a moment, or an ancestor read off its live path that is no longer there, is one
    ///         that a rename is moving.
    [[nodiscard]] std::unexpected<FsError> movedOr(FsError const& error)
    {
        if (error.code == FsErrorCode::NoEntry)
            return refuse(FsErrorCode::Busy);
        return std::unexpected { error };
    }

    /// What one handle's walk starts from.
    struct Start
    {
        ObjectIdentity identity;
        std::wstring path;
    };

    [[nodiscard]] FsResult<Start> startOf(HANDLE handle)
    {
        return identityOf(handle).and_then([&](ObjectIdentity const& identity) {
            return finalPathOf(handle).transform(
                [&](std::wstring path) { return Start { .identity = identity, .path = std::move(path) }; });
        });
    }

} // namespace

FsResult<Relation> nativeRelation(NativeContext const& context,
                                  platform::NativeHandle first,
                                  platform::NativeHandle second)
{
    auto const maxDepth = context.options.maxAncestorDepth;
    auto const firstStart = startOf(first);
    if (!firstStart)
        return movedOr(firstStart.error());
    auto const secondStart = startOf(second);
    if (!secondStart)
        return movedOr(secondStart.error());
    if (firstStart->identity == secondStart->identity)
        return Relation::Same;
    // Grown per step, never reserved to the cap: a deep tree costs what it walks.
    auto up =
        Walk { .chain = { firstStart->identity }, .held = {}, .volumesSeen = {}, .parentsLeft = maxDepth };
    auto down =
        Walk { .chain = { secondStart->identity }, .held = {}, .volumesSeen = {}, .parentsLeft = maxDepth };
    auto const walked =
        walkUp(up, firstStart->path).and_then([&] { return walkUp(down, secondStart->path); });
    // A rename during the walks could have led either one astray, or made it fail: what it found is
    // trusted only where both handles, and every ancestor met, are still where they were.
    if (!stillAt(first, firstStart->path) || !stillAt(second, secondStart->path) || !stillWhereMet(up)
        || !stillWhereMet(down))
        return refuse(FsErrorCode::Busy);
    if (!walked)
        return movedOr(walked.error());
    return decideRelation(up.chain, down.chain);
}

} // namespace core::fs::detail
