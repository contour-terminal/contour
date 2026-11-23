// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string_view>

#include <vtextension/RootPathSyntax.hpp>

namespace vtextension
{

/// The kind of a declared argument (`arg <name> = <kind>` in a version-2 manifest): what a value passed for
/// its placeholder must look like before the host starts anything with it.
enum class ArgumentKind : uint8_t
{
    Number, ///< 1–10 decimal digits.
    Uuid,   ///< A canonical, lower-case UUID.
    Name,   ///< `[A-Za-z0-9][A-Za-z0-9._-]{0,63}`.
    Ref,    ///< `[A-Za-z0-9][A-Za-z0-9._/@#+-]{0,199}`, without `..`, `//` or `@{`.
    Path,   ///< An absolute, normalised path inside its root (`path:<root>`).
    Text,   ///< Up to MaxTextArgumentBytes, without NUL, not starting with `-`.
};

/// The longest value a Text argument may have, in bytes.
inline constexpr std::size_t MaxTextArgumentBytes = std::size_t { 8 } * 1024;

namespace detail
{
    /// The most digits a Number argument has.
    inline constexpr std::size_t MaxNumberDigits = 10;

    /// The longest value a Name argument may have, in bytes.
    inline constexpr std::size_t MaxNameBytes = 64;

    /// The longest value a Ref argument may have, in bytes.
    inline constexpr std::size_t MaxRefBytes = 200;

    /// The positions of the dashes in a canonical UUID, and its length.
    inline constexpr auto UuidDashes = std::array<std::size_t, 4> { 8, 13, 18, 23 };
    inline constexpr std::size_t UuidLength = 36;

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

    /// Whether @p value is one alphanumeric character followed by at most @p maxBytes - 1 characters that
    /// are alphanumeric or among @p punctuation.
    [[nodiscard]] constexpr bool isWord(std::string_view value,
                                        std::size_t maxBytes,
                                        std::string_view punctuation) noexcept
    {
        return !value.empty() && value.size() <= maxBytes && isAlphanumeric(value.front())
               && std::ranges::all_of(value,
                                      [&](char c) { return isAlphanumeric(c) || punctuation.contains(c); });
    }

    [[nodiscard]] constexpr bool isUuid(std::string_view value) noexcept
    {
        if (value.size() != UuidLength)
            return false;
        return std::ranges::all_of(std::views::iota(std::size_t { 0 }, value.size()), [&](std::size_t index) {
            return std::ranges::contains(UuidDashes, index) ? value[index] == '-' : isLowerHex(value[index]);
        });
    }

    /// Whether @p value is absolute and normalised: every component is non-empty and neither `.` nor `..`.
    [[nodiscard]] constexpr bool isNormalisedAbsolutePath(std::string_view value) noexcept
    {
        if (value.size() > MaxPathArgumentBytes || !value.starts_with('/') || value.contains('\0'))
            return false;
        return std::ranges::all_of(std::views::split(value.substr(1), '/'), [](auto const& part) {
            auto const component = std::string_view(part.begin(), part.end());
            return !component.empty() && component != "." && component != "..";
        });
    }
} // namespace detail

/// Judges a value passed for an argument of @p kind, syntactically. A Path also needs resolving against
/// its root, which this cannot do: it only accepts the paths that can lie inside one.
/// @param kind The argument's declared kind.
/// @param value The value passed for it.
/// @return Whether @p value is one of @p kind.
[[nodiscard]] constexpr bool argumentAccepts(ArgumentKind kind, std::string_view value) noexcept
{
    switch (kind)
    {
        case ArgumentKind::Number:
            return !value.empty() && value.size() <= detail::MaxNumberDigits
                   && std::ranges::all_of(value, detail::isDigit);
        case ArgumentKind::Uuid: return detail::isUuid(value);
        case ArgumentKind::Name: return detail::isWord(value, detail::MaxNameBytes, "._-");
        case ArgumentKind::Ref:
            return detail::isWord(value, detail::MaxRefBytes, "._/@#+-") && !value.contains("..")
                   && !value.contains("//") && !value.contains("@{");
        case ArgumentKind::Path: return detail::isNormalisedAbsolutePath(value);
        case ArgumentKind::Text:
            return value.size() <= MaxTextArgumentBytes && !value.contains('\0') && !value.starts_with('-');
    }
    return false;
}

/// The prefix of the manifest word of a Path argument, which the root's label follows.
inline constexpr std::string_view PathKindPrefix = "path:";

/// One row of the table of the manifest words that name a kind without a root.
struct ArgumentKindName
{
    std::string_view word; ///< What a manifest's `arg` line says.
    ArgumentKind kind;     ///< The kind it names.
};

/// The kinds a manifest names by a single word; Path is `path:<root>` instead.
inline constexpr auto ArgumentKindNames = std::array {
    ArgumentKindName { .word = "number", .kind = ArgumentKind::Number },
    ArgumentKindName { .word = "uuid", .kind = ArgumentKind::Uuid },
    ArgumentKindName { .word = "name", .kind = ArgumentKind::Name },
    ArgumentKindName { .word = "ref", .kind = ArgumentKind::Ref },
    ArgumentKindName { .word = "text", .kind = ArgumentKind::Text },
};

/// Reads the kind a manifest's `arg` line names.
/// @param manifestWord The value of the `arg` line: `number`, `uuid`, `name`, `ref`, `text` or `path:<root>`.
/// @return The kind; Path for `path:` followed by a non-empty root label; std::nullopt for anything else.
[[nodiscard]] inline std::optional<ArgumentKind> argumentKindOf(std::string_view manifestWord) noexcept
{
    if (manifestWord.starts_with(PathKindPrefix))
        return manifestWord.size() > PathKindPrefix.size() ? std::optional { ArgumentKind::Path }
                                                           : std::nullopt;
    auto const known = std::ranges::find(ArgumentKindNames, manifestWord, &ArgumentKindName::word);
    return known != ArgumentKindNames.end() ? std::optional { known->kind } : std::nullopt;
}

} // namespace vtextension
