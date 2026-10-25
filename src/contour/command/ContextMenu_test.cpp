// SPDX-License-Identifier: Apache-2.0
#include <contour/command/Command.hpp>
#include <contour/command/ContextMenu.hpp>
#include <contour/command/ContextMenuTable.hpp>
#include <contour/config/Actions.hpp>

#include <vtbackend/links/LinkTypes.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <iterator>
#include <ranges>
#include <string>
#include <variant>
#include <vector>

using namespace contour;
using namespace contour::command;

namespace
{

/// The entry titled @p title, or nullptr. Recurses into submenus, so a row's identity is its title
/// wherever it lives.
[[nodiscard]] ContextMenuEntry const* find(std::vector<ContextMenuEntry> const& entries,
                                           std::string const& title)
{
    for (auto const& entry: entries)
    {
        if (entry.title == title)
            return &entry;
        if (auto const* nested = find(entry.children, title); nested != nullptr)
            return nested;
    }
    return nullptr;
}

/// Every command id in the menu, submenus included.
[[nodiscard]] std::vector<std::string> commandIds(std::vector<ContextMenuEntry> const& entries)
{
    auto ids = std::vector<std::string> {};
    for (auto const& entry: entries)
    {
        if (entry.kind == ContextMenuEntryKind::Command)
            ids.push_back(commandId(entry.action));
        for (auto const& id: commandIds(entry.children))
            ids.push_back(id);
    }
    return ids;
}

/// A state with everything switched on, so a test can switch exactly one thing off.
[[nodiscard]] ContextMenuState fullyEnabledState()
{
    return ContextMenuState {
        .hasSelection = true,
        .clipboardHasText = true,
        .hasLastCommand = true,
        .hasLocalWorkingDirectory = true,
        .hasSplits = true,
        .hyperlinkUnderCursor = "https://contour-terminal.org/",
        .foldLine = vtbackend::LineOffset(7),
        .blockUnderCursor = PointedBlock { .id = vtbackend::CommandBlockId(5),
                                           .output = OutputPresence::Present,
                                           .commandLine = CommandLinePresence::Present },
        .livePrompt = PromptPresence::Present,
        .lastCommandLine = CommandLinePresence::Present,
        .activeProfile = "dark",
        .profileNames = { "dark", "light", "work" },
    };
}

} // namespace

TEST_CASE("ContextMenu.copy.requiresSelection", "[contextmenu]")
{
    auto state = fullyEnabledState();

    state.hasSelection = true;
    auto const selected = buildContextMenu(state);
    auto const* withSelection = find(selected, "Copy");
    REQUIRE(withSelection != nullptr);
    CHECK(withSelection->availability == RowAvailability::Enabled);

    state.hasSelection = false;
    auto const unselected = buildContextMenu(state);
    auto const* withoutSelection = find(unselected, "Copy");
    // Present but grayed out -- Copy is always a meaningful thing for this terminal to offer; it just has
    // nothing to copy right now.
    REQUIRE(withoutSelection != nullptr);
    CHECK(withoutSelection->availability == RowAvailability::Disabled);
}

TEST_CASE("ContextMenu.paste.requiresClipboardText", "[contextmenu]")
{
    auto state = fullyEnabledState();

    state.clipboardHasText = false;
    auto const empty = buildContextMenu(state);
    auto const* whenEmpty = find(empty, "Paste");
    REQUIRE(whenEmpty != nullptr);
    CHECK(whenEmpty->availability == RowAvailability::Disabled);

    state.clipboardHasText = true;
    auto const filled = buildContextMenu(state);
    auto const* whenFilled = find(filled, "Paste");
    REQUIRE(whenFilled != nullptr);
    CHECK(whenFilled->availability == RowAvailability::Enabled);
}

