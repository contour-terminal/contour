// SPDX-License-Identifier: Apache-2.0
//
// Test support: a directory whose canonical path is exactly as long as a test asks, past MAX_PATH if need be,
// for the suites that check where a path stops being a current directory a Windows process may have.
#pragma once

#ifdef _WIN32

    #include <cstddef>
    #include <expected>
    #include <ranges>
    #include <string>
    #include <utility>
    #include <vector>

    #include <Windows.h>

namespace vtpty::testing
{

/// A chain of nested directories, made through `\\?\` so that no length limit applies, removed (deepest
/// first) when it goes out of scope.
class LongDirectory
{
  public:
    /// Makes directories beneath @p base until the deepest one's path is @p units UTF-16 units long.
    /// @param base A local directory's canonical DOS path (`C:\…`), with no trailing separator.
    /// @param units The length the deepest path is to have; longer than @p base by at least two.
    /// @param fill The character every component is spelled with, so that two chains beneath one base differ.
    /// @return The chain; or the Win32 error that stopped it (what was made is removed).
    [[nodiscard]] static std::expected<LongDirectory, DWORD> make(std::wstring const& base,
                                                                  std::size_t units,
                                                                  wchar_t fill)
    {
        // Short enough that each component, and the remainder left after it, is a legal name.
        constexpr auto Component = std::size_t { 100 };
        auto chain = LongDirectory {};
        auto path = base;
        while (path.size() < units)
        {
            auto const remaining = units - path.size();
            // A whole component only where at least two units (a separator and a name) are left after it.
            auto const length = remaining > Component + 2 ? Component : remaining - 1;
            path += L'\\';
            path.append(length, fill);
            auto verbatim = L"\\\\?\\" + path;
            if (CreateDirectoryW(verbatim.c_str(), nullptr) == FALSE)
                return std::unexpected(GetLastError());
            chain._created.push_back(std::move(verbatim));
        }
        chain._path = std::move(path);
        return chain;
    }

    LongDirectory(LongDirectory const&) = delete;
    LongDirectory& operator=(LongDirectory const&) = delete;
    LongDirectory(LongDirectory&& other) noexcept:
        _created { std::exchange(other._created, {}) }, _path { std::move(other._path) }
    {
    }
    LongDirectory& operator=(LongDirectory&&) = delete;
    ~LongDirectory()
    {
        for (auto const& created: _created | std::views::reverse)
            RemoveDirectoryW(created.c_str());
    }

    /// @return The deepest directory's path, without the `\\?\` prefix.
    [[nodiscard]] std::wstring const& path() const noexcept { return _path; }

  private:
    LongDirectory() = default;

    std::vector<std::wstring> _created; ///< Every directory made, outermost first, `\\?\`-prefixed.
    std::wstring _path;
};

} // namespace vtpty::testing

#endif
