// SPDX-License-Identifier: Apache-2.0
//
// The chord -> "Ctrl+Shift+P" renderer that feeds the command palette's shortcut column.
//
// The column exists to TEACH: a user who ran a command from the palette should read off the chord and
// press it next time. So these tests care about the two ways that promise can be broken — rendering a
// chord the user cannot type, and attaching a chord to the wrong command.

#include <contour/command/Command.hpp>
#include <contour/command/Shortcut.hpp>
#include <contour/config/Config.hpp>
#include <contour/test/CoreFixtures.hpp>

#include <catch2/catch_test_macros.hpp>

#include <utility>

using namespace contour;
using namespace contour::command;
using vtbackend::Key;
using vtbackend::Modifier;
using vtbackend::Modifiers;

TEST_CASE("shortcutText renders a chord the way a user would type it", "[contour][palette]")
{
    SECTION("modifiers render in the conventional order, not the bit order")
    {
        // std::formatter<Modifiers> would give "Shift|Control" — ascending bit order, pipe-joined.
        // Neither is what a keyboard hint looks like, which is the whole reason this renderer exists.
        CHECK(shortcutText(Modifiers { Modifier::Control, Modifier::Shift },
                           ShortcutInput { U'P' },
                           ShortcutStyle::Text)
              == "Ctrl+Shift+P");
        CHECK(shortcutText(Modifiers { Modifier::Shift, Modifier::Control },
                           ShortcutInput { U'P' },
                           ShortcutStyle::Text)
              == "Ctrl+Shift+P");
    }

    SECTION("the order is fixed regardless of how the chord was assembled")
    {
        // The key keeps the name the config gives it ("LeftArrow", via std::formatter<Key>) rather
        // than a prettier synonym: reusing that vocabulary means the shortcut column also teaches the
        // user what to write in `input_mapping:` if they want to rebind it.
        auto const expected = std::string { "Ctrl+Alt+Shift+LeftArrow" };
        CHECK(shortcutText(Modifiers { Modifier::Shift, Modifier::Alt, Modifier::Control },
                           ShortcutInput { Key::LeftArrow },
                           ShortcutStyle::Text)
              == expected);
        CHECK(shortcutText(Modifiers { Modifier::Control, Modifier::Shift, Modifier::Alt },
                           ShortcutInput { Key::LeftArrow },
                           ShortcutStyle::Text)
              == expected);
    }

    SECTION("both halves of the input mappings render: a named key and a character")
    {
        CHECK(shortcutText(Modifiers { Modifier::Alt }, ShortcutInput { Key::Enter }, ShortcutStyle::Text)
              == "Alt+Enter");
        CHECK(shortcutText(Modifiers { Modifier::Control, Modifier::Shift },
                           ShortcutInput { U'T' },
                           ShortcutStyle::Text)
              == "Ctrl+Shift+T");
    }

    SECTION("an unmodified key is just the key")
    {
        CHECK(shortcutText(Modifiers {}, ShortcutInput { Key::F3 }, ShortcutStyle::Text) == "F3");
    }
}

TEST_CASE("shortcutIndex advertises the built-in chords the user's bindings leave free", "[contour][palette]")
{
    auto const openOutput = commandId(actions::OpenCommandOutput {});

    // Ctrl+Shift+G is in no default binding, so only the fallback table can name it.
    auto const defaults = config::Config {};
    auto const index = shortcutIndex(defaults, ShortcutStyle::Text);
    REQUIRE(index.contains(openOutput));
    CHECK(index.at(openOutput) == "Ctrl+Shift+G");

    // A user binding of the same chord is found first, so the built-in one never runs.
    auto mappings = defaults.inputMappings.value();
    mappings.charMappings.push_back(
        config::CharInputMapping { .modes {},
                                   .modifiers { Modifier::Control, Modifier::Shift },
                                   .input = U'G',
                                   .binding = { actions::ScrollToTop {} } });
    auto shadowing = config::Config {};
    shadowing.inputMappings = std::move(mappings);
    auto const shadowed = shortcutIndex(shadowing, ShortcutStyle::Text);
    CHECK_FALSE(shadowed.contains(openOutput));
}

