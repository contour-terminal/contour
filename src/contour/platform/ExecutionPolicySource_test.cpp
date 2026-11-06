// SPDX-License-Identifier: Apache-2.0
//
// PowerShell's execution policy as read from a registry and PowerShell 7's configuration files, and the
// known-folders seam. The registry, the disk and the folders are fakes, so these run on every platform and
// never reach the real registry or the real Documents folder.

#include <contour/config/ExecutionPolicy.hpp>
#include <contour/config/ShellKind.hpp>
#include <contour/platform/ExecutionPolicySource.hpp>
#include <contour/platform/KnownFolders.hpp>
#include <contour/platform/Registry.hpp>

#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using namespace contour::platform;
using contour::config::EffectivePolicy;
using contour::config::ExecutionPolicy;
using contour::config::PolicyScope;
using contour::config::ShellKind;

namespace
{

constexpr auto ShellIdsKey =
    std::string_view { R"(SOFTWARE\Microsoft\PowerShell\1\ShellIds\Microsoft.PowerShell)" };
constexpr auto WindowsPolicyKey = std::string_view { R"(SOFTWARE\Policies\Microsoft\Windows\PowerShell)" };
constexpr auto CorePolicyKey = std::string_view { R"(SOFTWARE\Policies\Microsoft\PowerShellCore)" };
constexpr auto CurrentVersionKey = std::string_view { R"(SOFTWARE\Microsoft\Windows NT\CurrentVersion)" };
constexpr auto InstalledVersionsKey =
    std::string_view { R"(SOFTWARE\Microsoft\PowerShellCore\InstalledVersions)" };
constexpr auto ConfigPolicyKey = std::string_view { "Microsoft.PowerShell:ExecutionPolicy" };

/// Where a value is: its hive, its key and its name.
using ValuePath = std::tuple<RegistryRoot, std::string, std::string>;

/// One string written to the registry.
struct Write
{
    RegistryRoot root;
    std::string key;
    std::string value;
    std::string data;

    [[nodiscard]] bool operator==(Write const&) const = default;
};

/// Whether FakeRegistry::writeString succeeds.
enum class Writes : uint8_t
{
    Succeed = 0,
    Fail,
};

/// A registry held in maps, which records every write and can be told to refuse them.
class FakeRegistry final: public IRegistry
{
  public:
    explicit FakeRegistry(Writes writes = Writes::Succeed): _writes { writes } {}

    void putString(RegistryRoot root, std::string_view key, std::string_view value, std::string_view data)
    {
        _strings[ValuePath { root, key, value }] = data;
    }

    void putDword(RegistryRoot root, std::string_view key, std::string_view value, std::uint32_t data)
    {
        _dwords[ValuePath { root, key, value }] = data;
    }

    [[nodiscard]] std::vector<Write> const& writes() const noexcept { return _recorded; }

    [[nodiscard]] std::optional<std::string> readString(RegistryRoot root,
                                                        std::string_view key,
                                                        std::string_view value) const override
    {
        auto const found = _strings.find(ValuePath { root, key, value });
        if (found == _strings.end())
            return std::nullopt;
        return found->second;
    }

    [[nodiscard]] std::optional<std::uint32_t> readDword(RegistryRoot root,
                                                         std::string_view key,
                                                         std::string_view value) const override
    {
        auto const found = _dwords.find(ValuePath { root, key, value });
        if (found == _dwords.end())
            return std::nullopt;
        return found->second;
    }

    /// The subkeys of @p key that hold a value, by name, each once.
    [[nodiscard]] std::vector<std::string> subkeys(RegistryRoot root, std::string_view key) const override
    {
        auto names = std::vector<std::string> {};
        auto const prefix = std::string { key } + '\\';
        auto const collect = [&](ValuePath const& path) {
            auto const& [pathRoot, pathKey, value] = path;
            if (pathRoot != root || !pathKey.starts_with(prefix))
                return;
            auto name = pathKey.substr(prefix.size());
            name = name.substr(0, name.find('\\'));
            if (!std::ranges::contains(names, name))
                names.push_back(std::move(name));
        };
        for (auto const& path: _strings | std::views::keys)
            collect(path);
        for (auto const& path: _dwords | std::views::keys)
            collect(path);
        return names;
    }

