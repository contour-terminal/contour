// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the switcher says, as plain text decided here rather than in QML: a row's detail (its git text and
/// tab count, or, when it asks for the user, its reason and time), the footer's description of what Return
/// does and its key hints, the field's drawn hint, the empty state, the level title, and the New Daemon
/// prompt's verdict on the label typed. Pure and Qt-free.

#include <contour/command/Shortcut.hpp>
#include <contour/session/WindowSwitching.hpp>
#include <contour/window/StatusUrgency.hpp>
#include <contour/window/SwitcherKeys.hpp>
#include <contour/window/SwitcherRows.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace contour::window
{

/// The detail at a row's end: proportional text, then a monospace command, then proportional text again.
struct SwitcherDetail
{
    std::string text;      ///< `main ↑2 · 3 tabs`, or `main · exit 1` before a command.
    std::string separator; ///< ` · ` between the words and the command, shown only with it; empty for none.
    std::string command;   ///< The reason's command (monospace); empty for none.
    std::string tail;      ///< ` · 2m ago` after the reason; empty for none.

    bool operator==(SwitcherDetail const&) const = default;
};

/// Whether a row's detail shows its reason and time instead of its tab count.
enum class ShowReason : uint8_t
{
    No = 0,
    Yes,
};

/// What a row's detail shows at one urgency.
struct SwitcherDetailRule
{
    Urgency urgency = Urgency::Idle;    ///< The urgency the row describes.
    ShowReason reason = ShowReason::No; ///< Whether the reason and time replace the tab count.
};

/// One row per Urgency, in enumerator order: from DoneUnseen up, the row asks for the user and says why.
inline constexpr auto SwitcherDetailRules = std::array {
    SwitcherDetailRule { .urgency = Urgency::Idle, .reason = ShowReason::No },
    SwitcherDetailRule { .urgency = Urgency::UnseenOutput, .reason = ShowReason::No },
    SwitcherDetailRule { .urgency = Urgency::Working, .reason = ShowReason::No },
    SwitcherDetailRule { .urgency = Urgency::DoneUnseen, .reason = ShowReason::Yes },
    SwitcherDetailRule { .urgency = Urgency::Failed, .reason = ShowReason::Yes },
    SwitcherDetailRule { .urgency = Urgency::NeedsYou, .reason = ShowReason::Yes },
};

static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, SwitcherDetailRules.size()),
                                  [](std::size_t index) {
                                      return SwitcherDetailRules[index].urgency
                                             == static_cast<Urgency>(index);
                                  }),
              "SwitcherDetailRules lists every Urgency once, in enumerator order");

/// @param urgency An urgency.
/// @return Its rule.
[[nodiscard]] constexpr SwitcherDetailRule const& switcherDetailRule(Urgency urgency) noexcept
{
    return SwitcherDetailRules[static_cast<std::size_t>(urgency)];
}

/// What a row's detail says.
/// - A workspace: its git text, then, when it asks for the user (SwitcherDetailRules) and has a reason, the
///   reason's words, its command and ` · ` its time; otherwise ` · ` its tab count.
/// - A daemon: how many workspaces it holds, or how its listing stands (`Looking…`, the failure, `Not
///   connected`).
/// - New Workspace: `on ‹daemon›`.
/// @param row The row.
/// @param inputs What the list was made of.
/// @param now The wall clock, for another daemon's times.
/// @return The detail; empty on other rows.
[[nodiscard]] SwitcherDetail switcherDetail(SwitcherRow const& row,
                                            SwitcherInputs const& inputs,
                                            std::chrono::system_clock::time_point now);

/// The footer's description of what Return does on @p row.
/// @param row The selected row.
/// @param inputs What the list was made of.
/// @param view Where the user is and what they typed.
/// @param mode What Return does to a workspace: switch this window, or open a new one.
/// @return `Switch this window to api-gateway`, `Move this window to home · api-docs`, …; empty for a row
/// that
///         does nothing.
[[nodiscard]] std::string switcherDescription(SwitcherRow const& row,
                                              SwitcherInputs const& inputs,
                                              SwitcherView const& view,
                                              session::SwitchMode mode);

