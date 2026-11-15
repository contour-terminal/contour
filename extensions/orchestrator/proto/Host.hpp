// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Commands.hpp>
#include <orchestrator/core/Config.hpp>
#include <orchestrator/core/Run.hpp>

/// What the prototype driver reads of the machine: environment, paths, files, clocks and identifiers.
namespace orchestrator::proto
{

/// The most bytes a read file may have.
inline constexpr std::size_t MaxReadFileBytes = std::size_t { 1024 } * 1024;

/// @return This process's environment as `NAME=VALUE`, without what withheldVariables() names.
[[nodiscard]] std::vector<std::string> inheritedEnvironment();

/// Resolves what the engine's HostInfo holds: the home directory, `worktree_root` (created when missing)
/// and each `repos[].path`, `~/` expanded and symlinks resolved.
/// @param configJson The configuration, as JSON.
/// @param environment The environment `HOME` is read from.
/// @return The host's facts; or a message naming the path that could not be resolved.
[[nodiscard]] std::expected<HostInfo, std::string> resolveHost(std::string_view configJson,
                                                               std::span<std::string const> environment);

/// Reads a regular file; a symbolic link is refused, never followed.
/// @param path The file.
/// @return Its bytes, or why it could not be read.
[[nodiscard]] std::expected<std::string, FileReadError> readRegularFile(std::string const& path);

/// Reads a whole file, of any kind, for the driver's own use.
/// @param path The file.
/// @return Its bytes; std::nullopt when it cannot be read.
[[nodiscard]] std::optional<std::string> readWholeFile(std::string const& path);

/// @return A fresh random UUID from the kernel; std::nullopt when it cannot be read.
[[nodiscard]] std::optional<std::string> newSessionUuid();

/// @return The local time's offset from UTC, from `localtime_r`'s `tm_gmtoff`.
[[nodiscard]] int32_t utcOffsetMinutes() noexcept;

/// @return Now, in wall-clock milliseconds.
[[nodiscard]] uint64_t wallNowMs() noexcept;

} // namespace orchestrator::proto