    [[nodiscard]] std::expected<void, std::string> writeString(RegistryRoot root,
                                                               std::string_view key,
                                                               std::string_view value,
                                                               std::string_view data) override
    {
        _recorded.push_back(Write { .root = root,
                                    .key = std::string { key },
                                    .value = std::string { value },
                                    .data = std::string { data } });
        if (_writes == Writes::Fail)
            return std::unexpected(std::string { "access denied" });
        putString(root, key, value, data);
        return {};
    }

  private:
    Writes _writes;
    std::map<ValuePath, std::string> _strings;
    std::map<ValuePath, std::uint32_t> _dwords;
    std::vector<Write> _recorded;
};

/// A Documents folder the test chooses, or none.
class FakeKnownFolders final: public IKnownFolders
{
  public:
    explicit FakeKnownFolders(std::optional<std::filesystem::path> documents):
        _documents { std::move(documents) }
    {
    }

    [[nodiscard]] std::optional<std::filesystem::path> documents() const override { return _documents; }

  private:
    std::optional<std::filesystem::path> _documents;
};

using core::platform::testing::InMemoryFileSystem;

/// The fake Documents folder.
std::filesystem::path documents()
{
    return "/Users/me/Documents";
}

/// PowerShell 7's user configuration file under documents().
std::filesystem::path userConfig()
{
    return documents() / "PowerShell/powershell.config.json";
}

/// The registry, disk and folders an execution-policy source reads; none of them real.
struct PolicyRig
{
    FakeRegistry registry;
    InMemoryFileSystem disk;
    FakeKnownFolders knownFolders { documents() };
    RegistryExecutionPolicySource source { registry, disk, knownFolders };

    explicit PolicyRig(Writes writes = Writes::Succeed): registry { writes } {}

    /// Writes @p content to @p file, creating its folders.
    void put(std::filesystem::path const& file, std::string_view content) const
    {
        REQUIRE(disk.createDirectories(file.parent_path()));
        REQUIRE(disk.writeFile(file, content));
    }

    /// @return @p file's bytes, or `<none>`.
    [[nodiscard]] std::string text(std::filesystem::path const& file) const
    {
        return disk.readFile(file).value_or("<none>");
    }
};

[[nodiscard]] EffectivePolicy policyOf(FakeRegistry& registry, ShellKind shell)
{
    auto const disk = InMemoryFileSystem {};
    auto const knownFolders = FakeKnownFolders { documents() };
    return RegistryExecutionPolicySource { registry, disk, knownFolders }.effectivePolicy(shell);
}

} // namespace

TEST_CASE("ExecutionPolicySource.windowsPowerShell.default", "[executionpolicy]")
{
    auto registry = FakeRegistry {};
    CHECK(policyOf(registry, ShellKind::WindowsPowerShell)
          == EffectivePolicy { .policy = ExecutionPolicy::Restricted, .scope = PolicyScope::Default });
}

TEST_CASE("ExecutionPolicySource.windowsPowerShell.server", "[executionpolicy]")
{
    auto registry = FakeRegistry {};
    registry.putString(RegistryRoot::LocalMachine, CurrentVersionKey, "InstallationType", "Server");
    CHECK(policyOf(registry, ShellKind::WindowsPowerShell)
          == EffectivePolicy { .policy = ExecutionPolicy::RemoteSigned, .scope = PolicyScope::Default });

    registry.putString(RegistryRoot::LocalMachine, CurrentVersionKey, "InstallationType", "Client");
    CHECK(policyOf(registry, ShellKind::WindowsPowerShell)
          == EffectivePolicy { .policy = ExecutionPolicy::Restricted, .scope = PolicyScope::Default });
}

