// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Environment.hpp>
#include <core/platform/FileSystem.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace vtextension
{

/// The installation layout a build ships its bundled status-line extensions in.
enum class BundledExtensionPlatform : uint8_t
{
    /// `$XDG_DATA_DIRS/contour/extensions` (Linux, FreeBSD and the other XDG systems), then
    /// `<exe dir>/../share/contour/extensions` for a prefix the data dirs do not list.
    Linux,
    /// `<app bundle>/Contents/Resources/extensions`, then `<exe dir>/../share/contour/extensions` for a
    /// GUI-less build, which installs no bundle.
    MacOS,
    Windows, ///< `<exe dir>/../share/contour/extensions`.
};

/// The layout of the platform this binary was built for.
/// @return The layout bundled extensions are installed in on this platform.
[[nodiscard]] constexpr BundledExtensionPlatform nativeBundledExtensionPlatform() noexcept
{
#ifdef _WIN32
    return BundledExtensionPlatform::Windows;
#elifdef __APPLE__
    return BundledExtensionPlatform::MacOS;
#else
    return BundledExtensionPlatform::Linux;
#endif
}

/// The directories bundled extensions may be installed in, most preferred first.
///
/// A pure function of its inputs: nothing is read from the process or the disk, so each platform's layout is
/// testable on any other.
///
/// @param platform The installation layout to answer for.
/// @param environment Where `XDG_DATA_DIRS` is read from (Linux layout only).
/// @param executableDirectory The directory holding the running executable.
/// @return Each candidate directory, which may or may not exist. On the Linux layout the directory beside
///         the executable comes last, also when `XDG_DATA_DIRS` is unset or lists nothing usable.
[[nodiscard]] std::vector<std::filesystem::path> bundledExtensionDirectories(
    BundledExtensionPlatform platform,
    core::Environment const& environment,
    std::filesystem::path const& executableDirectory);

/// Finds the bundled extension called @p name.
///
/// @param name The extension's name as written under `extensions:`; the module is `<name>.wasm`. A name that
///             is empty, is `.` or `..`, or contains a path separator names no bundled extension, as it would
///             otherwise reach outside the directories searched.
/// @param directories Where to look, most preferred first; see bundledExtensionDirectories().
/// @param fileSystem What decides whether a candidate module exists.
/// @return The first module found, or no value when none of @p directories holds one.
[[nodiscard]] std::optional<std::filesystem::path> resolveBundledExtension(
    std::string_view name,
    std::span<std::filesystem::path const> directories,
    core::platform::FileSystem const& fileSystem);

} // namespace vtextension