TEST_CASE("ContextMenu.lastCommand.hiddenWithoutShellIntegration", "[contextmenu]")
{
    auto state = fullyEnabledState();

    SECTION("present when the scrollback holds a finished command")
    {
        state.hasLastCommand = true;
        auto const menu = buildContextMenu(state);
        CHECK(find(menu, "Copy Last Command") != nullptr);
        CHECK(find(menu, "Copy Last Prompt") != nullptr);
        CHECK(find(menu, "Copy Last Command Output") != nullptr);
        CHECK(find(menu, "Copy Last Prompt and Output") != nullptr);
    }

    SECTION("gone -- not merely grayed out -- when it does not")
    {
        // A permanently dead row would teach the user the feature is broken, when in truth their shell
        // simply never told the terminal where one command ended and the next began.
        state.hasLastCommand = false;
        auto const menu = buildContextMenu(state);
        CHECK(find(menu, "Copy Last Command") == nullptr);
        CHECK(find(menu, "Copy Last Prompt") == nullptr);
        CHECK(find(menu, "Copy Last Command Output") == nullptr);
        CHECK(find(menu, "Copy Last Prompt and Output") == nullptr);
    }
}

TEST_CASE("ContextMenu.lastCommand.copiesTheCommandLineWithoutThePrompt", "[contextmenu][semanticblocks]")
{
    // "Copy Last Prompt" copies the prompt's lines, PS1 and all; this row copies the bare command line.
    auto state = fullyEnabledState();
    auto const menu = buildContextMenu(state);
    auto const* const row = find(menu, "Copy Last Command");
    REQUIRE(row != nullptr);
    CHECK(row->availability == RowAvailability::Enabled);
    REQUIRE(std::holds_alternative<actions::CopyCommandLine>(row->action));
    auto const& action = std::get<actions::CopyCommandLine>(row->action);
    CHECK(action.target == vtbackend::CommandBlockTarget::Last);
    CHECK(action.block == vtbackend::CommandBlockId {});

    // A shell that reports no command line, and none was recovered: nothing to copy, so no row.
    state.lastCommandLine = CommandLinePresence::Absent;
    auto const noCommandLine = buildContextMenu(state);
    CHECK(find(noCommandLine, "Copy Last Command") == nullptr);
    CHECK(find(noCommandLine, "Copy Last Prompt") != nullptr);
}

TEST_CASE("ContextMenu.hyperlink.onlyUnderTheCursor", "[contextmenu]")
{
    auto state = fullyEnabledState();

    state.hyperlinkUnderCursor = "https://contour-terminal.org/";
    auto const hovering = buildContextMenu(state);
    REQUIRE(find(hovering, "Open Link") != nullptr);
    REQUIRE(find(hovering, "Copy Link Address") != nullptr);

    // The rows must CARRY the link, not go asking for it again when they are picked: by then the pointer
    // has left the cell and is sitting on the menu row itself.
    auto const* open = find(hovering, "Open Link");
    REQUIRE(std::holds_alternative<actions::FollowHyperlink>(open->action));
    CHECK(std::get<actions::FollowHyperlink>(open->action).uri == "https://contour-terminal.org/");

    auto const* copy = find(hovering, "Copy Link Address");
    REQUIRE(std::holds_alternative<actions::CopyHyperlink>(copy->action));
    CHECK(std::get<actions::CopyHyperlink>(copy->action).uri == "https://contour-terminal.org/");

    state.hyperlinkUnderCursor.clear();
    auto const elsewhere = buildContextMenu(state);
    CHECK(find(elsewhere, "Open Link") == nullptr);
    CHECK(find(elsewhere, "Copy Link Address") == nullptr);
}

