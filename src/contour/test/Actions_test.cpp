// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the keyboard-action repeat filter.
//
// A held key emits KeyboardEventType::Repeat events; structural/destructive actions (creating or
// closing a tab, closing a pane) must fire exactly once per physical keypress and be dropped on
// repeat. The dispatch path (TerminalSession::handleAction) delegates that filtering to
// actions::filterRepeatableActions, which this suite exercises directly.

#include <contour/Actions.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <variant>
#include <vector>

using namespace contour;

TEST_CASE("actions::isNonRepeatable flags the structural actions", "[actions][repeat]")
{
    // Non-repeatable: a held key must not amplify these into a burst.
    CHECK(actions::isNonRepeatable(actions::Action { actions::CreateNewTab {} }));
    CHECK(actions::isNonRepeatable(actions::Action { actions::CloseTab {} }));
    CHECK(actions::isNonRepeatable(actions::Action { actions::ClosePane {} })); // regression: was missing
    // Splits fork a new shell process per fire, so a held split key must not burst panes.
    CHECK(actions::isNonRepeatable(actions::Action { actions::SplitVertical {} })); // regression: was missing
    CHECK(
        actions::isNonRepeatable(actions::Action { actions::SplitHorizontal {} })); // regression: was missing

    // Swap/Move re-parent panes; a held key must not cascade structural changes. ToggleSplitOrientation
    // likewise flips the layout structurally.
    CHECK(actions::isNonRepeatable(actions::Action { actions::SwapPaneLeft {} }));
    CHECK(actions::isNonRepeatable(actions::Action { actions::SwapPaneDown {} }));
    CHECK(actions::isNonRepeatable(actions::Action { actions::MovePaneRight {} }));
    CHECK(actions::isNonRepeatable(actions::Action { actions::MovePaneUp {} }));
    CHECK(actions::isNonRepeatable(actions::Action { actions::ToggleSplitOrientation {} }));
    // A held toggle would flip zoom once per repeat, leaving the final state up to the repeat count
    // rather than the user.
    CHECK(actions::isNonRepeatable(actions::Action { actions::TogglePaneZoom {} }));

    // LaunchLayout spawns a whole window's worth of tabs/panes per fire, and SaveLayout writes a
    // file per fire; a held key must not amplify either into a burst.
    CHECK(actions::isNonRepeatable(actions::Action { actions::LaunchLayout { .name = "work" } }));
    CHECK(actions::isNonRepeatable(actions::Action { actions::SaveLayout { .name = "work" } }));

    // Repeatable: holding the key should keep firing these.
    CHECK_FALSE(actions::isNonRepeatable(actions::Action { actions::ScrollUp {} }));
    CHECK_FALSE(actions::isNonRepeatable(actions::Action { actions::MoveTabToLeft {} }));
    // Resizing by holding the key to grow a pane is desirable, so it stays repeatable.
    CHECK_FALSE(
        actions::isNonRepeatable(actions::Action { actions::ResizePane { actions::Direction::Right } }));
}

TEST_CASE("actions::fromString round-trips the new pane actions", "[actions][parse]")
{
    // Each parameterless pane action parses from its name to the right alternative. A couple use a
    // non-canonical case (swapPaneRight, movePaneDown) to exercise the case-insensitive lookup.
    CHECK(std::holds_alternative<actions::SwapPaneLeft>(*actions::fromString("SwapPaneLeft")));
    CHECK(std::holds_alternative<actions::SwapPaneRight>(*actions::fromString("swapPaneRight")));
    CHECK(std::holds_alternative<actions::MovePaneUp>(*actions::fromString("MovePaneUp")));
    CHECK(std::holds_alternative<actions::MovePaneDown>(*actions::fromString("movePaneDown")));
    CHECK(std::holds_alternative<actions::ToggleSplitOrientation>(
        *actions::fromString("ToggleSplitOrientation")));
    CHECK(std::holds_alternative<actions::TogglePaneZoom>(*actions::fromString("TogglePaneZoom")));
    CHECK(std::holds_alternative<actions::TogglePaneZoom>(*actions::fromString("togglePaneZoom")));
    // ResizePane parses to a default-constructed alternative here (its parameters are filled by the
    // YAML parseAction step, not fromString).
    CHECK(std::holds_alternative<actions::ResizePane>(*actions::fromString("ResizePane")));
    // An unknown name yields nullopt.
    CHECK_FALSE(actions::fromString("NoSuchPaneAction").has_value());
}

