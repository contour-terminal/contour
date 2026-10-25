// SPDX-License-Identifier: Apache-2.0
#include <contour/window/TabAttention.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>

#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

using namespace contour::window;
using contour::platform::SessionVisibility;
using vtbackend::CommandEnding;
using vtworkspace::SessionId;

namespace
{
constexpr auto Shell = SessionId { 1 };
constexpr auto Editor = SessionId { 2 };
} // namespace

TEST_CASE("tabAttention.offScreenLiveOutputMarksOnce", "[tabattention]")
{
    auto ledger = TabAttention {};
    CHECK(ledger.noteOutput(Shell, SessionVisibility::Hidden, OutputOrigin::Live)
          == AttentionChange::Changed);
    CHECK(ledger.of(Shell) == SessionAttention { .activity = TabActivity::UnseenOutput });
    CHECK(ledger.noteOutput(Shell, SessionVisibility::Hidden, OutputOrigin::Live)
          == AttentionChange::Unchanged);
    CHECK(ledger.of(Editor) == SessionAttention {});
}

TEST_CASE("tabAttention.onScreenOutputMarksNothing", "[tabattention]")
{
    auto ledger = TabAttention {};
    for (auto const visibility: { SessionVisibility::Focused, SessionVisibility::VisibleUnfocused })
    {
        CHECK(ledger.noteOutput(Shell, visibility, OutputOrigin::Live) == AttentionChange::Unchanged);
        CHECK(ledger.of(Shell) == SessionAttention {});
    }
}

TEST_CASE("tabAttention.resyncMarksNothing", "[tabattention]")
{
    auto ledger = TabAttention {};
    CHECK(ledger.noteOutput(Shell, SessionVisibility::Hidden, OutputOrigin::Resync)
          == AttentionChange::Unchanged);
    CHECK(ledger.of(Shell) == SessionAttention {});
}

TEST_CASE("tabAttention.finishedKeepsTheStrongerBadge", "[tabattention]")
{
    auto ledger = TabAttention {};
    CHECK(ledger.noteFinished(Shell, SessionVisibility::Hidden, CommandEnding::Failed)
          == AttentionChange::Changed);
    CHECK(ledger.noteFinished(Shell, SessionVisibility::Hidden, CommandEnding::Succeeded)
          == AttentionChange::Unchanged);
    CHECK(ledger.of(Shell).finished == FinishedBlockBadge::Failed);

    // A command whose ending nobody reported says nothing.
    CHECK(ledger.noteFinished(Editor, SessionVisibility::Hidden, CommandEnding::Unknown)
          == AttentionChange::Unchanged);
    CHECK(ledger.noteFinished(Editor, SessionVisibility::Hidden, CommandEnding::Succeeded)
          == AttentionChange::Changed);
    CHECK(ledger.noteFinished(Editor, SessionVisibility::Hidden, CommandEnding::Interrupted)
          == AttentionChange::Changed);
    CHECK(ledger.of(Editor).finished == FinishedBlockBadge::Failed);
}

TEST_CASE("tabAttention.finishedOnScreenMarksNothing", "[tabattention]")
{
    auto ledger = TabAttention {};
    CHECK(ledger.noteFinished(Shell, SessionVisibility::VisibleUnfocused, CommandEnding::Failed)
          == AttentionChange::Unchanged);
    CHECK(ledger.of(Shell) == SessionAttention {});
}

TEST_CASE("tabAttention.seenClearsBoth", "[tabattention]")
{
    auto ledger = TabAttention {};
    std::ignore = ledger.noteOutput(Shell, SessionVisibility::Hidden, OutputOrigin::Live);
    std::ignore = ledger.noteFinished(Shell, SessionVisibility::Hidden, CommandEnding::Failed);
    CHECK(ledger.noteSeen(Shell) == AttentionChange::Changed);
    CHECK(ledger.of(Shell) == SessionAttention {});
    CHECK(ledger.noteSeen(Shell) == AttentionChange::Unchanged);
}

TEST_CASE("tabAttention.forgetDropsTheEntry", "[tabattention]")
{
    auto ledger = TabAttention {};
    std::ignore = ledger.noteOutput(Shell, SessionVisibility::Hidden, OutputOrigin::Live);
    ledger.forget(Shell);
    CHECK(ledger.of(Shell) == SessionAttention {});
    CHECK(ledger.noteSeen(Shell) == AttentionChange::Unchanged);
}

TEST_CASE("tabAttention.pooledOverEveryLeaf", "[tabattention]")
{
    auto tab = vtworkspace::Tab { vtworkspace::TabId { 1 }, vtworkspace::PaneId { 1 }, Shell };
    std::ignore = tab.rootPane()->split(
        vtworkspace::SplitState::Vertical, vtworkspace::PaneId { 2 }, vtworkspace::PaneId { 3 }, Editor);

    auto ledger = TabAttention {};
    CHECK(pooledAttention(tab, ledger) == SessionAttention {});

    std::ignore = ledger.noteOutput(Editor, SessionVisibility::Hidden, OutputOrigin::Live);
    CHECK(pooledAttention(tab, ledger).activity == TabActivity::UnseenOutput);

    std::ignore = ledger.noteFinished(Shell, SessionVisibility::Hidden, CommandEnding::Failed);
    std::ignore = ledger.noteFinished(Editor, SessionVisibility::Hidden, CommandEnding::Succeeded);
    CHECK(pooledAttention(tab, ledger)
          == SessionAttention { .activity = TabActivity::UnseenOutput,
                                .finished = FinishedBlockBadge::Failed });

    std::ignore = ledger.noteSeen(Editor);
    CHECK(pooledAttention(tab, ledger) == SessionAttention { .finished = FinishedBlockBadge::Failed });
}

TEST_CASE("tabAttention.outputOrigin", "[tabattention]")
{
    using vthost::client::ConnectionState;
    auto const now = std::chrono::steady_clock::time_point { std::chrono::seconds { 100 } };

    SECTION("a connected host with no grace is live")
    {
        CHECK(outputOriginOf(ConnectionState::Connected, now, std::nullopt) == OutputOrigin::Live);
    }

    SECTION("every other connection state is a resync")
    {
        for (auto const state: { ConnectionState::Connecting,
                                 ConnectionState::Reconnecting,
                                 ConnectionState::Failed,
                                 ConnectionState::Refused,
                                 ConnectionState::Stopped })
            CHECK(outputOriginOf(state, now, std::nullopt) == OutputOrigin::Resync);
    }

    SECTION("the grace after a return to Connected is a resync, and ends")
    {
        auto const graceEnd = now + ResyncGracePeriod;
        CHECK(outputOriginOf(ConnectionState::Connected, now, graceEnd) == OutputOrigin::Resync);
        CHECK(outputOriginOf(ConnectionState::Connected, graceEnd - std::chrono::milliseconds { 1 }, graceEnd)
              == OutputOrigin::Resync);
        CHECK(outputOriginOf(ConnectionState::Connected, graceEnd, graceEnd) == OutputOrigin::Live);
    }

    SECTION("the grace is armed on a return to Connected from any other state only")
    {
        CHECK(resyncGraceAfter(ConnectionState::Reconnecting, ConnectionState::Connected, now)
              == now + ResyncGracePeriod);
        CHECK(resyncGraceAfter(ConnectionState::Connecting, ConnectionState::Connected, now)
              == now + ResyncGracePeriod);
        CHECK(resyncGraceAfter(ConnectionState::Connected, ConnectionState::Connected, now) == std::nullopt);
        CHECK(resyncGraceAfter(ConnectionState::Connected, ConnectionState::Reconnecting, now)
              == std::nullopt);
    }
}
