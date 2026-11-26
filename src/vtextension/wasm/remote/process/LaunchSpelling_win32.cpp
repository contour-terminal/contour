// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/process/LaunchSpelling_win32.hpp>

#include <algorithm>
#include <cstddef>
#include <ranges>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace vtextension::wasm::remote::process
{

namespace
{
    /// @return Whether @p a and @p b are one variable's name: Windows' names ignore ASCII case.
    [[nodiscard]] bool isSameName(std::string_view a, std::string_view b) noexcept
    {
        auto const fold = [](char c) {
            return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
        };
        return std::ranges::equal(a, b, {}, fold, fold);
    }
} // namespace

std::optional<std::wstring> wideOf(std::string_view text)
{
    if (text.empty())
        return std::wstring {};
    auto const size = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size == 0)
        return std::nullopt;
    auto wide = std::wstring(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

void appendArgument(std::wstring& commandLine, std::wstring_view argument)
{
    if (!commandLine.empty())
        commandLine += L' ';
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
    {
        commandLine += argument;
        return;
    }
    commandLine += L'"';
    auto backslashes = std::size_t { 0 };
    for (auto const c: argument)
    {
        if (c == L'\\')
        {
            ++backslashes;
            continue;
        }
        // Backslashes double only before a quote; the quote itself is escaped.
        commandLine.append(c == L'"' ? (backslashes * 2) + 1 : backslashes, L'\\');
        backslashes = 0;
        commandLine += c;
    }
    commandLine.append(backslashes * 2, L'\\');
    commandLine += L'"';
}

std::optional<std::wstring> environmentBlock(std::span<EnvironmentEntry const> environment)
{
    auto entries = std::vector<std::pair<std::wstring, std::wstring>> {};
    for (auto const& [name, value]: environment)
    {
        auto wideName = wideOf(name);
        auto wideValue = wideOf(value);
        if (!wideName || !wideValue)
            return std::nullopt;
        entries.emplace_back(*std::move(wideName), *std::move(wideValue));
    }
    // The order Windows documents for a block: by name, ordinal and ignoring case, regardless of locale.
    std::ranges::sort(entries, [](auto const& a, auto const& b) {
        return ::CompareStringOrdinal(a.first.data(),
                                      static_cast<int>(a.first.size()),
                                      b.first.data(),
                                      static_cast<int>(b.first.size()),
                                      TRUE)
               == CSTR_LESS_THAN;
    });
    auto block = std::wstring {};
    for (auto const& [name, value]: entries)
    {
        block += name;
        block += L'=';
        block += value;
        block += L'\0';
    }
    // An empty block still needs two terminators.
    if (entries.empty())
        block += L'\0';
    block += L'\0';
    return block;
}

std::optional<ContainerFolders> containerFoldersOf(std::filesystem::path const& containerFolder)
{
    // <profile>\Packages\<name>\AC: two names fixed, one the container's. A trailing separator names
    // the same folder.
    auto const folder = containerFolder.has_filename() ? containerFolder : containerFolder.parent_path();
    auto const named = folder.filename();
    auto const packages = folder.parent_path().parent_path();
    auto const profile = packages.parent_path();
    auto const utf8 = [](std::filesystem::path const& path) {
        auto const text = path.u8string();
        return std::string { text.begin(), text.end() };
    };
    if (!isSameName(utf8(named), "AC") || !isSameName(utf8(packages.filename()), "Packages")
        || !folder.parent_path().has_filename() || !profile.is_absolute() || !profile.has_relative_path())
        return std::nullopt;
    return ContainerFolders { .container = folder, .profile = profile };
}

std::vector<EnvironmentEntry> containedEnvironment(std::span<EnvironmentEntry const> environment,
                                                   ContainerFolders const& folders)
{
    auto contained = std::vector<EnvironmentEntry> {};
    for (auto const& entry: environment)
        if (std::ranges::none_of(ContainerVariables, [&](auto const& variable) {
                return isSameName(entry.first, variable.name);
            }))
            contained.push_back(entry);
    for (auto const& variable: ContainerVariables)
    {
        auto const folder = [&] {
            switch (variable.source)
            {
                case ContainerVariableSource::ProfileFolder: return folders.profile;
                case ContainerVariableSource::ContainerFolder: break;
            }
            return variable.subfolder.empty() ? folders.container
                                              : folders.container / std::wstring { variable.subfolder };
        }();
        // UTF-8, as every string of the launcher's is; path::string() would spell it in the ANSI code page.
        auto const utf8 = folder.u8string();
        contained.emplace_back(std::string { variable.name }, std::string { utf8.begin(), utf8.end() });
    }
    return contained;
}

} // namespace vtextension::wasm::remote::process