TEST_CASE("actions::filterRepeatableActions drops ClosePane on auto-repeat", "[actions][repeat]")
{
    std::vector<actions::Action> const held {
        actions::ClosePane {},
        actions::ScrollUp {},
    };

    auto const repeatable = actions::filterRepeatableActions(held);

    // ClosePane must be removed; the repeatable ScrollUp must survive.
    REQUIRE(repeatable.size() == 1);
    CHECK(std::holds_alternative<actions::ScrollUp>(repeatable.front()));
}

TEST_CASE("actions::filterRepeatableActions keeps order and the other non-repeatables out",
          "[actions][repeat]")
{
    std::vector<actions::Action> const held {
        actions::ScrollUp {},        // repeatable
        actions::CloseTab {},        // dropped
        actions::SplitVertical {},   // dropped (structural: forks a shell)
        actions::CreateNewTab {},    // dropped
        actions::SplitHorizontal {}, // dropped (structural: forks a shell)
        actions::MoveTabToLeft {},   // repeatable
    };

    auto const repeatable = actions::filterRepeatableActions(held);

    REQUIRE(repeatable.size() == 2);
    CHECK(std::holds_alternative<actions::ScrollUp>(repeatable[0]));
    CHECK(std::holds_alternative<actions::MoveTabToLeft>(repeatable[1]));
}

TEST_CASE("actions::filterRepeatableActions on an all-repeatable list is identity-sized", "[actions][repeat]")
{
    std::vector<actions::Action> const held { actions::ScrollUp {}, actions::ScrollUp {} };
    CHECK(actions::filterRepeatableActions(held).size() == 2);
}

TEST_CASE("Actions: LaunchLayout maps by name", "[actions][layout]")
{
    auto const action = contour::actions::fromString("LaunchLayout");
    REQUIRE(action.has_value());
    CHECK(std::holds_alternative<contour::actions::LaunchLayout>(*action));
}

TEST_CASE("Actions: SaveLayout maps by name", "[actions][layout]")
{
    auto const action = contour::actions::fromString("SaveLayout");
    REQUIRE(action.has_value());
    CHECK(std::holds_alternative<contour::actions::SaveLayout>(*action));
}

// ============================================================================================
// The action catalog: the single table that names every action, hands out an instance of it, and
// documents it. fromString() (the `input_mapping:` parse path), the generated key-mapping docs and
// the command palette all read from it, so a hole in the table breaks a user's key bindings.
// ============================================================================================

TEST_CASE("actions: every action in the catalog round-trips through its name", "[actions][catalog]")
{
    // The guard on the fromString() rewrite. A row whose name no longer parses back to its own
    // alternative would silently break every binding that uses it — the config would still load, the
    // key would just quietly stop working.
    for (auto const& entry: contour::actions::actionCatalog())
    {
        INFO("action: " << entry.name);

        auto const parsed = contour::actions::fromString(std::string(entry.name));
        REQUIRE(parsed.has_value());
        CHECK(parsed->index() == entry.prototype.index());

        // And back again: the catalog's own name lookup agrees with the row it came from.
        CHECK(contour::actions::name(entry.prototype) == entry.name);
        CHECK(contour::actions::describe(entry.prototype) == entry.documentation);
        CHECK_FALSE(entry.documentation.empty());
    }
}

TEST_CASE("actions::fromString is case-insensitive, as the config has always allowed", "[actions][catalog]")
{
    auto const lower = contour::actions::fromString("splitvertical");
    REQUIRE(lower.has_value());
    CHECK(std::holds_alternative<contour::actions::SplitVertical>(*lower));

    auto const upper = contour::actions::fromString("SPLITVERTICAL");
    REQUIRE(upper.has_value());
    CHECK(std::holds_alternative<contour::actions::SplitVertical>(*upper));
}

