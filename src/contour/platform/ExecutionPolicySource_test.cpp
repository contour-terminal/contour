// SPDX-License-Identifier: Apache-2.0
//
// PowerShell's execution policy as read from a registry, and the known-folders seam. The registry is a
// fake, so these run on every platform.

#include <contour/config/ExecutionPolicy.hpp>
#include <contour/config/ShellKind.hpp>
#include <contour/platform/ExecutionPolicySource.hpp>
#include <contour/platform/KnownFolders.hpp>
#include <contour/platform/Registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace contour::platform;
using contour::config::EffectivePolicy;
using contour::config::ExecutionPolicy;
using contour::config::PolicyScope;
using contour::config::ShellKind;

namespace
{

constexpr auto ShellIdsKey =
    std::string_view { "SOFTWARE\\Microsoft\\PowerShell\\1\\ShellIds\\Microsoft.PowerShell" };
constexpr auto WindowsPolicyKey = std::string_view { "SOFTWARE\\Policies\\Microsoft\\Windows\\PowerShell" };
constexpr auto CorePolicyKey = std::string_view { "SOFTWARE\\Policies\\Microsoft\\PowerShellCore" };
constexpr auto CurrentVersionKey = std::string_view { "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion" };

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

[[nodiscard]] EffectivePolicy policyOf(FakeRegistry& registry, ShellKind shell)
{
    return RegistryExecutionPolicySource { registry }.effectivePolicy(shell);
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
        auto registry = FakeRegistry {};
        auto source = RegistryExecutionPolicySource { registry };
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
        auto registry = FakeRegistry { Writes::Fail };
        auto source = RegistryExecutionPolicySource { registry };
        auto const result = source.allowUserScripts(ShellKind::WindowsPowerShell);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == "access denied");
        CHECK(registry.writes().size() == 1);
    }

    SECTION("no other shell has a user policy to set")
    {
        auto registry = FakeRegistry {};
        auto source = RegistryExecutionPolicySource { registry };
        for (auto const shell: { ShellKind::Pwsh, ShellKind::Bash, ShellKind::Unknown })
            CHECK_FALSE(source.allowUserScripts(shell).has_value());
        CHECK(registry.writes().empty());
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
    CHECK(registry.writeString(RegistryRoot::CurrentUser, ShellIdsKey, "ExecutionPolicy", "RemoteSigned")
              .error()
          == "no registry");
}

TEST_CASE("KnownFolders.null", "[executionpolicy]")
{
    CHECK(NullKnownFolders {}.documents() == std::nullopt);
}
