// SPDX-License-Identifier: Apache-2.0
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/ExtensionGrantStore.hpp>

using core::platform::testing::FileEntry;
using core::platform::testing::InMemoryFileSystem;
using vtextension::ExtensionGrantKey;
using vtextension::ExtensionGrants;
using vtextension::ExtensionGrantsHeader;
using vtextension::ExtensionGrantStore;
using vtextension::GrantFileError;
using vtextension::GrantState;
using vtextension::parseExtensionGrants;
using vtextension::serializeExtensionGrants;
using namespace std::string_literals;

namespace
{
[[nodiscard]] std::filesystem::path grantsFile()
{
    return { "/home/user/.config/contour/extension-grants.txt" };
}

constexpr auto GitId = std::string_view { "git@/usr/share/contour/extensions/git.wasm" };
constexpr auto GitDigest = std::string_view { "0123abcd" };
} // namespace

TEST_CASE("ExtensionGrants: what a grants file reads as", "[vtextension][extensions]")
{
    struct Case
    {
        std::string_view name;
        std::string text;
        ExtensionGrants expected;
    };
    auto const header = std::string { ExtensionGrantsHeader };
    auto const cases = std::vector<Case> {
        { .name = "just the header", .text = header + "\n", .expected = {} },
        { .name = "an answer of each kind",
          .text = header + "\ngranted\tabc\tgit@/a.wasm\ndenied\tdef\tclock@/b.wasm\n",
          .expected = { { { "git@/a.wasm", "abc" }, GrantState::Granted },
                        { { "clock@/b.wasm", "def" }, GrantState::Denied } } },
        { .name = "comments and blank lines, before the header too",
          .text = "# remembered answers\n\n" + header + "\n# a comment\n\ngranted\tabc\tgit@/a.wasm\n",
          .expected = { { { "git@/a.wasm", "abc" }, GrantState::Granted } } },
        { .name = "CRLF line ends, as Windows editors save it",
          .text = header + "\r\ngranted\tabc\tgit@/a.wasm\r\n",
          .expected = { { { "git@/a.wasm", "abc" }, GrantState::Granted } } },
        { .name = "the host id may hold tabs and spaces: it is the rest of the line",
          .text = header + "\ngranted\tabc\tgit@/my dir/a\tb.wasm\n",
          .expected = { { { "git@/my dir/a\tb.wasm", "abc" }, GrantState::Granted } } },
        { .name = "a damaged line costs that answer alone",
          .text =
              header + "\nmaybe\tabc\tgit@/a.wasm\ngranted\tabc\ngranted\t\tx\ngranted\tdef\tclock@/b.wasm\n",
          .expected = { { { "clock@/b.wasm", "def" }, GrantState::Granted } } },
        { .name = "the last answer for one key wins",
          .text = header + "\ngranted\tabc\tgit@/a.wasm\ndenied\tabc\tgit@/a.wasm\n",
          .expected = { { { "git@/a.wasm", "abc" }, GrantState::Denied } } },
        { .name = "no line break after the last line",
          .text = header + "\ngranted\tabc\tgit@/a.wasm",
          .expected = { { { "git@/a.wasm", "abc" }, GrantState::Granted } } },
    };
    for (auto const& testCase: cases)
    {
        INFO(testCase.name);
        auto const parsed = parseExtensionGrants(testCase.text);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == testCase.expected);
    }
}

TEST_CASE("ExtensionGrants: a file that is not one", "[vtextension][extensions]")
{
    for (auto const& text: { ""s, "granted\tabc\tgit@/a.wasm\n"s, "contour-extension-grants 2\n"s })
    {
        INFO(text);
        auto const parsed = parseExtensionGrants(text);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == GrantFileError::UnknownFormat);
    }
}

