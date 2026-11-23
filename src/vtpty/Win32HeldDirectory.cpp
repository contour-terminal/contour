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
    /// @return Which file @p handle is, by volume and file id; std::nullopt where neither can be read.
    [[nodiscard]] std::optional<FILE_ID_INFO> fileIdOf(HANDLE handle) noexcept
    {
        auto id = FILE_ID_INFO {};
        if (GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) != FALSE)
            return id;
        // A file system without 128-bit ids (FAT) still has the 64-bit index.
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
    auto entry = HeldDirectoryEntry { .path = std::move(*path), .identity = *identity };
    return confirmHeldDirectoryEntry(entry).transform([&entry] { return std::move(entry); });
}

std::expected<void, HeldDirectoryEntryError> confirmHeldDirectoryEntry(HeldDirectoryEntry const& entry)
{
    // The path as CreateProcessW reads it: no `\\?\` prefix, which would skip the normalization it applies.
    auto const opened = UniqueHandle { CreateFileW(entry.path.c_str(),
                                                   FILE_READ_ATTRIBUTES,
                                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                   nullptr,
                                                   OPEN_EXISTING,
                                                   FILE_FLAG_BACKUP_SEMANTICS,
                                                   nullptr) };
    if (!opened.valid())
        return std::unexpected(HeldDirectoryEntryError::Replaced);
    auto const atPath = fileIdOf(opened.get());
    if (!atPath || !isSameFile(*atPath, entry.identity))
        return std::unexpected(HeldDirectoryEntryError::Replaced);
    return {};
}

} // namespace vtpty