TEST_CASE("shortcutIndex maps commands to the chord that runs them", "[contour][palette]")
{
    auto const& defaults = config::defaultInputMappings();
    auto const index = shortcutIndex(defaults, ShortcutStyle::Text);

    SECTION("the default bindings are found under their command id")
    {
        REQUIRE(index.contains("CreateNewTab"));
        CHECK(index.at("CreateNewTab") == "Ctrl+Shift+T");

        REQUIRE(index.contains("SplitVertical"));
        CHECK(index.at("SplitVertical") == "Ctrl+Shift+E");

        REQUIRE(index.contains("ToggleFullscreen"));
        CHECK(index.at("ToggleFullscreen") == "Alt+Enter");

        // The palette's own chord, so opening the palette teaches you how to open the palette.
        REQUIRE(index.contains("OpenCommandPalette"));
        CHECK(index.at("OpenCommandPalette") == "Ctrl+Shift+P");
    }

    SECTION("a parameterized binding is indexed under its ARGUMENT-BEARING id")
    {
        // Alt+3 runs SwitchToTab{position:3}. Indexing it under a bare "SwitchToTab" would advertise
        // Alt+3 next to every tab in the list — telling the user that Alt+3 switches to tab 7.
        REQUIRE(index.contains("SwitchToTab:3"));
        CHECK(index.at("SwitchToTab:3") == "Alt+3");
        CHECK_FALSE(index.contains("SwitchToTab"));
    }

    SECTION("an action with no binding has no entry")
    {
        // Nothing binds these by default; the palette shows them with an empty shortcut column — and
        // being reachable at all despite having no chord is exactly the point of having a palette.
        CHECK_FALSE(index.contains("ClearHistoryAndReset"));
        CHECK_FALSE(index.contains("SetTabTitle"));
        CHECK_FALSE(index.contains("ResetConfig"));
    }

    SECTION("a multi-action chord is not advertised as any one command's shortcut")
    {
        // A chord that fires three actions is not "the" shortcut for any of them: pressing it would do
        // the other two as well, so putting it next to a single command would be a lie.
        auto mappings = config::InputMappings {};
        mappings.charMappings.push_back(
            config::CharInputMapping { .modes {},
                                       .modifiers { Modifier::Control },
                                       .input = U'Q',
                                       .binding = { actions::ScrollToTop {}, actions::CancelSelection {} } });

        auto const multi = shortcutIndex(mappings, ShortcutStyle::Text);
        CHECK_FALSE(multi.contains("ScrollToTop"));
        CHECK_FALSE(multi.contains("CancelSelection"));
    }

    SECTION("when a command is bound twice, the first binding wins")
    {
        // Otherwise the rendered hint would depend on hash/iteration order and could differ run to run.
        auto mappings = config::InputMappings {};
        mappings.charMappings.push_back(config::CharInputMapping { .modes {},
                                                                   .modifiers { Modifier::Control },
                                                                   .input = U'A',
                                                                   .binding = { actions::ScrollToTop {} } });
        mappings.charMappings.push_back(config::CharInputMapping { .modes {},
                                                                   .modifiers { Modifier::Control },
                                                                   .input = U'B',
                                                                   .binding = { actions::ScrollToTop {} } });

        auto const twice = shortcutIndex(mappings, ShortcutStyle::Text);
        CHECK(twice.at("ScrollToTop") == "Ctrl+A");
    }
}

TEST_CASE("describeLinkGesture spells the follow gesture as the shortcut column spells a chord",
          "[contour][links]")
{
    // Classic: the edition whose follow modifier is Ctrl on every platform (Native2026's is Command on
    // macOS).
    auto const classic = config::Config { .defaultsEdition = config::DefaultsEdition::Classic };
    CHECK(describeLinkGesture(classic, ShortcutStyle::Text)
          == "Ctrl+Click (Shift+Ctrl+Click inside applications that use the mouse)");
    CHECK(describeLinkGesture(
              contour::test::loadConfigFromYaml(
                  "input_mapping:\n  - { mods: [Alt], mouse: Left, action: FollowHyperlink }\n"),
              ShortcutStyle::Text)
          == "Alt+Click (Shift+Alt+Click inside applications that use the mouse)");
    CHECK(
        describeLinkGesture(contour::test::loadConfigFromYaml(
                                "input_mapping:\n  - { mods: [Alt], mouse: Left, action: PasteClipboard }\n"),
                            ShortcutStyle::Text)
            .starts_with("No mouse binding follows links"));
}

TEST_CASE("Shortcut.macGlyphs", "[command]")
{
    using vtbackend::Modifier;
    auto const cmdShift = Modifiers { Modifier::Super, Modifier::Shift };
    CHECK(shortcutText(cmdShift, ShortcutInput { U'P' }, ShortcutStyle::MacGlyphs) == "⇧⌘P");
    CHECK(
        shortcutText(Modifiers { Modifier::Control }, ShortcutInput { Key::Enter }, ShortcutStyle::MacGlyphs)
        == "⌃↩");
    CHECK(shortcutText(cmdShift, ShortcutInput { U'P' }, ShortcutStyle::Text) == "Shift+Super+P");
    // A modifier without a glyph falls back to its name, never vanishes.
    CHECK(shortcutText(Modifiers { Modifier::Hyper }, ShortcutInput { U'K' }, ShortcutStyle::MacGlyphs)
          == "Hyper+K");
    // A key without a glyph keeps its name.
    CHECK(shortcutText(Modifiers { Modifier::Alt }, ShortcutInput { Key::F3 }, ShortcutStyle::MacGlyphs)
          == "⌥F3");
}

TEST_CASE("Shortcut.keyGlyphTable", "[command]")
{
    for (auto const& row: ShortcutKeyGlyphTable)
        CHECK(shortcutText(Modifiers {}, ShortcutInput { row.key }, ShortcutStyle::MacGlyphs) == row.glyph);
    CHECK(shortcutText(Modifiers {}, ShortcutInput { Key::PageDown }, ShortcutStyle::MacGlyphs) == "⇟");
    CHECK(shortcutText(Modifiers {}, ShortcutInput { Key::Home }, ShortcutStyle::Text) == "Home");
}

TEST_CASE("Shortcut.indexAndLinkGestureFollowStyle", "[command]")
{
    auto const index = shortcutIndex(config::defaultInputMappings(), ShortcutStyle::MacGlyphs);
    CHECK(index.at("CreateNewTab") == "⌃⇧T");
    auto const classic = config::Config { .defaultsEdition = config::DefaultsEdition::Classic };
    CHECK(describeLinkGesture(classic, ShortcutStyle::MacGlyphs)
          == "⌃Click (⇧⌃Click inside applications that use the mouse)");
#ifdef __APPLE__
    // The Native2026 defaults follow links with Command on macOS.
    CHECK(describeLinkGesture(config::Config {}, ShortcutStyle::MacGlyphs)
          == "⌘Click (⇧⌘Click inside applications that use the mouse)");
#endif
}