TEST_CASE("ExecutionPolicySource.windowsPowerShell.precedence", "[executionpolicy]")
{
    auto registry = FakeRegistry {};
    registry.putString(RegistryRoot::LocalMachine, ShellIdsKey, "ExecutionPolicy", "AllSigned");
    CHECK(policyOf(registry, ShellKind::WindowsPowerShell)
          == EffectivePolicy { .policy = ExecutionPolicy::AllSigned, .scope = PolicyScope::LocalMachine });

    registry.putString(RegistryRoot::CurrentUser, ShellIdsKey, "ExecutionPolicy", "RemoteSigned");
    CHECK(policyOf(registry, ShellKind::WindowsPowerShell)
          == EffectivePolicy { .policy = ExecutionPolicy::RemoteSigned, .scope = PolicyScope::CurrentUser });

    SECTION("a name that is no policy sets none")
    {
        registry.putString(RegistryRoot::CurrentUser, ShellIdsKey, "ExecutionPolicy", "Lenient");
        CHECK(
            policyOf(registry, ShellKind::WindowsPowerShell)
            == EffectivePolicy { .policy = ExecutionPolicy::AllSigned, .scope = PolicyScope::LocalMachine });
    }
}

TEST_CASE("ExecutionPolicySource.windowsPowerShell.groupPolicy", "[executionpolicy]")
{
    auto registry = FakeRegistry {};
    registry.putString(RegistryRoot::CurrentUser, ShellIdsKey, "ExecutionPolicy", "RemoteSigned");

    SECTION("scripts disabled")
    {
        registry.putDword(RegistryRoot::LocalMachine, WindowsPolicyKey, "EnableScripts", 0);
        CHECK(
            policyOf(registry, ShellKind::WindowsPowerShell)
            == EffectivePolicy { .policy = ExecutionPolicy::Restricted, .scope = PolicyScope::GroupPolicy });
    }

    SECTION("scripts enabled under a named policy")
    {
        registry.putDword(RegistryRoot::LocalMachine, WindowsPolicyKey, "EnableScripts", 1);
        registry.putString(RegistryRoot::LocalMachine, WindowsPolicyKey, "ExecutionPolicy", "AllSigned");
        CHECK(policyOf(registry, ShellKind::WindowsPowerShell)
              == EffectivePolicy { .policy = ExecutionPolicy::AllSigned, .scope = PolicyScope::GroupPolicy });
    }

    SECTION("the user's group policy, when the machine sets none")
    {
        registry.putDword(RegistryRoot::CurrentUser, WindowsPolicyKey, "EnableScripts", 0);
        CHECK(
            policyOf(registry, ShellKind::WindowsPowerShell)
            == EffectivePolicy { .policy = ExecutionPolicy::Restricted, .scope = PolicyScope::GroupPolicy });
    }

    SECTION("the machine's group policy before the user's")
    {
        registry.putDword(RegistryRoot::LocalMachine, WindowsPolicyKey, "EnableScripts", 1);
        registry.putString(RegistryRoot::LocalMachine, WindowsPolicyKey, "ExecutionPolicy", "Bypass");
        registry.putDword(RegistryRoot::CurrentUser, WindowsPolicyKey, "EnableScripts", 0);
        CHECK(policyOf(registry, ShellKind::WindowsPowerShell)
              == EffectivePolicy { .policy = ExecutionPolicy::Bypass, .scope = PolicyScope::GroupPolicy });
    }
}

TEST_CASE("ExecutionPolicySource.pwsh", "[executionpolicy]")
{
    auto registry = FakeRegistry {};
    CHECK(policyOf(registry, ShellKind::Pwsh)
          == EffectivePolicy { .policy = ExecutionPolicy::RemoteSigned, .scope = PolicyScope::Default });

    SECTION("Windows PowerShell's settings do not apply")
    {
        registry.putString(RegistryRoot::CurrentUser, ShellIdsKey, "ExecutionPolicy", "Restricted");
        registry.putDword(RegistryRoot::LocalMachine, WindowsPolicyKey, "EnableScripts", 0);
        CHECK(policyOf(registry, ShellKind::Pwsh)
              == EffectivePolicy { .policy = ExecutionPolicy::RemoteSigned, .scope = PolicyScope::Default });
    }

    SECTION("its own group policy")
    {
        registry.putDword(RegistryRoot::LocalMachine, CorePolicyKey, "EnableScripts", 0);
        CHECK(
            policyOf(registry, ShellKind::Pwsh)
            == EffectivePolicy { .policy = ExecutionPolicy::Restricted, .scope = PolicyScope::GroupPolicy });
    }
}

