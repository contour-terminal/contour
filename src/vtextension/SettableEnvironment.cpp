// SPDX-License-Identifier: Apache-2.0
#include <vtextension/SettableEnvironment.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

namespace vtextension
{

namespace
{
    /// @return Whether @p text is a decimal number as git prints an index: digits, no sign, no leading zero.
    [[nodiscard]] constexpr bool isCanonicalDecimal(std::string_view text) noexcept
    {
        return !text.empty() && std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; })
               && (text.size() == 1 || text.front() != '0');
    }

    /// @return Whether @p text is a full git object name, SHA-1 or SHA-256, in lower case.
    [[nodiscard]] constexpr bool isObjectId(std::string_view text) noexcept
    {
        return (text.size() == 40 || text.size() == 64) && std::ranges::all_of(text, [](char c) {
                   return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
               });
    }

    /// @return Whether @p text is a colon-separated list of SettableGitProtocols, none empty.
    [[nodiscard]] bool isGitProtocolList(std::string_view text)
    {
        return std::ranges::all_of(std::views::split(text, ':'), [](auto const& protocol) {
            return std::ranges::contains(SettableGitProtocols, std::string_view { protocol });
        });
    }

    /// @return Whether @p a and @p b are equal but for the case of ASCII letters.
    [[nodiscard]] bool equalIgnoringCase(std::string_view a, std::string_view b) noexcept
    {
        return std::ranges::equal(a, b, [](char x, char y) {
            return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
        });
    }

    /// A declared name, as the row that lists it reads it.
    struct Matched
    {
        SettableVariable const* row;
        std::string_view index; ///< For an Indexed row, the index; empty otherwise.
    };

    /// @return The row of SettableVariables @p name matches, with its index; std::nullopt where none does.
    [[nodiscard]] std::optional<Matched> settableRowOf(std::string_view name) noexcept
    {
        for (auto const& row: SettableVariables)
            switch (row.shape)
            {
                case SettableName::Exact:
                    if (name == row.name)
                        return Matched { .row = &row, .index = {} };
                    break;
                case SettableName::Indexed:
                    if (name.starts_with(row.name) && isCanonicalDecimal(name.substr(row.name.size())))
                        return Matched { .row = &row, .index = name.substr(row.name.size()) };
                    break;
            }
        return std::nullopt;
    }

    /// @return The row of SettableGitConfigs for @p key; nullptr where git configuration may not set it.
    [[nodiscard]] SettableGitConfig const* gitConfigRowOf(std::string_view key) noexcept
    {
        auto const found = std::ranges::find_if(SettableGitConfigs, [key](SettableGitConfig const& row) {
            return equalIgnoringCase(row.key, key);
        });
        return found == SettableGitConfigs.end() ? nullptr : &*found;
    }

    /// @return The value of @p name in @p environment, if set.
    [[nodiscard]] std::optional<std::string_view> valueOf(std::span<EnvironmentEntry const> environment,
                                                          std::string_view name) noexcept
    {
        auto const found = std::ranges::find(environment, name, &EnvironmentEntry::name);
        return found == environment.end() ? std::nullopt : std::optional<std::string_view> { found->value };
    }

    /// @return Whether git configuration may set the key @p key to @p value: both listed.
    [[nodiscard]] bool isSettableGitConfig(std::string_view key, std::optional<std::string_view> value)
    {
        auto const* const row = gitConfigRowOf(key);
        return row != nullptr && value && std::ranges::contains(row->values, *value);
    }

    /// Judges one variable of @p environment by the row its name matched.
    [[nodiscard]] std::expected<void, ManifestFault> checkVariable(
        std::span<EnvironmentEntry const> environment, Matched matched, std::string_view value)
    {
        auto const forbidden = std::unexpected(ManifestFault::ForbiddenEnvironment);
        switch (matched.row->value)
        {
            case SettableValue::Listed:
                if (!std::ranges::contains(matched.row->listed, value))
                    return forbidden;
                return {};
            case SettableValue::Decimal:
                if (!isCanonicalDecimal(value))
                    return forbidden;
                return {};
            case SettableValue::ObjectId:
                if (!isObjectId(value))
                    return forbidden;
                return {};
            case SettableValue::GitProtocols:
                if (!isGitProtocolList(value))
                    return forbidden;
                return {};
            case SettableValue::GitConfigKey:
                if (!isSettableGitConfig(
                        value, valueOf(environment, std::format("GIT_CONFIG_VALUE_{}", matched.index))))
                    return std::unexpected(ManifestFault::ForbiddenGitConfig);
                return {};
            case SettableValue::GitConfigValue:
                // A value without a key configures nothing; with one, its key's check judges it.
                return {};
        }
        return forbidden;
    }
} // namespace

std::expected<void, ManifestFault> checkSettableEnvironment(std::span<EnvironmentEntry const> environment)
{
    for (auto const& [name, value]: environment)
    {
        auto const matched = settableRowOf(name);
        if (!matched)
            return std::unexpected(ManifestFault::ForbiddenEnvironment);
        if (auto const checked = checkVariable(environment, *matched, value); !checked)
            return checked;
    }
    return {};
}

std::expected<void, ManifestFault> checkSettableEnvironments(Manifest const& manifest)
{
    if (auto const shared = checkSettableEnvironment(manifest.commandEnvironment); !shared)
        return shared;
    for (auto const& command: manifest.commands)
    {
        // As childEnvironment() lays them: the command's own replace the shared ones by name.
        auto environment = std::vector<EnvironmentEntry> {};
        for (auto const& entry: manifest.commandEnvironment)
            if (!std::ranges::contains(command.environment, entry.name, &EnvironmentEntry::name))
                environment.push_back(entry);
        std::ranges::copy(command.environment, std::back_inserter(environment));
        if (auto const checked = checkSettableEnvironment(environment); !checked)
            return checked;
    }
    for (auto const& pane: manifest.panes)
        if (auto const checked = checkSettableEnvironment(pane.environment); !checked)
            return checked;
    return {};
}

} // namespace vtextension
