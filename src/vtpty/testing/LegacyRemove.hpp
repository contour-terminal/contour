// SPDX-License-Identifier: Apache-2.0
//
// Test support: removing an entry the way Windows did before POSIX delete semantics, for the suites that
// check what a directory removed while another handle holds it answers.
#pragma once

#ifdef _WIN32

    #include <vtpty/UniqueHandle.hpp>

    #include <expected>
    #include <filesystem>

    #include <Windows.h>

namespace vtpty::testing
{

/// Removes @p path under legacy (non-POSIX) delete semantics (FileDispositionInfo): while any other handle
/// holds it, it stays delete-pending, still at its path, and an open of that path is refused
/// (STATUS_DELETE_PENDING, which Win32 reports as ERROR_ACCESS_DENIED); it is gone once the last handle
/// closes. `std::filesystem::remove` asks for POSIX semantics instead, which unlink the name at once.
/// @param path A file, or an empty directory.
/// @return Nothing once its deletion is pending; or the Win32 error that stopped it.
[[nodiscard]] inline std::expected<void, DWORD> removeLegacy(std::filesystem::path const& path)
{
    auto const handle = UniqueHandle { CreateFileW(path.c_str(),
                                                   DELETE,
                                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                   nullptr,
                                                   OPEN_EXISTING,
                                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                                   nullptr) };
    if (!handle.valid())
        return std::unexpected(GetLastError());
    auto disposition = FILE_DISPOSITION_INFO { .DeleteFile = TRUE };
    if (SetFileInformationByHandle(handle.get(), FileDispositionInfo, &disposition, sizeof(disposition))
        == FALSE)
        return std::unexpected(GetLastError());
    return {};
}

} // namespace vtpty::testing

#endif
