// SPDX-License-Identifier: Apache-2.0
#include <core/platform/PathUtils.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
    #include <cwctype>
    #include <memory>
    #include <type_traits>

    #include <windows.h>
#endif

namespace core::platform
{

std::string stripTrailingSeparator(std::filesystem::path const& path)
{
    // normalizePath() rather than generic_string(): the latter narrows to the platform's native
    // narrow encoding, which on Windows is the ANSI code page -- it mangles any path the code
    // page cannot spell, and MSVC's implementation throws outright. normalizePath()'s own
    // declaration says so. It matters more here than anywhere: this result keys
    // InMemoryFileSystem's whole file map, so two paths collapsing onto one spelling is a
    // fixture quietly answering with another file's content.
    auto const normal = path.lexically_normal();
    auto text = normalizePath(normal);
    // Never strip into the root itself: "/" stays "/" and, on Windows, "C:/" stays "C:/"
    // (reducing it to "C:" would name the drive's current directory, not its root). Measured
    // from the path, not from a path rebuilt out of `text`, which would narrow it right back:
    // std::filesystem::path reads a narrow string in that same native encoding.
    auto const rootLength =
        std::max<std::size_t>(normalizePath(normal.root_path()).size(), std::size_t { 1 });
    while (text.size() > rootLength && text.back() == '/')
        text.pop_back();
    return text;
}

bool isCaseOnlyRename(std::filesystem::path const& from, std::filesystem::path const& to)
{
    // Both paths reduced to their canonical lexical spelling, with any trailing separator
    // dropped, so that `foo/` and `foo` are treated identically. The spellings are then split
    // as strings rather than rebuilt into a std::filesystem::path, which would read them back in
    // the native narrow encoding and undo what stripTrailingSeparator() just spelled in UTF-8.
    // The result is forward-slash normalized, so the last '/' separates parent from final
    // component.
    auto const source = stripTrailingSeparator(from);
    auto const dest = stripTrailingSeparator(to);

    auto const split = [](std::string const& text) {
        auto const view = std::string_view { text };
        auto const slash = view.rfind('/');
        return slash == std::string_view::npos ? std::pair { std::string_view {}, view }
                                               : std::pair { view.substr(0, slash), view.substr(slash + 1) };
    };
    auto const [sourceParent, sourceName] = split(source);
    auto const [destParent, destName] = split(dest);

    if (sourceName.empty() || destName.empty())
        return false;
    if (sourceName == destName)
        return false; // Identical spelling — a no-op, not a recase.
    if (sourceParent != destParent)
        return false;

    return equalsCaseInsensitive(sourceName, destName);
}

auto canonicalCasePath(std::filesystem::path const& p) -> std::string
{
#ifdef _WIN32
    auto const native = p.wstring();
    if (native.empty())
        return normalizePath(p);

    // Open a handle to the path so its canonical, correctly-cased name can be queried.
    // GetLongPathNameW only expands 8.3 short names — it leaves already-long components
    // in whatever case they were passed — so it cannot fix the casing callers need.
    // FILE_FLAG_BACKUP_SEMANTICS is required to obtain a handle to a directory.
    auto* const rawHandle = CreateFileW(native.c_str(),
                                        0,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS,
                                        nullptr);
    if (rawHandle == INVALID_HANDLE_VALUE)
        return normalizePath(p); // Non-existent path or access failure: fall back.

    // Own the handle via RAII so it is closed on every return path (including exceptions).
    auto const handle =
        std::unique_ptr<std::remove_pointer_t<HANDLE>, decltype(&CloseHandle)>(rawHandle, &CloseHandle);

    auto const flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    auto const needed = GetFinalPathNameByHandleW(handle.get(), nullptr, 0, flags);
    if (needed == 0)
        return normalizePath(p);

    auto buffer = std::wstring(needed, L'\0');
    auto const written = GetFinalPathNameByHandleW(handle.get(), buffer.data(), needed, flags);
    if (written == 0 || written >= needed)
        return normalizePath(p);
    buffer.resize(written);

    // GetFinalPathNameByHandleW returns an extended-length path. Strip the prefix:
    //   \\?\C:\dir          -> C:\dir
    //   \\?\UNC\server\share -> \\server\share
    auto view = std::wstring_view(buffer);
    auto canonical = std::wstring {};
    if (view.starts_with(LR"(\\?\UNC\)"))
        canonical = L"\\\\" + std::wstring(view.substr(8));
    else if (view.starts_with(LR"(\\?\)"))
        canonical = std::wstring(view.substr(4));
    else
        canonical = std::wstring(view);

    // The DOS volume name already comes back upper-cased, but normalize it defensively.
    if (canonical.size() >= 2 && canonical[1] == L':' && std::iswalpha(static_cast<wint_t>(canonical[0])))
        canonical[0] = static_cast<wchar_t>(std::towupper(static_cast<wint_t>(canonical[0])));

    return normalizePath(std::filesystem::path(canonical));
#else
    return normalizePath(p);
#endif
}

} // namespace core::platform
