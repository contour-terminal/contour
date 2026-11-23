// SPDX-License-Identifier: Apache-2.0
#include <vtpty/WindowsProcessRules.hpp>

#include <vtpty/AsciiFold.hpp>

#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vtpty
{

namespace
{
    /// The characters that end a directory component in a Win32 path.
    constexpr auto Separators = std::string_view { R"(\/)" };

    /// The characters a relative program path may not carry: a directory, or a drive.
    constexpr auto DirectoryOrDriveCharacters = std::string_view { R"(\/:)" };

    /// The prefix of a UNC path, and of the verbatim form of one.
    constexpr auto UncPrefix = std::string_view { R"(\\)" };

    /// @param text A name, UTF-8.
    /// @return It with its ASCII letters folded to lower case. Windows folds Unicode too, which no real
    ///         environment variable name needs, so the fold is ASCII only.
    [[nodiscard]] std::string asciiFold(std::string_view text)
    {
        auto folded = std::string { text };
        for (auto& c: folded)
            c = asciiLower(c);
        return folded;
    }

    /// @param path A path, UTF-8.
    /// @return Its last component: what follows its last separator, or the whole of it where it has none.
    [[nodiscard]] std::string_view lastComponentOf(std::string_view path) noexcept
    {
        auto const cut = path.find_last_of(Separators);
        return cut == std::string_view::npos ? path : path.substr(cut + 1);
    }
} // namespace

bool isAbsoluteWindowsPath(std::string_view path) noexcept
{
    // A drive, its colon, then a separator: `C:\x` or `C:/x`. `C:x` is relative to the drive's directory.
    auto const isDriveAbsolute =
        path.size() >= 3 && isAsciiLetter(path[0]) && path[1] == ':' && Separators.contains(path[2]);
    // A UNC path, `\\server\share`, or a verbatim one, `\\?\…`: anything after the two separators.
    auto const isUncAbsolute = path.starts_with(UncPrefix) && path.size() > UncPrefix.size();
    return isDriveAbsolute || isUncAbsolute;
}

std::optional<std::string> windowsImageName(std::string_view program)
{
    auto const name = lastComponentOf(program);
    if (name.empty())
        return std::nullopt;
    auto const dot = name.find_last_of('.');
    if (dot == std::string_view::npos)
        return std::string { program } + std::string { WindowsImageExtensions.front() };
    auto const extension = name.substr(dot);
    for (auto const allowed: WindowsImageExtensions)
        if (equalsIgnoringAsciiCase(extension, allowed))
            return std::string { program };
    return std::nullopt;
}

std::optional<std::string> findWindowsHeldDirectoryProgram(std::string_view program,
                                                           std::string_view searchPath,
                                                           ExecutableTest const& isExecutable)
{
    auto image = windowsImageName(program);
    if (!image)
        return std::nullopt;

    if (isAbsoluteWindowsPath(*image))
    {
        if (!isExecutable(*image))
            return std::nullopt;
        return image;
    }

    // A relative path naming a directory would resolve against the held directory: refused, see the header.
    if (image->find_first_of(DirectoryOrDriveCharacters) != std::string::npos)
        return std::nullopt;

    for (auto const entry: std::views::split(searchPath, ';'))
    {
        auto const directory = std::string_view { std::ranges::data(entry), std::ranges::size(entry) };
        if (!isAbsoluteWindowsPath(directory))
            continue;
        auto candidate = std::string { directory };
        if (!candidate.ends_with('\\') && !candidate.ends_with('/'))
            candidate += '\\';
        candidate += *image;
        if (isExecutable(candidate))
            return candidate;
    }
    return std::nullopt;
}

std::vector<std::string> windowsEnvironmentEntries(
    std::span<std::pair<std::string, std::string> const> environment)
{
    // Keyed by the folded name, so the later of two spellings overwrites the earlier one.
    auto byName = std::map<std::string, std::string> {};
    for (auto const& [name, value]: environment)
        byName.insert_or_assign(asciiFold(name), std::string { name } + '=' + value);

    auto entries = std::vector<std::string> {};
    entries.reserve(byName.size());
    std::ranges::move(byName | std::views::values, std::back_inserter(entries));
    return entries;
}

std::string windowsSearchPathOf(std::span<std::pair<std::string, std::string> const> environment)
{
    auto searchPath = std::string {};
    for (auto const& [name, value]: environment)
        if (equalsIgnoringAsciiCase(name, "PATH"))
            searchPath = value;
    return searchPath;
}

} // namespace vtpty
