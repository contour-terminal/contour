// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where the extension host executable is installed, relative to the program that starts it.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <string_view>

namespace vtextension::wasm::remote::process
{

/// Why the extension host executable was not found.
enum class HelperLocationError : uint8_t
{
    NotFound, ///< No candidate is an executable.
};

/// The extension host executable's file name.
#ifdef _WIN32
inline constexpr std::string_view ExtensionHostName = "contour-extension-host.exe";
#else
inline constexpr std::string_view ExtensionHostName = "contour-extension-host";
#endif

/// Looks for the extension host executable where this platform installs it, beside @p selfExecutable: on
/// Unix in the private executables' directory (libexec/contour, as the install made it relative to the
/// binaries' directory), then the same directory; in a macOS bundle in Contents/Helpers first; on Windows
/// in the same directory.
/// @param selfExecutable The running program's absolute path.
/// @param isExecutable Whether a path names an executable file.
/// @return The first candidate that is one; `NotFound` if none is.
[[nodiscard]] std::expected<std::filesystem::path, HelperLocationError> locateExtensionHost(
    std::filesystem::path const& selfExecutable,
    std::function<bool(std::filesystem::path const&)> const& isExecutable);

} // namespace vtextension::wasm::remote::process
