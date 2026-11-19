// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/InputEvent.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

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

/// What the dashboard asks for: the guest steps each as the engine input of the same name (DispatchAction as
/// Dispatch), but for RevealAgentAction, which it carries out itself with the host.
using UiAction = std::variant<RefreshNowAction,
                              EndRunAction,
                              RetryRemovalAction,
                              DismissAction,
                              MarkSeenAction,
                              RevealAgentAction,
                              DispatchAction>;

// }}}
// {{{ the key table

/// What a key does on the dashboard.
enum class KeyCommand : uint8_t
{
    Up,           ///< Select the row above.
    Down,         ///< Select the row below.
    Home,         ///< Select the first row.
    End,          ///< Select the last row.
    Enter,        ///< Open the selected row's details.
    Back,         ///< Close what is open: the help, the details page, the filter.
    Tab,          ///< In the Medium band, swap the details in for the list, and back.
    Filter,       ///< Type a filter for the rows.
    Help,         ///< Show or hide the keys.
    Refresh,      ///< Read the lists again now.
    EndRun,       ///< End the selected run.
    RetryRemoval, ///< Check the selected kept worktree again.
    Dismiss,      ///< Drop the selected finished run.
};

/// One key: what it is called, what it does, and what the hint line calls it.
struct KeyRow
{
    std::string_view
        key = {}; ///< Its name: a printable character, or `up`, `down`, `enter`, `esc` and the like.
    KeyCommand command = KeyCommand::Up; ///< What it does.
    std::string_view hint = {}; ///< How the hint line and the help name it; empty: neither shows the row.
    std::optional<Band> onlyIn =
        std::nullopt; ///< The band whose hint line alone shows it; absent: every band's.
};

/// @param row A key.
/// @param band The tab's band.
/// @return Whether the hint line of a tab in @p band shows the row's hint.
[[nodiscard]] constexpr bool isHintedIn(KeyRow const& row, Band band) noexcept
{
    return !row.hint.empty() && (!row.onlyIn || *row.onlyIn == band);
}

/// @return Every key the dashboard acts on now, in the order the hint line and the help show them.
[[nodiscard]] std::span<KeyRow const> keyRows() noexcept;

/// @param key A key the terminal sent.
/// @return What it does on the dashboard; std::nullopt for a key no row names, or one with Ctrl or Alt.
[[nodiscard]] std::optional<KeyCommand> commandFor(core::tui::KeyEvent const& key) noexcept;

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
    std::optional<DispatchFormState> form = {}; ///< The dispatch form, while it is open: it takes every key.
    std::string notice = {}; ///< A one-line notice on the summary line, until the next key; empty: none.
    [[nodiscard]] bool operator==(Interaction const&) const = default;
};

/// What a key is applied to besides the interaction.
struct KeyContext
{
    Band band = Band::Wide;        ///< The tab's band.
    std::size_t selectable = 0;    ///< How many rows can be selected.
    Row const* selected = nullptr; ///< The selected row; nullptr when there is none.
};

/// Applies one key's command.
/// @param command What the key does.
/// @param interaction Where the user is; changed by the command.
/// @param context The band and the selected row.
/// @return What the engine is asked, if anything.
[[nodiscard]] std::optional<UiAction> dispatch(KeyCommand command,
                                               Interaction& interaction,
                                               KeyContext const& context);

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

/// Applies one key: to the filter while it is typed, to a pending end's confirmation, else by the key table.
/// While an end waits for its confirmation, any key but `e` on the same run keeps the run and does nothing
/// else.
/// @param key A key the terminal sent.
/// @param interaction Where the user is; changed by the key.
/// @param context The band and the selected row.
/// @return What the engine is asked, if anything.
[[nodiscard]] std::optional<UiAction> applyKey(core::tui::KeyEvent const& key,
                                               Interaction& interaction,
                                               KeyContext const& context);

// }}}

} // namespace orchestrator::view
