// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/ExecutionPolicySource.hpp>

#include <core/Utf8Path.hpp>

#include <QtCore/QByteArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtCore/QString>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace contour::platform
{

namespace
{

    using config::ExecutionPolicy;
    using config::PolicyLayer;
    using config::PolicyScope;
    using config::ShellKind;

    /// How a layer spells its policy, and so where it is.
    enum class PolicySpelling : uint8_t
    {
        /// The registry key's `ExecutionPolicy` string alone, as `Set-ExecutionPolicy` writes it.
        Named = 0,
        /// Group policy: the registry key's `EnableScripts` DWORD, 0 for no scripts at all, 1 for the
        /// policy the `ExecutionPolicy` string names.
        GroupPolicy,
        /// PowerShell 7's `powershell.config.json`: the `Microsoft.PowerShell:ExecutionPolicy` string of its
        /// top-level object. The root says which file: CurrentUser's is below the Documents folder,
        /// LocalMachine's beside an installed PowerShell 7; the key is the file's path below that folder.
        ConfigFile,
    };

    /// One place a policy can be set, and the scope it is in effect at.
    struct LayerSource
    {
        PolicyScope scope;
        RegistryRoot root;    ///< The hive, or for PolicySpelling::ConfigFile whose folder the file is in.
        std::string_view key; ///< The registry key, or for PolicySpelling::ConfigFile the file's path.
        PolicySpelling spelling;
    };

    /// Where a setting is written: its hive, key, value name and data.
    struct RegistrySetting
    {
        RegistryRoot root;
        std::string_view key;
        std::string_view value;
        std::string_view data;
    };

    constexpr auto ExecutionPolicyValue = std::string_view { "ExecutionPolicy" };
    constexpr auto EnableScriptsValue = std::string_view { "EnableScripts" };
    constexpr auto ShellIdsKey =
        std::string_view { "SOFTWARE\\Microsoft\\PowerShell\\1\\ShellIds\\Microsoft.PowerShell" };
    constexpr auto WindowsPolicyKey =
        std::string_view { "SOFTWARE\\Policies\\Microsoft\\Windows\\PowerShell" };
    constexpr auto CorePolicyKey = std::string_view { "SOFTWARE\\Policies\\Microsoft\\PowerShellCore" };
    /// One subkey per installed PowerShell 7, each naming the folder it is installed in.
    constexpr auto CoreInstalledVersionsKey =
        std::string_view { "SOFTWARE\\Microsoft\\PowerShellCore\\InstalledVersions" };
    constexpr auto InstallLocationValue = std::string_view { "InstallLocation" };
    /// The setting `Set-ExecutionPolicy` writes to PowerShell 7's configuration files.
    constexpr auto ConfigFilePolicyKey = std::string_view { "Microsoft.PowerShell:ExecutionPolicy" };
    /// The user's PowerShell 7 configuration, below the Documents folder.
    constexpr auto UserConfigFile = std::string_view { "PowerShell/powershell.config.json" };
    /// The machine's PowerShell 7 configuration, in the folder PowerShell 7 is installed in.
    constexpr auto MachineConfigFile = std::string_view { "powershell.config.json" };
    constexpr auto Utf8ByteOrderMark = std::string_view { "\xEF\xBB\xBF" };

    /// The edition of Windows: `InstallationType` is `Server` on Windows Server.
    constexpr auto InstallationType = RegistrySetting {
        .root = RegistryRoot::LocalMachine,
        .key = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        .value = "InstallationType",
        .data = "Server",
    };

    /// Windows PowerShell's layers, strongest first.
    constexpr auto WindowsPowerShellLayers = std::array {
        LayerSource { .scope = PolicyScope::GroupPolicy,
                      .root = RegistryRoot::LocalMachine,
                      .key = WindowsPolicyKey,
                      .spelling = PolicySpelling::GroupPolicy },
        LayerSource { .scope = PolicyScope::GroupPolicy,
                      .root = RegistryRoot::CurrentUser,
                      .key = WindowsPolicyKey,
                      .spelling = PolicySpelling::GroupPolicy },
        LayerSource { .scope = PolicyScope::CurrentUser,
                      .root = RegistryRoot::CurrentUser,
                      .key = ShellIdsKey,
                      .spelling = PolicySpelling::Named },
        LayerSource { .scope = PolicyScope::LocalMachine,
                      .root = RegistryRoot::LocalMachine,
                      .key = ShellIdsKey,
                      .spelling = PolicySpelling::Named },
    };

    /// PowerShell 7's layers, strongest first: its group policy in the registry, its own scopes in JSON
    /// files.
    constexpr auto PwshLayers = std::array {
        LayerSource { .scope = PolicyScope::GroupPolicy,
                      .root = RegistryRoot::LocalMachine,
                      .key = CorePolicyKey,
                      .spelling = PolicySpelling::GroupPolicy },
        LayerSource { .scope = PolicyScope::GroupPolicy,
                      .root = RegistryRoot::CurrentUser,
                      .key = CorePolicyKey,
                      .spelling = PolicySpelling::GroupPolicy },
        LayerSource { .scope = PolicyScope::CurrentUser,
                      .root = RegistryRoot::CurrentUser,
                      .key = UserConfigFile,
                      .spelling = PolicySpelling::ConfigFile },
        LayerSource { .scope = PolicyScope::LocalMachine,
                      .root = RegistryRoot::LocalMachine,
                      .key = MachineConfigFile,
                      .spelling = PolicySpelling::ConfigFile },
    };

    /// The user's own policy, which Contour sets to allow the user's scripts.
    struct UserPolicySetting
    {
        PolicySpelling spelling; ///< PolicySpelling::Named for a registry value, ConfigFile for a JSON file.
        RegistrySetting setting; ///< Where it is, as LayerSource reads it for @c spelling, and what to write.
    };

    /// What one shell's policy is read from, and what Contour writes to allow the user's scripts.
    struct ShellPolicy
    {
        ShellKind shell;
        std::span<LayerSource const> layers;
        ExecutionPolicy clientFallback; ///< The policy when no layer sets one, on a client Windows.
        ExecutionPolicy serverFallback; ///< The policy when no layer sets one, on Windows Server.
        std::optional<UserPolicySetting> allowUserScripts;
    };

    constexpr auto ShellPolicies = std::array {
        ShellPolicy { .shell = ShellKind::WindowsPowerShell,
                      .layers = WindowsPowerShellLayers,
                      .clientFallback = ExecutionPolicy::Restricted,
                      .serverFallback = ExecutionPolicy::RemoteSigned,
                      .allowUserScripts =
                          UserPolicySetting { .spelling = PolicySpelling::Named,
                                              .setting = RegistrySetting { .root = RegistryRoot::CurrentUser,
                                                                           .key = ShellIdsKey,
                                                                           .value = ExecutionPolicyValue,
                                                                           .data = "RemoteSigned" } } },
        ShellPolicy { .shell = ShellKind::Pwsh,
                      .layers = PwshLayers,
                      .clientFallback = ExecutionPolicy::RemoteSigned,
                      .serverFallback = ExecutionPolicy::RemoteSigned,
                      .allowUserScripts =
                          UserPolicySetting { .spelling = PolicySpelling::ConfigFile,
                                              .setting = RegistrySetting { .root = RegistryRoot::CurrentUser,
                                                                           .key = UserConfigFile,
                                                                           .value = ConfigFilePolicyKey,
                                                                           .data = "RemoteSigned" } } },
    };

    /// A shell with no execution policy: nothing to read, every script runs.
    constexpr auto NoPolicy = ShellPolicy { .shell = ShellKind::Unknown,
                                            .layers = {},
                                            .clientFallback = ExecutionPolicy::Unrestricted,
                                            .serverFallback = ExecutionPolicy::Unrestricted,
                                            .allowUserScripts = std::nullopt };

    [[nodiscard]] ShellPolicy const& shellPolicyOf(ShellKind shell) noexcept
    {
        auto const row = std::ranges::find(ShellPolicies, shell, &ShellPolicy::shell);
        return row == ShellPolicies.end() ? NoPolicy : *row;
    }

    /// What the policy is read from and written to.
    struct PolicyStores
    {
        IRegistry const& registry;
        core::platform::FileSystem const& fileSystem;
        IKnownFolders const& knownFolders;
    };

    /// @return The policy the `ExecutionPolicy` string under @p key names; Undefined when absent or no
    /// policy.
    [[nodiscard]] ExecutionPolicy namedPolicy(IRegistry const& registry,
                                              RegistryRoot root,
                                              std::string_view key)
    {
        return registry.readString(root, key, ExecutionPolicyValue)
            .and_then([](std::string const& name) { return config::executionPolicyNamed(name); })
            .value_or(ExecutionPolicy::Undefined);
    }

    /// @return The JSON object @p bytes hold, after a UTF-8 byte order mark when they start with one;
    ///         nullopt when they hold anything else.
    [[nodiscard]] std::optional<QJsonObject> jsonObjectOf(std::string_view bytes)
    {
        if (bytes.starts_with(Utf8ByteOrderMark))
            bytes.remove_prefix(Utf8ByteOrderMark.size());
        auto error = QJsonParseError {};
        auto const document =
            QJsonDocument::fromJson(QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            return std::nullopt;
        return document.object();
    }

    /// @return @p text as a QString.
    [[nodiscard]] QString qStringOf(std::string_view text)
    {
        return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    }

    /// @return The folder @p file is in for @p root: the Documents folder for the user; for the machine, the
    ///         folder of the first installed PowerShell 7 with a readable @p file. Nullopt when none is.
    [[nodiscard]] std::optional<std::filesystem::path> configFolderOf(PolicyStores const& stores,
                                                                      RegistryRoot root,
                                                                      std::string_view file)
    {
        switch (root)
        {
            case RegistryRoot::CurrentUser: return stores.knownFolders.documents();
            case RegistryRoot::LocalMachine: break;
        }
        auto const relative = core::pathFromTrustedUtf8(file);
        for (auto const& version:
             stores.registry.subkeys(RegistryRoot::LocalMachine, CoreInstalledVersionsKey))
        {
            auto const location =
                stores.registry.readString(RegistryRoot::LocalMachine,
                                           std::format("{}\\{}", CoreInstalledVersionsKey, version),
                                           InstallLocationValue);
            if (!location)
                continue;
            auto folder = core::pathFromUtf8(*location);
            if (folder && stores.fileSystem.readFile(*folder / relative))
                return *std::move(folder);
        }
        return std::nullopt;
    }

    /// @return The policy the configuration file of @p source names; Undefined when the file is absent,
    ///         unreadable or not a JSON object, or names no policy.
    [[nodiscard]] ExecutionPolicy configFilePolicy(PolicyStores const& stores, LayerSource const& source)
    {
        auto const folder = configFolderOf(stores, source.root, source.key);
        if (!folder)
            return ExecutionPolicy::Undefined;
        auto const bytes = stores.fileSystem.readFile(*folder / core::pathFromTrustedUtf8(source.key));
        if (!bytes)
            return ExecutionPolicy::Undefined;
        auto const object = jsonObjectOf(*bytes);
        if (!object)
            return ExecutionPolicy::Undefined;
        auto const name = object->value(qStringOf(ConfigFilePolicyKey)).toString().toStdString();
        return config::executionPolicyNamed(name).value_or(ExecutionPolicy::Undefined);
    }

    /// @return What @p source says about the policy.
    [[nodiscard]] PolicyLayer readLayer(PolicyStores const& stores, LayerSource const& source)
    {
        auto const policy = [&] {
            switch (source.spelling)
            {
                case PolicySpelling::Named: return namedPolicy(stores.registry, source.root, source.key);
                case PolicySpelling::GroupPolicy:
                    switch (
                        stores.registry.readDword(source.root, source.key, EnableScriptsValue).value_or(~0U))
                    {
                        case 0: return ExecutionPolicy::Restricted;
                        case 1: return namedPolicy(stores.registry, source.root, source.key);
                        default: return ExecutionPolicy::Undefined;
                    }
                case PolicySpelling::ConfigFile: return configFilePolicy(stores, source);
            }
            return ExecutionPolicy::Undefined;
        }();
        return PolicyLayer { .scope = source.scope, .policy = policy };
    }

    /// @return Whether this Windows is Windows Server.
    [[nodiscard]] bool isServer(IRegistry const& registry)
    {
        return registry.readString(InstallationType.root, InstallationType.key, InstallationType.value)
               == InstallationType.data;
    }

    /// Replaces @p file's content with @p text in one step: a reader sees the old content or the new, never
    /// a mix, and a failure leaves the old.
    /// @return Nothing, or why the content could not be replaced.
    [[nodiscard]] std::expected<void, std::string> replaceContent(
        core::platform::FileSystem const& fileSystem,
        std::filesystem::path const& file,
        std::string_view text)
    {
        auto temporary = file;
        temporary += ".contour-new";
        std::ignore = fileSystem.remove(temporary); // an earlier attempt's leftover, under Contour's own name
        return fileSystem.createNewFile(temporary)
            .and_then([&] { return fileSystem.writeFile(temporary, text); })
            .and_then([&] { return fileSystem.syncFile(temporary); })
            .and_then([&] { return fileSystem.replaceFile(temporary, file); })
            .transform([&] {
                // The new content is in place either way: a folder that does not sync only makes it less
                // durable, which is no reason to report a change that was made as one that was not.
                std::ignore = fileSystem.syncDirectory(file.parent_path());
            })
            .or_else([&](std::string const& error) -> std::expected<void, std::string> {
                std::ignore = fileSystem.remove(temporary);
                return std::unexpected(error);
            });
    }

    /// Sets @p setting in its JSON configuration file, keeping every other key, and creates the file and its
    /// folder when missing. A file that is not a JSON object is left as it is.
    /// @return Nothing, or why the setting could not be written.
    [[nodiscard]] std::expected<void, std::string> writeConfigFile(PolicyStores const& stores,
                                                                   RegistrySetting const& setting)
    {
        auto const folder = configFolderOf(stores, setting.root, setting.key);
        if (!folder)
            return std::unexpected(
                std::string { "the folder of PowerShell's configuration file is not known" });
        auto const& fileSystem = stores.fileSystem;
        // Through a symlink to its target, as a profile is appended to: the link itself stays.
        auto const file = fileSystem.weaklyCanonical(*folder / core::pathFromTrustedUtf8(setting.key));
        auto object = QJsonObject {};
        auto preamble = std::string_view {};
        if (fileSystem.exists(file))
        {
            if (!fileSystem.isRegularFile(file))
                return std::unexpected(std::format("{} is not a regular file", core::pathToUtf8(file)));
            auto const bytes = fileSystem.readFile(file);
            if (!bytes)
                return std::unexpected(bytes.error());
            auto parsed = jsonObjectOf(*bytes);
            if (!parsed)
                return std::unexpected(
                    std::format("{} is not a JSON object, so it was left as it is", core::pathToUtf8(file)));
            object = *std::move(parsed);
            if (bytes->starts_with(Utf8ByteOrderMark))
                preamble = Utf8ByteOrderMark;
        }
        else if (auto const parent = file.parent_path(); !parent.empty() && !fileSystem.isDirectory(parent))
        {
            if (auto created = fileSystem.createDirectories(parent); !created)
                return std::unexpected(created.error());
        }
        object.insert(qStringOf(setting.value), qStringOf(setting.data));
        auto const json = QJsonDocument { object }.toJson(QJsonDocument::Indented);
        auto text = std::string { preamble };
        text.append(json.constData(), static_cast<std::size_t>(json.size()));
        return replaceContent(fileSystem, file, text);
    }

} // namespace

config::EffectivePolicy RegistryExecutionPolicySource::effectivePolicy(config::ShellKind shell) const
{
    auto const& row = shellPolicyOf(shell);
    auto const stores =
        PolicyStores { .registry = _registry, .fileSystem = _fileSystem, .knownFolders = _knownFolders };
    auto const layers =
        row.layers
        | std::views::transform([&stores](LayerSource const& source) { return readLayer(stores, source); })
        | std::ranges::to<std::vector>();
    // Only Windows PowerShell's default depends on the edition; ask the registry only when it matters.
    auto const fallback = row.clientFallback == row.serverFallback || !isServer(_registry)
                              ? row.clientFallback
                              : row.serverFallback;
    return config::effectivePolicyOf(layers, fallback);
}

std::expected<void, std::string> RegistryExecutionPolicySource::allowUserScripts(config::ShellKind shell)
{
    auto const& user = shellPolicyOf(shell).allowUserScripts;
    if (!user)
        return std::unexpected(std::string { "this shell has no user execution policy to set" });
    auto const& setting = user->setting;
    switch (user->spelling)
    {
        case PolicySpelling::Named:
        case PolicySpelling::GroupPolicy: break;
        case PolicySpelling::ConfigFile:
            return writeConfigFile(PolicyStores { .registry = _registry,
                                                  .fileSystem = _fileSystem,
                                                  .knownFolders = _knownFolders },
                                   setting);
    }
    return _registry.writeString(setting.root, setting.key, setting.value, setting.data);
}

std::unique_ptr<IExecutionPolicySource> makePlatformExecutionPolicySource(
    [[maybe_unused]] IRegistry& registry,
    [[maybe_unused]] core::platform::FileSystem const& fileSystem,
    [[maybe_unused]] IKnownFolders const& knownFolders)
{
#ifdef _WIN32
    return std::make_unique<RegistryExecutionPolicySource>(registry, fileSystem, knownFolders);
#else
    return std::make_unique<NoExecutionPolicySource>();
#endif
}

} // namespace contour::platform
