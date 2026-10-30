// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The bytes of a saved host's `hosts/<name>.yml` GUI side file, written and read back as written. The loader
/// reads such a file as an entry of `hosts:` (see mergeGuiManagedSideFiles in Config.cpp).

#include <contour/config/HostUrl.hpp>

#include <optional>
#include <string>

namespace contour::config
{

/// Serializes a saved host as the exact text written to a `hosts/<name>.yml` GUI side file: a bare top-level
/// map of the keys a `hosts:` entry takes, the empty ones left out, so the reader of `hosts:` reads it back.
/// @param host The host as written.
/// @return The complete YAML document text for the side file.
[[nodiscard]] std::string emitHostYaml(HostDraft const& host);

/// Reads the bytes of a `hosts/<name>.yml` side file as written: each key's text as the file spells it,
/// unresolved, so a path such as `~/studio.token` stays as typed.
/// @param contents The file's bytes.
/// @return The host as written; nullopt when the contents are no YAML map.
[[nodiscard]] std::optional<HostDraft> parseHostDraftYaml(std::string const& contents);

} // namespace contour::config