TEST_CASE("ContextMenu.detectedLinkIsPinned", "[contextmenu][links]")
{
    // A detected link is carried as resolved, for the reason an OSC 8 one is carried: by the time a row is
    // picked the pointer has left it.
    auto state = fullyEnabledState();
    state.hyperlinkUnderCursor.clear(); // no OSC 8 link: the detected one alone shows the rows
    state.linkUnderCursor =
        vtbackend::LinkTarget { .kind = vtbackend::LinkKind::Path,
                                .target = "/w/a.cpp",
                                .location = vtbackend::LinkLocation { .line = 3, .column = 1 },
                                .pathKind = vtbackend::PathKind::File,
                                .directory = "/w" };
    auto const entries = buildContextMenu(state);

    auto const* open = find(entries, "Open Link");
    REQUIRE(open != nullptr);
    REQUIRE(std::holds_alternative<actions::FollowHyperlink>(open->action));
    CHECK(std::get<actions::FollowHyperlink>(open->action).link == state.linkUnderCursor);
    CHECK(std::get<actions::FollowHyperlink>(open->action).uri.empty());

    auto const* copy = find(entries, "Copy Link Address");
    REQUIRE(copy != nullptr);
    REQUIRE(std::holds_alternative<actions::CopyHyperlink>(copy->action));
    CHECK(std::get<actions::CopyHyperlink>(copy->action).link == state.linkUnderCursor);
}

TEST_CASE("ContextMenu.toggleFold.grayedRatherThanHiddenWithoutAFold", "[contextmenu]")
{
    auto state = fullyEnabledState();

    // Present and enabled where a fold reaches -- and it must CARRY the clicked line, because by the
    // time the row is picked the pointer has left that line for the menu itself.
    state.foldLine = vtbackend::LineOffset(-12);
    auto const overFold = buildContextMenu(state);
    auto const* row = find(overFold, "Toggle Output Fold");
    REQUIRE(row != nullptr);
    CHECK(row->availability == RowAvailability::Enabled);
    REQUIRE(std::holds_alternative<actions::ToggleFoldAt>(row->action));
    CHECK(std::get<actions::ToggleFoldAt>(row->action).line == -12);

    // Elsewhere the row stays PUT and goes gray. Hiding it would make the menu change shape depending
    // on where you right-clicked, and would hide the fact that folding exists at all.
    state.foldLine.reset();
    auto const elsewhere = buildContextMenu(state);
    auto const* grayed = find(elsewhere, "Toggle Output Fold");
    REQUIRE(grayed != nullptr);
    CHECK(grayed->availability == RowAvailability::Disabled);
}

TEST_CASE("ContextMenu.openCurrentFolder.requiresLocalWorkingDirectory", "[contextmenu]")
{
    // Grayed out unless the cwd is on this host: a remote (SSH) working directory cannot be opened by the
    // local file manager, so resolving it to a local path is what gates the row (see localPathOfFileUrl).
    auto state = fullyEnabledState();

    state.hasLocalWorkingDirectory = false;
    auto const unknown = buildContextMenu(state);
    auto const* whenUnknown = find(unknown, "Open Current Folder");
    REQUIRE(whenUnknown != nullptr);
    CHECK(whenUnknown->availability == RowAvailability::Disabled);

    state.hasLocalWorkingDirectory = true;
    auto const known = buildContextMenu(state);
    auto const* whenKnown = find(known, "Open Current Folder");
    REQUIRE(whenKnown != nullptr);
    CHECK(whenKnown->availability == RowAvailability::Enabled);
}

TEST_CASE("ContextMenu.paneRows.dependOnThereBeingSplits", "[contextmenu]")
{
    auto state = fullyEnabledState();

    SECTION("a lone pane offers no zoom and no orientation flip")
    {
        state.hasSplits = false;
        auto const menu = buildContextMenu(state);
        CHECK(find(menu, "Toggle Pane Zoom") == nullptr);
        CHECK(find(menu, "Toggle Split Orientation") == nullptr);
        // Splitting and closing are still on offer: closing the only pane closes the tab.
        CHECK(find(menu, "Split Vertically") != nullptr);
        CHECK(find(menu, "Split Horizontally") != nullptr);
        CHECK(find(menu, "Close Pane") != nullptr);
    }

    SECTION("a split tab offers both")
    {
        state.hasSplits = true;
        auto const menu = buildContextMenu(state);
        CHECK(find(menu, "Toggle Pane Zoom") != nullptr);
        CHECK(find(menu, "Toggle Split Orientation") != nullptr);
    }
}

