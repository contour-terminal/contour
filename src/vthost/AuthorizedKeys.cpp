// SPDX-License-Identifier: Apache-2.0
#include <vthost/AuthorizedKeys.hpp>

#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <ranges>

namespace vthost
{

namespace
{
    constexpr auto GrantsOption = std::string_view { "grants=" };

    /// @param value A `grants=` option's value, as written.
    /// @return It without the double quotes that surround it, if it has them.
    [[nodiscard]] std::string_view unquoted(std::string_view value)
    {
        if (value.size() >= 2 && value.starts_with('"') && value.ends_with('"'))
            return value.substr(1, value.size() - 2);
        return value;
    }

    /// @param options The options of one line, each as written.
    /// @return The grants they state (everything for none), or why the line is unusable.
    [[nodiscard]] std::expected<vtcontrol::ControlGrants, std::string> grantsOf(
        std::span<std::string const> options)
    {
        auto grants = std::optional<vtcontrol::ControlGrants> {};
        for (auto const& option: options)
        {
            if (!option.starts_with(GrantsOption))
            {
                // Named whole: `from="1.2.3.4"` and `no-pty` alike, so the user finds what to remove.
                auto const name = option.substr(0, option.find('='));
                return std::unexpected(
                    std::format("it carries the option '{}', which is not supported", name));
            }
            if (grants)
                return std::unexpected(std::string { "it states grants twice" });
            auto const parsed =
                vtcontrol::parseGrants(unquoted(std::string_view { option }.substr(GrantsOption.size())));
            if (!parsed)
                return std::unexpected(parsed.error().message);
            grants = *parsed;
        }
        return grants.value_or(vtcontrol::ControlGrants::all());
    }

    [[nodiscard]] std::string describe(core::net::SshKeyError error)
    {
        switch (error)
        {
            case core::net::SshKeyError::UnsupportedType:
                return "its key type is not supported (ssh-ed25519, ecdsa-sha2-nistp256 and the "
                       "sk- variants are)";
            case core::net::SshKeyError::Malformed: break;
        }
        return "it is not a well-formed key line";
    }
} // namespace

std::expected<DaemonAuth, std::string> parseDaemonAuth(std::string_view text)
{
    struct Choice
    {
        std::string_view name;
        DaemonAuth auth;
    };
    constexpr auto Choices =
        std::to_array<Choice>({ { "any", DaemonAuth::Any }, { "keys", DaemonAuth::KeysOnly } });
    if (text.empty())
        return DaemonAuth::Any;
    if (auto const found = std::ranges::find(Choices, text, &Choice::name); found != Choices.end())
        return found->auth;
    return std::unexpected(std::format("--auth '{}' is not any of: any, keys", text));
}

AuthorizedKeysFile parseAuthorizedKeys(std::string_view text)
{
    auto file = AuthorizedKeysFile {};
    // The line each accepted key came from, by its blob, to name it when a later line repeats it.
    auto lineOfBlob = std::map<std::vector<std::byte>, std::size_t> {};
    for (auto const [index, rawLine]: core::views::enumerate(std::views::split(text, '\n')))
    {
        auto const lineNumber = static_cast<std::size_t>(index) + 1;
        auto const skip = [&](std::string reason) {
            file.skipped.push_back(
                AuthorizedKeysSkip { .lineNumber = lineNumber, .reason = std::move(reason) });
        };
        auto const parsed =
            core::net::parseAuthorizedKeysLine(std::string_view { rawLine.begin(), rawLine.end() });
        if (!parsed)
        {
            skip(describe(parsed.error()));
            continue;
        }
        if (!*parsed)
            continue; // blank or comment
        auto const grants = grantsOf((*parsed)->options);
        if (!grants)
        {
            skip(grants.error());
            continue;
        }
        auto const& key = (*parsed)->key;
        if (auto const earlier = lineOfBlob.find(key.blob); earlier != lineOfBlob.end())
        {
            skip(std::format("it repeats the key of line {}", earlier->second));
            continue;
        }
        file.keys.push_back(AuthorizedKey { .key = key, .grants = *grants });
        lineOfBlob.emplace(key.blob, lineNumber);
    }
    return file;
}

std::expected<AuthorizedKeysFile, std::string> readAuthorizedKeys(
    core::platform::FileSystem const& fileSystem, std::filesystem::path const& path)
{
    if (!fileSystem.exists(path))
        return AuthorizedKeysFile {};
    auto const text = fileSystem.readFile(path);
    if (!text)
        return std::unexpected(
            std::format("cannot read authorized keys '{}': {}", core::pathToUtf8(path), text.error()));
    return parseAuthorizedKeys(*text);
}

AuthorizedKey const* findAuthorizedKeyByBlob(std::span<AuthorizedKey const> keys,
                                             std::span<std::byte const> blob)
{
    auto const found = std::ranges::find_if(
        keys, [&](AuthorizedKey const& entry) { return std::ranges::equal(entry.key.blob, blob); });
    return found == keys.end() ? nullptr : &*found;
}

AuthorizedKey const* findAuthorizedKeyByFingerprint(std::span<AuthorizedKey const> keys,
                                                    std::string_view fingerprint)
{
    auto const found = std::ranges::find_if(keys, [&](AuthorizedKey const& entry) {
        return core::net::sshFingerprintOf(entry.key) == fingerprint;
    });
    return found == keys.end() ? nullptr : &*found;
}

std::expected<void, std::string> checkAuthOptions(DaemonAuth auth, std::size_t tokenOptionsGiven)
{
    if (auth == DaemonAuth::KeysOnly && tokenOptionsGiven != 0)
        return std::unexpected(std::string { "--auth keys accepts authorized keys alone; remove --token, "
                                             "--token-file and --access-file" });
    return {};
}

std::expected<void, std::string> checkListenerCredentials(ListenerCredentials const& credentials)
{
    if (auto const options = checkAuthOptions(credentials.auth, credentials.tokenOptionsGiven); !options)
        return options;
    if (credentials.auth == DaemonAuth::KeysOnly)
    {
        if (credentials.authorizedKeys == 0)
            return std::unexpected(std::string { "--auth keys needs at least one usable key in the "
                                                 "authorized_keys file (--authorized-keys)" });
        return {};
    }
    if (credentials.tokens == 0 && credentials.authorizedKeys == 0)
        return std::unexpected(std::string {
            "--listen-tcp requires --token, --token-file, a non-empty --access-file or at least one usable "
            "key in the authorized_keys file (--authorized-keys); the TCP transport has no filesystem "
            "permissions to fall back on." });
    return {};
}

} // namespace vthost
