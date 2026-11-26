// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>

#include <vthost/AttentionLedger.hpp>

using vthost::AttentionChange;
using vthost::AttentionLedger;
using vthost::attentionStateOf;
using vthost::PaneAttention;
using vthost::SeenSerials;
using vthost::proto::FinishedCommand;
using vthost::proto::OutputActivity;
using vthost::proto::PaneBell;

namespace
{

constexpr auto Pane = uint64_t { 7 };
constexpr auto OtherPane = uint64_t { 9 };

/// A fixed instant: the ledger reads no clock, so a test states the time it passes.
[[nodiscard]] std::chrono::system_clock::time_point someTime()
{
    return std::chrono::system_clock::time_point { std::chrono::milliseconds { 1'790'000'000'000 } };
}

} // namespace

TEST_CASE("AttentionLedger.output_marks_once", "[vthost][attention-ledger]")
{
    auto ledger = AttentionLedger {};
    CHECK(ledger.of(Pane) == nullptr);
    CHECK(ledger.noteOutput(Pane) == AttentionChange::Changed);
    REQUIRE(ledger.of(Pane) != nullptr);
    CHECK(ledger.of(Pane)->activity == OutputActivity::Output);
    CHECK(ledger.of(Pane)->activitySerial == 1);
    // Further output while the pane is still flagged raises nothing and takes no serial.
    CHECK(ledger.noteOutput(Pane) == AttentionChange::Unchanged);
    CHECK(ledger.of(Pane)->activitySerial == 1);
    CHECK(ledger.lastSerial() == 1);
}

TEST_CASE("AttentionLedger.serials_rise_across_kinds", "[vthost][attention-ledger]")
{
    auto ledger = AttentionLedger {};
    CHECK(ledger.lastSerial() == 0);
    std::ignore = ledger.noteOutput(Pane);
    std::ignore = ledger.noteBell(OtherPane);
    std::ignore = ledger.noteFinished(Pane, FinishedCommand { .exitCode = 0 }, someTime());
    std::ignore = ledger.noteBell(Pane);
    CHECK(ledger.of(Pane)->activitySerial == 1);
    CHECK(ledger.of(OtherPane)->bellSerial == 2);
    CHECK(ledger.of(Pane)->finishedSerial == 3);
    CHECK(ledger.of(Pane)->bellSerial == 4);
    CHECK(ledger.lastSerial() == 4);
}

TEST_CASE("AttentionLedger.mark_seen_clears_covered_flags_only", "[vthost][attention-ledger]")
{
    auto ledger = AttentionLedger {};
    std::ignore = ledger.noteOutput(Pane);                                                  // 1
    std::ignore = ledger.noteBell(Pane);                                                    // 2
    std::ignore = ledger.noteFinished(Pane, FinishedCommand { .exitCode = 1 }, someTime()); // 3
    // The window saw the output and the bell, but not the finish yet.
    CHECK(ledger.markSeen(Pane, SeenSerials { .activity = 1, .bell = 2, .finished = 0 })
          == AttentionChange::Changed);
    auto const& pane = *ledger.of(Pane);
    CHECK(pane.activity == OutputActivity::None);
    CHECK(pane.bell == PaneBell::Silent);
    REQUIRE(pane.finished.has_value());
    CHECK(pane.finished->exitCode == 1);
    CHECK(pane.finishedAt == someTime());
    CHECK(ledger.markSeen(Pane, SeenSerials { .finished = 3 }) == AttentionChange::Changed);
    CHECK_FALSE(ledger.of(Pane)->finished.has_value());
    CHECK_FALSE(ledger.of(Pane)->finishedAt.has_value());
}

TEST_CASE("AttentionLedger.newer_bell_after_seen_rings_again", "[vthost][attention-ledger]")
{
    auto ledger = AttentionLedger {};
    std::ignore = ledger.noteBell(Pane); // 1
    std::ignore = ledger.noteBell(Pane); // 2, while the window still looks at 1
    CHECK(ledger.markSeen(Pane, SeenSerials { .bell = 1 }) == AttentionChange::Unchanged);
    CHECK(ledger.of(Pane)->bell == PaneBell::Rang);
    CHECK(ledger.markSeen(Pane, SeenSerials { .bell = 2 }) == AttentionChange::Changed);
    CHECK(ledger.of(Pane)->bell == PaneBell::Silent);
    CHECK(ledger.noteBell(Pane) == AttentionChange::Changed);
    CHECK(ledger.of(Pane)->bell == PaneBell::Rang);
    CHECK(ledger.of(Pane)->bellSerial == 3);
}

TEST_CASE("AttentionLedger.stale_mark_seen_is_unchanged", "[vthost][attention-ledger]")
{
    auto ledger = AttentionLedger {};
    CHECK(ledger.markSeen(Pane, SeenSerials { .activity = 5, .bell = 5 }) == AttentionChange::Unchanged);
    CHECK(ledger.of(Pane) == nullptr);
    std::ignore = ledger.noteOutput(Pane); // 1
    std::ignore = ledger.noteBell(Pane);   // 2
    auto const before = *ledger.of(Pane);
    // An acknowledgement older than every flag clears none of them.
    CHECK(ledger.markSeen(Pane, SeenSerials {}) == AttentionChange::Unchanged);
    CHECK(*ledger.of(Pane) == before);
}