TEST_CASE("ContextMenu.profiles.oneRowEachWithTheActiveOneChecked", "[contextmenu]")
{
    auto state = fullyEnabledState();
    state.profileNames = { "dark", "light", "work" };
    state.activeProfile = "light";

    auto const menu = buildContextMenu(state);
    auto const* profiles = find(menu, "Change Profile");
    REQUIRE(profiles != nullptr);
    REQUIRE(profiles->kind == ContextMenuEntryKind::Submenu);
    REQUIRE(profiles->children.size() == 3);

    for (auto const& child: profiles->children)
    {
        CHECK(child.check != RowCheck::None);
        CHECK((child.check == RowCheck::Checked) == (child.title == "light"));
        // The row carries the concrete, argument-bearing action; nothing has to re-parse its name later.
        CHECK(commandId(child.action) == "ChangeProfile:" + child.title);
    }
}

TEST_CASE("ContextMenu.readOnly.reflectsInputProtection", "[contextmenu]")
{
    // The row is always present and always checkable; its check mirrors the pane's input-protection
    // (read-only) state at the moment the menu was opened.
    SECTION("unchecked while input is allowed")
    {
        auto state = fullyEnabledState();
        state.inputProtected = false;

        auto const menu = buildContextMenu(state);
        auto const* readOnly = find(menu, "Read-Only Mode");
        REQUIRE(readOnly != nullptr);
        CHECK(readOnly->check != RowCheck::None);
        CHECK(readOnly->check != RowCheck::Checked);
        // Picking it flips the very state the check reflects.
        CHECK(std::holds_alternative<actions::ToggleInputProtection>(readOnly->action));
    }

    SECTION("checked while input is protected")
    {
        auto state = fullyEnabledState();
        state.inputProtected = true;

        auto const menu = buildContextMenu(state);
        auto const* readOnly = find(menu, "Read-Only Mode");
        REQUIRE(readOnly != nullptr);
        CHECK(readOnly->check == RowCheck::Checked);
    }
}

TEST_CASE("ContextMenu.profiles.submenuVanishesWhenThereAreNone", "[contextmenu]")
{
    // A submenu that opens onto nothing is worse than no submenu.
    auto state = fullyEnabledState();
    state.profileNames.clear();

    auto const menu = buildContextMenu(state);
    CHECK(find(menu, "Change Profile") == nullptr);
}

TEST_CASE("ContextMenu.advanced.holdsBothResets", "[contextmenu]")
{
    auto const menu = buildContextMenu(fullyEnabledState());
    auto const* advanced = find(menu, "Advanced");

    REQUIRE(advanced != nullptr);
    REQUIRE(advanced->kind == ContextMenuEntryKind::Submenu);
    REQUIRE(advanced->children.size() == 2);

    CHECK(commandId(advanced->children[0].action) == "SoftReset");
    CHECK(advanced->children[0].title == "Soft Reset Terminal");
    CHECK(commandId(advanced->children[1].action) == "ClearHistoryAndReset");
    CHECK(advanced->children[1].title == "Hard Reset Terminal");
}

TEST_CASE("ContextMenu.titles.derivedFromTheActionUnlessOverridden", "[contextmenu]")
{
    auto const menu = buildContextMenu(fullyEnabledState());

    // "Select All" is not authored anywhere: it falls out of splitting the action's catalog name. That is
    // what keeps the menu from growing a second title table that can drift from the first.
    auto const* selectAll = find(menu, "Select All");
    REQUIRE(selectAll != nullptr);
    CHECK(commandId(selectAll->action) == "SelectAll");

    // ... and an override wins where the derived name would read badly ("Copy Selection", "Paste
    // Clipboard", "Create New Tab").
    CHECK(find(menu, "Copy") != nullptr);
    CHECK(find(menu, "Paste") != nullptr);
    CHECK(find(menu, "New Tab") != nullptr);
    CHECK(find(menu, "Copy Selection") == nullptr);
    CHECK(find(menu, "Paste Clipboard") == nullptr);
}

