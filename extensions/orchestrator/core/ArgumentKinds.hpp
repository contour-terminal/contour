// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string_view>

#include <orchestrator/core/HostPaths.hpp>

namespace orchestrator
{

/// What a declared argument may hold. The host checks every value against its kind before it starts
/// anything, so a guest cannot pass `ext::sh -c …` as a git remote or an option where a value belongs.
enum class ArgumentKind : uint8_t
{
    Number, ///< 1 to MaxNumberDigits decimal digits.
    Uuid,   ///< A canonical, lower-case UUID.
    Name,   ///< `[A-Za-z0-9][A-Za-z0-9._-]`, at most MaxNameBytes.
    Ref,    ///< `[A-Za-z0-9][A-Za-z0-9._/@#+-]`, at most MaxRefBytes, without `..`, `//` or `@{`.
    Path,   ///< An absolute, normalised path; the host also resolves it inside the argument's root.
    Text,   ///< At most MaxTextBytes, no NUL, not starting with '-'.
};

/// The directories a command, a pane or a path argument is bound to.
enum class Root : uint8_t
{
    Repos,     ///< The user's repositories, one per configured repository.
    Worktrees, ///< The worktree root, under which every agent's worktree lies.
};

/// How long the host lets a declared command run.
enum class Timeout : uint8_t
{
    Short, ///< 30 seconds.
    Long,  ///< 10 minutes.
};

/// The most digits a Number may have.
inline constexpr std::size_t MaxNumberDigits = 10;
/// The most bytes a Name may have.
inline constexpr std::size_t MaxNameBytes = 64;
/// The most bytes a Ref may have.
inline constexpr std::size_t MaxRefBytes = 200;
/// The most bytes a Path may have, the longest path a POSIX system has to accept.
inline constexpr std::size_t MaxPathBytes = 4096;
/// The most bytes a Text may have.
inline constexpr std::size_t MaxTextBytes = 8192;

namespace detail
{
    [[nodiscard]] constexpr bool isDigit(char c) noexcept
    {
        return c >= '0' && c <= '9';
    }

    [[nodiscard]] constexpr bool isAlphanumeric(char c) noexcept
    {
        return isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }

    [[nodiscard]] constexpr bool isLowerHex(char c) noexcept
    {
        return isDigit(c) || (c >= 'a' && c <= 'f');
    }

    [[nodiscard]] constexpr bool isNameChar(char c) noexcept
    {
        return isAlphanumeric(c) || c == '.' || c == '_' || c == '-';
    }

    [[nodiscard]] constexpr bool isRefChar(char c) noexcept
    {
        return isNameChar(c) || c == '/' || c == '@' || c == '#' || c == '+';
    }

    /// Whether @p value is 1 to @p maxBytes bytes, starts alphanumeric and continues in @p isTail.
    [[nodiscard]] constexpr bool isWord(std::string_view value,
                                        std::size_t maxBytes,
                                        bool (*isTail)(char)) noexcept
    {
        return !value.empty() && value.size() <= maxBytes && isAlphanumeric(value.front())
               && std::ranges::all_of(value.substr(1), isTail);
    }

    [[nodiscard]] constexpr bool isUuid(std::string_view value) noexcept
    {
        constexpr auto Dashes = std::array<std::size_t, 4> { 8, 13, 18, 23 };
        if (value.size() != 36)
            return false;
        return std::ranges::all_of(std::views::iota(std::size_t { 0 }, value.size()), [&](std::size_t i) {
            return std::ranges::contains(Dashes, i) ? value[i] == '-' : isLowerHex(value[i]);
        });
    }

    /// The device names Win32 reserves in every directory, by their stems; the superscript digits are UTF-8.
    inline constexpr auto ReservedWindowsStems = std::to_array<std::string_view>({
        "CON",  "PRN",         "AUX",         "NUL",         "CONIN$",      "CONOUT$", "COM0",
        "COM1", "COM2",        "COM3",        "COM4",        "COM5",        "COM6",    "COM7",
        "COM8", "COM9",        "COM\xc2\xb9", "COM\xc2\xb2", "COM\xc2\xb3", "LPT0",    "LPT1",
        "LPT2", "LPT3",        "LPT4",        "LPT5",        "LPT6",        "LPT7",    "LPT8",
        "LPT9", "LPT\xc2\xb9", "LPT\xc2\xb2", "LPT\xc2\xb3",
    });

    [[nodiscard]] constexpr char asciiUpper(char c) noexcept
    {
        return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    }

    /// Whether @p name is one component Win32 addresses by exactly this spelling, by the host's rule: not
    /// empty, `.` or `..`; none of `<>:"/\|?*` or a control character; not ending in `.` or ` `; its stem (up
    /// to the first `.`, trailing spaces dropped) not a reserved device name, ASCII case folded.
    [[nodiscard]] constexpr bool isPlainWindowsName(std::string_view name) noexcept
    {
        constexpr auto Forbidden = std::string_view { R"(<>:"/\|?*)" };
        if (name.empty() || name == "." || name == ".." || name.back() == '.' || name.back() == ' ')
            return false;
        if (std::ranges::any_of(
                name, [&](char c) { return Forbidden.contains(c) || static_cast<unsigned char>(c) < 0x20; }))
            return false;
        auto stem = name.substr(0, name.find('.'));
        stem = stem.substr(0, stem.find_last_not_of(' ') + 1);
        return std::ranges::none_of(ReservedWindowsStems, [stem](std::string_view row) {
            return std::ranges::equal(stem, row, std::ranges::equal_to {}, asciiUpper, asciiUpper);
        });
    }

