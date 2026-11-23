// SPDX-License-Identifier: Apache-2.0
#include <vtpty/Win32HeldDirectory.hpp>

#include <vtpty/WindowsProcessRules.hpp>

#include <core/WideString.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <utility>

namespace vtpty
{

namespace
{
    /// What the guard is opened for: listing the directory -- a data access, without which NTFS lets a
    /// sharer rename it -- and its attributes, to read its id.
    constexpr auto GuardAccess = DWORD { FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES };

    /// What the guard lets others do: read and write, but not delete, which a rename needs too.
    constexpr auto GuardShare = DWORD { FILE_SHARE_READ | FILE_SHARE_WRITE };

    /// What the confirmation's look lets others do: anything; it is closed at once.
    constexpr auto LookShare = DWORD { FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE };

    /// @return Which file @p handle is, by volume and file id; std::nullopt where neither can be read.
    ///         On a file system without 128-bit ids (FAT) it is the 64-bit file index, which names an entry's
    ///         slot and may be given to another entry once that one is freed, so two looks at one name could
    ///         match a different directory. The guard keeps the directory, and so its slot, from being freed
    ///         while the entry lives, which is all the index is trusted for.
    [[nodiscard]] std::optional<FILE_ID_INFO> fileIdOf(HANDLE handle) noexcept
    {
        auto id = FILE_ID_INFO {};
        if (GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) != FALSE)
            return id;
        auto info = BY_HANDLE_FILE_INFORMATION {};
        if (GetFileInformationByHandle(handle, &info) == FALSE)
            return std::nullopt;
        id = FILE_ID_INFO {};
        id.VolumeSerialNumber = info.dwVolumeSerialNumber;
        auto const index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U) | info.nFileIndexLow;
        id.FileId = std::bit_cast<FILE_ID_128>(std::array { index, std::uint64_t { 0 } });
        return id;
    }

    /// @return Whether @p a and @p b name one file.
    [[nodiscard]] bool isSameFile(FILE_ID_INFO const& a, FILE_ID_INFO const& b) noexcept
    {
        return a.VolumeSerialNumber == b.VolumeSerialNumber
               && std::ranges::equal(a.FileId.Identifier, b.FileId.Identifier);
    }

    /// Opens what stands at @p path itself, a reparse point not followed, not inheritable. The path is taken
    /// as CreateProcessW reads it: no `\\?\` prefix, which would skip the normalization it applies.
    /// @param path The path.
    /// @param access What it is opened for.
    /// @param share What others may do with it while it is open.
    /// @return The handle; an invalid one where it could not be opened (GetLastError() says why).
    [[nodiscard]] UniqueHandle openAtPath(std::wstring const& path, DWORD access, DWORD share) noexcept
    {
        return UniqueHandle { CreateFileW(path.c_str(),
                                          access,
                                          share,
                                          nullptr,
                                          OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                          nullptr) };
    }
} // namespace

std::expected<HeldDirectoryEntry, HeldDirectoryEntryError> prepareHeldDirectoryEntry(
    DirectoryHandle const& held)
{
    if (held.nativeHandle() == nullptr)
        return std::unexpected(HeldDirectoryEntryError::Unbacked);
    if (held.isRemoved())
        return std::unexpected(HeldDirectoryEntryError::Removed);
    auto path = core::toUtf16(held.resolvedPath());
    if (!path)
        return std::unexpected(HeldDirectoryEntryError::NotUtf8);
    if (path->size() > MaxWindowsCurrentDirectoryUnits)
        return std::unexpected(HeldDirectoryEntryError::TooLong);
    // A directory whose identity cannot be read cannot be shown to be the one at its path: refused.
    auto const identity = fileIdOf(held.nativeHandle());
    if (!identity)
        return std::unexpected(HeldDirectoryEntryError::Replaced);
    auto guard = openAtPath(*path, GuardAccess, GuardShare);
    if (!guard.valid())
        return std::unexpected(GetLastError() == ERROR_SHARING_VIOLATION
                                   ? HeldDirectoryEntryError::Unguardable
                                   : HeldDirectoryEntryError::Replaced);
    // Checked through the guard itself: what it keeps at the name is what was compared.
    auto const guarded = fileIdOf(guard.get());
    if (!guarded || !isSameFile(*guarded, *identity))
        return std::unexpected(HeldDirectoryEntryError::Replaced);
    return HeldDirectoryEntry { .path = std::move(*path), .identity = *identity, .guard = std::move(guard) };
}

std::expected<void, HeldDirectoryEntryError> confirmHeldDirectoryEntry(HeldDirectoryEntry const& entry)
{
    auto const opened = openAtPath(entry.path, FILE_READ_ATTRIBUTES, LookShare);
    if (!opened.valid())
        return std::unexpected(HeldDirectoryEntryError::Replaced);
    auto const atPath = fileIdOf(opened.get());
    if (!atPath || !isSameFile(*atPath, entry.identity))
        return std::unexpected(HeldDirectoryEntryError::Replaced);
    return {};
}

} // namespace vtpty