TEST_CASE("ContextMenu.everyCommandIdResolvesToARealAction", "[contextmenu]")
{
    // Pins the menu to the action catalog: rename an action and this fails loudly at test time rather
    // than quietly handing the GUI a row that does nothing.
    auto const menu = buildContextMenu(fullyEnabledState());
    auto const ids = commandIds(menu);

    REQUIRE_FALSE(ids.empty());

    for (auto const& id: ids)
    {
        auto const name = id.substr(0, id.find(':'));
        INFO("command id: " << id);
        CHECK(actions::fromString(name).has_value());
    }
}

TEST_CASE("ContextMenu.separators.neverLeadingTrailingOrDoubled", "[contextmenu]")
{
    // Hiding rows leaves separators stranded, and every predicate added later is a fresh chance to strand
    // one. Sweep the whole state space rather than trusting the eye.
    auto const flags = { false, true };

    for (auto const selection: flags)
        for (auto const clipboard: flags)
            for (auto const lastCommand: flags)
                for (auto const hyperlink: flags)
                    for (auto const splits: flags)
                        for (auto const profiles: flags)
                        {
                            auto state = ContextMenuState {
                                .hasSelection = selection,
                                .clipboardHasText = clipboard,
                                .hasLastCommand = lastCommand,
                                .hasLocalWorkingDirectory = true,
                                .hasSplits = splits,
                                .hyperlinkUnderCursor = hyperlink ? "https://example.org/" : "",
                                .foldLine = std::nullopt,
                                .activeProfile = profiles ? "dark" : "",
                                .profileNames = profiles ? std::vector<std::string> { "dark" }
                                                         : std::vector<std::string> {},
                            };

                            auto const menu = buildContextMenu(state);
                            INFO("selection=" << selection << " clipboard=" << clipboard
                                              << " lastCommand=" << lastCommand << " hyperlink=" << hyperlink
                                              << " splits=" << splits << " profiles=" << profiles);

                            REQUIRE_FALSE(menu.empty());
                            CHECK(menu.front().kind != ContextMenuEntryKind::Separator);
                            CHECK(menu.back().kind != ContextMenuEntryKind::Separator);

                            auto const doubled =
                                std::ranges::adjacent_find(menu, [](auto const& a, auto const& b) {
                                    return a.kind == ContextMenuEntryKind::Separator
                                           && b.kind == ContextMenuEntryKind::Separator;
                                });
                            CHECK(doubled == menu.end());
                        }
}

