// SPDX-License-Identifier: Apache-2.0
#include <vtpty/DirectoryHandle.hpp>
#include <vtpty/UniqueHandle.hpp>
#include <vtpty/WindowsPathRules.hpp>

#include <core/Utf8Path.hpp>

#include <algorithm>
#include <array>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <Windows.h>

namespace vtpty
{

namespace
{
    /// What a held directory is opened for: listing and entering it, reading its attributes, and waiting on
    /// it.
    constexpr DWORD HeldDirectoryAccess =
        FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE;

    /// Every sharing mode: holding a directory stops nobody from reading, writing, renaming or removing it.
    constexpr DWORD ShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

    /// A Win32 error, and the DirectoryError it is.
    struct ErrorRow
    {
        DWORD error;
        DirectoryError meaning;
    };

    /// The Win32 errors that are not AccessDenied, as the POSIX opener splits `ENOENT`/`ELOOP`/`ENAMETOOLONG`
    /// from `ENOTDIR`. A row is a new error.
    constexpr auto ErrorRows = std::to_array<ErrorRow>({
        { .error = ERROR_FILE_NOT_FOUND, .meaning = DirectoryError::Missing },
        { .error = ERROR_PATH_NOT_FOUND, .meaning = DirectoryError::Missing },
        { .error = ERROR_INVALID_NAME, .meaning = DirectoryError::Missing },
        { .error = ERROR_BAD_NETPATH, .meaning = DirectoryError::Missing },
        { .error = ERROR_BAD_NET_NAME, .meaning = DirectoryError::Missing },
        { .error = ERROR_FILENAME_EXCED_RANGE, .meaning = DirectoryError::Missing },
        { .error = ERROR_CANT_RESOLVE_FILENAME, .meaning = DirectoryError::Missing },
        { .error = ERROR_NOT_READY, .meaning = DirectoryError::Missing },
        { .error = ERROR_DIRECTORY, .meaning = DirectoryError::NotADirectory },
    });

    /// @param error A Win32 error code.
    /// @return The DirectoryError it is; AccessDenied for any not in ErrorRows.
    [[nodiscard]] DirectoryError directoryError(DWORD error) noexcept
    {
        auto const row = std::ranges::find(ErrorRows, error, &ErrorRow::error);
        return row != ErrorRows.end() ? row->meaning : DirectoryError::AccessDenied;
    }

    /// @param path A wide path.
    /// @return The absolute form GetFullPathNameW makes of it; empty where it fails.
    [[nodiscard]] std::wstring fullPathOf(std::wstring const& path)
    {
        auto const size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
        if (size == 0)
            return {};
        auto full = std::wstring(size, L'\0');
        auto const written = GetFullPathNameW(path.c_str(), size, full.data(), nullptr);
        if (written == 0 || written >= size)
            return {};
        full.resize(written);
        return full;
    }

    /// Opens what @p path names, following every link on the way; a directory as well as a file.
    /// @param path The path, UTF-8; relative to the current directory, or absolute, of any length.
    /// @param access The access the handle is opened with.
    /// @return The handle, shared for read, write and delete, not inheritable; or why it could not be opened.
    [[nodiscard]] std::expected<UniqueHandle, DirectoryError> openFollowing(std::string_view path,
                                                                            DWORD access)
    {
        auto const wide = core::pathFromUtf8(path);
        if (!wide)
            return std::unexpected(DirectoryError::AccessDenied);
        auto const full = fullPathOf(wide->native());
        if (full.empty())
            return std::unexpected(directoryError(GetLastError()));
        auto const verbatim = core::pathFromTrustedUtf8(verbatimPathOf(core::pathToUtf8(full)));
        auto handle = UniqueHandle { CreateFileW(verbatim.c_str(),
                                                 access,
                                                 ShareAll,
                                                 nullptr,
                                                 OPEN_EXISTING,
                                                 FILE_FLAG_BACKUP_SEMANTICS,
                                                 nullptr) };
        if (!handle.valid())
            return std::unexpected(directoryError(GetLastError()));
        return handle;
    }

    /// @param handle An open file or directory.
    /// @return Its normalized path, read from @p handle: UTF-8, DOS form, without the verbatim prefix; or
    ///         AccessDenied where it cannot be read.
    [[nodiscard]] std::expected<std::string, DirectoryError> finalPathOf(HANDLE handle)
    {
        constexpr DWORD Flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
        auto const size = GetFinalPathNameByHandleW(handle, nullptr, 0, Flags);
        if (size == 0)
            return std::unexpected(DirectoryError::AccessDenied);
        auto path = std::wstring(size, L'\0');
        auto const written = GetFinalPathNameByHandleW(handle, path.data(), size, Flags);
        if (written == 0 || written >= size)
            return std::unexpected(DirectoryError::AccessDenied);
        path.resize(written);
        return dosPathOf(core::pathToUtf8(std::filesystem::path { std::move(path) }));
    }

    /// @param handle An open file or directory.
    /// @return Its standard information; none where it cannot be read.
    [[nodiscard]] std::optional<FILE_STANDARD_INFO> standardInfo(HANDLE handle) noexcept
    {
        auto info = FILE_STANDARD_INFO {};
        if (GetFileInformationByHandleEx(handle, FileStandardInfo, &info, sizeof(info)) == FALSE)
            return std::nullopt;
        return info;
    }

    /// @param handle An open file or directory.
    /// @return Whether it has been removed: its deletion is pending, or no name links to it any more.
    [[nodiscard]] bool isRemovedHandle(HANDLE handle) noexcept
    {
        auto const info = standardInfo(handle);
        return info && (info->DeletePending != FALSE || info->NumberOfLinks == 0);
    }

    /// Opens directories by HANDLE, reading each one's path back from the handle.
    class NativeDirectoryOpener final: public IDirectoryOpener
    {
      public:
        [[nodiscard]] std::expected<DirectoryHandle, DirectoryError> open(std::string_view path) override
        {
            auto handle = openFollowing(path, HeldDirectoryAccess);
            if (!handle)
                return std::unexpected(handle.error());
            auto const info = standardInfo(handle->get());
            if (!info)
                return std::unexpected(DirectoryError::AccessDenied);
            if (info->Directory == FALSE)
                return std::unexpected(DirectoryError::NotADirectory);
            if (info->DeletePending != FALSE || info->NumberOfLinks == 0)
                return std::unexpected(DirectoryError::Missing);
            return finalPathOf(handle->get()).transform([&handle](std::string resolved) {
                return DirectoryHandle { std::move(*handle), std::move(resolved) };
            });
        }

        [[nodiscard]] std::expected<std::string, DirectoryError> canonical(
            std::string_view existingPath) override
        {
            return openFollowing(existingPath, FILE_READ_ATTRIBUTES).and_then([](UniqueHandle const& handle) {
                return finalPathOf(handle.get());
            });
        }

        [[nodiscard]] std::expected<DirectoryHandle, DirectoryError> createBeneath(
            DirectoryHandle const& /*parent*/, std::string_view /*name*/) override
        {
            return std::unexpected(DirectoryError::Unsupported);
        }
    };
} // namespace

std::unique_ptr<IDirectoryOpener> makeDirectoryOpener(DescriptorPaths /*paths*/)
{
    return std::make_unique<NativeDirectoryOpener>();
}

IDirectoryOpener& nativeDirectoryOpener() noexcept
{
    static auto instance = NativeDirectoryOpener {};
    return instance;
}

bool DirectoryHandle::isRemoved() const noexcept
{
    return nativeHandle() != nullptr && isRemovedHandle(nativeHandle());
}

} // namespace vtpty
