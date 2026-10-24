// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/CommandPolicy.hpp>

using vtextension::CommandPermission;
using vtextension::CommandPolicy;
using vtextension::ExtensionError;
using vtextension::GrantState;
using vtextension::IGrantStore;
using vtextension::Manifest;

namespace
{

using Args = std::vector<std::pair<std::string, std::string>>;

/// An in-memory grant store.
class FakeGrantStore final: public IGrantStore
{
  public:
    [[nodiscard]] GrantState lookup(std::string_view hostId, std::string_view digest) const override
    {
        auto const it = _grants.find({ std::string(hostId), std::string(digest) });
        return it != _grants.end() ? it->second : GrantState::Pending;
    }

    void record(std::string_view hostId, std::string_view digest, GrantState state) override
    {
        _grants[{ std::string(hostId), std::string(digest) }] = state;
    }

  private:
    std::map<std::pair<std::string, std::string>, GrantState> _grants;
};

/// What the host calls the installation under test.
constexpr auto HostId = std::string_view("config:git|/opt/ext/git.wasm");

[[nodiscard]] Manifest gitManifest(std::string digest = "digest-a")
{
    return Manifest {
        .name = "git",
        .version = 1,
        .commands = { { .name = "status",
                        .argvTemplate = { "git", "-C", "{cwd}", "status", "--porcelain=v2", "--branch" } },
                      { .name = "version", .argvTemplate = { "git", "--version" } } },
        .digest = std::move(digest),
    };
}

} // namespace

TEST_CASE("CommandPolicy.undeclared", "[vtextension]")
{
    auto store = FakeGrantStore();
    store.record(HostId, "digest-a", GrantState::Granted);
    auto const policy = CommandPolicy(gitManifest(), std::string(HostId), store, CommandPermission::Ask);

    auto const result = policy.resolve("push", Args {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ExtensionError::CommandNotDeclared);
}

TEST_CASE("CommandPolicy.ask.pending", "[vtextension]")
{
    auto store = FakeGrantStore();
    auto const policy = CommandPolicy(gitManifest(), std::string(HostId), store, CommandPermission::Ask);

    auto const result = policy.resolve("version", Args {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ExtensionError::PermissionPending);
}

TEST_CASE("CommandPolicy.granted", "[vtextension]")
{
    auto store = FakeGrantStore();
    store.record(HostId, "digest-a", GrantState::Granted);
    auto const policy = CommandPolicy(gitManifest(), std::string(HostId), store, CommandPermission::Ask);

    auto const result = policy.resolve("status", Args { { "cwd", "/home/me/my project" } });
    REQUIRE(result.has_value());
    CHECK(*result
          == std::vector<std::string> {
              "git", "-C", "/home/me/my project", "status", "--porcelain=v2", "--branch" });
}

TEST_CASE("CommandPolicy.digestChanged", "[vtextension]")
{
    auto store = FakeGrantStore();
    store.record(HostId, "digest-a", GrantState::Granted);
    auto const policy =
        CommandPolicy(gitManifest("digest-b"), std::string(HostId), store, CommandPermission::Ask);

    auto const result = policy.resolve("version", Args {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ExtensionError::PermissionPending);
}

TEST_CASE("CommandPolicy.deny", "[vtextension]")
{
    auto store = FakeGrantStore();
    store.record(HostId, "digest-a", GrantState::Granted);
    auto const policy = CommandPolicy(gitManifest(), std::string(HostId), store, CommandPermission::Deny);

    SECTION("a declared command")
    {
        auto const result = policy.resolve("version", Args {});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ExtensionError::PermissionDenied);
    }
    SECTION("an undeclared command: Deny is checked first")
    {
        auto const result = policy.resolve("push", Args {});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ExtensionError::PermissionDenied);
    }
}

TEST_CASE("CommandPolicy.storeDenied", "[vtextension]")
{
    auto store = FakeGrantStore();
    store.record(HostId, "digest-a", GrantState::Denied);

    SECTION("Ask")
    {
        auto const policy = CommandPolicy(gitManifest(), std::string(HostId), store, CommandPermission::Ask);
        auto const result = policy.resolve("version", Args {});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ExtensionError::PermissionDenied);
    }
    SECTION("a configured Allow does not override the user's refusal")
    {
        auto const policy =
            CommandPolicy(gitManifest(), std::string(HostId), store, CommandPermission::Allow);
        auto const result = policy.resolve("version", Args {});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ExtensionError::PermissionDenied);
    }
}

TEST_CASE("CommandPolicy.allow", "[vtextension]")
{
    auto store = FakeGrantStore();
    auto const policy = CommandPolicy(gitManifest(), std::string(HostId), store, CommandPermission::Allow);

    SECTION("pending is pre-approved")
    {
        auto const result = policy.resolve("version", Args {});
        REQUIRE(result.has_value());
        CHECK(*result == std::vector<std::string> { "git", "--version" });
    }
    SECTION("an undeclared command still fails")
    {
        auto const result = policy.resolve("push", Args {});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ExtensionError::CommandNotDeclared);
    }
}

