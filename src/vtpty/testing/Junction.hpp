// SPDX-License-Identifier: Apache-2.0
//
// Test support: a directory junction, the redirection any Windows user may create (unlike a symbolic link),
// for the suites that check a path resolves through one.
#pragma once

#ifdef _WIN32

    #include <cstddef>
    #include <cstring>
    #include <expected>
    #include <filesystem>
    #include <vector>

    #include <Windows.h>
    #include <winioctl.h>

namespace vtpty::testing
{

namespace detail
{
    /// The layout of `REPARSE_DATA_BUFFER::MountPointReparseBuffer`, which `<Windows.h>` does not declare:
    /// the header of a junction's reparse data, followed by its two NUL-terminated names.
    struct MountPointHeader
    {
        DWORD reparseTag;
        WORD reparseDataLength;
        WORD reserved;
        WORD substituteNameOffset;
        WORD substituteNameLength;
        WORD printNameOffset;
        WORD printNameLength;
    };
} // namespace detail

/// Makes @p link a junction to the directory @p target: a redirection any user may create, unlike a symlink.
/// @param link Where the junction is made; must not exist yet.
/// @param target The directory it redirects to.
/// @return Nothing once it is made; or the Win32 error that stopped it.
[[nodiscard]] inline std::expected<void, DWORD> createJunction(std::filesystem::path const& link,
                                                               std::filesystem::path const& target)
{
    using detail::MountPointHeader;
    if (CreateDirectoryW(link.c_str(), nullptr) == FALSE)
        return std::unexpected(GetLastError());
    auto const handle = CreateFileW(link.c_str(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                    nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return std::unexpected(GetLastError());

    auto const printName = std::filesystem::absolute(target).native();
    auto const substituteName = L"\\??\\" + printName;
    auto const substituteBytes = substituteName.size() * sizeof(wchar_t);
    auto const printBytes = printName.size() * sizeof(wchar_t);
    auto const namesBytes = substituteBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t);
    auto const header = MountPointHeader {
        .reparseTag = IO_REPARSE_TAG_MOUNT_POINT,
        .reparseDataLength = static_cast<WORD>(
            sizeof(MountPointHeader) - offsetof(MountPointHeader, substituteNameOffset) + namesBytes),
        .reserved = 0,
        .substituteNameOffset = 0,
        .substituteNameLength = static_cast<WORD>(substituteBytes),
        .printNameOffset = static_cast<WORD>(substituteBytes + sizeof(wchar_t)),
        .printNameLength = static_cast<WORD>(printBytes),
    };
    auto buffer = std::vector<std::byte>(sizeof(MountPointHeader) + namesBytes);
    std::memcpy(buffer.data(), &header, sizeof(header));
    std::memcpy(buffer.data() + sizeof(header), substituteName.c_str(), substituteBytes + sizeof(wchar_t));
    std::memcpy(buffer.data() + sizeof(header) + header.printNameOffset,
                printName.c_str(),
                printBytes + sizeof(wchar_t));

    auto returned = DWORD { 0 };
    auto const set = DeviceIoControl(handle,
                                     FSCTL_SET_REPARSE_POINT,
                                     buffer.data(),
                                     static_cast<DWORD>(buffer.size()),
                                     nullptr,
                                     0,
                                     &returned,
                                     nullptr);
    auto const error = GetLastError();
    CloseHandle(handle);
    if (set == FALSE)
        return std::unexpected(error);
    return {};
}

} // namespace vtpty::testing

#endif