/// The verbs of one subject's key hints; an empty verb leaves its key out.
struct SwitcherHintRow
{
    SwitcherHintSubject subject = SwitcherHintSubject::None; ///< What the hints are about.
    std::string_view enterVerb;                              ///< What Return does.
    std::string_view shiftEnterVerb;                         ///< What Shift+Return does.
    std::string_view rightVerb;                              ///< What → does.
    /// What Return does when it opens a new window (an Open pick's mode); empty where the mode changes
    /// nothing.
    std::string_view openVerb;
};

/// One row per SwitcherHintSubject, in enumerator order.
inline constexpr auto SwitcherHintRows = std::array {
    SwitcherHintRow { .subject = SwitcherHintSubject::None,
                      .enterVerb = {},
                      .shiftEnterVerb = {},
                      .rightVerb = {},
                      .openVerb = {} },
    SwitcherHintRow { .subject = SwitcherHintSubject::Workspace,
                      .enterVerb = "Switch",
                      .shiftEnterVerb = "New Window",
                      .rightVerb = {},
                      .openVerb = "New Window" },
    SwitcherHintRow { .subject = SwitcherHintSubject::Daemon,
                      .enterVerb = "Open",
                      .shiftEnterVerb = "New Window",
                      .rightVerb = "Open daemon",
                      .openVerb = {} },
    SwitcherHintRow { .subject = SwitcherHintSubject::NewWorkspace,
                      .enterVerb = "Create",
                      .shiftEnterVerb = "Create in New Window",
                      .rightVerb = {},
                      .openVerb = "Create in New Window" },
    SwitcherHintRow { .subject = SwitcherHintSubject::NewDaemon,
                      .enterVerb = "Open",
                      .shiftEnterVerb = {},
                      .rightVerb = "Open",
                      .openVerb = {} },
    SwitcherHintRow { .subject = SwitcherHintSubject::AllDaemons,
                      .enterVerb = "Show",
                      .shiftEnterVerb = {},
                      .rightVerb = "Show",
                      .openVerb = {} },
    SwitcherHintRow { .subject = SwitcherHintSubject::CreateDaemon,
                      .enterVerb = "Create",
                      .shiftEnterVerb = "Create in New Window",
                      .rightVerb = {},
                      .openVerb = "Create in New Window" },
    SwitcherHintRow { .subject = SwitcherHintSubject::SwitchToRunning,
                      .enterVerb = "Switch",
                      .shiftEnterVerb = "New Window",
                      .rightVerb = {},
                      .openVerb = "New Window" },
};

static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, SwitcherHintRows.size()),
                                  [](std::size_t index) {
                                      return SwitcherHintRows[index].subject
                                             == static_cast<SwitcherHintSubject>(index);
                                  }),
              "SwitcherHintRows lists every SwitcherHintSubject once, in enumerator order");

/// @param row The selected row.
/// @return What the footer's hints are about; None for a row that only says something, and for the New Daemon
///         level's action, whose subject the label's verdict decides (SwitcherLabelState::subject).
[[nodiscard]] SwitcherHintSubject switcherHintSubject(SwitcherRow const& row) noexcept;

/// A key the footer hints at.
enum class SwitcherHintKey : uint8_t
{
    Enter = 0,  ///< Return: the subject's enterVerb.
    ShiftEnter, ///< Shift+Return: its shiftEnterVerb.
    Right,      ///< →: its rightVerb.
    Escape,     ///< Escape: what switcherEscapeCommand says it does.
};

/// How a footer too narrow for every hint gives one up.
struct SwitcherHintKeyRule
{
    SwitcherHintKey key = SwitcherHintKey::Enter; ///< The key.
    /// 0 for a hint the footer always keeps; else the order it goes in, lowest first.
    uint8_t dropRank = 0;
};

