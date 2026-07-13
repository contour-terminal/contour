// SPDX-License-Identifier: Apache-2.0
//
// The chord -> "Ctrl+Shift+P" renderer that feeds the command palette's shortcut column.
//
// The column exists to TEACH: a user who ran a command from the palette should read off the chord and
// press it next time. So these tests care about the two ways that promise can be broken — rendering a
// chord the user cannot type, and attaching a chord to the wrong command.

#include <contour/Command.h>
#include <contour/Config.h>
#include <contour/Shortcut.h>

#include <catch2/catch_test_macros.hpp>

using namespace contour;
using vtbackend::Key;
using vtbackend::Modifier;
using vtbackend::Modifiers;

TEST_CASE("shortcutText renders a chord the way a user would type it", "[contour][palette]")
{
    SECTION("modifiers render in the conventional order, not the bit order")
    {
        // std::formatter<Modifiers> would give "Shift|Control" — ascending bit order, pipe-joined.
        // Neither is what a keyboard hint looks like, which is the whole reason this renderer exists.
        CHECK(shortcutText(Modifiers { Modifier::Control, Modifier::Shift }, ShortcutInput { U'P' })
              == "Ctrl+Shift+P");
        CHECK(shortcutText(Modifiers { Modifier::Shift, Modifier::Control }, ShortcutInput { U'P' })
              == "Ctrl+Shift+P");
    }

    SECTION("the order is fixed regardless of how the chord was assembled")
    {
        // The key keeps the name the config gives it ("LeftArrow", via std::formatter<Key>) rather
        // than a prettier synonym: reusing that vocabulary means the shortcut column also teaches the
        // user what to write in `input_mapping:` if they want to rebind it.
        auto const expected = std::string { "Ctrl+Alt+Shift+LeftArrow" };
        CHECK(shortcutText(Modifiers { Modifier::Shift, Modifier::Alt, Modifier::Control },
                           ShortcutInput { Key::LeftArrow })
              == expected);
        CHECK(shortcutText(Modifiers { Modifier::Control, Modifier::Shift, Modifier::Alt },
                           ShortcutInput { Key::LeftArrow })
              == expected);
    }

    SECTION("both halves of the input mappings render: a named key and a character")
    {
        CHECK(shortcutText(Modifiers { Modifier::Alt }, ShortcutInput { Key::Enter }) == "Alt+Enter");
        CHECK(shortcutText(Modifiers { Modifier::Control, Modifier::Shift }, ShortcutInput { U'T' })
              == "Ctrl+Shift+T");
    }

    SECTION("an unmodified key is just the key")
    {
        CHECK(shortcutText(Modifiers {}, ShortcutInput { Key::F3 }) == "F3");
    }
}

TEST_CASE("shortcutIndex maps commands to the chord that runs them", "[contour][palette]")
{
    auto const& defaults = config::defaultInputMappings;
    auto const index = shortcutIndex(defaults);

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

        auto const multi = shortcutIndex(mappings);
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

        auto const twice = shortcutIndex(mappings);
        CHECK(twice.at("ScrollToTop") == "Ctrl+A");
    }
}
