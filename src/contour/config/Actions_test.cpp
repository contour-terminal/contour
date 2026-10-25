// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the keyboard-action repeat filter.
//
// A held key emits KeyboardEventType::Repeat events; structural/destructive actions (creating or
// closing a tab, closing a pane) must fire exactly once per physical keypress and be dropped on
// repeat. The dispatch path (TerminalSession::handleAction) delegates that filtering to
// actions::filterRepeatableActions, which this suite exercises directly.

#include <contour/config/Actions.hpp>

#include <catch2/catch_test_macros.hpp>

#include <format>
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

TEST_CASE("actions: a held key runs a command block action once", "[actions][repeat][semanticblocks]")
{
    // Each fire walks the blocks, builds the text and sets the clipboard (or drops rows); a repeat adds
    // nothing the first press did not do.
    CHECK(actions::isNonRepeatable(actions::Action { actions::SelectCommandBlock {} }));
    CHECK(actions::isNonRepeatable(actions::Action { actions::CopyCommandBlock {} }));
    CHECK(actions::isNonRepeatable(actions::Action { actions::CopyCommandLine {} }));
    CHECK(actions::isNonRepeatable(actions::Action { actions::ClearToPrompt {} }));
    CHECK(actions::isNonRepeatable(actions::Action { actions::CopyLastCommandPrompt {} }));
    CHECK(actions::isNonRepeatable(actions::Action { actions::CopyLastCommandOutput {} }));
    CHECK(actions::isNonRepeatable(actions::Action { actions::CopyLastCommandBlock {} }));
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

TEST_CASE("Actions: AttachToHost maps by name, and needs a host to run", "[actions][hosts]")
{
    auto const action = contour::actions::fromString("AttachToHost");
    REQUIRE(action.has_value());
    CHECK(std::holds_alternative<contour::actions::AttachToHost>(*action));
    // Bare it names no host, so the palette offers it only per saved host; and a held key opens one window.
    CHECK(contour::actions::isParameterized(*action));
    CHECK(contour::actions::isNonRepeatable(
        contour::actions::Action { contour::actions::AttachToHost { .name = "home" } }));
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
    // ToggleFoldAt's default names grid line 0 -- a REAL line, so a bare palette entry would toggle
    // whatever fold covers the top of the page rather than doing nothing visible. It is built by the
    // context menu from the line that was right-clicked, never run bare.
    CHECK(isParameterized(Action { ToggleFoldAt {} }));

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

TEST_CASE("actions: SelectCommandBlock writes its arguments as config tokens", "[actions][semanticblocks]")
{
    CHECK(std::format("{}", actions::Action { actions::SelectCommandBlock {} })
          == "SelectCommandBlock, target: last, part: output");
    CHECK(std::format("{}",
                      actions::Action {
                          actions::SelectCommandBlock { .target = vtbackend::CommandBlockTarget::Cursor,
                                                        .part = vtbackend::CommandBlockPart::PromptAndOutput,
                                                        .block = vtbackend::CommandBlockId(7) } })
          == "SelectCommandBlock, target: cursor, part: all, block: 7");
    CHECK(std::format("{}",
                      actions::Action {
                          actions::SelectCommandBlock { .target = vtbackend::CommandBlockTarget::Pointer,
                                                        .part = vtbackend::CommandBlockPart::Prompt } })
          == "SelectCommandBlock, target: pointer, part: input");
    CHECK_FALSE(actions::isParameterized(actions::Action { actions::SelectCommandBlock {} }));
}

TEST_CASE("actions: the block copy actions write their arguments as config tokens",
          "[actions][semanticblocks]")
{
    CHECK(std::format("{}", actions::Action { actions::CopyCommandBlock {} })
          == "CopyCommandBlock, target: last, part: output");
    CHECK(std::format(
              "{}",
              actions::Action { actions::CopyCommandBlock { .target = vtbackend::CommandBlockTarget::Pointer,
                                                            .part = vtbackend::CommandBlockPart::Prompt,
                                                            .block = vtbackend::CommandBlockId(4) } })
          == "CopyCommandBlock, target: pointer, part: input, block: 4");
    CHECK(std::format("{}", actions::Action { actions::CopyCommandLine {} })
          == "CopyCommandLine, target: last");
    CHECK(std::format(
              "{}",
              actions::Action { actions::CopyCommandLine { .target = vtbackend::CommandBlockTarget::Cursor,
                                                           .block = vtbackend::CommandBlockId(2) } })
          == "CopyCommandLine, target: cursor, block: 2");
}

TEST_CASE("actions: CopySelection writes the arguments it carries", "[actions][semanticblocks]")
{
    CHECK(std::format("{}", actions::Action { actions::CopySelection {} }) == "CopySelection");
    CHECK(
        std::format("{}", actions::Action { actions::CopySelection { .format = actions::CopyFormat::HTML } })
        == "CopySelection, format: HTML");
    CHECK(std::format("{}",
                      actions::Action {
                          actions::CopySelection { .fallback = actions::CopyFallback::LastCommandOutput } })
          == "CopySelection, fallback: last_command_output");
    CHECK(std::format("{}",
                      actions::Action {
                          actions::CopySelection { .format = actions::CopyFormat::HTML,
                                                   .fallback = actions::CopyFallback::LastCommandOutput } })
          == "CopySelection, format: HTML, fallback: last_command_output");
}

TEST_CASE("actions: ClearToPrompt is bound by its name and carries nothing", "[actions][semanticblocks]")
{
    auto const parsed = actions::fromString("ClearToPrompt");
    REQUIRE(parsed.has_value());
    CHECK(std::holds_alternative<actions::ClearToPrompt>(*parsed));
    CHECK(std::format("{}", actions::Action { actions::ClearToPrompt {} }) == "ClearToPrompt");
    CHECK_FALSE(actions::isParameterized(actions::Action { actions::ClearToPrompt {} }));
}

TEST_CASE("actions: OpenCommandOutput writes its arguments and is not repeated", "[actions][semanticblocks]")
{
    CHECK(std::format("{}", actions::Action { actions::OpenCommandOutput {} })
          == "OpenCommandOutput, target: last, placement: split");
    CHECK(std::format(
              "{}",
              actions::Action { actions::OpenCommandOutput { .target = vtbackend::CommandBlockTarget::Pointer,
                                                             .program = "wl-copy",
                                                             .placement = actions::OutputPlacement::Detached,
                                                             .format = actions::OutputFormat::Plain,
                                                             .block = vtbackend::CommandBlockId(9) } })
          == "OpenCommandOutput, target: pointer, block: 9, program: 'wl-copy', placement: detached, "
             "format: plain");

    // A pager's own quoting survives being written into a single-quoted scalar.
    CHECK(std::format("{}", actions::Action { actions::OpenCommandOutput { .program = "less --prompt='x'" } })
          == "OpenCommandOutput, target: last, program: 'less --prompt=''x''', placement: split");

    // Every fire forks a pager pane, as a split does: a held key must not open a burst of them.
    CHECK(actions::isNonRepeatable(actions::Action { actions::OpenCommandOutput {} }));
}

TEST_CASE("Actions: the tab-menu actions map by name", "[actions][catalog]")
{
    CHECK(std::holds_alternative<actions::CloseOtherTabs>(*actions::fromString("CloseOtherTabs")));
    CHECK(std::holds_alternative<actions::CloseTabsToRight>(*actions::fromString("CloseTabsToRight")));
    CHECK(std::holds_alternative<actions::MoveTabToNewWindow>(*actions::fromString("MoveTabToNewWindow")));
    CHECK(std::holds_alternative<actions::ShowAbout>(*actions::fromString("ShowAbout")));
}

TEST_CASE("Actions: the tab-menu actions are dropped on key repeat", "[actions][repeat]")
{
    std::vector<actions::Action> const held {
        actions::CloseOtherTabs {},
        actions::CloseTabsToRight {},
        actions::MoveTabToNewWindow {},
        actions::ShowAbout {},
    };
    CHECK(actions::filterRepeatableActions(held).empty());
}
