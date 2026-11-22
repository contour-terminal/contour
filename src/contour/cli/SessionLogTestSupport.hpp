// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Test support for the verbs that read a Claude Code session's log (`contour agent transcript` and
/// `contour agent stats`): a home with Claude Code's projects directory, logs held in memory, and the paths
/// the tests are written in turned into this platform's. Linked into no shipped binary.

#include <contour/cli/AgentSessionLog.hpp>

#include <algorithm>
#include <expected>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

namespace contour::cli::testing
{

using namespace std::string_view_literals;

inline constexpr auto Session = "00000000-0000-4000-8000-000000000001"sv;

// The tests and their fixtures spell paths the POSIX way; nativePath() turns them into this platform's.
inline constexpr auto Home = "/home/user"sv;
inline constexpr auto Worktree = "/home/user/proj"sv;

/// @return @p posix, a path the tests are written in, as this platform spells it: itself on POSIX; on Windows
///         `\`-separated and on drive C:, with the home `/home/user` as `C:\Users\user` (a relative path
///         becomes drive-relative, and so stays not absolute).
[[nodiscard]] inline std::string nativePath(std::string_view posix)
{
#ifdef _WIN32
    auto const inHome = posix.starts_with(Home) && (posix.size() == Home.size() || posix[Home.size()] == '/');
    auto native =
        inHome ? std::format("C:/Users/user{}", posix.substr(Home.size())) : std::format("C:{}", posix);
    std::ranges::replace(native, '/', '\\');
    return native;
#else
    return std::string { posix };
#endif
}

/// @return @p utf8 as a path, read as UTF-8 on every platform.
[[nodiscard]] inline std::filesystem::path utf8Path(std::string_view utf8)
{
    return std::filesystem::path { std::u8string { utf8.begin(), utf8.end() } };
}

/// @return Claude Code's projects directory in the configuration directory @p config, a POSIX spelling.
[[nodiscard]] inline std::filesystem::path projectsIn(std::string_view config)
{
    return utf8Path(nativePath(config)) / "projects";
}

/// @return Claude Code's projects directory in the tests' home.
[[nodiscard]] inline std::filesystem::path projectsDirectory()
{
    return utf8Path(nativePath(Home)) / ".claude" / "projects";
}

/// @return @p log with every `"cwd":"…"` value, written the POSIX way, as this platform spells it.
[[nodiscard]] inline std::string withNativeCwds(std::string_view log)
{
    constexpr auto Key = R"("cwd":")"sv;
    auto result = std::string {};
    while (true)
    {
        auto const key = log.find(Key);
        auto const end = key == std::string_view::npos ? key : log.find('"', key + Key.size());
        if (end == std::string_view::npos)
            break;
        result += log.substr(0, key + Key.size());
        for (auto const c: nativePath(log.substr(key + Key.size(), end - key - Key.size())))
            result += c == '\\' ? std::string_view { R"(\\)" } : std::string_view { &c, 1 };
        log.remove_prefix(end);
    }
    return result + std::string { log };
}

/// Hands out logs held in memory, and counts how often it was asked. Its paths are keyed lexically normal, so
/// a listing's spelling (`C:/…` from the fake file system) finds what a test added as `C:\…`.
class MemoryOpener final: public ISessionLogOpener
{
  public:
    void add(std::filesystem::path const& path, std::string content)
    {
        _logs[path.lexically_normal()] = std::move(content);
    }
    void refuse(std::filesystem::path const& path, TranscriptError error)
    {
        _errors[path.lexically_normal()] = error;
    }

    [[nodiscard]] std::expected<std::unique_ptr<std::istream>, TranscriptError> open(
        std::filesystem::path const& log) override
    {
        ++_calls;
        if (auto const error = _errors.find(log.lexically_normal()); error != _errors.end())
            return std::unexpected(error->second);
        auto const found = _logs.find(log.lexically_normal());
        if (found == _logs.end())
            return std::unexpected(TranscriptError::Unreadable);
        // A copy rather than a std::ispanstream over it: libc++ 19 (AppleClang 17's) has no <spanstream>.
        return std::make_unique<std::istringstream>(found->second);
    }

    [[nodiscard]] bool isLinkedDirectory(std::filesystem::path const& directory) override
    {
        return _linkedDirectories.contains(directory.lexically_normal());
    }

    void link(std::filesystem::path const& directory)
    {
        _linkedDirectories.insert(directory.lexically_normal());
    }

    [[nodiscard]] int calls() const noexcept { return _calls; }

  private:
    std::map<std::filesystem::path, std::string> _logs;
    std::map<std::filesystem::path, TranscriptError> _errors;
    std::set<std::filesystem::path> _linkedDirectories;
    int _calls = 0;
};

} // namespace contour::cli::testing