TEST_CASE("ExtensionGrants: what is written reads back", "[vtextension][extensions]")
{
    auto const grants = ExtensionGrants {
        { { "git@/a.wasm", "abc" }, GrantState::Granted },
        { { "clock@/my dir/b\tc.wasm", "def" }, GrantState::Denied },
    };
    auto const text = serializeExtensionGrants(grants);
    CHECK(text.starts_with("#"));
    auto const parsed = parseExtensionGrants(text);
    REQUIRE(parsed.has_value());
    CHECK(*parsed == grants);

    SECTION("what a line could not hold is left out, not written broken")
    {
        auto withBreaks = grants;
        withBreaks[{ "evil@/a\nb.wasm", "abc" }] = GrantState::Granted;
        withBreaks[{ "evil@/a.wasm", "ab\tc" }] = GrantState::Granted;
        auto const reparsed = parseExtensionGrants(serializeExtensionGrants(withBreaks));
        REQUIRE(reparsed.has_value());
        CHECK(*reparsed == grants);
    }
}

TEST_CASE("ExtensionGrantStore: answers survive in the side file", "[vtextension][extensions]")
{
    auto fileSystem =
        InMemoryFileSystem { FileEntry { .path = grantsFile().parent_path(), .isDirectory = true } };

    SECTION("no file: nothing answered")
    {
        auto const store = ExtensionGrantStore { fileSystem, grantsFile() };
        CHECK(store.lookup(GitId, GitDigest) == GrantState::Pending);
    }

    SECTION("an answer is written, and a new store reads it")
    {
        {
            auto store = ExtensionGrantStore { fileSystem, grantsFile() };
            store.record(GitId, GitDigest, GrantState::Granted);
            CHECK(store.lookup(GitId, GitDigest) == GrantState::Granted);
        }
        auto const reopened = ExtensionGrantStore { fileSystem, grantsFile() };
        CHECK(reopened.lookup(GitId, GitDigest) == GrantState::Granted);
        // Bound to the digest: a changed manifest is a new question.
        CHECK(reopened.lookup(GitId, "ffff") == GrantState::Pending);
        // Bound to the host id: another installation with the same manifest is a new question too.
        CHECK(reopened.lookup("git@/home/user/git.wasm", GitDigest) == GrantState::Pending);
        CHECK_FALSE(fileSystem.exists(grantsFile().string() + ".tmp"));
    }

    SECTION("Pending forgets the answer")
    {
        auto store = ExtensionGrantStore { fileSystem, grantsFile() };
        store.record(GitId, GitDigest, GrantState::Denied);
        store.record(GitId, GitDigest, GrantState::Pending);
        CHECK(store.lookup(GitId, GitDigest) == GrantState::Pending);
        CHECK(ExtensionGrantStore { fileSystem, grantsFile() }.lookup(GitId, GitDigest)
              == GrantState::Pending);
    }

    SECTION("a file of another format is not read, and an answer still counts for this run")
    {
        REQUIRE(fileSystem.writeFile(grantsFile(), "something else\n").has_value());
        auto store = ExtensionGrantStore { fileSystem, grantsFile() };
        CHECK(store.lookup(GitId, GitDigest) == GrantState::Pending);
        store.record(GitId, GitDigest, GrantState::Granted);
        CHECK(store.lookup(GitId, GitDigest) == GrantState::Granted);
    }
}

namespace
{
constexpr auto TestDefaultLabel = std::string_view { "default" };

/// @return The identity of a daemon serving @p label at @p socket, where its label would serve at @p usual.
[[nodiscard]] vtextension::DaemonIdentity daemonAt(std::string_view label,
                                                   std::filesystem::path socket,
                                                   std::filesystem::path usual)
{
    return vtextension::DaemonIdentity { .label = label,
                                         .defaultLabel = TestDefaultLabel,
                                         .socketPath = std::move(socket),
                                         .labelSocketPath = std::move(usual) };
}

/// @return The identity of a daemon serving @p label where that label serves when nothing names a socket.
[[nodiscard]] vtextension::DaemonIdentity daemonOf(std::string_view label)
{
    auto const usual = std::filesystem::path { "/run/user/1000/contour" } / std::string { label };
    return daemonAt(label, usual, usual);
}
} // namespace