TEST_CASE("AttentionLedger.mark_seen_raises_status_generation_never_lowers_it", "[vthost][attention-ledger]")
{
    auto ledger = AttentionLedger {};
    std::ignore = ledger.noteOutput(Pane);
    CHECK(ledger.markSeen(Pane, SeenSerials { .statusGeneration = 4 }) == AttentionChange::Changed);
    CHECK(ledger.of(Pane)->seenStatusGeneration == 4);
    CHECK(ledger.markSeen(Pane, SeenSerials { .statusGeneration = 2 }) == AttentionChange::Unchanged);
    CHECK(ledger.of(Pane)->seenStatusGeneration == 4);
    CHECK(ledger.markSeen(Pane, SeenSerials { .statusGeneration = 4 }) == AttentionChange::Unchanged);
    // The output flag was not covered by any of them.
    CHECK(ledger.of(Pane)->activity == OutputActivity::Output);
}

TEST_CASE("AttentionLedger.finished_keeps_time", "[vthost][attention-ledger]")
{
    auto ledger = AttentionLedger {};
    auto const command = FinishedCommand { .exitCode = 2, .durationMs = 1500, .commandLine = "make" };
    CHECK(ledger.noteFinished(Pane, command, someTime()) == AttentionChange::Changed);
    CHECK(ledger.of(Pane)->finished == command);
    CHECK(ledger.of(Pane)->finishedAt == someTime());
    // A later finish replaces the earlier unseen one, time and all.
    auto const later = someTime() + std::chrono::seconds { 3 };
    std::ignore = ledger.noteFinished(Pane, FinishedCommand { .exitCode = 0 }, later);
    CHECK(ledger.of(Pane)->finished == FinishedCommand { .exitCode = 0 });
    CHECK(ledger.of(Pane)->finishedAt == later);
}

TEST_CASE("AttentionLedger.restored_continues_serials", "[vthost][attention-ledger]")
{
    auto const panes = std::array {
        std::pair { Pane, PaneAttention { .bell = PaneBell::Rang, .bellSerial = 11 } },
        std::pair { OtherPane, PaneAttention { .activity = OutputActivity::Output, .activitySerial = 4 } },
    };
    auto ledger = AttentionLedger::restored(12, panes);
    CHECK(ledger.lastSerial() == 12);
    CHECK(ledger.entries() == std::vector(panes.begin(), panes.end()));
    std::ignore = ledger.noteBell(Pane);
    CHECK(ledger.of(Pane)->bellSerial == 13);

    // An image whose last serial falls behind a pane's never hands that serial out again.
    auto behind = AttentionLedger::restored(2, panes);
    CHECK(behind.lastSerial() == 11);
    std::ignore = behind.noteFinished(OtherPane, FinishedCommand {}, someTime());
    CHECK(behind.of(OtherPane)->finishedSerial == 12);
}

TEST_CASE("AttentionLedger.forget_drops_pane", "[vthost][attention-ledger]")
{
    auto ledger = AttentionLedger {};
    std::ignore = ledger.noteBell(Pane);
    std::ignore = ledger.noteOutput(OtherPane);
    ledger.forget(Pane);
    CHECK(ledger.of(Pane) == nullptr);
    REQUIRE(ledger.entries().size() == 1);
    CHECK(ledger.entries().front().first == OtherPane);
    // The serial counter is not rewound: the next raise still follows every earlier one.
    CHECK(ledger.lastSerial() == 2);
    ledger.forget(Pane);
    CHECK(ledger.entries().size() == 1);
}

TEST_CASE("AttentionLedger.tracked_pane_takes_its_status_seen", "[vthost][attention-ledger]")
{
    auto ledger = AttentionLedger {};
    ledger.track(Pane);
    REQUIRE(ledger.of(Pane) != nullptr);
    CHECK(*ledger.of(Pane) == PaneAttention {});
    CHECK(ledger.lastSerial() == 0);
    // A status seen of a pane that raised nothing is kept, for every window to read.
    CHECK(ledger.markSeen(Pane, SeenSerials { .statusGeneration = 3 }) == AttentionChange::Changed);
    CHECK(ledger.of(Pane)->seenStatusGeneration == 3);
    // Tracking again leaves what the entry holds.
    ledger.track(Pane);
    CHECK(ledger.of(Pane)->seenStatusGeneration == 3);
}

TEST_CASE("AttentionLedger.state_on_the_wire", "[vthost][attention-ledger]")
{
    auto ledger = AttentionLedger {};
    std::ignore = ledger.noteOutput(Pane);
    std::ignore = ledger.noteFinished(Pane, FinishedCommand { .exitCode = 2 }, someTime());
    auto const state = attentionStateOf(Pane, *ledger.of(Pane));
    CHECK(state.session == Pane);
    CHECK(state.activity == OutputActivity::Output);
    CHECK(state.activitySerial == 1);
    CHECK(state.finishedSerial == 2);
    REQUIRE(state.finished.has_value());
    CHECK(state.finished->exitCode == std::optional<int32_t> { 2 });
    auto const expectedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(someTime().time_since_epoch()).count();
    CHECK(state.finishedAtUnixMs == std::optional<int64_t> { expectedMs });
    CHECK_FALSE(attentionStateOf(Pane, PaneAttention {}).finishedAtUnixMs.has_value());
}