TEST_CASE("ContextMenu.blockRows.pinTheRightClickedBlock", "[contextmenu][semanticblocks]")
{
    // Pinned by id, as the fold row pins its line: by the time a row is picked the pointer has left
    // the block for the menu itself.
    auto const menu = buildContextMenu(fullyEnabledState());
    auto const pinned = vtbackend::CommandBlockId(5);

    auto const* select = find(menu, "Select Output");
    REQUIRE(select != nullptr);
    CHECK(select->availability == RowAvailability::Enabled);
    REQUIRE(std::holds_alternative<actions::SelectCommandBlock>(select->action));
    CHECK(std::get<actions::SelectCommandBlock>(select->action).block == pinned);
    CHECK(std::get<actions::SelectCommandBlock>(select->action).part == vtbackend::CommandBlockPart::Output);

    auto const* copy = find(menu, "Copy Output");
    REQUIRE(copy != nullptr);
    REQUIRE(std::holds_alternative<actions::CopyCommandBlock>(copy->action));
    CHECK(std::get<actions::CopyCommandBlock>(copy->action).block == pinned);
    CHECK(std::get<actions::CopyCommandBlock>(copy->action).part == vtbackend::CommandBlockPart::Output);

    auto const* command = find(menu, "Copy Command");
    REQUIRE(command != nullptr);
    CHECK(command->availability == RowAvailability::Enabled);
    REQUIRE(std::holds_alternative<actions::CopyCommandLine>(command->action));
    CHECK(std::get<actions::CopyCommandLine>(command->action).block == pinned);

    auto const* pager = find(menu, "Open Output in Pager");
    REQUIRE(pager != nullptr);
    REQUIRE(std::holds_alternative<actions::OpenCommandOutput>(pager->action));
    CHECK(std::get<actions::OpenCommandOutput>(pager->action).block == pinned);
    CHECK(std::get<actions::OpenCommandOutput>(pager->action).placement == actions::OutputPlacement::Split);
    // No format of its own: the configured pager's applies.
    CHECK_FALSE(std::get<actions::OpenCommandOutput>(pager->action).format.has_value());
    CHECK(std::get<actions::OpenCommandOutput>(pager->action).program.empty());

    // Each row is a command of its own: an identity that dropped the block would merge it with the same
    // action unpinned, which acts on whatever block the pointer is over when it runs.
    for (auto const* row: { select, copy, command, pager })
    {
        INFO(row->title);
        auto unpinned = row->action;
        std::visit(
            [](auto& action) {
                if constexpr (requires { action.block; })
                    action.block = {};
            },
            unpinned);
        CHECK(commandId(row->action) != commandId(unpinned));
    }
}

TEST_CASE("ContextMenu.blockRows.rerunWantsACommandLineAndAPrompt", "[contextmenu][semanticblocks]")
{
    // Pinned to the block as the other block rows are, and grayed rather than hidden where it cannot run.
    auto state = fullyEnabledState();
    auto const menu = buildContextMenu(state);
    auto const* rerun = find(menu, "Rerun Command…");
    REQUIRE(rerun != nullptr);
    CHECK(rerun->availability == RowAvailability::Enabled);
    REQUIRE(std::holds_alternative<actions::RerunCommand>(rerun->action));
    CHECK(std::get<actions::RerunCommand>(rerun->action).block == vtbackend::CommandBlockId(5));
    CHECK(std::get<actions::RerunCommand>(rerun->action).target == vtbackend::CommandBlockTarget::Pointer);

    // While a command runs there is no prompt to type at: the line would be that command's input.
    state.livePrompt = PromptPresence::Absent;
    REQUIRE(find(buildContextMenu(state), "Rerun Command…") != nullptr);
    CHECK(find(buildContextMenu(state), "Rerun Command…")->availability == RowAvailability::Disabled);

    // Nor without a command line to type.
    state = fullyEnabledState();
    state.blockUnderCursor->commandLine = CommandLinePresence::Absent;
    CHECK(find(buildContextMenu(state), "Rerun Command…")->availability == RowAvailability::Disabled);
}

TEST_CASE("ContextMenu.blockRows.grayedOffABlock", "[contextmenu][semanticblocks]")
{
    // Grayed, not hidden, for the reason the fold row is: the menu must not change shape with where
    // the click landed.
    auto state = fullyEnabledState();
    state.blockUnderCursor = std::nullopt;
    auto const offBlock = buildContextMenu(state);
    for (auto const* title: { "Select Output", "Copy Output", "Copy Command", "Open Output in Pager" })
    {
        INFO(title);
        auto const* row = find(offBlock, title);
        REQUIRE(row != nullptr);
        CHECK(row->availability == RowAvailability::Disabled);
    }

    // A block whose command line is not known has output, but no command to copy.
    state = fullyEnabledState();
    state.blockUnderCursor->commandLine = CommandLinePresence::Absent;
    auto const noCommandLine = buildContextMenu(state);
    CHECK(find(noCommandLine, "Copy Command")->availability == RowAvailability::Disabled);
    CHECK(find(noCommandLine, "Copy Output")->availability == RowAvailability::Enabled);
}