TEST_CASE("ExecutionPolicySource.otherShells", "[executionpolicy]")
{
    auto registry = FakeRegistry {};
    registry.putDword(RegistryRoot::LocalMachine, WindowsPolicyKey, "EnableScripts", 0);
    for (auto const shell: { ShellKind::Unknown, ShellKind::Bash, ShellKind::Zsh, ShellKind::Fish })
        CHECK(policyOf(registry, shell)
              == EffectivePolicy { .policy = ExecutionPolicy::Unrestricted, .scope = PolicyScope::Default });
}

TEST_CASE("ExecutionPolicySource.allowUserScripts", "[executionpolicy]")
{
    SECTION("Windows PowerShell")
    {
        auto rig = PolicyRig {};
        auto& registry = rig.registry;
        auto& source = rig.source;
        CHECK(source.allowUserScripts(ShellKind::WindowsPowerShell).has_value());
        CHECK(registry.writes()
              == std::vector { Write { .root = RegistryRoot::CurrentUser,
                                       .key = std::string { ShellIdsKey },
                                       .value = "ExecutionPolicy",
                                       .data = "RemoteSigned" } });
        CHECK(source.effectivePolicy(ShellKind::WindowsPowerShell)
              == EffectivePolicy { .policy = ExecutionPolicy::RemoteSigned,
                                   .scope = PolicyScope::CurrentUser });
    }

    SECTION("a refused write is reported")
    {
        auto rig = PolicyRig { Writes::Fail };
        auto const& registry = rig.registry;
        auto& source = rig.source;
        auto const result = source.allowUserScripts(ShellKind::WindowsPowerShell);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == "access denied");
        CHECK(registry.writes().size() == 1);
    }

    SECTION("no other shell has a user policy to set")
    {
        auto rig = PolicyRig {};
        for (auto const shell: { ShellKind::Bash, ShellKind::Unknown })
            CHECK_FALSE(rig.source.allowUserScripts(shell).has_value());
        CHECK(rig.registry.writes().empty());
    }
}

TEST_CASE("ExecutionPolicySource.null", "[executionpolicy]")
{
    auto source = NoExecutionPolicySource {};
    CHECK(source.effectivePolicy(ShellKind::WindowsPowerShell)
          == EffectivePolicy { .policy = ExecutionPolicy::Unrestricted, .scope = PolicyScope::Default });
    CHECK_FALSE(source.allowUserScripts(ShellKind::WindowsPowerShell).has_value());

    auto registry = NullRegistry {};
    CHECK_FALSE(registry.readString(RegistryRoot::CurrentUser, ShellIdsKey, "ExecutionPolicy").has_value());
    CHECK_FALSE(
        registry.readDword(RegistryRoot::LocalMachine, WindowsPolicyKey, "EnableScripts").has_value());
    CHECK(registry.subkeys(RegistryRoot::LocalMachine, InstalledVersionsKey).empty());
    CHECK(registry.writeString(RegistryRoot::CurrentUser, ShellIdsKey, "ExecutionPolicy", "RemoteSigned")
              .error()
          == "no registry");
}

TEST_CASE("KnownFolders.null", "[executionpolicy]")
{
    CHECK(NullKnownFolders {}.documents() == std::nullopt);
}

