// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The daemon's `--access-file`: preshared tokens for the TCP listener, each with the control API grants it
/// carries, so a script can be handed a token that reads the screen but never types.
///
/// One entry per line: a grant list (`all`, or a comma list of `read`, `screen`, `input` and `write`), one
/// space, then the token to the end of the line. The token is cleaned as readTokenFile() cleans one -- `\r`
/// removed, trailing whitespace trimmed -- so interior spaces stay part of it; a line's leading whitespace is
/// dropped. Blank lines and lines starting with `#` are ignored. Its tokens are accepted beside `--token`,
/// never instead of it.

#include <core/platform/FileSystem.hpp>

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <vtcontrol/Catalog.hpp>

namespace vthost
{

/// A preshared token a native endpoint accepts, and what a connection that presents it is granted.
struct AcceptedToken
{
    std::string token {};                                              ///< The ClientHello token.
    vtcontrol::ControlGrants grants = vtcontrol::ControlGrants::all(); ///< What it grants.
};

/// @param text An access file's contents.
/// @param fullToken The `--token` the file's entries are accepted beside, which grants everything; empty for
///        none. A line repeating it is refused: the first match wins, so its narrower grants would silently
///        read as everything.
/// @return Its entries in file order, or a message naming the first line at fault: an unknown grant, an
///         empty token, a token an earlier line already has, or @p fullToken.
[[nodiscard]] std::expected<std::vector<AcceptedToken>, std::string> parseAccessFile(
    std::string_view text, std::string_view fullToken = {});

/// @param fileSystem What the file is read through.
/// @param path The access file.
/// @param fullToken The `--token` beside the file, as parseAccessFile() takes it.
/// @return Its entries, or why it cannot be read or parsed, naming the file.
[[nodiscard]] std::expected<std::vector<AcceptedToken>, std::string> readAccessFile(
    core::platform::FileSystem const& fileSystem,
    std::filesystem::path const& path,
    std::string_view fullToken = {});

} // namespace vthost
