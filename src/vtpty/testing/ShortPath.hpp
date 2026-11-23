// SPDX-License-Identifier: Apache-2.0
//
// Test support: a path's 8.3 short spelling, the second name NTFS may give an entry, for the suites that
// check an entry reached by it is still the same one.
#pragma once

#ifdef _WIN32

    #include <filesystem>
    #include <string>

    #include <Windows.h>

namespace vtpty::testing
{

/// @param path An existing file or directory.
/// @return @p path's 8.3 spelling (GetShortPathNameW); @p path itself when it has none, as on a volume that
///         makes no 8.3 names.
[[nodiscard]] inline std::filesystem::path shortPathOf(std::filesystem::path const& path)
{
    auto shortName = std::wstring(MAX_PATH, L'\0');
    auto const length =
        GetShortPathNameW(path.c_str(), shortName.data(), static_cast<DWORD>(shortName.size()));
    if (length == 0 || length >= shortName.size())
        return path;
    shortName.resize(length);
    return std::filesystem::path { shortName };
}

} // namespace vtpty::testing

#endif