TEST_CASE("CommandPolicy.grantIsPerHostId", "[vtextension]")
{
    auto store = FakeGrantStore();
    store.record("config:other|/opt/ext/git.wasm", "digest-a", GrantState::Granted);
    auto const policy = CommandPolicy(gitManifest(), std::string(HostId), store, CommandPermission::Ask);

    auto const result = policy.resolve("version", Args {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ExtensionError::PermissionPending);
}

TEST_CASE("CommandPolicy.placeholders", "[vtextension]")
{
    auto store = FakeGrantStore();
    store.record(HostId, "digest-a", GrantState::Granted);
    auto const policy = CommandPolicy(gitManifest(), std::string(HostId), store, CommandPermission::Ask);

    SECTION("a missing argument does not match the declared command")
    {
        auto const result = policy.resolve("status", Args {});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ExtensionError::CommandNotDeclared);
    }
    SECTION("an argument that names no placeholder is ignored")
    {
        auto const result = policy.resolve("version", Args { { "cwd", "/x" } });
        REQUIRE(result.has_value());
        CHECK(*result == std::vector<std::string> { "git", "--version" });
    }
    SECTION("a value is one argv element, whatever it contains")
    {
        auto const result = policy.resolve("status", Args { { "cwd", "x; rm -rf / $(id)" } });
        REQUIRE(result.has_value());
        CHECK(result->at(2) == "x; rm -rf / $(id)");
        CHECK(result->size() == 6);
    }
    SECTION("the first of repeated names wins")
    {
        auto const result = policy.resolve("status", Args { { "cwd", "/first" }, { "cwd", "/second" } });
        REQUIRE(result.has_value());
        CHECK(result->at(2) == "/first");
    }
}

TEST_CASE("CommandPolicy.identicalManifestsDoNotShareGrants", "[vtextension]")
{
    // The manifest's own name is chosen by the guest; a second installation that copies it byte for
    // byte must not inherit the first one's grant.
    auto store = FakeGrantStore();
    store.record("config:first|/opt/ext/a.wasm", "digest-a", GrantState::Granted);

    auto const first =
        CommandPolicy(gitManifest(), "config:first|/opt/ext/a.wasm", store, CommandPermission::Ask);
    auto const second =
        CommandPolicy(gitManifest(), "config:second|/opt/ext/b.wasm", store, CommandPermission::Ask);

    CHECK(first.resolve("version", Args {}).has_value());
    auto const result = second.resolve("version", Args {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ExtensionError::PermissionPending);
}

TEST_CASE("CommandPolicy.argumentRejected", "[vtextension]")
{
    auto store = FakeGrantStore();
    store.record(HostId, "digest-a", GrantState::Granted);
    auto manifest = gitManifest();
    manifest.commands.push_back({ .name = "log", .argvTemplate = { "git", "log", "{ref}" } });
    manifest.commands.push_back({ .name = "logSafe", .argvTemplate = { "git", "log", "--", "{path}" } });
    manifest.commands.push_back(
        { .name = "late", .argvTemplate = { "git", "log", "{ref}", "--", "{path}" } });
    auto const policy = CommandPolicy(manifest, std::string(HostId), store, CommandPermission::Ask);

    SECTION("a value that looks like an option, before any --")
    {
        auto const result = policy.resolve("log", Args { { "ref", "--output=/home/u/.bashrc" } });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ExtensionError::ArgumentRejected);
    }
    SECTION("a single dash is refused too")
    {
        auto const result = policy.resolve("log", Args { { "ref", "-" } });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ExtensionError::ArgumentRejected);
    }
    SECTION("a value that merely contains a dash")
    {
        auto const result = policy.resolve("log", Args { { "ref", "feature-x" } });
        REQUIRE(result.has_value());
        CHECK(result->back() == "feature-x");
    }
    SECTION("after a literal --, a leading dash is data")
    {
        auto const result = policy.resolve("logSafe", Args { { "path", "-weird" } });
        REQUIRE(result.has_value());
        CHECK(*result == std::vector<std::string> { "git", "log", "--", "-weird" });
    }
    SECTION("the -- must precede the placeholder")
    {
        auto const result = policy.resolve("late", Args { { "ref", "-x" }, { "path", "-y" } });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ExtensionError::ArgumentRejected);

        auto const ok = policy.resolve("late", Args { { "ref", "x" }, { "path", "-y" } });
        REQUIRE(ok.has_value());
    }
    SECTION("a NUL is refused even after --")
    {
        auto const result = policy.resolve("logSafe", Args { { "path", std::string("a\0b", 3) } });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ExtensionError::ArgumentRejected);
    }
}

TEST_CASE("CommandPolicy.controlConsent", "[vtextension][control]")
{
    // The control permissions share the commands' answer, but `allow` pre-approves commands only.
    auto store = FakeGrantStore();
    auto const policyFor = [&](CommandPermission configured) {
        return CommandPolicy(gitManifest(), std::string(HostId), store, configured);
    };
    CHECK(policyFor(CommandPermission::Ask).controlConsent() == GrantState::Pending);
    CHECK(policyFor(CommandPermission::Allow).controlConsent() == GrantState::Pending);
    CHECK(policyFor(CommandPermission::Deny).controlConsent() == GrantState::Denied);

    store.record(HostId, "digest-a", GrantState::Granted);
    CHECK(policyFor(CommandPermission::Ask).controlConsent() == GrantState::Granted);
    CHECK(policyFor(CommandPermission::Deny).controlConsent() == GrantState::Denied);
    CHECK(CommandPolicy(gitManifest("digest-b"), std::string(HostId), store, CommandPermission::Ask)
              .controlConsent()
          == GrantState::Pending);

    store.record(HostId, "digest-a", GrantState::Denied);
    CHECK(policyFor(CommandPermission::Ask).controlConsent() == GrantState::Denied);
}