TEST_CASE("ExtensionGrants: a daemon keeps its answers in a file of its own", "[vtextension][extensions]")
{
    // A daemon's answers come from whichever client is attached, and govern commands that run on the
    // daemon's machine: another authority than the GUI's, in another file -- one per daemon, as several
    // daemons can share one configuration directory, told apart by their label or their socket.
    using vtextension::daemonExtensionGrantsFileName;

    SECTION("by label")
    {
        struct Case
        {
            std::string_view label;
            std::string_view expected;
        };
        constexpr auto Cases = std::array {
            Case { .label = "default", .expected = "extension-grants.daemon.txt" },
            Case { .label = "", .expected = "extension-grants.daemon.txt" },
            Case { .label = "work", .expected = "extension-grants.daemon.work.txt" },
            Case { .label = "a.b-c_d", .expected = "extension-grants.daemon.a.b-c_d.txt" },
            // A label is a socket name, never a path: nothing in it leaves the configuration directory.
            Case { .label = "../../x", .expected = "extension-grants.daemon..._.._x.txt" },
            Case { .label = "a/b\\c", .expected = "extension-grants.daemon.a_b_c.txt" },
        };
        for (auto const& [label, expected]: Cases)
        {
            INFO("label " << label);
            CHECK(daemonExtensionGrantsFileName(daemonOf(label)) == expected);
        }
        CHECK(daemonExtensionGrantsFileName(daemonOf(TestDefaultLabel))
              == vtextension::DaemonExtensionGrantsFileName);
        CHECK(vtextension::DaemonExtensionGrantsFileName != vtextension::ExtensionGrantsFileName);
    }

    SECTION("by socket, where one is named in place of the label's own")
    {
        auto const usual = std::filesystem::path { "/run/user/1000/contour/default" };
        auto const first = daemonExtensionGrantsFileName(daemonAt(TestDefaultLabel, "/tmp/a/sock", usual));
        auto const second = daemonExtensionGrantsFileName(daemonAt(TestDefaultLabel, "/tmp/b/sock", usual));
        // Two daemons on the default label at two sockets: two files, each its own every time.
        CHECK(first != second);
        CHECK(first != vtextension::DaemonExtensionGrantsFileName);
        CHECK(first == daemonExtensionGrantsFileName(daemonAt(TestDefaultLabel, "/tmp/a/sock", usual)));
        // Named by the socket's path as it is, not as it was spelled.
        CHECK(first
              == daemonExtensionGrantsFileName(daemonAt(TestDefaultLabel, "/tmp/a/./../a/sock", usual)));
        // A short hash: `extension-grants.daemon.` plus eight hex digits.
        CHECK(first.size() == std::string_view { "extension-grants.daemon.0123abcd.txt" }.size());
        CHECK(first.starts_with("extension-grants.daemon."));
        CHECK(first.ends_with(".txt"));
        // A label and a socket of its own name both.
        auto const labelled =
            daemonExtensionGrantsFileName(daemonAt("work", "/tmp/a/sock", "/run/user/1000/contour/work"));
        CHECK(labelled.starts_with("extension-grants.daemon.work."));
        CHECK(labelled != daemonExtensionGrantsFileName(daemonOf("work")));
    }
}

TEST_CASE("ExtensionGrantStore: a GUI's denial survives a daemon's record", "[vtextension][extensions]")
{
    // Each store keeps its answers in memory and rewrites its file whole: two of them on one file would
    // undo each other's answers. Revoking an extension's commands in the GUI must stay revoked.
    auto const directory = grantsFile().parent_path();
    auto fileSystem = InMemoryFileSystem { FileEntry { .path = directory, .isDirectory = true } };
    auto const guiFile = directory / vtextension::ExtensionGrantsFileName;
    auto const daemonFile =
        directory / vtextension::daemonExtensionGrantsFileName(daemonOf(TestDefaultLabel));
    {
        auto gui = ExtensionGrantStore { fileSystem, guiFile };
        gui.record(GitId, GitDigest, GrantState::Granted);
    }

    auto gui = ExtensionGrantStore { fileSystem, guiFile };
    auto daemon = ExtensionGrantStore { fileSystem, daemonFile };
    gui.record(GitId, GitDigest, GrantState::Denied);
    daemon.record("other@/x.wasm", "ffff", GrantState::Granted);

    CHECK(ExtensionGrantStore { fileSystem, guiFile }.lookup(GitId, GitDigest) == GrantState::Denied);
    CHECK(ExtensionGrantStore { fileSystem, daemonFile }.lookup(GitId, GitDigest) == GrantState::Pending);
}
