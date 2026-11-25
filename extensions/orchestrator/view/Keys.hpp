// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/InputEvent.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/QuickReplies.hpp>
#include <orchestrator/view/DispatchForm.hpp>
#include <orchestrator/view/Fleet.hpp>
#include <orchestrator/view/Launcher.hpp>
#include <orchestrator/view/RowMenu.hpp>
#include <orchestrator/view/StartDialog.hpp>
#include <orchestrator/view/TaskBox.hpp>

namespace orchestrator::view
{

struct Row;

// {{{ what a key asks of the engine

/// Read the lists again now.
struct RefreshNowAction
{
    [[nodiscard]] bool operator==(RefreshNowAction const&) const = default;
};

/// The user sent a message to a run's agent from the message box: typed into its tab.
struct SendMessageAction
{
    std::string run;  ///< The run's id.
    std::string text; ///< Exactly what the user wrote.
    [[nodiscard]] bool operator==(SendMessageAction const&) const = default;
};

/// The user answered a run's agent that asks with a quick reply: its key pressed in the agent's tab.
struct QuickReplyAction
{
    std::string run = {};               ///< The run's id.
    char key = '1';                     ///< The dashboard's digit the reply is on.
    std::string label = {};             ///< What its chip says, for the notices.
    PaneKey press = PaneKey::Digit1;    ///< What it presses in the agent's tab.
    AfterReply then = AfterReply::Wait; ///< What follows it.
    [[nodiscard]] bool operator==(QuickReplyAction const&) const = default;
};

/// The user confirmed installing the Claude Code plugins the agents need and lack.
struct InstallPluginsAction
{
    [[nodiscard]] bool operator==(InstallPluginsAction const&) const = default;
};

/// End a run.
struct EndRunAction
{
    std::string run = {}; ///< The run's id.
    [[nodiscard]] bool operator==(EndRunAction const&) const = default;
};

/// Check a kept worktree again.
struct RetryRemovalAction
{
    std::string run = {}; ///< The run's id.
    [[nodiscard]] bool operator==(RetryRemovalAction const&) const = default;
};

/// Drop a finished run from the Finished list.
struct DismissAction
{
    std::string run = {}; ///< The run's id.
    [[nodiscard]] bool operator==(DismissAction const&) const = default;
};

/// Bring a run's agent tab in front of the window the dashboard was opened from.
struct RevealAgentAction
{
    std::string run = {}; ///< The run's id.
    [[nodiscard]] bool operator==(RevealAgentAction const&) const = default;
};

/// Open a web link on the machine the user sits at, through the frontend they typed or clicked in.
struct OpenUrlAction
{
    std::string url = {};   ///< The link.
    std::string label = {}; ///< What the action bar calls it, such as `#2369`.
    [[nodiscard]] bool operator==(OpenUrlAction const&) const = default;
};

/// Show a run's status board in the dashboard's tab: it is read now, and again while the tab shows it.
struct ShowBoardAction
{
    std::string run = {}; ///< The run's id.
    [[nodiscard]] bool operator==(ShowBoardAction const&) const = default;
};

/// The dashboard's tab stopped showing a status board.
struct HideBoardAction
{
    [[nodiscard]] bool operator==(HideBoardAction const&) const = default;
};

/// Fold or unfold a group of the Fleet, such as Finished.
struct ToggleFoldAction
{
    std::string group = {}; ///< The group's key.
    [[nodiscard]] bool operator==(ToggleFoldAction const&) const = default;
};

/// Read a page of a run's transcript, for the dashboard's tab to show.
struct ReadTranscriptAction
{
    std::string run = {}; ///< The run's id.
    uint32_t page = 1;    ///< The 1-based page.
    [[nodiscard]] bool operator==(ReadTranscriptAction const&) const = default;
};

/// What the dashboard asks for: the guest steps each as the engine input of the same name (DispatchAction as
/// Dispatch, ShowBoardAction and HideBoardAction as ShowBoard and HideBoard for the dashboard's tab,
/// ReadTranscriptAction as ReadTranscript), but for RevealAgentAction, SendMessageAction, QuickReplyAction
/// and OpenUrlAction, which it carries out itself with the host.
using UiAction = std::variant<RefreshNowAction,
                              InstallPluginsAction,
                              SendMessageAction,
                              QuickReplyAction,
                              EndRunAction,
                              RetryRemovalAction,
                              DismissAction,
                              RevealAgentAction,
                              OpenUrlAction,
                              DispatchAction,
                              ShowBoardAction,
                              HideBoardAction,
                              ReadTranscriptAction,
                              ToggleFoldAction>;

// }}}
// {{{ the key table

/// What a key, the palette or `⏎` does on the dashboard.
enum class KeyCommand : uint8_t
{
    Up,      ///< In the Fleet, select the row above; in the Focus, scroll its tab up a line.
    Down,    ///< In the Fleet, select the row below; in the Focus, scroll its tab down a line.
    Home,    ///< In the Fleet, select the first row, the run that waited longest; in the Focus, the top.
    End,     ///< In the Fleet, select the last row; in the Focus, the end of the tab, the transcript live.
    Enter,   ///< The selected row's primary action (primaryAction()).
    Back,    ///< Back to the Fleet; in it, clear the filter.
    Tab,     ///< To the Focus, and back.
    Filter,  ///< Type a filter for the rows.
    Palette, ///< Open the command palette: every action, those that do not apply dimmed with the reason.
    Refresh, ///< Read the lists again now; the palette's.
    EndRun,  ///< End the selected run; asks once.
    RetryRemoval,   ///< Check the selected kept worktree again.
    Dismiss,        ///< Drop the selected finished run.
    BoardTab,       ///< Show the Focus's Board tab.
    TranscriptTab,  ///< Show the Focus's Transcript tab.
    DetailsTab,     ///< Show the Focus's Details tab.
    ScrollPageUp,   ///< Scroll the Focus's tab up by a screen.
    ScrollPageDown, ///< Scroll the Focus's tab down by a screen.
    PreviousPage,   ///< Show the transcript's previous page.
    NextPage,       ///< Show the transcript's next page; its last follows the agent again.
    Fold,           ///< Fold or unfold the group of the selected row, and select its heading.
    NewTask,        ///< Open the `n` box: a custom task, typed and started at once.
    InstallPlugins, ///< Install the Claude Code plugins the agents need and lack; the palette's.
    Reveal,         ///< Bring the selected run's agent tab in front.
    OpenLink,       ///< Open the selected run's link on the machine the user sits at (`url-open`).
    Open,           ///< To the Focus of the selected row: to write to its agent, or to read it.
    RunAgain,       ///< Start the selected finished run again: a topic's text in the `n` box, an item in the
                    ///< start dialog on its task.
    Timeline,       ///< Show the last 24 hours, an hour a bar; the palette's.
    SwitchView,     ///< Show the other view: Agents ⇄ Work.
    Sort,           ///< The Work view: sort by importance ⇄ by last update.
    RepoPrevious,   ///< The Work view: step the repository chip back.
    RepoNext,       ///< The Work view: step the repository chip on.
    RowMenu,        ///< The Work view: open the row menu on the selected item.
    Reply1,         ///< Answer the selected agent that asks with the quick reply on `1`.
    Reply2,         ///< … on `2`.
    Reply3,         ///< … on `3`.
    Reply4,         ///< … on `4`.
    Reply5,         ///< … on `5`.
    Reply6,         ///< … on `6`.
    Reply7,         ///< … on `7`.
    Reply8,         ///< … on `8`.
    Reply9,         ///< … on `9`.
};

/// @param command A command.
/// @return The digit of a quick-reply command (Reply1 to Reply9); std::nullopt for any other command.
[[nodiscard]] constexpr std::optional<char> replyDigitOf(KeyCommand command) noexcept
{
    if (command < KeyCommand::Reply1 || command > KeyCommand::Reply9)
        return std::nullopt;
    return static_cast<char>('1' + (std::to_underlying(command) - std::to_underlying(KeyCommand::Reply1)));
}

/// Where the user is: which pane has the keys.
enum class Place : uint8_t
{
    Fleet = 0, ///< The Fleet: letters are commands.
    Focus,     ///< The Focus: its message box has the keys, where the run has one; else the Fleet's letters
               ///< act and ↑↓ scroll.
};

/// One key and what it does: the only place a key is bound to a command.
struct KeyRow
{
    std::string_view key = {}; ///< Its name: a printable character, `^k` for Ctrl+K, or `up`, `enter`, `esc`
                               ///< and the like.
    KeyCommand command = KeyCommand::Up; ///< What it does.
};

/// @return Every key the dashboard acts on; the first row of a command names its key (keyNameOf()).
[[nodiscard]] std::span<KeyRow const> keyRows() noexcept;

/// @return The keys the Work view acts on; the first row of a command names its key (workKeyNameOf()). None
///         is added to dashboardKeys(): that would refuse today's configurations with a task keyed `w`, `s`
///         or `o`.
[[nodiscard]] std::span<KeyRow const> workKeyRows() noexcept;

/// @param key A key the terminal sent.
/// @return What it does in the Work view; std::nullopt for a key no row of workKeyRows() names, or one with
///         Alt.
[[nodiscard]] std::optional<KeyCommand> workCommandFor(core::tui::KeyEvent const& key) noexcept;

/// @param command A command.
/// @return How the Work view's bar names its key, such as `z` or `←`; empty for a command no key of the Work
///         view is bound to.
[[nodiscard]] std::string_view workKeyNameOf(KeyCommand command) noexcept;

/// @param key A key the terminal sent.
/// @return What it does; std::nullopt for a key no row names, or one with Alt.
[[nodiscard]] std::optional<KeyCommand> commandFor(core::tui::KeyEvent const& key) noexcept;

/// @param command A command.
/// @return How the bar and the palette name its key, such as `⏎`, `e`, `del` or `^k`; empty for a command no
///         key is bound to.
[[nodiscard]] std::string_view keyNameOf(KeyCommand command) noexcept;

/// @param command A command.
/// @return Whether it acts on the selected row only where the row offers it (Row::offers): the Focus tabs and
///         the run commands.
[[nodiscard]] bool isRowCommand(KeyCommand command) noexcept;

// }}}
// {{{ the interaction

/// Which view the dashboard shows.
enum class View : uint8_t
{
    Agents = 0, ///< The Fleet and the Focus.
    Work,       ///< The Work board: the merge requests and tickets, by what they need.
};

/// Where the Work view's keys go.
enum class WorkFocus : uint8_t
{
    List = 0, ///< The list: the keys move the selection.
    Filter,   ///< The filter: printable keys type into it.
    RepoChip, ///< The repository chip: ⏎ steps it.
    SortChip, ///< The sort chip: ⏎ switches the sort.
};

/// What is shown over the dashboard, besides the launcher and the palette.
enum class Overlay : uint8_t
{
    None = 0, ///< Nothing.
    Timeline, ///< The last 24 hours, an hour a bar: esc or ⏎ closes it.
};

/// An open command palette.
struct PaletteState
{
    std::string query = {};      ///< What was typed.
    std::size_t highlighted = 0; ///< Into the entries the query matches (paletteMatches()).
    [[nodiscard]] bool operator==(PaletteState const&) const = default;
};

/// Whether typed text goes to the filter.
enum class Filtering : uint8_t
{
    Off,    ///< Keys act by the key table.
    Typing, ///< Printable keys extend the filter; Backspace shortens it; Enter keeps it; Esc drops it.
};

/// Whether the transcript follows the agent.
enum class Follow : uint8_t
{
    Live = 0, ///< Its last page, pinned to the end, read again while the agent runs.
    Away,     ///< Scrolled up, or turned to an earlier page: `↓ live` shows, `end` returns.
};

/// A Focus tab the user chose for a run: it sticks until the run's state changes.
struct TabChoice
{
    std::string run = {};           ///< The run.
    std::string state = {};         ///< Its state as it was chosen (Row::state).
    FocusTab tab = FocusTab::Board; ///< The tab.
    [[nodiscard]] bool operator==(TabChoice const&) const = default;
};

/// A message sent from the message box, kept until the transcript shows it, or the host refused it.
struct SentMessage
{
    std::string run = {};   ///< The run whose agent it went to.
    std::string text = {};  ///< What was sent.
    std::string clock = {}; ///< When, as `HH:MM` in local time.
    [[nodiscard]] bool operator==(SentMessage const&) const = default;
};

/// A quick reply sent, until its agent stops asking: its chip is filled and the asking box dimmed.
struct AnsweredRequest
{
    std::string run = {}; ///< The run whose agent was answered.
    char key = '1';       ///< The reply's digit.
    uint64_t atMs = 0;    ///< When, in the engine's monotonic milliseconds.
    [[nodiscard]] bool operator==(AnsweredRequest const&) const = default;
};

/// The Work view's interaction. The filter does not persist; the folds and the sort are the engine's fold
/// keys.
struct WorkState
{
    std::size_t selected = 0;             ///< Into the lines drawn (workLines()): headings and items.
    std::size_t scroll = 0;               ///< The first line shown.
    WorkFocus focus = WorkFocus::List;    ///< Where the keys go.
    Filtering filtering = Filtering::Off; ///< Whether typed text goes to the filter.
    std::string filter = {};              ///< Items it does not match (matchesFilter()) are left out.
    std::size_t repoChip = 0;             ///< 0: every repository; else 1 + its index in the configuration.
    std::string selectedKey = {}; ///< workItemKey() of the selected item, or `group:<foldKey>` of a heading;
                                  ///< empty: none chosen yet, the first item is.
    std::optional<RowMenuState> menu = {}; ///< The row menu, while it is open: it takes every key.
    std::optional<std::size_t>
        hovered = {}; ///< The item line under the pointer, into the lines drawn; none:
                      ///< the pointer is on no item, or the terminal reports no motion.
    [[nodiscard]] bool operator==(WorkState const&) const = default;
};

/// How long an answered agent may still ask before the action bar says so.
inline constexpr uint64_t StillAsksAfterMs = 10'000;

/// Where the user is on the dashboard: what is selected, open, filtered and written.
struct Interaction
{
    std::size_t selected = 0;             ///< The selected row, counted among the rows that can be selected.
    Place place = Place::Fleet;           ///< Which pane has the keys.
    Overlay overlay = Overlay::None;      ///< What is shown over the dashboard.
    Filtering filtering = Filtering::Off; ///< Whether typed text goes to the filter.
    std::string filter = {};              ///< Rows whose text does not hold it are left out; empty: none is.
    std::string pendingEnd = {}; ///< The run whose end waits for a second `e` (D14: asks once); empty: none.
    std::optional<LauncherState> launcher = {}; ///< The launcher, while it is open: it takes every key.
    std::optional<PaletteState> palette = {};   ///< The palette, while it is open: it takes every key.
    std::string paletteQuery = {}; ///< The query the palette had when esc closed it: it opens with it again.
    LauncherScope
        launcherScope = {};  ///< What the launcher's list was narrowed to: it opens narrowed so again.
    std::string notice = {}; ///< A one-line notice on the action bar, until the next key; empty: none.
    std::optional<TabChoice>
        chosenTab = {};           ///< The tab the user chose for a run; none: each opens on its own.
    std::size_t scroll = 0;       ///< The first line the Focus's tab shows.
    Follow follow = Follow::Live; ///< Whether the transcript follows the agent.
    uint32_t page = 0; ///< The transcript's 1-based page while Away on an earlier one; 0: its last.
    std::map<std::string, std::string, std::less<>> drafts = {}; ///< The message box's draft, per run.
    std::optional<SentMessage> sent = {};         ///< The last message sent, until it shows or is refused.
    std::optional<AnsweredRequest> answered = {}; ///< The last quick reply, until its agent stops asking.
    std::string followGroup = {}; ///< A group whose heading the selection moves to once the rows are built
                                  ///< again, as folding from one of its runs asks; empty: none.
    View view = View::Agents;     ///< Which view is shown.
    WorkState work = {};          ///< Where the user is in the Work view.
    /// The start dialog, while it is open: it takes every key. Here rather than in WorkState, since a
    /// finished run's "run again" opens it from the Agents view too.
    std::optional<StartDialogState> startDialog = {};
    /// The `n` box, while it is open: it takes every key. Opened from either view.
    std::optional<TaskBoxState> taskBox = {};
    [[nodiscard]] bool operator==(Interaction const&) const = default;
};

/// What the Focus's Board and Transcript tabs draw: the run, and how far they are scrolled.
struct FocusSubject
{
    std::string run = {};         ///< The run; empty: none.
    uint32_t page = 1;            ///< The transcript's page asked for.
    std::size_t scroll = 0;       ///< The first line shown, while not following.
    Follow follow = Follow::Live; ///< Whether the transcript shows its end.
};

/// What a key is applied to besides the interaction.
struct KeyContext
{
    std::size_t selectable = 0;     ///< How many rows can be selected.
    Row const* selected = nullptr;  ///< The selected row; nullptr when there is none.
    Engine const* engine = nullptr; ///< Its runs, lists and tasks; nullptr: no key dispatches.
    FocusTab tab = FocusTab::Board; ///< The Focus tab shown for the selected row (activeTabOf()).
    std::size_t lines = 0;          ///< How many lines the Focus's tab has, as last drawn.
    std::size_t rows = 0;           ///< How many of them it showed at once.
    uint32_t pages = 0;             ///< How many pages the shown transcript has, as last read; 0: unknown.
    uint64_t nowMonoMs = 0;         ///< Now, in the engine's monotonic milliseconds: when a reply is sent.
};

/// @param interaction Where the user is.
/// @param row The selected row; nullptr: none.
/// @return The Focus tab shown for @p row: the one the user chose while the run's state did not change, else
///         the one its group opens on (FleetGroupRow::opensOn); Details for a row without a run. A tab the
///         row does not offer gives way to Details.
[[nodiscard]] FocusTab activeTabOf(Interaction const& interaction, Row const* row) noexcept;

/// @param engine The engine; nullptr: none.
/// @param row The selected row; nullptr: none.
/// @return Whether the row's run has a message box: it is active, its agent has a tab to type into, and it
///         does not ask (an asking agent's menu takes keys, not messages: it has the asking box instead).
[[nodiscard]] bool hasMessageBox(Engine const* engine, Row const* row) noexcept;

/// Applies one key's command: Enter as the selected row's primary action; a command that does not apply to
/// the selected row, or that the engine does not allow now, only says why on the action bar
/// (refusalNotice()).
/// @param command What the key does.
/// @param interaction Where the user is; changed by the command.
/// @param context The selected row, the engine and the Focus tab as last drawn.
/// @return What the engine is asked, if anything.
[[nodiscard]] std::optional<UiAction> dispatch(KeyCommand command,
                                               Interaction& interaction,
                                               KeyContext const& context);

/// Shows the Fleet with the keys: the Focus's scroll, follow and page start over for the row selected next.
/// @param interaction Where the user is.
void backToFleet(Interaction& interaction) noexcept;

/// Selects a row, as a click on it does: the Fleet gets the keys, and the Focus starts over for a row that
/// was not selected; an end waiting for its confirmation is dropped.
/// @param interaction Where the user is.
/// @param context How many rows can be selected.
/// @param index The row's position among them; kept within them.
void selectRow(Interaction& interaction, KeyContext const& context, std::size_t index) noexcept;

/// Scrolls the Focus's tab by @p lines, as the wheel does: the transcript stops following the agent when
/// scrolled away from its end, and follows it again once back.
/// @param interaction Where the user is.
/// @param context The Focus tab shown, and its extent as last drawn.
/// @param lines How far: negative is up.
void scrollFocus(Interaction& interaction, KeyContext const& context, std::ptrdiff_t lines) noexcept;

/// Opens the start dialog on an item, as running a finished run again does: on @p task while it still applies
/// to the item, else on the one its state suggests. The dialog previews and refuses as every dispatch does
/// (an item with an active run shows the refusal); nothing starts before it is confirmed. When it does not
/// open, such as for an item the engine no longer lists among its Work items, the action bar says why.
/// @param interaction Where the user is: its start dialog opened, or its notice set. Its session id is left
///        empty for the caller, which holds the source of them, to mint.
/// @param engine The engine: its configuration and its Work items.
/// @param repo The repository's configured name.
/// @param item The item.
/// @param task The id of the task it starts on, such as the run's.
void openStartDialogFor(Interaction& interaction,
                        Engine const& engine,
                        std::string_view repo,
                        ItemRef const& item,
                        std::string_view task);

/// Opens the `n` box (openTaskBox()), or says on the action bar why it does not open.
/// @param interaction Where the user is: its box opened, or its notice set. Its session id is left empty for
///        the caller, which holds the source of them, to mint.
/// @param engine The engine: its configuration and its runs.
/// @param repo The repository it starts in while configured, such as a run's; empty: the first.
/// @param text The text it opens with, such as a finished topic run's; empty for a new one.
void openTaskBoxFor(Interaction& interaction, Engine const& engine, std::string_view repo, std::string text);

/// @param key A key the terminal sent.
/// @param letter A lower-case letter.
/// @return Whether @p key is Ctrl and @p letter, in either case: such as Ctrl+P.
[[nodiscard]] bool isCtrl(core::tui::KeyEvent const& key, char32_t letter) noexcept;

/// @param engine The engine; nullptr: none.
/// @param run A run's id.
/// @return Whether the run is active and has an agent tab to go to.
[[nodiscard]] bool hasAgentTab(Engine const* engine, std::string_view run) noexcept;

/// @param command A command.
/// @return Whether dispatch() has a row for it.
[[nodiscard]] bool isDispatched(KeyCommand command) noexcept;

/// @param key A key the terminal sent.
/// @return The printable character it carries; std::nullopt for anything else, a key with Ctrl or Alt
///         included.
[[nodiscard]] std::optional<char32_t> printableOf(core::tui::KeyEvent const& key) noexcept;

/// Drops the last code point of @p text: its continuation bytes and the byte that leads them.
void dropLastCodepoint(std::string& text) noexcept;

/// @param text What was pasted.
/// @return It as one line: line ends and tabs folded to a space each, every other control character dropped.
[[nodiscard]] std::string pastedText(std::string_view text);

/// Applies a paste: into the start dialog's note or the `n` box's text while one is open, into the palette's
/// or the launcher's query while one is open, into the message box while it has the keys (its line feeds
/// kept), into the filter while it is typed; dropped anywhere else. A paste is text, never keys: nothing in
/// it confirms, moves or acts.
/// @param interaction Where the user is; its query, draft or filter extended.
/// @param context The engine the launcher lists from, and the selected row.
/// @param text What was pasted.
void pasteInto(Interaction& interaction, KeyContext const& context, std::string_view text);

/// Whether a key went to the filter.
enum class FilterInput : uint8_t
{
    NotTaken, ///< The filter is not being typed, or the key is not text: the key table decides.
    Taken,    ///< The key edited the filter, kept it or dropped it.
};

/// Applies a key to the filter while it is being typed.
/// @param key A key the terminal sent.
/// @param interaction Where the user is; its filter changed.
/// @return Whether the key was taken.
[[nodiscard]] FilterInput typeIntoFilter(core::tui::KeyEvent const& key, Interaction& interaction);

/// Applies a key to a filter while it is being typed: line edits edit it, printable keys extend it, Enter
/// keeps it, Esc drops it; either ends the typing.
/// @param key A key the terminal sent.
/// @param filtering Whether the filter is being typed; set Off by Enter and Esc.
/// @param filter The filter.
/// @return Whether the key was taken.
[[nodiscard]] FilterInput typeIntoFilter(core::tui::KeyEvent const& key,
                                         Filtering& filtering,
                                         std::string& filter);

/// Applies one key: to the palette while it is open (applyPaletteKey()), to the timeline while it is shown
/// (esc and ⏎ close it, `^K` opens the palette), to the message box while it has the keys, to the filter
/// while it is typed, to a pending end's confirmation, else by the key table. While an end waits for its
/// confirmation, any key but `e` on the same run keeps the run and does nothing else.
/// @param key A key the terminal sent.
/// @param interaction Where the user is; changed by the key.
/// @param context The selected row, the engine and the Focus tab as last drawn.
/// @return What the engine is asked, if anything.
[[nodiscard]] std::optional<UiAction> applyKey(core::tui::KeyEvent const& key,
                                               Interaction& interaction,
                                               KeyContext const& context);

// }}}

} // namespace orchestrator::view
