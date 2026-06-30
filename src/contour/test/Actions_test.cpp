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

    // Repeatable: holding the key should keep firing these.
    CHECK_FALSE(actions::isNonRepeatable(actions::Action { actions::ScrollUp {} }));
    CHECK_FALSE(actions::isNonRepeatable(actions::Action { actions::MoveTabToLeft {} }));
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
