// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the ConEmu-style progress extension (OSC 9;4): the state table it defines, what
// each state does to the percentage already in effect, and the payloads it must reject.

#include <vtbackend/MockTerm.hpp>
#include <vtbackend/StatusLineBuilder.hpp>
#include <vtbackend/vt/ProgressState.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

using namespace std::string_view_literals;
using namespace vtbackend;

namespace
{
/// The state a session starts in: nothing shown, nothing measured.
constexpr auto Fresh = Progress {};

/// A session mid-way through a normal operation, for the "what survives?" cases below.
constexpr auto AtSixty = Progress { .state = ProgressState::Normal, .percentage = 60 };
} // namespace

TEST_CASE("ProgressState.normal_takes_the_supplied_percentage", "[progress]")
{
    auto const progress = applyProgressSequence(Fresh, "4;1;40"sv);
    REQUIRE(progress.has_value());
    CHECK(progress->state == ProgressState::Normal);
    CHECK(progress->percentage == 40);
}

TEST_CASE("ProgressState.inactive_clears", "[progress]")
{
    // State 0 withdraws the indicator AND zeroes the percentage, so a later state that preserves the
    // percentage cannot resurrect a stale number.
    auto const progress = applyProgressSequence(AtSixty, "4;0"sv);
    REQUIRE(progress.has_value());
    CHECK(progress->state == ProgressState::Inactive);
    CHECK(progress->percentage == 0);
}

TEST_CASE("ProgressState.inactive_ignores_a_supplied_percentage", "[progress]")
{
    auto const progress = applyProgressSequence(AtSixty, "4;0;90"sv);
    REQUIRE(progress.has_value());
    CHECK(progress->state == ProgressState::Inactive);
    CHECK(progress->percentage == 0);
}

TEST_CASE("ProgressState.indeterminate_keeps_the_percentage", "[progress]")
{
    // The pulsing state says nothing about progress, so the bar an application had drawn survives it
    // -- pausing into indeterminate and back must not lose the position.
    auto const progress = applyProgressSequence(AtSixty, "4;3"sv);
    REQUIRE(progress.has_value());
    CHECK(progress->state == ProgressState::Indeterminate);
    CHECK(progress->percentage == 60);
}

TEST_CASE("ProgressState.indeterminate_ignores_a_supplied_percentage", "[progress]")
{
    auto const progress = applyProgressSequence(AtSixty, "4;3;10"sv);
    REQUIRE(progress.has_value());
    CHECK(progress->state == ProgressState::Indeterminate);
    CHECK(progress->percentage == 60);
}

TEST_CASE("ProgressState.error_takes_a_percentage_when_given", "[progress]")
{
    auto const progress = applyProgressSequence(AtSixty, "4;2;80"sv);
    REQUIRE(progress.has_value());
    CHECK(progress->state == ProgressState::Error);
    CHECK(progress->percentage == 80);
}

TEST_CASE("ProgressState.error_keeps_the_percentage_when_omitted", "[progress]")
{
    auto const progress = applyProgressSequence(AtSixty, "4;2"sv);
    REQUIRE(progress.has_value());
    CHECK(progress->state == ProgressState::Error);
    CHECK(progress->percentage == 60);
}

TEST_CASE("ProgressState.paused_takes_a_percentage_when_given", "[progress]")
{
    auto const progress = applyProgressSequence(AtSixty, "4;4;25"sv);
    REQUIRE(progress.has_value());
    CHECK(progress->state == ProgressState::Paused);
    CHECK(progress->percentage == 25);
}

TEST_CASE("ProgressState.paused_keeps_the_percentage_when_omitted", "[progress]")
{
    auto const progress = applyProgressSequence(AtSixty, "4;4"sv);
    REQUIRE(progress.has_value());
    CHECK(progress->state == ProgressState::Paused);
    CHECK(progress->percentage == 60);
}

TEST_CASE("ProgressState.normal_without_a_percentage_is_zero", "[progress]")
{
    // Unlike Error and Paused, Normal is *about* the number -- omitting it means "at the start",
    // not "wherever you were".
    auto const progress = applyProgressSequence(AtSixty, "4;1"sv);
    REQUIRE(progress.has_value());
    CHECK(progress->state == ProgressState::Normal);
    CHECK(progress->percentage == 0);
}