    [[nodiscard]] constexpr bool isPlainPosixName(std::string_view name) noexcept
    {
        return !name.empty() && name != "." && name != "..";
    }

    /// Whether @p part, split at @p separator, is a run of components each @p isPlain accepts.
    [[nodiscard]] constexpr bool arePlain(std::string_view part,
                                          char separator,
                                          bool (*isPlain)(std::string_view) noexcept) noexcept
    {
        return std::ranges::all_of(std::views::split(part, separator), [&](auto const& component) {
            return isPlain(std::string_view(component.begin(), component.end()));
        });
    }

    /// Whether @p value is absolute and normalised, in the style its spelling names (the host judges it
    /// again in its own). Posix: `/` and then `/`-separated components, none empty, `.` or `..`. Windows:
    /// `X:\` alone, or `X:\` or `\\server\share` and then `\`-separated components, each of them, the server
    /// and the share among them, isPlainWindowsName(); so a `/` or a trailing `\` is refused there.
    [[nodiscard]] constexpr bool isNormalisedAbsolute(std::string_view value) noexcept
    {
        constexpr auto UncPrefix = std::string_view { R"(\\)" };
        constexpr auto DriveRootBytes = std::size_t { 3 };
        if (value.size() < 2 || value.size() > MaxPathBytes || value.contains('\0'))
            return false;
        switch (pathStyleOf(value).value_or(PathStyle::Posix))
        {
            case PathStyle::Posix:
                return value.front() == '/' && arePlain(value.substr(1), '/', isPlainPosixName);
            case PathStyle::Windows:
                if (value.starts_with(UncPrefix))
                    return value.substr(UncPrefix.size()).contains('\\')
                           && arePlain(value.substr(UncPrefix.size()), '\\', isPlainWindowsName);
                return value.size() == DriveRootBytes
                       || arePlain(value.substr(DriveRootBytes), '\\', isPlainWindowsName);
        }
        return false;
    }
} // namespace detail

/// Whether @p c may stand in a URL the extension passes on: printable ASCII other than a space or a quote. A
/// forge spells anything else percent-encoded.
/// @param c One byte of a URL.
/// @return Whether it may stand there.
[[nodiscard]] constexpr bool isUrlByte(char c) noexcept
{
    return c > ' ' && c < '\x7f' && c != '"' && c != '\'' && c != '`';
}

/// Judges @p value syntactically against @p kind.
///
/// A Path additionally needs the host's resolution against its root: that it lies inside the root is not
/// decided here.
/// @param kind The kind the argument is declared with.
/// @param value The value a call passes for it.
/// @return Whether the value is acceptable for that kind.
[[nodiscard]] constexpr bool argumentAccepts(ArgumentKind kind, std::string_view value) noexcept
{
    switch (kind)
    {
        case ArgumentKind::Number:
            return !value.empty() && value.size() <= MaxNumberDigits
                   && std::ranges::all_of(value, detail::isDigit);
        case ArgumentKind::Uuid: return detail::isUuid(value);
        case ArgumentKind::Name: return detail::isWord(value, MaxNameBytes, detail::isNameChar);
        case ArgumentKind::Ref:
            return detail::isWord(value, MaxRefBytes, detail::isRefChar) && !value.contains("..")
                   && !value.contains("//") && !value.contains("@{");
        case ArgumentKind::Path: return detail::isNormalisedAbsolute(value);
        case ArgumentKind::Text:
            return value.size() <= MaxTextBytes && !value.contains('\0') && !value.starts_with('-');
    }
    return false;
}

/// The manifest's spelling of @p kind; a Path is followed by `:` and its root's label.
/// @param kind The argument kind.
/// @return Its label, such as "number".
[[nodiscard]] constexpr std::string_view manifestLabel(ArgumentKind kind) noexcept
{
    switch (kind)
    {
        case ArgumentKind::Number: return "number";
        case ArgumentKind::Uuid: return "uuid";
        case ArgumentKind::Name: return "name";
        case ArgumentKind::Ref: return "ref";
        case ArgumentKind::Path: return "path";
        case ArgumentKind::Text: return "text";
    }
    return {};
}

/// The manifest's spelling of @p root.
/// @param root The root.
/// @return Its label, "repos" or "worktrees".
[[nodiscard]] constexpr std::string_view manifestLabel(Root root) noexcept
{
    switch (root)
    {
        case Root::Repos: return "repos";
        case Root::Worktrees: return "worktrees";
    }
    return {};
}

/// The manifest's spelling of @p timeout.
/// @param timeout The timeout class.
/// @return Its label, "short" or "long".
[[nodiscard]] constexpr std::string_view manifestLabel(Timeout timeout) noexcept
{
    switch (timeout)
    {
        case Timeout::Short: return "short";
        case Timeout::Long: return "long";
    }
    return {};
}

} // namespace orchestrator
