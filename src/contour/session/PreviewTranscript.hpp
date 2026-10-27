// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>

#include <format>
#include <string>
#include <string_view>

namespace contour::session
{

/// What the Settings preview terminal shows: a prompt, a command and its output, then the prompt again, in
/// the scheme's ANSI green, magenta and red so that a scheme's colours are the ones being judged.
inline constexpr std::string_view PreviewTranscript =
    "\x1b[32m~/repo/contour\x1b[0m \x1b[35m❯\x1b[0m git status --short\r\n"
    "\x1b[31m M\x1b[0m src/contour/config/Config.cpp\r\n"
    "\x1b[32m~/repo/contour\x1b[0m \x1b[35m❯\x1b[0m ";

/// The size of the preview terminal: the transcript's three lines with room for a status line.
inline constexpr vtbackend::PageSize PreviewPageSize { vtbackend::LineCount(6), vtbackend::ColumnCount(80) };

/// The line that says which font and scheme the preview shows.
/// @param family    The font family of the previewed profile.
/// @param pointSize Its size in points; a whole size is written without decimals.
/// @param scheme    The colour scheme's name.
/// @return "Live preview — {family} {size} · {scheme}".
[[nodiscard]] inline std::string previewCaption(std::string_view family,
                                                double pointSize,
                                                std::string_view scheme)
{
    return std::format("Live preview — {} {} · {}", family, pointSize, scheme);
}

} // namespace contour::session
