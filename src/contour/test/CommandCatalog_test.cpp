// SPDX-License-Identifier: Apache-2.0
//
// What the command palette offers, and how each command is identified and titled.
//
// The contract these tests hold down: every action a user could want is reachable from the palette,
// no action is offered in a form that cannot actually run, and a command's ID is stable — because the
// id is what gets written into command-history.yml and read back on the next start.

#include <contour/Command.h>
#include <contour/CommandCatalog.h>
#include <contour/Config.h>
#include <contour/test/GuiTestFixtures.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

using namespace contour;

namespace
{
[[nodiscard]] bool hasCommand(std::vector<Command> const& commands, std::string_view id)
{
    return std::ranges::any_of(commands, [&](auto const& c) { return c.id == id; });
}

[[nodiscard]] Command const* find(std::vector<Command> const& commands, std::string_view id)
{
    auto const found = std::ranges::find_if(commands, [&](auto const& c) { return c.id == id; });
    return found == commands.end() ? nullptr : &*found;
}
} // namespace

TEST_CASE("splitCamelCase turns an action name into a readable title", "[contour][palette]")
{
    CHECK(splitCamelCase("SplitVertical") == "Split Vertical");
    CHECK(splitCamelCase("ToggleSplitOrientation") == "Toggle Split Orientation");
    CHECK(splitCamelCase("OpenCommandPalette") == "Open Command Palette");

    SECTION("an acronym run stays whole")
    {
        // "Screenshot V T" would be nonsense; the run of capitals is one word.
        CHECK(splitCamelCase("ScreenshotVT") == "Screenshot VT");
        CHECK(splitCamelCase("ViNormalMode") == "Vi Normal Mode");
    }

    SECTION("degenerate inputs do not crash or gain a leading space")
    {
        CHECK(splitCamelCase("").empty());
        CHECK(splitCamelCase("Quit") == "Quit");
    }
}

TEST_CASE("A command's id encodes everything that distinguishes it", "[contour][palette]")
{
    SECTION("a parameterless action's id is simply its name")
    {
        CHECK(commandId(actions::SplitVertical {}) == "SplitVertical");
        CHECK(commandTitle(actions::SplitVertical {}) == "Split Vertical");
    }

    SECTION("an argument is part of the id, so two instances are two commands")
    {
        CHECK(commandId(actions::SwitchToTab { .position = 3 }) == "SwitchToTab:3");
        CHECK(commandId(actions::SwitchToTab { .position = 4 }) == "SwitchToTab:4");
        CHECK(commandTitle(actions::SwitchToTab { .position = 3 }) == "Switch To Tab 3");

        CHECK(commandId(actions::ChangeProfile { .name = "dark" }) == "ChangeProfile:dark");
        CHECK(commandTitle(actions::ChangeProfile { .name = "dark" }) == "Change Profile: dark");
    }

    SECTION("a DEFAULT argument stays out of the id, so the plain id is stable")
    {
        // "PasteClipboard" and "PasteClipboard:strip" are genuinely different commands — but the
        // unstripped one must keep the bare id, or every MRU entry recorded under it would orphan the
        // moment this logic was touched.
        CHECK(commandId(actions::PasteClipboard { .strip = false }) == "PasteClipboard");
        CHECK(commandId(actions::PasteClipboard { .strip = true }) == "PasteClipboard:strip");
        CHECK(commandId(actions::CopySelection { .format = actions::CopyFormat::Text }) == "CopySelection");
        CHECK(commandId(actions::CopySelection { .format = actions::CopyFormat::HTML })
              == "CopySelection:HTML");

        // Same story for the tab color: colorless is the default (it opens the picker), so it keeps the
        // bare id, while a color names a genuinely different command and must not collide with it.
        CHECK(commandId(actions::SetTabColor {}) == "SetTabColor");
        CHECK(commandTitle(actions::SetTabColor {}) == "Set Tab Color");
        CHECK(commandId(actions::SetTabColor { vtbackend::RGBColor { 0xFF, 0x00, 0x00 } })
              == "SetTabColor:#FF0000");
        CHECK(commandTitle(actions::SetTabColor { vtbackend::RGBColor { 0xFF, 0x00, 0x00 } })
              == "Set Tab Color: #FF0000");
        CHECK(commandId(actions::SetTabColor { vtbackend::RGBColor { 0x00, 0x80, 0xFF } })
              == "SetTabColor:#0080FF");

        // SaveLayout mirrors SetTabColor from the layout side: a nameless instance opens the save-as
        // prompt and keeps the bare id, while a named one is a genuinely different command.
        CHECK(commandId(actions::SaveLayout {}) == "SaveLayout");
        CHECK(commandTitle(actions::SaveLayout {}) == "Save Layout");
        CHECK(commandId(actions::SaveLayout { .name = "dev" }) == "SaveLayout:dev");
        CHECK(commandTitle(actions::SaveLayout { .name = "dev" }) == "Save Layout: dev");
    }
}