TEST_CASE("ContextMenu.blockRows.outputRowsGrayedAtAPrompt", "[contextmenu][semanticblocks]")
{
    // A block still at its prompt has no output yet: its output rows would only decline into the log.
    auto state = fullyEnabledState();
    state.blockUnderCursor->output = OutputPresence::Absent;
    auto const menu = buildContextMenu(state);
    for (auto const* title: { "Select Output", "Copy Output", "Open Output in Pager" })
    {
        INFO(title);
        auto const* row = find(menu, title);
        REQUIRE(row != nullptr);
        CHECK(row->availability == RowAvailability::Disabled);
    }
    CHECK(find(menu, "Copy Command")->availability == RowAvailability::Enabled);
}

TEST_CASE("ContextMenu.blockRows.shownOnABlockBeforeAnyCommandFinished", "[contextmenu][semanticblocks]")
{
    // The first command still running, or the prompt ClearToPrompt left: no command has finished, but
    // the click landed on a block's row, so its rows act on it.
    auto state = fullyEnabledState();
    state.hasLastCommand = false;
    auto const menu = buildContextMenu(state);
    for (auto const* title: { "Select Output", "Copy Output", "Copy Command", "Open Output in Pager" })
    {
        INFO(title);
        auto const* row = find(menu, title);
        REQUIRE(row != nullptr);
        CHECK(row->availability == RowAvailability::Enabled);
    }

    // The rows about the last finished command keep their own gate.
    CHECK(find(menu, "Copy Last Command Output") == nullptr);
}

TEST_CASE("ContextMenu.blockRows.hiddenWithoutShellIntegration", "[contextmenu][semanticblocks]")
{
    auto state = fullyEnabledState();
    state.hasLastCommand = false;
    state.blockUnderCursor = std::nullopt;
    auto const menu = buildContextMenu(state);
    for (auto const* title: { "Select Output", "Copy Output", "Copy Command", "Open Output in Pager" })
    {
        INFO(title);
        CHECK(find(menu, title) == nullptr);
    }
}

TEST_CASE("the Attach to Host submenu lists saved hosts", "[contextmenu][hosts]")
{
    auto state = fullyEnabledState();
    state.hostNames = { "home", "studio" };

    auto const menu = buildContextMenu(state);
    auto const* hosts = find(menu, "Attach to Host");
    REQUIRE(hosts != nullptr);
    REQUIRE(hosts->kind == ContextMenuEntryKind::Submenu);
    REQUIRE(hosts->children.size() == 2);
    for (auto const& [child, name]: std::views::zip(hosts->children, state.hostNames))
    {
        CHECK(child.title == name);
        CHECK(child.check == RowCheck::None);
        auto const* attach = std::get_if<actions::AttachToHost>(&child.action);
        REQUIRE(attach != nullptr);
        CHECK(attach->name == name);
    }

    // Directly after "Change Profile".
    auto const profiles = std::ranges::find(menu, std::string { "Change Profile" }, &ContextMenuEntry::title);
    REQUIRE(profiles != menu.end());
    REQUIRE(std::next(profiles) != menu.end());
    CHECK(std::next(profiles)->title == "Attach to Host");
}

TEST_CASE("no hosts, no Attach to Host submenu", "[contextmenu][hosts]")
{
    auto const menu = buildContextMenu(fullyEnabledState());
    CHECK(find(menu, "Attach to Host") == nullptr);
}

TEST_CASE("ContextMenu.rolesPassThrough", "[contextmenu]")
{
    using Table = command::detail::MenuTable<ContextMenuState>;
    auto const rows = std::array {
        Table::command(actions::Quit {}).withRole(MenuItemRole::Quit),
        Table::command(actions::CopySelection {}),
    };

    auto const entries = Table::buildRows(rows, ContextMenuState {});
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].role == MenuItemRole::Quit);
    CHECK(entries[1].role == MenuItemRole::None);
}
