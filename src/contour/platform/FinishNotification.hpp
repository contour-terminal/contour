// SPDX-License-Identifier: Apache-2.0
//
// The finish-notification decision: whether a finished command is worth telling the user about, and in
// which words. Header-only and Qt-free, so it is table-tested in contour_test; the session supplies the
// facts and does the delivery.
#pragma once

#include <contour/config/FinishNotificationConfig.hpp>
#include <contour/platform/NotificationOccasion.hpp>
#include <contour/platform/SessionVisibility.hpp>

#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/core/WorkingDirectory.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/shell/CommandLineSanitizer.hpp>
#include <vtbackend/shell/CommandLineText.hpp>

#include <core/Flags.hpp>
#include <core/Utils.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace contour::platform
{

/// What a finish notification says, and how it is delivered.
struct FinishNotificationRequest
{
    std::string title; ///< e.g. `✗ make failed (exit 2)`.
    std::string body;  ///< e.g. `took 3m 12s · ~/src/contour · build`.
    config::NotifyAction action = config::NotifyAction::Notify; ///< How to deliver it.

    bool operator==(FinishNotificationRequest const&) const = default;
};

// {{{ How a command ended

/// How one way of ending is worded in a title.
struct FinishWording
{
    vtbackend::CommandEnding ending; ///< The ending this row words.
    std::string_view mark;           ///< Leading glyph and space, or empty when nothing is known.
    std::string_view verb;           ///< What the title says the command did: `finished`, `failed`, ...
    vtbackend::FinishDetail detail;  ///< What the title adds in parentheses.
};

/// One row per vtbackend::CommandEnding: a seventh ending is a row here, not a branch in three functions.
/// Whether an ending is a failure is not the wording's to say; vtbackend::isFailure() decides it for the
/// gutter and the notification alike.
inline constexpr auto FinishWordingTable = std::array {
    FinishWording { vtbackend::CommandEnding::Unknown, "", "finished", vtbackend::FinishDetail::None },
    FinishWording { vtbackend::CommandEnding::Succeeded, "✓ ", "finished", vtbackend::FinishDetail::None },
    FinishWording { vtbackend::CommandEnding::Failed, "✗ ", "failed", vtbackend::FinishDetail::ExitCode },
    FinishWording { vtbackend::CommandEnding::Killed, "✗ ", "killed", vtbackend::FinishDetail::Signal },
    FinishWording { vtbackend::CommandEnding::Crashed, "✗ ", "crashed", vtbackend::FinishDetail::None },
    FinishWording {
        vtbackend::CommandEnding::Interrupted, "✗ ", "interrupted", vtbackend::FinishDetail::None },
};

/// The wording row of @p ending.
/// @param ending The ending to word.
/// @return Its row of FinishWordingTable (the Unknown row for an ending the table lacks).
[[nodiscard]] constexpr FinishWording const& wordingOf(vtbackend::CommandEnding ending) noexcept
{
    for (auto const& row: FinishWordingTable)
        if (row.ending == ending)
            return row;
    return FinishWordingTable.front();
}

/// What a title puts before each detail in its parentheses, by vtbackend::FinishDetail: `exit 2`, but a
/// bare `SIGSEGV`. The sticky header's chip words the same text as `✗ 2`; only this is the notification's.
inline constexpr auto FinishDetailPrefixes = std::array<std::string_view, 3> { "", "exit ", "" };

/// The parenthesised detail of a title: `exit 2`, `SIGSEGV`, or nothing.
/// @param detail   Which detail the ending's wording row asks for.
/// @param exitCode The reported exit code, if any.
/// @param outcome  What OSC 3008 said about the end, if anything.
/// @return The detail, or an empty string when the title carries none.
[[nodiscard]] inline std::string finishDetail(vtbackend::FinishDetail detail,
                                              std::optional<int> exitCode,
                                              vtbackend::ContextOutcome const& outcome)
{
    auto const text = vtbackend::finishDetailText(detail, exitCode, outcome);
    if (text.empty())
        return {};
    return std::format("{}{}", FinishDetailPrefixes.at(static_cast<size_t>(detail)), text);
}

// }}}

// {{{ The words

/// The longest command name a title carries, in bytes, the `…` of a cut name included; longer ones are
/// cut at a character boundary.
/// @see vtbackend::truncateUtf8
inline constexpr size_t MaxCommandNameBytes = 48;

/// The longest directory or tab name a body carries, in bytes, the `…` of a cut one included: both arrive
/// from whatever wrote to the terminal, so without a bound a window title could make the body any size.
/// @see vtbackend::truncateUtf8
inline constexpr size_t MaxBodySegmentBytes = 128;

/// What a title calls a command whose line is unknown.
inline constexpr std::string_view FallbackCommandName = "command";

/// The first word of the command line, sanitised for display -- what a title calls the command.
/// @param commandLine The block's command line; untrusted, so it is read through its display form.
/// @return Its first word, cut to MaxCommandNameBytes, or FallbackCommandName when the line is blank.
[[nodiscard]] inline std::string commandShortName(vtbackend::CommandLineText const& commandLine)
{
    auto const display = commandLine.display();
    auto const text = std::string_view { display };
    auto const begin = text.find_first_not_of(' ');
    if (begin == std::string_view::npos)
        return std::string { FallbackCommandName };
    // With no space after the word, end is npos, and substr() clamps the count to the rest of the text.
    auto const end = text.find(' ', begin);
    return vtbackend::truncateUtf8(text.substr(begin, end - begin), MaxCommandNameBytes);
}

/// `✓ make finished`, `✗ make failed (exit 2)`, `✗ make killed (SIGSEGV)`, ...
/// @param summary The finished block.
/// @param ending  How it ended, as vtbackend::commandEndingOf() classified it.
/// @return The title: mark, command name, verb and, when there is one, the parenthesised detail.
[[nodiscard]] inline std::string finishNotificationTitle(vtbackend::CommandBlockSummary const& summary,
                                                         vtbackend::CommandEnding ending)
{
    auto const& wording = wordingOf(ending);
    auto const detail = finishDetail(wording.detail, summary.exitCode, summary.outcome);
    auto title = std::format("{}{} {}", wording.mark, commandShortName(summary.commandLine), wording.verb);
    if (!detail.empty())
        title += std::format(" ({})", detail);
    return title;
}

/// `✓ make finished`, `✗ make failed (exit 2)`, `✗ make killed (SIGSEGV)`, ...
/// @param summary The finished block.
/// @return The title, for the ending vtbackend::commandEndingOf() classifies @p summary as.
[[nodiscard]] inline std::string finishNotificationTitle(vtbackend::CommandBlockSummary const& summary)
{
    return finishNotificationTitle(summary, vtbackend::commandEndingOf(summary.exitCode, summary.outcome));
}

/// `took 3m 12s · ~/src/contour · build`; an unknown duration or directory, or an unnamed tab, leaves its
/// segment out.
/// The duration reads as everywhere else a command's duration is shown.
/// @see vtbackend::formatCommandDuration
/// @param summary       The finished block.
/// @param tabName       The tab strip's label for the session's tab; untrusted (it may be a window title).
/// @param homeDirectory The local home, for `~`; only a directory known to be on this machine is shortened.
/// @return The body, its segments joined by ` · `, each sanitised for display and cut to MaxBodySegmentBytes.
[[nodiscard]] inline std::string finishNotificationBody(vtbackend::CommandBlockSummary const& summary,
                                                        std::string_view tabName,
                                                        std::string_view homeDirectory)
{
    // Sanitised first, then cut: a replaced control byte grows to three bytes, so a cut made first would
    // not bound what is shown.
    auto const segmentOf = [](std::string_view text) {
        return vtbackend::truncateUtf8(
            vtbackend::sanitizeCommandLine(text, vtbackend::SanitizePurpose::Display), MaxBodySegmentBytes);
    };

    auto segments = std::vector<std::string> {};
    if (summary.duration)
        segments.push_back("took " + vtbackend::formatCommandDuration(*summary.duration));

    if (auto const& directory = summary.workingDirectory; !directory.path.empty())
    {
        segments.push_back(
            segmentOf(vtbackend::homeAbbreviatedPath(directory.path, directory.locality, homeDirectory)));
    }

    if (!tabName.empty())
        segments.push_back(segmentOf(tabName));

    return segments | core::views::joinWith(std::string_view { " · " });
}

// }}}

// {{{ The decision

/// Whether @p wanted asks for a notice about a command that ended as @p ending.
/// @param wanted The profile's `notify_on_command_finish.outcome`.
/// @param ending How the command ended.
/// @see vtbackend::isFailure
/// @return True when a notice is wanted.
[[nodiscard]] constexpr bool isOutcomeWanted(config::NotifyOutcome wanted,
                                             vtbackend::CommandEnding ending) noexcept
{
    switch (wanted)
    {
        case config::NotifyOutcome::Any: return true;
        case config::NotifyOutcome::Failure: return vtbackend::isFailure(ending);
    }
    return true;
}

/// Decides whether a finished command is worth telling the user about -- and nothing more, so a caller
/// gathers what the words need (the tab's name, the home) only for a command that passes. The floor
/// rejects nearly every command, and it should cost nothing to do so.
/// @see finishNotificationText
/// @see sessionVisibilityFor
///
/// @param summary    The finished block (a value copy made on the parser thread).
/// @param policy     The profile's `notify_on_command_finish`.
/// @param visibility Where the block's session is right now.
/// @return How the command ended, when the policy wants a notice about it; nullopt when it does not.
[[nodiscard]] constexpr std::optional<vtbackend::CommandEnding> finishNoticeKind(
    vtbackend::CommandBlockSummary const& summary,
    config::FinishNotificationConfig const& policy,
    SessionVisibility visibility) noexcept
{
    if (!isNoticeWanted(policy.when, visibility))
        return std::nullopt;
    // In whole seconds, the floor's own unit: converting the floor to the clock's nanoseconds instead would
    // overflow for a floor set high to mean "practically never". Exact for an integral floor, since a
    // duration is below it exactly when its whole seconds are.
    // A command whose duration is unknown is not known to have reached any floor above zero.
    if (std::chrono::floor<std::chrono::seconds>(
            summary.duration.value_or(std::chrono::steady_clock::duration {}))
        < policy.minDuration)
        return std::nullopt;
    auto const ending = vtbackend::commandEndingOf(summary.exitCode, summary.outcome);
    if (!isOutcomeWanted(policy.outcome, ending))
        return std::nullopt;
    return ending;
}

/// What a finish notice says, once finishNoticeKind() has decided there is one.
///
/// Pure: everything it reads is a parameter. The command line, the directory and the tab name all
/// arrive from whatever wrote to the terminal, so each is sanitised for display before it can reach the
/// desktop.
/// @see vtbackend::sanitizeCommandLine
///
/// @param summary       The finished block.
/// @param ending        How it ended, as finishNoticeKind() returned it.
/// @param tabName       The tab strip's label for the session's tab; empty leaves it out.
/// @param homeDirectory The user's home, for writing the directory as `~/...`; empty shortens nothing.
/// @param action        The profile's `notify_on_command_finish.action`.
/// @return The notification to deliver.
[[nodiscard]] inline FinishNotificationRequest finishNotificationText(
    vtbackend::CommandBlockSummary const& summary,
    vtbackend::CommandEnding ending,
    std::string_view tabName,
    std::string_view homeDirectory,
    config::NotifyAction action)
{
    return FinishNotificationRequest { .title = finishNotificationTitle(summary, ending),
                                       .body = finishNotificationBody(summary, tabName, homeDirectory),
                                       .action = action };
}

// }}}

// {{{ Delivery

/// What delivering a finish notice does.
enum class FinishEffect : uint8_t
{
    Notification = 0b01, ///< A desktop notification (Linux) or tray message (elsewhere).
    Bell = 0b10,         ///< The profile's bell sound, and its window alert when configured.
};

/// A set of FinishEffect.
using FinishEffects = core::Flags<FinishEffect>;

/// The effects @p action asks for. A switch, so a fourth action is a compile error here.
/// @param action The profile's `notify_on_command_finish.action`.
/// @return The effects delivering the notice has.
[[nodiscard]] constexpr FinishEffects effectsOf(config::NotifyAction action) noexcept
{
    switch (action)
    {
        case config::NotifyAction::Notify: return FinishEffects { FinishEffect::Notification };
        case config::NotifyAction::Bell: return FinishEffects { FinishEffect::Bell };
        case config::NotifyAction::NotifyBell:
            return FinishEffects { FinishEffect::Notification, FinishEffect::Bell };
    }
    return FinishEffects { FinishEffect::Notification };
}

/// The prefix of every finish notification's identifier. An application's OSC 99 notifications share the
/// session notifier's identifier space, so this must be one no `i=` can spell -- and none can contain a
/// `:`, which separates OSC 99's metadata entries. An application can therefore neither close, replace
/// nor answer the click of a finish notification.
inline constexpr std::string_view FinishNotificationIdPrefix = "contour:command-finished:";

/// The one identifier session @p sessionId's finish notifications carry, so a newer one replaces the
/// older on the desktop instead of stacking under it.
/// @param sessionId The session's id.
/// @return FinishNotificationIdPrefix followed by @p sessionId.
[[nodiscard]] inline std::string finishNotificationIdentifier(int sessionId)
{
    return std::format("{}{}", FinishNotificationIdPrefix, sessionId);
}

// }}}

} // namespace contour::platform
