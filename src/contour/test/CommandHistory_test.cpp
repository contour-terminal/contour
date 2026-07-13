// SPDX-License-Identifier: Apache-2.0
//
// The command palette's most-recently-used list, and its persistence.
//
// The MRU is the one part of the palette that carries state across restarts, so these tests drive the
// full record -> persist -> reload cycle through the injected store — no filesystem involved.

#include <contour/CommandHistory.h>
#include <contour/CommandHistoryStore.h>
#include <contour/test/GuiTestFixtures.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

using namespace contour;

namespace
{
/// The history's contents as a plain vector, for comparison.
[[nodiscard]] std::vector<std::string> contents(CommandHistory const& history)
{
    auto const recent = history.recent();
    return std::vector<std::string>(recent.begin(), recent.end());
}
} // namespace

TEST_CASE("CommandHistory remembers what was used most recently", "[contour][palette]")
{
    SECTION("the newest command comes first")
    {
        auto history = CommandHistory { 5 };
        history.record("SplitVertical");
        history.record("CreateNewTab");
        history.record("TogglePaneZoom");

        CHECK(contents(history)
              == std::vector<std::string> { "TogglePaneZoom", "CreateNewTab", "SplitVertical" });
    }

    SECTION("re-running a command moves it to the front rather than duplicating it")
    {
        // This is what makes it a most-recently-USED list and not a log of every invocation: a user who
        // splits ten times must not see ten "Split Vertical" rows and nothing else.
        auto history = CommandHistory { 5 };
        history.record("SplitVertical");
        history.record("CreateNewTab");
        history.record("SplitVertical");

        CHECK(contents(history) == std::vector<std::string> { "SplitVertical", "CreateNewTab" });
    }

    SECTION("the oldest command falls off the end at capacity")
    {
        auto history = CommandHistory { 2 };
        history.record("A");
        history.record("B");
        history.record("C");

        CHECK(contents(history) == std::vector<std::string> { "C", "B" });
    }

    SECTION("a capacity of zero disables the history entirely")
    {
        // The documented meaning of `command_palette_recent_count: 0` — no section, and nothing
        // recorded either (the user asked not to be tracked).
        auto history = CommandHistory { 0 };
        history.record("SplitVertical");

        CHECK(history.recent().empty());
    }

    SECTION("an empty id is not recorded")
    {
        auto history = CommandHistory { 5 };
        history.record("");

        CHECK(history.recent().empty());
    }
}

TEST_CASE("CommandHistory::reset seeds the list from persistence", "[contour][palette]")
{
    SECTION("the stored order is preserved, newest first")
    {
        auto history = CommandHistory { 5 };
        auto const stored = std::vector<std::string> { "C", "B", "A" };
        history.reset(stored);

        CHECK(contents(history) == stored);
    }

    SECTION("a stored list longer than the capacity is truncated")
    {
        // Shrinking `command_palette_recent_count` must actually take effect, rather than silently
        // keeping a longer list alive because it happened to be on disk already.
        auto history = CommandHistory { 2 };
        history.reset(std::vector<std::string> { "C", "B", "A" });

        CHECK(contents(history) == std::vector<std::string> { "C", "B" });
    }
}

TEST_CASE("CommandHistory::setCapacity re-applies a changed recent_count", "[contour][palette]")
{
    // Editing recent_count and reloading the config takes effect without a restart, so the capacity is
    // re-applied on every palette open rather than being frozen at construction.
    auto history = CommandHistory { 5 };
    history.record("A");
    history.record("B");
    history.record("C");

    history.setCapacity(2);
    CHECK(contents(history) == std::vector<std::string> { "C", "B" });

    history.setCapacity(5);
    // Growing it back cannot resurrect what was already dropped — but it must accept new entries again.
    history.record("D");
    CHECK(contents(history) == std::vector<std::string> { "D", "C", "B" });
}

TEST_CASE("The command history survives a restart", "[contour][palette]")
{
    // The whole point of persisting it. One "session" records commands through the store; a second,
    // fresh CommandHistory reads them back and sees the same order.
    auto store = test::InMemoryCommandHistoryStore {};
    auto const path = std::filesystem::path { "command-history.yml" };

    {
        auto session = CommandHistory { 5 };
        session.record("SplitVertical");
        session.record("CreateNewTab");
        REQUIRE(store.save(path, session.recent()).has_value());
    }

    auto restarted = CommandHistory { 5 };
    auto const stored = store.load(path);
    REQUIRE(stored.has_value());
    restarted.reset(*stored);

    CHECK(contents(restarted) == std::vector<std::string> { "CreateNewTab", "SplitVertical" });
}

TEST_CASE("An unreadable command-history file leaves the palette usable", "[contour][palette]")
{
    // A corrupt file must degrade to "no recent commands", not to a broken palette: the MRU is a
    // convenience, and losing it is not worth failing an open over.
    auto store = test::InMemoryCommandHistoryStore {};
    store.loadError = "boom";

    auto const stored = store.load(std::filesystem::path { "command-history.yml" });
    REQUIRE_FALSE(stored.has_value());
    CHECK(stored.error() == "boom");

    auto history = CommandHistory { 5 };
    CHECK(history.recent().empty());
}

TEST_CASE("FileCommandHistoryStore round-trips through a real file", "[contour][palette]")
{
    // The production store, against a real (temporary) file — the in-memory double above cannot catch a
    // YAML quoting bug, and an id CAN carry YAML-significant characters (a SendChars binding's escape
    // sequence), which would otherwise produce a file that fails to parse on the next start.
    auto const tempDir = QTemporaryDir {};
    REQUIRE(tempDir.isValid());
    auto const path = std::filesystem::path(tempDir.path().toStdString()) / "command-history.yml";

    auto store = FileCommandHistoryStore {};

    SECTION("a missing file is not an error — a first run simply has no history")
    {
        auto const loaded = store.load(path);
        REQUIRE(loaded.has_value());
        CHECK(loaded->empty());
    }

    SECTION("what is saved is what is loaded, order intact")
    {
        auto const ids = std::vector<std::string> { "SplitVertical", "SwitchToTab:3", "ChangeProfile:dark" };
        REQUIRE(store.save(path, ids).has_value());
        CHECK(std::filesystem::exists(path));

        auto const loaded = store.load(path);
        REQUIRE(loaded.has_value());
        CHECK(*loaded == ids);
    }

    SECTION("an id carrying YAML-significant characters survives the round trip")
    {
        auto const ids = std::vector<std::string> { "SendChars:\x1b[A", "WriteScreen:#: {not a map}" };
        REQUIRE(store.save(path, ids).has_value());

        auto const loaded = store.load(path);
        REQUIRE(loaded.has_value());
        CHECK(*loaded == ids);
    }
}