/// One row per SwitcherHintKey, in the order the footer names them. A narrow picker (the Terminal row's)
/// gives Shift+Return up first, then →; Return and Escape are the two keys the picker cannot be used without,
/// and stay.
inline constexpr auto SwitcherHintKeyRules = std::array {
    SwitcherHintKeyRule { .key = SwitcherHintKey::Enter, .dropRank = 0 },
    SwitcherHintKeyRule { .key = SwitcherHintKey::ShiftEnter, .dropRank = 1 },
    SwitcherHintKeyRule { .key = SwitcherHintKey::Right, .dropRank = 2 },
    SwitcherHintKeyRule { .key = SwitcherHintKey::Escape, .dropRank = 0 },
};

static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, SwitcherHintKeyRules.size()),
                                  [](std::size_t index) {
                                      return SwitcherHintKeyRules[index].key
                                             == static_cast<SwitcherHintKey>(index);
                                  }),
              "SwitcherHintKeyRules lists every SwitcherHintKey once, in enumerator order");

/// The footer's key hints, as the palette spells its own: `<key> <verb>` joined by HintSeparator. Return,
/// Shift+Return and → as the subject's row has verbs for them, then Escape's own verb.
/// @param subject What the hints are about.
/// @param escape What Escape does (switcherEscapeCommand): ClearFilter, Back or Close.
/// @param style How to spell the keys.
/// @param mode What Return does: switch this window, or (an Open pick) open a new one.
/// @return `↩ Switch · ⇧↩ New Window · ⎋ Close`.
[[nodiscard]] std::string switcherFooterHints(SwitcherHintSubject subject,
                                              SwitcherCommand escape,
                                              command::ShortcutStyle style,
                                              session::SwitchMode mode);

/// The footer's key hints for every width, widest first: all of them (switcherFooterHints), then with one
/// more given up per SwitcherHintKeyRules, down to Return and Escape. No two sets alike; the picker shows the
/// widest that leaves the description its room.
/// @param subject What the hints are about.
/// @param escape What Escape does (switcherEscapeCommand).
/// @param style How to spell the keys.
/// @param mode What Return does.
/// @return `↩ Switch · ⇧↩ New Window · ⎋ Close`, then `↩ Switch · ⎋ Close`.
[[nodiscard]] std::vector<std::string> switcherFooterHintSets(SwitcherHintSubject subject,
                                                              SwitcherCommand escape,
                                                              command::ShortcutStyle style,
                                                              session::SwitchMode mode);

/// The New Daemon prompt's state for the label typed.
struct SwitcherLabelState
{
    LabelVerdict verdict = LabelVerdict::Empty; ///< What the label amounts to.
    std::string message;                        ///< The verdict line; empty for an empty label.
    std::string actionTitle;                    ///< The one action row; empty for none.
    std::string equivalentCommand;              ///< `contour daemon --label ‹label›`, when Available.
    SwitcherHintSubject subject = SwitcherHintSubject::None; ///< What the footer's hints are about.

    bool operator==(SwitcherLabelState const&) const = default;
};

/// Judges a label as it is typed.
/// @param label What the user typed.
/// @param runningLabels The labels of the local daemons running now.
/// @param style How to spell the Return key the running message names.
/// @return The verdict, its message, the action row, the equivalent command and the hints' subject.
[[nodiscard]] SwitcherLabelState switcherLabelState(std::string_view label,
                                                    std::span<std::string const> runningLabels,
                                                    command::ShortcutStyle style);

/// @param view Where the user is.
/// @param daemonName The name of the daemon the level or the scope is about.
/// @return The field's drawn hint: `Switch workspace or daemon…`, `Filter workspaces on ‹daemon›…`, or
///         `Label, such as work2`.
[[nodiscard]] std::string switcherHintText(SwitcherView const& view, std::string_view daemonName);

/// @param filter What the user typed.
/// @return The empty state: `No workspace matches “‹filter›”`.
[[nodiscard]] std::string switcherEmptyText(std::string_view filter);

/// @param view Where the user is.
/// @param daemonName The name of the daemon a Daemon level is about.
/// @return The level chip before the field: the daemon's name, `New Daemon`, or empty at the top level.
[[nodiscard]] std::string switcherLevelTitle(SwitcherView const& view, std::string_view daemonName);

} // namespace contour::window