TEST_CASE("ExecutionPolicySource.windowsPowerShell.groupPolicyWithoutAPolicy", "[executionpolicy]")
{
    // Group policy that enables scripts without naming a policy, or holds a value that means neither, sets
    // none: the next layer decides.
    auto registry = FakeRegistry {};
    registry.putString(RegistryRoot::CurrentUser, ShellIdsKey, "ExecutionPolicy", "RemoteSigned");
    auto const user =
        EffectivePolicy { .policy = ExecutionPolicy::RemoteSigned, .scope = PolicyScope::CurrentUser };

    SECTION("EnableScripts=1 without ExecutionPolicy")
    {
        registry.putDword(RegistryRoot::LocalMachine, WindowsPolicyKey, "EnableScripts", 1);
        CHECK(policyOf(registry, ShellKind::WindowsPowerShell) == user);
    }

    SECTION("EnableScripts=2")
    {
        registry.putDword(RegistryRoot::LocalMachine, WindowsPolicyKey, "EnableScripts", 2);
        registry.putString(RegistryRoot::LocalMachine, WindowsPolicyKey, "ExecutionPolicy", "AllSigned");
        CHECK(policyOf(registry, ShellKind::WindowsPowerShell) == user);
    }
}

TEST_CASE("ExecutionPolicySource.pwsh.configFiles", "[executionpolicy]")
{
    auto rig = PolicyRig {};
    auto const machineFolder = std::filesystem::path { "/Program Files/PowerShell/7" };
    auto const installedAt = [&](std::string_view version, std::filesystem::path const& folder) {
        // GCC's link-time inlining reports a -Wstringop-overflow for the natural form of this (a string
        // temporary joined with `+`) that cannot happen, so it is built in pieces.
        auto key = std::string { InstalledVersionsKey };
        key += '\\';
        key += version;
        rig.registry.putString(RegistryRoot::LocalMachine, key, "InstallLocation", folder.generic_string());
    };

    SECTION("the user's file")
    {
        rig.put(userConfig(), R"({ "Microsoft.PowerShell:ExecutionPolicy": "Restricted" })");
        CHECK(
            rig.source.effectivePolicy(ShellKind::Pwsh)
            == EffectivePolicy { .policy = ExecutionPolicy::Restricted, .scope = PolicyScope::CurrentUser });
    }

    SECTION("the user's file, after a UTF-8 byte order mark")
    {
        rig.put(userConfig(), "\xEF\xBB\xBF{ \"Microsoft.PowerShell:ExecutionPolicy\": \"Restricted\" }");
        CHECK(
            rig.source.effectivePolicy(ShellKind::Pwsh)
            == EffectivePolicy { .policy = ExecutionPolicy::Restricted, .scope = PolicyScope::CurrentUser });
    }

    SECTION("the machine's file, beside the first installed PowerShell 7 that has one")
    {
        installedAt("aaaa-without-config", "/Program Files/PowerShell/6");
        installedAt("bbbb-with-config", machineFolder);
        rig.put(machineFolder / "powershell.config.json",
                R"({ "Microsoft.PowerShell:ExecutionPolicy": "AllSigned" })");
        CHECK(
            rig.source.effectivePolicy(ShellKind::Pwsh)
            == EffectivePolicy { .policy = ExecutionPolicy::AllSigned, .scope = PolicyScope::LocalMachine });

        SECTION("the user's file before the machine's")
        {
            rig.put(userConfig(), R"({ "Microsoft.PowerShell:ExecutionPolicy": "Bypass" })");
            CHECK(
                rig.source.effectivePolicy(ShellKind::Pwsh)
                == EffectivePolicy { .policy = ExecutionPolicy::Bypass, .scope = PolicyScope::CurrentUser });
        }

        SECTION("group policy before both")
        {
            rig.put(userConfig(), R"({ "Microsoft.PowerShell:ExecutionPolicy": "Bypass" })");
            rig.registry.putDword(RegistryRoot::CurrentUser, CorePolicyKey, "EnableScripts", 0);
            CHECK(rig.source.effectivePolicy(ShellKind::Pwsh)
                  == EffectivePolicy { .policy = ExecutionPolicy::Restricted,
                                       .scope = PolicyScope::GroupPolicy });
        }
    }

    SECTION("a file that sets no policy, or is no JSON object, sets none")
    {
        auto const content = GENERATE(as<std::string> {},
                                      R"({ "Microsoft.PowerShell:ExecutionPolicy": )",
                                      R"([ "Restricted" ])",
                                      R"({ "PSModulePath": "C:/Modules" })",
                                      R"({ "Microsoft.PowerShell:ExecutionPolicy": "Lenient" })");
        rig.put(userConfig(), content);
        CHECK(rig.source.effectivePolicy(ShellKind::Pwsh)
              == EffectivePolicy { .policy = ExecutionPolicy::RemoteSigned, .scope = PolicyScope::Default });
    }

    SECTION("Windows PowerShell does not read them")
    {
        rig.put(userConfig(), R"({ "Microsoft.PowerShell:ExecutionPolicy": "Bypass" })");
        CHECK(rig.source.effectivePolicy(ShellKind::WindowsPowerShell)
              == EffectivePolicy { .policy = ExecutionPolicy::Restricted, .scope = PolicyScope::Default });
    }
}

