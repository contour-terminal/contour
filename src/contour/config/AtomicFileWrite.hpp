// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/FileSystem.hpp>

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace contour::config
{

/// Replaces @p path with @p content atomically: writes a temp sibling, then renames it over the target.
///
/// Every machine-written file Contour owns goes through here (`layouts.yml`, `command-history.yml`),
/// because an interrupted or failing write must never leave a TRUNCATED file behind — a half-written
/// YAML file does not parse, and a store the program cannot read back is one the user has to go and
/// delete by hand before their layouts (or their command history) work again.
///
/// The two subtleties this centralizes, both of which are easy to get subtly wrong twice:
///   - the stream is CLOSED before its state is checked, because close() is what flushes the buffer and
///     therefore what surfaces a write error (disk full, quota) as failbit. Checking before the flush
///     would miss it, and the rename would then swap a good file for a truncated one — the very loss
///     this exists to prevent;
///   - the temp file is removed on every failure path, so a failing save cannot litter the config
///     directory with `.tmp` leftovers.
///
/// @param path    The file to replace. Its parent directory is created if missing.
/// @param content The bytes to write.
/// @return Nothing on success, or a human-readable error describing why the write failed.
[[nodiscard]] std::expected<void, std::string> atomicWriteFile(std::filesystem::path const& path,
                                                               std::string_view content);

/// Replaces @p path with @p content durably, through @p fileSystem: the content survives a crash or a
/// power loss, and a reader sees either the old file or the new one, never a mix.
///
/// The sequence core::platform::FileSystem documents under "Durability": create the parent directory,
/// drop a temporary `.<name>.tmp` a crash left behind, create the temporary exclusively (so a link
/// planted there is not followed), write it, sync it, replace @p path with it, and sync the directory.
/// The temporary is removed on every failure, so a refused write leaves the old file and nothing else.
///
/// @param fileSystem The file system @p path lives on.
/// @param path       The file to replace. Its parent directory is created if missing.
/// @param content    The bytes to write.
/// @return Nothing on success, or the file system's reason the write failed.
[[nodiscard]] std::expected<void, std::string> writeFileDurably(core::platform::FileSystem const& fileSystem,
                                                                std::filesystem::path const& path,
                                                                std::string_view content);

} // namespace contour::config