TEST_CASE("ProgressState.percentage_clamps_at_100", "[progress]")
{
    // Overshooting still means "finished"; rejecting the sequence would strand a stale bar on screen.
    auto const progress = applyProgressSequence(Fresh, "4;1;150"sv);
    REQUIRE(progress.has_value());
    CHECK(progress->percentage == 100);
}

TEST_CASE("ProgressState.exactly_100_is_accepted", "[progress]")
{
    auto const progress = applyProgressSequence(Fresh, "4;1;100"sv);
    REQUIRE(progress.has_value());
    CHECK(progress->percentage == 100);
}

TEST_CASE("ProgressState.empty_percentage_field_reads_as_absent", "[progress]")
{
    // `OSC 9;4;2;` -- a trailing separator with nothing after it is a missing value, not a bad one.
    auto const progress = applyProgressSequence(AtSixty, "4;2;"sv);
    REQUIRE(progress.has_value());
    CHECK(progress->state == ProgressState::Error);
    CHECK(progress->percentage == 60);
}

TEST_CASE("ProgressState.bare_sequence_is_rejected", "[progress]")
{
    // The regression this extension was written around: `OSC 9;4 ST` used to fall through to the
    // ConEmu notification path and pop a desktop notification whose body was the literal text "4".
    auto const progress = applyProgressSequence(AtSixty, "4"sv);
    REQUIRE_FALSE(progress.has_value());
    CHECK(progress.error() == ProgressError::MalformedPayload);
}

TEST_CASE("ProgressState.unknown_state_is_rejected", "[progress]")
{
    auto const progress = applyProgressSequence(AtSixty, "4;9"sv);
    REQUIRE_FALSE(progress.has_value());
    CHECK(progress.error() == ProgressError::UnknownState);
}

TEST_CASE("ProgressState.non_numeric_state_is_rejected", "[progress]")
{
    auto const progress = applyProgressSequence(AtSixty, "4;x"sv);
    REQUIRE_FALSE(progress.has_value());
    CHECK(progress.error() == ProgressError::UnknownState);
}

TEST_CASE("ProgressState.non_numeric_percentage_is_rejected", "[progress]")
{
    auto const progress = applyProgressSequence(AtSixty, "4;1;abc"sv);
    REQUIRE_FALSE(progress.has_value());
    CHECK(progress.error() == ProgressError::MalformedProgress);
}

TEST_CASE("ProgressState.negative_percentage_is_rejected", "[progress]")
{
    // from_chars on an unsigned target refuses the sign outright, rather than wrapping it.
    auto const progress = applyProgressSequence(AtSixty, "4;1;-5"sv);
    REQUIRE_FALSE(progress.has_value());
    CHECK(progress.error() == ProgressError::MalformedProgress);
}

TEST_CASE("ProgressState.empty_state_field_is_rejected", "[progress]")
{
    auto const progress = applyProgressSequence(AtSixty, "4;"sv);
    REQUIRE_FALSE(progress.has_value());
    CHECK(progress.error() == ProgressError::UnknownState);
}

TEST_CASE("ProgressState.a_rejected_sequence_leaves_the_caller_free_to_keep_state", "[progress]")
{
    // The contract the Screen handler relies on: nothing partial is returned on rejection, so a
    // malformed sequence can never clear an indicator the application still wants shown.
    for (auto const payload: { "4"sv, "4;"sv, "4;9"sv, "4;1;abc"sv })
        CHECK_FALSE(applyProgressSequence(AtSixty, payload).has_value());
}

TEST_CASE("ProgressState.a_fresh_progress_shows_nothing", "[progress]")
{
    // Zero-initialized must mean "nothing to show", which is what lets the wire protocol and the
    // frontends treat a default-constructed Progress as the absent case.
    CHECK(Fresh.state == ProgressState::Inactive);
    CHECK(Fresh.percentage == 0);
}

// {{{ the sequence driven through a real terminal

TEST_CASE("ProgressState.osc_sets_terminal_state_and_announces_it", "[progress]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };
    mock.writeToScreen("\033]9;4;1;40\033\\");

    CHECK(mock.terminal.progress() == Progress { .state = ProgressState::Normal, .percentage = 40 });
    REQUIRE(mock.progressNotifications.size() == 1);
    CHECK(mock.progressNotifications.front()
          == Progress { .state = ProgressState::Normal, .percentage = 40 });
    // The progress sub-function must never be mistaken for a ConEmu notification.
    CHECK(mock.notifications.empty());
}

