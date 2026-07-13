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