TEST_CASE("ActionCommandSource offers every action that can run without an argument", "[contour][palette]")
{
    auto const source = ActionCommandSource {};
    auto const commands = source.commands();

    SECTION("an unbound action is still reachable — the reason the palette exists")
    {
        // None of these has a default key binding. Before the palette they were unreachable without
        // first editing contour.yml.
        CHECK(hasCommand(commands, "ClearHistoryAndReset"));
        CHECK(hasCommand(commands, "SetTabTitle"));
        CHECK(hasCommand(commands, "ResetConfig"));

        // Coloring a tab was mouse-only (right-click -> "Choose Color…") before these actions existed.
        // Bare, SetTabColor opens that same picker, which is precisely why the palette may offer it
        // without the user having bound anything.
        CHECK(hasCommand(commands, "SetTabColor"));
        CHECK(hasCommand(commands, "ResetTabColor"));

        // Saving a layout had no palette entry at all before this — SaveLayout needed a name and so was
        // skipped like any parameterized action. Bare, it now opens the save-as name prompt, so the
        // palette offers it from the catalog. That is what lets a user create the FIRST layout (and hence
        // any LaunchLayout rows) from the palette rather than only from a hand-written key binding.
        CHECK(hasCommand(commands, "SaveLayout"));
    }

    SECTION("an action needing an argument is NOT offered bare")
    {
        // A default-constructed ChangeProfile names no profile and a default SwitchToTab means tab 0
        // (there is no tab 0). Offering them would put rows in the palette that do nothing when picked.
        CHECK_FALSE(hasCommand(commands, "ChangeProfile"));
        CHECK_FALSE(hasCommand(commands, "SwitchToTab"));
        CHECK_FALSE(hasCommand(commands, "SendChars"));
        CHECK_FALSE(hasCommand(commands, "LaunchLayout"));
        CHECK_FALSE(hasCommand(commands, "ResizePane"));
    }

    SECTION("an action whose argument has a meaningful default IS offered")
    {
        CHECK(hasCommand(commands, "PasteClipboard")); // strip=false is a perfectly good paste
        CHECK(hasCommand(commands, "CopySelection"));  // as text
        CHECK(hasCommand(commands, "NewTerminal"));    // current profile
    }

    SECTION("every command carries its documentation, for the palette's second line")
    {
        auto const* split = find(commands, "SplitVertical");
        REQUIRE(split != nullptr);
        CHECK(split->description == actions::documentation::SplitVertical);
        CHECK_FALSE(split->description.empty());
    }

    SECTION("every offered command has a description")
    {
        // The static_assert in Actions.h guarantees a catalog row per action; this guarantees the row's
        // documentation actually reached the palette.
        for (auto const& command: commands)
        {
            INFO("command: " << command.id);
            CHECK_FALSE(command.description.empty());
            CHECK_FALSE(command.title.empty());
        }
    }
}

TEST_CASE("BoundCommandSource makes a parameterized action reachable via its binding", "[contour][palette]")
{
    // This is the source that carries ARGUMENTS: the catalog can only offer HintMode empty, but the
    // user's Ctrl+Shift+U binding carries patterns="url", which is a command that actually runs.
    auto const source = BoundCommandSource { config::defaultInputMappings() };
    auto const commands = source.commands();

    CHECK(hasCommand(commands, "SwitchToTab:1"));
    CHECK(hasCommand(commands, "SwitchToTab:3"));
    CHECK(hasCommand(commands, "PasteClipboard:strip"));

    SECTION("the palette's own action is bound by default")
    {
        CHECK(hasCommand(commands, "OpenCommandPalette"));
    }
}

TEST_CASE("A bound SetTabColor reaches the palette as its own concrete row", "[contour][palette]")
{
    // The other half of the optional-argument design. The catalog offers the colorless SetTabColor (it
    // opens the picker); a user who bound a SPECIFIC color gets a SECOND, distinct row for it — so
    // "make this tab red" is one palette pick, not a pick plus a trip through the swatch grid.
    auto mappings = config::InputMappings {};
    mappings.keyMappings.push_back(config::KeyInputMapping {
        .modes { vtbackend::MatchModes {} },
        .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
        .input = vtbackend::Key::F5,
        .binding = { { actions::SetTabColor { vtbackend::RGBColor { 0xFF, 0x00, 0x00 } } } } });

    auto const commands = BoundCommandSource { mappings }.commands();

    REQUIRE(hasCommand(commands, "SetTabColor:#FF0000"));
    auto const* red = find(commands, "SetTabColor:#FF0000");
    REQUIRE(red != nullptr);
    CHECK(red->title == "Set Tab Color: #FF0000");
    CHECK_FALSE(red->description.empty());

    // And it did NOT collapse onto the colorless row: they are two commands, not one.
    CHECK_FALSE(hasCommand(commands, "SetTabColor"));
}

