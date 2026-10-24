// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The operating system's durability calls, behind NativeFileSystem's createNewFile(), syncFile(),
/// replaceFile() and syncDirectory().
///
/// Private: this header is in no file set, so it is not part of core::platform's public API. It
/// exists only so the one declaration is shared between NativeFileSystem.cpp and the two
/// implementations that answer it -- an operating-system difference is an implementation, never an
/// `#ifdef` inside the code that uses it (`.agent/rules/platform.md`). The errors are the operating
/// system's own; NativeFileSystem phrases them.

#include <expected>
#include <filesystem>
#include <system_error>

namespace core::platform::detail
{

/// Creates @p path empty and owner-only, failing on anything there, a symbolic link included.
/// @param path The file to create.
/// @return Nothing, or the operating system's reason.
[[nodiscard]] std::expected<void, std::error_code> createNewFile(std::filesystem::path const& path);

/// Flushes @p path's content to stable storage.
/// @param path The existing file.
/// @return Nothing, or the operating system's reason.
[[nodiscard]] std::expected<void, std::error_code> syncFile(std::filesystem::path const& path);

/// Renames @p from over @p to atomically, in one step, writing through where the platform can.
/// @param from The file to move.
/// @param to The file it replaces, if any.
/// @return Nothing, or the operating system's reason.
[[nodiscard]] std::expected<void, std::error_code> replaceFile(std::filesystem::path const& from,
                                                               std::filesystem::path const& to);

/// Flushes @p directory's entries to stable storage, where the platform can.
/// @param directory The existing directory.
/// @return Nothing, or the operating system's reason.
[[nodiscard]] std::expected<void, std::error_code> syncDirectory(std::filesystem::path const& directory);

} // namespace core::platform::detail
