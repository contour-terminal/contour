// SPDX-License-Identifier: Apache-2.0
#include <vtpty/EnvironmentBlock.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <set>

#ifdef _WIN32
    #include <core/WideString.hpp>

    #include <Windows.h>
#endif

namespace vtpty
{

namespace
{
    /// @return The name of @p entry ("NAME=VALUE"): what precedes the first '=' after its first character,
    ///         since Windows names the directory of each drive "=C:" and so on, as in "=C:=C:\dir".
    [[nodiscard]] std::string_view nameOf(std::string_view entry) noexcept
    {
        return entry.substr(0, std::min(entry.find('=', 1), entry.size()));
    }

    /// @return @p name as it compares under @p names: itself, or its ASCII letters in upper case.
    [[nodiscard]] std::string comparableName(std::string_view name, EnvironmentNames names)
    {
        switch (names)
        {
            case EnvironmentNames::CaseSensitive: return std::string { name };
            case EnvironmentNames::CaseInsensitive: return core::toUpper(name);
        }
        return std::string { name };
    }

    /// @return The value of @p entry ("NAME=VALUE"); std::nullopt for an entry without one.
    [[nodiscard]] std::optional<std::string_view> valueOf(std::string_view entry) noexcept
    {
        auto const name = nameOf(entry);
        if (name.size() == entry.size())
            return std::nullopt;
        return entry.substr(name.size() + 1);
    }

    /// @return The value @p name has in @p entries, compared as @p names says; std::nullopt where it has
    /// none.
    template <typename Entries>
    [[nodiscard]] std::optional<std::string_view> findValue(Entries const& entries,
                                                            std::string_view name,
                                                            EnvironmentNames names)
    {
        auto const wanted = comparableName(name, names);
        for (std::string_view const entry: entries)
            if (comparableName(nameOf(entry), names) == wanted)
                return valueOf(entry);
        return std::nullopt;
    }
} // namespace

std::vector<std::string> buildEnvironmentBlock(std::span<std::string_view const> inherited,
                                               std::map<std::string, std::string> const& overrides,
                                               std::span<std::string const> removed,
                                               EnvironmentNames names)
{
    auto dropped = std::set<std::string> {};
    for (auto const& name: overrides | std::views::keys)
        dropped.insert(comparableName(name, names));
    for (auto const& name: removed)
        dropped.insert(comparableName(name, names));

    auto entries = std::vector<std::string> {};
    for (auto const line: inherited)
        if (!dropped.contains(comparableName(nameOf(line), names)))
            entries.emplace_back(line);
    for (auto const& [name, value]: overrides)
        entries.emplace_back(std::format("{}={}", name, value));
    return entries;
}

std::vector<std::string> buildWindowsEnvironment(std::span<std::string_view const> inherited,
                                                 std::map<std::string, std::string> const& configured,
                                                 std::span<std::string const> removed)
{
    constexpr auto Names = EnvironmentNames::CaseInsensitive;
    auto const inheritedPath = findValue(inherited, "PATH", Names);

    auto overrides = std::map<std::string, std::string> {};
    auto dropped = std::vector<std::string> { removed.begin(), removed.end() };
    for (auto const& [name, value]: configured)
    {
        auto const comparable = comparableName(name, Names);
        if (comparable == "TERM")
            continue;
        if (value.empty())
            dropped.push_back(name);
        else if (comparable == "PATH" && inheritedPath)
            overrides[name] = std::format("{};{}", value, *inheritedPath);
        else
            overrides[name] = value;
    }
    return buildEnvironmentBlock(inherited, overrides, dropped, Names);
}

std::optional<std::string_view> environmentValue(std::span<std::string const> entries,
                                                 std::string_view name,
                                                 EnvironmentNames names)
{
    return findValue(entries, name, names);
}

#ifdef _WIN32
std::optional<std::wstring> windowsEnvironmentBlock(std::span<std::string const> entries)
{
    auto wideEntries = std::vector<std::wstring> {};
    for (auto const& entry: entries)
    {
        auto wide = core::toUtf16(entry);
        if (!wide)
            return std::nullopt;
        wideEntries.push_back(std::move(*wide));
    }

    // CompareStringOrdinal() with case ignored is the comparison Windows sorts its own blocks by.
    auto const wideNameOf = [](std::wstring const& entry) {
        return std::wstring_view { entry }.substr(0, std::min(entry.find(L'=', 1), entry.size()));
    };
    std::ranges::sort(wideEntries, [&](std::wstring const& a, std::wstring const& b) {
        auto const nameA = wideNameOf(a);
        auto const nameB = wideNameOf(b);
        return CompareStringOrdinal(nameA.data(),
                                    static_cast<int>(nameA.size()),
                                    nameB.data(),
                                    static_cast<int>(nameB.size()),
                                    TRUE)
               == CSTR_LESS_THAN;
    });

    auto block = std::wstring {};
    for (auto const& entry: wideEntries)
        block.append(entry).push_back(L'\0');
    block.push_back(L'\0');
    return block;
}
#endif

} // namespace vtpty