TEST_CASE("ExecutionPolicySource.pwsh.allowUserScripts", "[executionpolicy]")
{
    auto rig = PolicyRig {};

    SECTION("creates the missing file and its folder with the one key")
    {
        REQUIRE(rig.source.allowUserScripts(ShellKind::Pwsh).has_value());
        auto const written = QJsonDocument::fromJson(QByteArray::fromStdString(rig.text(userConfig())));
        CHECK(written.object()
              == QJsonObject {
                  { QString::fromUtf8(ConfigPolicyKey.data(), static_cast<qsizetype>(ConfigPolicyKey.size())),
                    QStringLiteral("RemoteSigned") } });
        CHECK(rig.source.effectivePolicy(ShellKind::Pwsh)
              == EffectivePolicy { .policy = ExecutionPolicy::RemoteSigned,
                                   .scope = PolicyScope::CurrentUser });
        CHECK(rig.registry.writes().empty());
        CHECK_FALSE(rig.disk.exists(std::filesystem::path { userConfig() } += ".contour-new"));
    }

    SECTION("keeps every other key, and the byte order mark")
    {
        rig.put(userConfig(),
                "\xEF\xBB\xBF{ \"Microsoft.PowerShell:ExecutionPolicy\": \"Restricted\", \"PSModulePath\": "
                "\"C:/Modules\", \"ExperimentalFeatures\": [ \"PSFeedbackProvider\" ] }");
        REQUIRE(rig.source.allowUserScripts(ShellKind::Pwsh).has_value());
        auto const text = rig.text(userConfig());
        REQUIRE(text.starts_with("\xEF\xBB\xBF"));
        auto const written = QJsonDocument::fromJson(QByteArray::fromStdString(text.substr(3))).object();
        CHECK(written.size() == 3);
        CHECK(written.value(QStringLiteral("Microsoft.PowerShell:ExecutionPolicy")).toString()
              == QStringLiteral("RemoteSigned"));
        CHECK(written.value(QStringLiteral("PSModulePath")).toString() == QStringLiteral("C:/Modules"));
        CHECK(written.value(QStringLiteral("ExperimentalFeatures")).toArray().first().toString()
              == QStringLiteral("PSFeedbackProvider"));
    }

    SECTION("a file that is no JSON object is left exactly as it is")
    {
        auto const malformed = std::string { "{ \"PSModulePath\": \"C:/Modules\", // a comment\n" };
        rig.put(userConfig(), malformed);
        auto const result = rig.source.allowUserScripts(ShellKind::Pwsh);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().contains("not a JSON object"));
        CHECK(rig.text(userConfig()) == malformed);
    }

    SECTION("no Documents folder, no file")
    {
        auto const disk = InMemoryFileSystem {};
        auto const knownFolders = FakeKnownFolders { std::nullopt };
        auto source = RegistryExecutionPolicySource { rig.registry, disk, knownFolders };
        CHECK_FALSE(source.allowUserScripts(ShellKind::Pwsh).has_value());
    }
}