TEST_CASE("ProgressState.osc_bare_four_raises_no_notification", "[progress]")
{
    // The regression: `OSC 9;4 ST` used to reach the notification path and pop a desktop
    // notification whose body was the literal text "4".
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };
    mock.writeToScreen("\033]9;4\033\\");

    CHECK(mock.notifications.empty());
    CHECK(mock.progressNotifications.empty());
    CHECK(mock.terminal.progress() == Progress {});
}

TEST_CASE("ProgressState.osc_still_delivers_plain_conemu_notifications", "[progress]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };
    mock.writeToScreen("\033]9;Build finished\033\\");

    REQUIRE(mock.notifications.size() == 1);
    CHECK(mock.notifications.front().first == "ConEmu");
    CHECK(mock.notifications.front().second == "Build finished");
    CHECK(mock.progressNotifications.empty());
}

TEST_CASE("ProgressState.osc_notification_starting_with_four_is_not_progress", "[progress]")
{
    // The first FIELD must be exactly "4"; a message that merely begins with a 4 is still a message.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };
    mock.writeToScreen("\033]9;4 files copied\033\\");

    REQUIRE(mock.notifications.size() == 1);
    CHECK(mock.notifications.front().second == "4 files copied");
    CHECK(mock.progressNotifications.empty());
}

TEST_CASE("ProgressState.osc_malformed_leaves_the_indicator_alone", "[progress]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };
    mock.writeToScreen("\033]9;4;1;70\033\\");
    REQUIRE(mock.terminal.progress().percentage == 70);

    // An unknown state must not clear what is on screen.
    mock.writeToScreen("\033]9;4;9\033\\");
    CHECK(mock.terminal.progress() == Progress { .state = ProgressState::Normal, .percentage = 70 });
    CHECK(mock.progressNotifications.size() == 1);
    CHECK(mock.notifications.empty());
}

TEST_CASE("ProgressState.osc_sequence_of_updates_tracks_the_operation", "[progress]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };
    mock.writeToScreen("\033]9;4;1;10\033\\");
    mock.writeToScreen("\033]9;4;3\033\\");    // busy, position preserved
    mock.writeToScreen("\033]9;4;4\033\\");    // paused, still preserved
    mock.writeToScreen("\033]9;4;2;95\033\\"); // failed, at a stated position
    mock.writeToScreen("\033]9;4;0\033\\");    // withdrawn

    REQUIRE(mock.progressNotifications.size() == 5);
    CHECK(mock.progressNotifications[0] == Progress { .state = ProgressState::Normal, .percentage = 10 });
    CHECK(mock.progressNotifications[1]
          == Progress { .state = ProgressState::Indeterminate, .percentage = 10 });
    CHECK(mock.progressNotifications[2] == Progress { .state = ProgressState::Paused, .percentage = 10 });
    CHECK(mock.progressNotifications[3] == Progress { .state = ProgressState::Error, .percentage = 95 });
    CHECK(mock.progressNotifications[4] == Progress {});
    CHECK(mock.terminal.progress() == Progress {});
}

TEST_CASE("ProgressState.status_line_placeholder_renders_each_state", "[progress]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) } };
    auto const segment = parseStatusLineSegment("{Progress}"sv);
    REQUIRE(segment.size() == 1);
    REQUIRE(std::holds_alternative<StatusLineDefinitions::Progress>(segment[0]));

    auto const rendered = [&](std::string_view sequence) {
        mock.writeToScreen(sequence);
        return serializeToVT(mock.terminal, segment, StatusLineStyling::Disabled);
    };

    // Inactive collapses to nothing, so the default status line stays clean until an app says otherwise.
    CHECK(rendered("\033]9;4;0\033\\").empty());
    CHECK(rendered("\033]9;4;1;45\033\\") == "45%");
    CHECK(rendered("\033]9;4;3\033\\") == "BUSY");
    CHECK(rendered("\033]9;4;4\033\\") == "PAUSED 45%");
    CHECK(rendered("\033]9;4;2;99\033\\") == "ERROR 99%");
}

TEST_CASE("ProgressState.hard_reset_withdraws_the_indicator", "[progress]")
{
    // A program that dies mid-operation must not leave a half-filled bar behind.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };
    mock.writeToScreen("\033]9;4;1;50\033\\");
    REQUIRE(mock.terminal.progress().percentage == 50);

    mock.writeToScreen("\033c"); // RIS
    CHECK(mock.terminal.progress() == Progress {});
    CHECK(mock.progressNotifications.back() == Progress {});
}

// }}}