TEST_CASE("actions::fromString rejects a name no action has", "[actions][catalog]")
{
    CHECK_FALSE(contour::actions::fromString("NotAnAction").has_value());
    CHECK_FALSE(contour::actions::fromString("").has_value());
}

TEST_CASE("actions::isParameterized marks the actions the palette cannot run bare", "[actions][catalog]")
{
    using namespace contour::actions;

    // These carry a REQUIRED argument: a default-constructed instance names no profile / no tab / no
    // characters, so running one would do nothing (or something arbitrary).
    CHECK(isParameterized(Action { ChangeProfile {} }));
    CHECK(isParameterized(Action { SwitchToTab {} }));
    CHECK(isParameterized(Action { SendChars {} }));
    CHECK(isParameterized(Action { LaunchLayout {} }));
    CHECK(isParameterized(Action { ResizePane {} }));

    // These have a MEANINGFUL default (copy as text, paste unstripped, current profile), so they run
    // as-is and the palette offers them.
    CHECK_FALSE(isParameterized(Action { CopySelection {} }));
    CHECK_FALSE(isParameterized(Action { PasteClipboard {} }));
    CHECK_FALSE(isParameterized(Action { NewTerminal {} }));

    // And the plain empty-struct actions, of course.
    CHECK_FALSE(isParameterized(Action { SplitVertical {} }));
    CHECK_FALSE(isParameterized(Action { OpenCommandPalette {} }));

    // SetTabColor's color is OPTIONAL, and omitting it is a meaningful default: the action opens the
    // tab's color picker. That is exactly what keeps it out of this set, and therefore what lets the
    // palette offer it straight from the catalog rather than only once the user has bound it.
    CHECK_FALSE(isParameterized(Action { SetTabColor {} }));
    CHECK_FALSE(isParameterized(Action { SetTabColor { vtbackend::RGBColor { 0xFF, 0x00, 0x00 } } }));
    CHECK_FALSE(isParameterized(Action { ResetTabColor {} }));

    // SaveLayout is the layout-side mirror of SetTabColor: a nameless instance opens the save-as name
    // prompt rather than doing nothing, so it too stays out of this set and the palette offers it
    // straight from the catalog. (LaunchLayout above stays IN — a nameless "launch which layout?" has no
    // default, and its per-name rows already reach the palette from the saved-layout source.)
    CHECK_FALSE(isParameterized(Action { SaveLayout {} }));
    CHECK_FALSE(isParameterized(Action { SaveLayout { "dev" } }));
}

TEST_CASE("actions: every catalog row sits at its own variant index", "[actions][catalog]")
{
    // THE invariant behind catalogEntry(), which is `actionCatalog()[action.index()]` — a direct index,
    // not a search. The static_assert next to it pins only the table's SIZE; nothing pinned its ORDER.
    //
    // That gap is silent and nasty: insert an alternative into the middle of the variant but append its
    // row at the end of the catalog (or vice versa) and every action from that point on answers to the
    // wrong name and carries the wrong documentation — while still compiling and still passing the
    // round-trip test above, which only ever asks the catalog about its own rows.
    auto index = size_t { 0 };
    for (auto const& entry: contour::actions::actionCatalog())
    {
        INFO("catalog row " << index << ": " << entry.name);
        CHECK(entry.prototype.index() == index);
        ++index;
    }
}

TEST_CASE("actions: picking a tab color is not repeated by a held key", "[actions][repeat]")
{
    // Bare, SetTabColor opens the color flyout: the same one-shot UI gesture as OpenCommandPalette, and
    // the popup already holds the keyboard by the second repeat.
    using namespace contour::actions;
    CHECK(isNonRepeatable(Action { SetTabColor {} }));

    // Clearing a color is idempotent and pops nothing up, so a held key may repeat it harmlessly.
    CHECK_FALSE(isNonRepeatable(Action { ResetTabColor {} }));
}

TEST_CASE("actions: opening the command palette is not repeated by a held key", "[actions][repeat]")
{
    // The popup is up (and holding the terminal's keyboard focus) by the second repeat, so re-firing
    // could only ever re-open what is already open.
    CHECK(contour::actions::isNonRepeatable(
        contour::actions::Action { contour::actions::OpenCommandPalette {} }));
}