TEST_CASE("A bound SaveLayout with a name reaches the palette as its own concrete row", "[contour][palette]")
{
    // The layout-side mirror of the SetTabColor case above. The catalog offers the nameless SaveLayout
    // (it opens the save-as prompt); a user who bound SaveLayout to a SPECIFIC name gets a SECOND,
    // distinct row that saves straight to that name without the prompt.
    auto mappings = config::InputMappings {};
    mappings.keyMappings.push_back(
        config::KeyInputMapping { .modes { vtbackend::MatchModes {} },
                                  .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                                  .input = vtbackend::Key::F6,
                                  .binding = { { actions::SaveLayout { .name = "dev" } } } });

    auto const commands = BoundCommandSource { mappings }.commands();

    REQUIRE(hasCommand(commands, "SaveLayout:dev"));
    auto const* dev = find(commands, "SaveLayout:dev");
    REQUIRE(dev != nullptr);
    CHECK(dev->title == "Save Layout: dev");
    CHECK_FALSE(dev->description.empty());

    // And it did NOT collapse onto the nameless row: they are two commands, not one.
    CHECK_FALSE(hasCommand(commands, "SaveLayout"));
}

TEST_CASE("The live-state sources offer what actually exists right now", "[contour][palette]")
{
    auto config = config::Config {};
    config.profiles.value() = { { "main", config::TerminalProfile {} },
                                { "dark", config::TerminalProfile {} } };
    config.layouts.value() = { { "dev", config::Layout {} } };

    SECTION("one ChangeProfile per configured profile")
    {
        auto const source = ProfileCommandSource { config };
        auto const commands = source.commands();

        CHECK(commands.size() == 2);
        CHECK(hasCommand(commands, "ChangeProfile:main"));
        CHECK(hasCommand(commands, "ChangeProfile:dark"));
    }

    SECTION("one LaunchLayout per saved layout")
    {
        auto const source = LayoutCommandSource { config };
        auto const commands = source.commands();

        CHECK(commands.size() == 1);
        CHECK(hasCommand(commands, "LaunchLayout:dev"));
    }

    SECTION("one SwitchToTab per open tab, titled with the tab's own title")
    {
        auto const tabs = test::StubTabs { { "zsh", "vim" } };
        auto const source = TabCommandSource { tabs };
        auto const commands = source.commands();

        REQUIRE(commands.size() == 2);
        // 1-based, matching SwitchToTab's own documented positions — not the 0-based row index.
        auto const* first = find(commands, "SwitchToTab:1");
        REQUIRE(first != nullptr);
        // The user picks the tab they can SEE, rather than counting positions along the strip.
        CHECK(first->title == "Switch To Tab 1: zsh");

        auto const* second = find(commands, "SwitchToTab:2");
        REQUIRE(second != nullptr);
        CHECK(second->title == "Switch To Tab 2: vim");
    }

    SECTION("an untitled tab keeps the derived title rather than an empty one")
    {
        auto const tabs = test::StubTabs { { "" } };
        auto const source = TabCommandSource { tabs };
        auto const commands = source.commands();

        REQUIRE(commands.size() == 1);
        CHECK(commands.front().title == "Switch To Tab 1");
    }
}

TEST_CASE("collectCommands merges the sources without offering anything twice", "[contour][palette]")
{
    auto config = config::Config {};
    config.profiles.value() = { { "main", config::TerminalProfile {} } };

    auto const tabs = test::StubTabs { { "zsh", "vim", "htop" } };
    auto const tabCommands = TabCommandSource { tabs };
    auto const profileCommands = ProfileCommandSource { config };
    auto const boundCommands = BoundCommandSource { config::defaultInputMappings() };
    auto const actionCommands = ActionCommandSource {};

    auto const sources =
        std::vector<CommandSource const*> { &tabCommands, &profileCommands, &boundCommands, &actionCommands };
    auto const commands = collectCommands(sources);

    SECTION("every id is unique")
    {
        // A duplicate id would show the user the same command twice AND make the MRU ambiguous.
        auto seen = std::unordered_set<std::string> {};
        for (auto const& command: commands)
        {
            INFO("duplicate id: " << command.id);
            CHECK(seen.insert(command.id).second);
        }
    }

    SECTION("the first source wins a contested id")
    {
        // Both the tab source and the default Alt+3 binding offer SwitchToTab:3. The tab source is
        // listed first precisely because its row is the richer one — it names the tab.
        auto const* tab3 = find(commands, "SwitchToTab:3");
        REQUIRE(tab3 != nullptr);
        CHECK(tab3->title == "Switch To Tab 3: htop");
    }

    SECTION("commands from every source survive the merge")
    {
        CHECK(hasCommand(commands, "SwitchToTab:1"));        // tabs
        CHECK(hasCommand(commands, "ChangeProfile:main"));   // profiles
        CHECK(hasCommand(commands, "PasteClipboard:strip")); // bindings
        CHECK(hasCommand(commands, "ClearHistoryAndReset")); // the unbound catalog floor
    }

    SECTION("a null source is skipped rather than dereferenced")
    {
        auto const withNull = std::vector<CommandSource const*> { nullptr, &actionCommands };
        CHECK_FALSE(collectCommands(withNull).empty());
    }
}
