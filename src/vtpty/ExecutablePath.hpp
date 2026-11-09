// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <system_error>

namespace vtpty
{

/// @return The running executable's absolute path, resolved (`/proc/self/exe`'s target,
///         `_NSGetExecutablePath`, `GetModuleFileNameW`); `errc::not_supported` elsewhere, FreeBSD
///         included, where what is built on it (the daemon's own images, relocation, bundled fonts) has not
///         been brought up.
[[nodiscard]] std::expected<std::filesystem::path, std::error_code> currentExecutablePath();

/// Why selfExecutablePath() cannot name the running executable, in selfExecutableCategory().
enum class SelfExecutableError : std::uint8_t
{
    /// Linux without /proc mounted: the executable is reached through /proc/self/exe and spawned through
    /// /proc/self/fd.
    NoProcFileSystem = 1,
    /// A platform with no way to name the running executable, OpenBSD among them.
    NoWayToName,
};

/// @return The category of SelfExecutableError, whose messages say what is missing.
[[nodiscard]] std::error_category const& selfExecutableCategory() noexcept;

/// @param error The reason.
/// @return @p error as an error code of selfExecutableCategory().
[[nodiscard]] std::error_code makeErrorCode(SelfExecutableError error) noexcept;

/// @return The file that runs this process's own executable again: "/proc/self/exe" on Linux, which names
///         the running image even once a package upgrade replaced it on disk, and which spawnTrampoline()
///         opens and spawns through the descriptor (/proc/self/fd/N) rather than as itself;
///         `KERN_PROC_PATHNAME` on FreeBSD and NetBSD; elsewhere currentExecutablePath(); looked up once
///         outside Linux. Or why there is none: a SelfExecutableError where what is missing is known (/proc,
///         or any way at all).
[[nodiscard]] std::expected<std::string, std::error_code> selfExecutablePath();

} // namespace vtpty
