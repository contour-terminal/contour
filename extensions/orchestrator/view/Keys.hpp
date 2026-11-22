// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/InputEvent.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/view/Band.hpp>
#include <orchestrator/view/DispatchForm.hpp>

namespace orchestrator::view
{

struct Row;

// {{{ what a key asks of the engine

/// Read the lists again now.
struct RefreshNowAction
{
    [[nodiscard]] bool operator==(RefreshNowAction const&) const = default;
};

/// The user sent a message to a run's agent from the message line (`m`): typed into its tab.
struct SendMessageAction
{
    std::string run;  ///< The run's id.
    std::string text; ///< Exactly what the user wrote.
    [[nodiscard]] bool operator==(SendMessageAction const&) const = default;
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

/// The user opened a finished run's details.
struct MarkSeenAction
{
    std::string run = {}; ///< The run's id.
    [[nodiscard]] bool operator==(MarkSeenAction const&) const = default;
};

/// Bring a run's agent tab in front of the window the dashboard was opened from.
struct RevealAgentAction
{
    std::string run = {}; ///< The run's id.
    [[nodiscard]] bool operator==(RevealAgentAction const&) const = default;
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

/// Fold or unfold a group of the list, such as one repository's issues.
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
/// ReadTranscriptAction as ReadTranscript), but for RevealAgentAction, which it carries out itself with the
/// host.
using UiAction = std::variant<RefreshNowAction,
                              InstallPluginsAction,
                              SendMessageAction,
                              EndRunAction,
                              RetryRemovalAction,
                              DismissAction,
                              MarkSeenAction,
                              RevealAgentAction,
                              DispatchAction,
                              ShowBoardAction,
                              HideBoardAction,
                              ReadTranscriptAction,
                              ToggleFoldAction>;

// }}}
// {{{ the key table

/// What a key does on the dashboard.
enum class KeyCommand : uint8_t
{
    Up,           ///< Select the row above.
    Down,         ///< Select the row below.
    Home,         ///< Select the first row.
    End,          ///< Select the last row.
    Enter,        ///< Dispatch on the selected item, reveal its agent, or open a finished run's details.
    Back,         ///< Close what is open: a board or a transcript, the help, the details page, the filter.
    Tab,          ///< In the Medium and Narrow bands, swap the details in for the list, and back.
    Filter,       ///< Type a filter for the rows.
    Help,         ///< Show or hide the keys.
    Refresh,      ///< Read the lists again now.
    EndRun,       ///< End the selected run.
    RetryRemoval, ///< Check the selected kept worktree again.
    Dismiss,      ///< Drop the selected finished run.
    Dispatch,     ///< Open the dispatch form on the selected item, on the first task that applies to it; on a
                  ///< finished run, a board or a transcript, on the run's item and task.
    Task,         ///< Open the dispatch form on the selected item, on the task whose key was typed.
    Board,        ///< Show the selected run's status board; on the board, read it again now.
    Transcript,   ///< Show the transcript of the selected run, or of the board's run, from its first page.
    ScrollPageUp, ///< Scroll a board or a transcript up by a screen.
    ScrollPageDown, ///< Scroll a board or a transcript down by a screen.
    PreviousPage,   ///< Show the transcript's previous page.
    NextPage,       ///< Show the transcript's next page.
    FirstPage,      ///< Show the transcript's first page.
    LastPage,       ///< Show the transcript's last page.
    Fold,           ///< Fold or unfold the group of the selected row, and select its header.
    NewTopic,       ///< Open the dispatch form on a new topic, in the selected row's repository or the first.
    InstallPlugins, ///< Install the Claude Code plugins the agents need and lack; asks once first.
    Message,        ///< Open the message line to the selected run's agent, or to the board's or transcript's.
};

/// A message being written to a run's agent (`m`): it takes every key until Enter sends it or Esc drops it.
struct MessageDraft
{
    std::string run;  ///< The run whose agent it goes to.
    std::string text; ///< What was written so far; line feeds come from a paste.
    [[nodiscard]] bool operator==(MessageDraft const&) const = default;
};

/// How far the user got installing the missing plugins (`I`): it asks once, as ending a run does.
enum class InstallConfirm : uint8_t
{
    None = 0, ///< Not asked.
    Asked,    ///< The summary line asked; the next `I` installs, any other key forgets it.
};

/// What the dashboard's tab shows.
enum class Screen : uint8_t
{
    Dashboard = 0, ///< The lists and the details.
    Board,         ///< A run's status board (Interaction::subject).
    Transcript,    ///< A run's transcript (Interaction::subject).
};

/// The screens a key row acts and is hinted on: a bit per Screen.
enum class ScreenSet : uint8_t
{
    Dashboard = 0b001,  ///< The dashboard only.
    Board = 0b010,      ///< The status board only.
    Transcript = 0b100, ///< The transcript only.
    All = 0b111,        ///< Every screen.
};

/// @param set Some screens.
/// @param screen A screen.
/// @return Whether @p set holds @p screen.
[[nodiscard]] constexpr bool holds(ScreenSet set, Screen screen) noexcept
{
    return ((std::to_underlying(set) >> std::to_underlying(screen)) & 1U) != 0;
}

/// One key: what it is called, what it does, and what the hint line calls it.
struct KeyRow
{
    std::string_view
        key = {}; ///< Its name: a printable character, or `up`, `down`, `enter`, `esc` and the like.
    KeyCommand command = KeyCommand::Up; ///< What it does.
    std::string_view hint = {}; ///< How the hint line and the help name it; empty: neither shows the row.
    std::optional<Band> hiddenIn =
        std::nullopt; ///< The band whose hint line does not show it, where it does nothing; absent: none.
    ScreenSet on = ScreenSet::Dashboard; ///< The screens it acts and is hinted on.
};

/// @param row A key.
/// @param band The tab's band.
/// @param screen What the tab shows.
/// @return Whether the hint line of a tab in @p band showing @p screen shows the row's hint.
[[nodiscard]] constexpr bool isHintedIn(KeyRow const& row, Band band, Screen screen) noexcept
{
    return !row.hint.empty() && row.hiddenIn != band && holds(row.on, screen);
}

/// @return Every key the dashboard acts on now, in the order the hint line and the help show them.
[[nodiscard]] std::span<KeyRow const> keyRows() noexcept;

/// @param key A key the terminal sent.
/// @param screen What the tab shows.
/// @return What it does on @p screen; std::nullopt for a key no row of that screen names, or one with Ctrl or
///         Alt. A task's key is not in the table: applyKey() looks it up in the configuration.
[[nodiscard]] std::optional<KeyCommand> commandFor(core::tui::KeyEvent const& key, Screen screen) noexcept;

// }}}
// {{{ the interaction

/// What the body of the dashboard shows in the bands that cannot show the list and the details together.
enum class Pane : uint8_t
{
    List,    ///< The list (in the Medium band, with the details below it).
    Details, ///< The selected row's details in the list's place.
};

/// Whether the help is shown over the dashboard.
enum class Overlay : uint8_t
{
    None, ///< Nothing over the dashboard.
    Help, ///< The keys.
};

/// Whether typed text goes to the filter.
enum class Filtering : uint8_t
{
    Off,    ///< Keys act by the key table.
    Typing, ///< Printable keys extend the filter; Backspace shortens it; Enter keeps it; Esc drops it.
};

/// Where the user is on the dashboard: what is selected, open and filtered.
struct Interaction
{
    std::size_t selected = 0;             ///< The selected row, counted among the rows that can be selected.
    Pane pane = Pane::List;               ///< What the body shows where the band cannot show both.
    Overlay overlay = Overlay::None;      ///< Whether the help is shown.
    Filtering filtering = Filtering::Off; ///< Whether typed text goes to the filter.
    std::string filter = {};              ///< Rows whose text does not hold it are left out; empty: none is.
    std::string pendingEnd = {}; ///< The run whose end waits for a second `e` (D14: asks once); empty: none.
    InstallConfirm pendingInstall =
        InstallConfirm::None;                   ///< Whether a second `I` installs the missing plugins.
    std::optional<MessageDraft> message = {};   ///< The message line, while it is open: it takes every key.
    std::optional<DispatchFormState> form = {}; ///< The dispatch form, while it is open: it takes every key.
    std::string notice = {}; ///< A one-line notice on the summary line, until the next key; empty: none.
    Screen screen = Screen::Dashboard; ///< What the tab shows.
    std::string subject = {};          ///< The run a board or a transcript is of; empty on the dashboard.
    Screen back = Screen::Dashboard;   ///< Where Esc on a board or a transcript returns to: the screen it was
                                       ///< opened from.
    uint32_t page = 1;                 ///< The transcript's 1-based page.
    std::size_t scroll = 0;            ///< The first line a board or a transcript shows.
    std::string followGroup = {}; ///< A group whose header the selection moves to once the rows are built
                                  ///< again, as folding from one of its items asks; empty: none.
    [[nodiscard]] bool operator==(Interaction const&) const = default;
};

/// What a key is applied to besides the interaction.
struct KeyContext
{
    Band band = Band::Wide;               ///< The tab's band.
    std::size_t selectable = 0;           ///< How many rows can be selected.
    Row const* selected = nullptr;        ///< The selected row; nullptr when there is none.
    Engine const* engine = nullptr;       ///< Its runs, lists and tasks; nullptr: no key dispatches.
    std::optional<std::size_t> task = {}; ///< For KeyCommand::Task: the task, an index into Config::tasks().
    std::size_t lines = 0;                ///< How many lines a board or a transcript has, as last drawn.
    std::size_t rows = 0;                 ///< How many of them the tab showed at once.
    std::string_view tab = {};            ///< The tab, whose page of a transcript the engine keeps.
};

/// Applies one key's command.
/// @param command What the key does.
/// @param interaction Where the user is; changed by the command.
/// @param context The band and the selected row.
/// @return What the engine is asked, if anything.
[[nodiscard]] std::optional<UiAction> dispatch(KeyCommand command,
                                               Interaction& interaction,
                                               KeyContext const& context);

/// Returns from a board or a transcript to the dashboard. Leaving a board, the caller asks the engine to stop
/// reading it (HideBoardAction).
/// @param interaction Where the user is.
void leaveSubject(Interaction& interaction) noexcept;

/// Opens the dispatch form on an item, as `d` does on a finished run, a board or a transcript: on @p task
/// while it still applies to the item, else on the first task that does. The form previews and refuses as
/// every dispatch does (an item with an active run shows the refusal); nothing starts before it is
/// confirmed. When no form opens, the summary line says why.
/// @param interaction Where the user is: its form opened, or its notice set. Its session id is left empty
///        for the caller, which holds the source of them, to mint.
/// @param engine The engine: its configuration and its lists.
/// @param repo The repository's configured name.
/// @param item The item; a TopicRef opens the topic form on @p topic.
/// @param task The id of the task the form starts on, such as the run's.
/// @param topic A topic's text, such as the run's; empty for any other item.
void openFormFor(Interaction& interaction,
                 Engine const& engine,
                 std::string_view repo,
                 ItemRef const& item,
                 std::string_view task,
                 std::string_view topic);

/// @param engine The engine; nullptr: none.
/// @param run A run's id.
/// @return Whether the run is active and has an agent tab to go to.
[[nodiscard]] bool hasAgentTab(Engine const* engine, std::string_view run) noexcept;

/// @param engine The engine; nullptr: none.
/// @param run A run's id.
/// @return Whether the run has a board to show: it is known, and its worktree is still there
///         (keepsWorktree()).
[[nodiscard]] bool hasBoardToShow(Engine const* engine, std::string_view run) noexcept;

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

/// Applies a paste: into the dispatch form's note while it has the focus, into the filter while it is typed;
/// dropped anywhere else. A paste is text, never keys: nothing in it confirms, moves or acts.
/// @param interaction Where the user is; its note or filter extended.
/// @param text What was pasted.
void pasteInto(Interaction& interaction, std::string_view text);

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

/// Applies one key: to the filter while it is typed, to a pending end's confirmation, else by the key table,
/// or as a task's key when the configuration binds it. While an end waits for its confirmation, any key but
/// `e` on the same run keeps the run and does nothing else.
/// @param key A key the terminal sent.
/// @param interaction Where the user is; changed by the key.
/// @param context The band and the selected row.
/// @return What the engine is asked, if anything.
[[nodiscard]] std::optional<UiAction> applyKey(core::tui::KeyEvent const& key,
                                               Interaction& interaction,
                                               KeyContext const& context);

// }}}

} // namespace orchestrator::view
