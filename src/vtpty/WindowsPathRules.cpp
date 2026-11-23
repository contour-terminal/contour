// SPDX-License-Identifier: Apache-2.0
#include <vtpty/WindowsPathRules.hpp>

#include <vtpty/AsciiFold.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <string>
#include <string_view>

namespace vtpty
{

namespace
{
    /// The device names Win32 reserves in every directory, by their stems. A row is a new name; the
    /// superscript digits are UTF-8, and match byte for byte.
    constexpr auto ReservedStems = std::to_array<std::string_view>({
        "CON",  "PRN",         "AUX",         "NUL",         "CONIN$",      "CONOUT$", "COM0",
        "COM1", "COM2",        "COM3",        "COM4",        "COM5",        "COM6",    "COM7",
        "COM8", "COM9",        "COM\xc2\xb9", "COM\xc2\xb2", "COM\xc2\xb3", "LPT0",    "LPT1",
        "LPT2", "LPT3",        "LPT4",        "LPT5",        "LPT6",        "LPT7",    "LPT8",
        "LPT9", "LPT\xc2\xb9", "LPT\xc2\xb2", "LPT\xc2\xb3",
    });

    /// The characters a Win32 file name may not hold.
    constexpr auto ForbiddenCharacters = std::string_view { "<>:\"/\\|?*" };

    /// The verbatim prefix of a DOS path, and of a UNC one.
    constexpr auto VerbatimPrefix = std::string_view { R"(\\?\)" };
    constexpr auto VerbatimUncPrefix = std::string_view { R"(\\?\UNC\)" };
    constexpr auto DevicePrefix = std::string_view { R"(\\.\)" };
    constexpr auto UncPrefix = std::string_view { R"(\\)" };

    /// Whether @p stem is one of the reserved device names, compared ASCII-case-insensitively.
    [[nodiscard]] bool isReservedStem(std::string_view stem) noexcept
    {
        return std::ranges::any_of(
            ReservedStems, [stem](std::string_view row) { return equalsIgnoringAsciiCase(stem, row); });
    }
} // namespace

bool isPlainWindowsName(std::string_view name) noexcept
{
    if (name.empty() || name == "." || name == "..")
        return false;
    if (std::ranges::any_of(name, [](char c) {
            return ForbiddenCharacters.contains(c) || static_cast<unsigned char>(c) < 0x20;
        }))
        return false;
    if (name.back() == '.' || name.back() == ' ')
        return false;

    // The stem Win32 matches against the device names: up to the first dot, without trailing spaces.
    auto stem = name.substr(0, name.find('.'));
    auto const lastNonSpace = stem.find_last_not_of(' ');
    stem = (lastNonSpace == std::string_view::npos) ? std::string_view {} : stem.substr(0, lastNonSpace + 1);
    return !isReservedStem(stem);
}

std::string dosPathOf(std::string_view finalPath)
{
    if (finalPath.starts_with(VerbatimUncPrefix))
        return std::string(UncPrefix) + std::string(finalPath.substr(VerbatimUncPrefix.size()));
    if (finalPath.starts_with(VerbatimPrefix))
        return std::string(finalPath.substr(VerbatimPrefix.size()));
    return std::string(finalPath);
}

std::string verbatimPathOf(std::string_view fullPath)
{
    if (fullPath.starts_with(VerbatimPrefix) || fullPath.starts_with(DevicePrefix))
        return std::string(fullPath);
    if (fullPath.starts_with(UncPrefix))
        return std::string(VerbatimUncPrefix) + std::string(fullPath.substr(UncPrefix.size()));
    return std::string(VerbatimPrefix) + std::string(fullPath);
}

bool isFileSystemPath(std::string_view fullPath) noexcept
{
    if (fullPath.starts_with(DevicePrefix))
        return false;
    if (fullPath.starts_with(VerbatimUncPrefix))
        return true;
    if (!fullPath.starts_with(VerbatimPrefix))
        return true;
    // `\\?\X:\`: a drive letter, a colon and the root. `\\?\X:` alone is the volume, not its root.
    auto const rest = fullPath.substr(VerbatimPrefix.size());
    return rest.size() >= 3 && isAsciiLetter(rest[0]) && rest[1] == ':' && rest[2] == '\\';
}

} // namespace vtpty
