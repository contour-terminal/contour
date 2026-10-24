// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <vector>

namespace text
{

/// Why registering the application's bundled fonts failed.
enum class FontRegistrationError : uint8_t
{
    DirectoryMissing = 1, ///< A directory in the list does not exist or is not a directory.
    RegistrationFailed,   ///< The platform font registry rejected a font file.
};

/// Directories holding the fonts that ship with the application (e.g. JetBrains Mono).
/// The composition root computes these from the app bundle or the install prefix and hands them in.
struct BundledFontDirectories
{
    std::vector<std::filesystem::path> paths; ///< Directories scanned for `*.ttf` files.
};

/// Makes every `*.ttf` in each of the given directories visible to font lookup for this process.
///
/// Call once from the composition root, before the first locator lookup. Every directory is attempted;
/// a missing one does not stop the others, and is reported as DirectoryMissing afterwards. Registering a
/// font that is already registered is not an error. A no-op on Windows, which ships no bundled fonts.
///
/// This is a free function called once from the composition root, not a factory parameter, because it
/// targets the process-wide OS font registry, and FontLocatorProvider is a call_once singleton: passing
/// the directories to its factory would need a static configure call, the global the design avoids.
///
/// @param directories The directories to scan.
/// @return Nothing on success, otherwise the first failure encountered.
[[nodiscard]] std::expected<void, FontRegistrationError> registerBundledFonts(
    BundledFontDirectories const& directories);

/// Lists the `*.ttf` files directly inside @p directory, sorted by name.
/// @param directory The directory to scan.
/// @return The font files, or DirectoryMissing if @p directory is not a directory.
[[nodiscard]] std::expected<std::vector<std::filesystem::path>, FontRegistrationError> listBundledFontFiles(
    std::filesystem::path const& directory);

} // namespace text
