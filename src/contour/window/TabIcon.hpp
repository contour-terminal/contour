// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a tab's icon shows, from its profile's `tab_icon` and the program running in its active pane.
/// Dependency-free and table-driven, so every chrome row and the tests read the same answer.

#include <contour/window/ProgramName.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace contour::window
{

/// What a tab's icon slot shows.
enum class TabIconKind : uint8_t
{
    Row = 0,  ///< The chrome row's own: the kit's tinted square on a pill, nothing on the other shapes.
    Monogram, ///< The running program's initial, in the kit's square.
    Image,    ///< The profile's image, on every shape.
    Hidden,   ///< No icon at all.
};

/// A tab's icon.
struct TabIcon
{
    TabIconKind kind = TabIconKind::Row; ///< What the slot shows.
    std::string initial;                 ///< The program's initial, for a Monogram; empty otherwise.
    std::filesystem::path image;         ///< The image, for an Image; empty otherwise.

    bool operator==(TabIcon const&) const = default;
};

/// One spelling of `tab_icon` that is not a path.
struct TabIconKeyword
{
    std::string_view token; ///< As written in the profile.
    TabIconKind kind;       ///< What it shows: Row stands for "decide from the running program".
};

/// The spellings that are not a path; anything else names an image.
inline constexpr auto TabIconKeywords = std::array {
    TabIconKeyword { .token = "auto", .kind = TabIconKind::Row },
    TabIconKeyword { .token = "", .kind = TabIconKind::Row },
    TabIconKeyword { .token = "none", .kind = TabIconKind::Hidden },
};

/// The initial a program's tab shows: the first letter or digit of its base name, upper-cased.
/// @param program The program, as a path or a name ("/usr/bin/nvim", "python3.exe").
/// @return The initial; empty when the name holds no ASCII letter or digit.
[[nodiscard]] inline std::string programInitial(std::string_view program)
{
    auto const name = programBaseName(program);
    auto const isWordChar = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0;
    };
    auto const first = std::ranges::find_if(name, isWordChar);
    if (first == name.end())
        return {};
    return std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(*first))));
}

/// Decides a tab's icon.
/// @param setting        The profile's `tab_icon`: a keyword of @c TabIconKeywords or an image path.
/// @param runningProgram The program in front of the pane's shell; empty while the shell itself is, or
///                       when no program can be named.
/// @param home           The home directory a leading `~` in an image path stands for.
/// @return The icon; the row's own for a path that is not UTF-8.
[[nodiscard]] inline TabIcon tabIconOf(std::string_view setting,
                                       std::string_view runningProgram,
                                       std::filesystem::path const& home)
{
    auto const keyword = std::ranges::find(TabIconKeywords, setting, &TabIconKeyword::token);
    if (keyword == TabIconKeywords.end())
        return core::homeResolvedPath(setting, home)
            .transform([](std::filesystem::path image) {
                return TabIcon { .kind = TabIconKind::Image, .initial = {}, .image = std::move(image) };
            })
            .value_or(TabIcon {});
    if (keyword->kind != TabIconKind::Row)
        return { .kind = keyword->kind, .initial = {}, .image = {} };
    if (auto initial = programInitial(runningProgram); !initial.empty())
        return { .kind = TabIconKind::Monogram, .initial = std::move(initial), .image = {} };
    return {};
}

} // namespace contour::window
