// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SidebarJumps.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using contour::window::HistoryCause;
using contour::window::JumpCancel;
using contour::window::JumpEvent;
using contour::window::JumpMode;
using contour::window::JumpRefusal;
using contour::window::PendingJumps;
using contour::window::SidebarJumps;
using contour::window::SidebarTarget;
using vthost::proto::SwitchOutcome;

namespace
{

/// The window the jumps are made in.
constexpr auto Window = vtworkspace::WorkspaceId { 3 };

/// A control id the directory states for the row's workspace.
constexpr auto ControlId = std::array<uint8_t, 16> { 7, 7, 7 };

/// A switching service that keeps every request for the test to answer, and records the cancels.
class HeldSwitching final: public contour::session::WindowSwitching
{
  public:
    struct Asked
    {
        vtworkspace::WorkspaceId window;
        contour::session::SwitchTarget target;
        contour::session::SwitchMode mode;
        contour::session::SwitchCompletion done;
    };

    void request(vtworkspace::WorkspaceId window,
                 contour::session::SwitchTarget target,
                 contour::session::SwitchMode mode,
                 contour::session::SwitchWaiter /*waiter*/,
                 contour::session::SwitchCompletion done) override
    {
        // As the switcher does for the user's newer request: the one under way of its mode ends first, told
        // so; the other mode's goes on.
        auto const sameMode = std::ranges::find_if(
            asked | std::views::reverse, [mode](Asked const& earlier) { return earlier.mode == mode; });
        if (sameMode != (asked | std::views::reverse).end() && sameMode->done)
            std::exchange(sameMode->done, {})(std::unexpected(std::string { "superseded" }));
        asked.push_back(
            Asked { .window = window, .target = std::move(target), .mode = mode, .done = std::move(done) });
    }

    /// A creation, kept as a request of its own: told as the switch to the made workspace would be.
    void createWorkspace(vtworkspace::WorkspaceId window,
                         contour::session::SwitchTarget const& daemon,
                         std::string label,
                         contour::session::SwitchMode mode,
                         contour::session::SwitchCompletion done) override
    {
        created.push_back(
            Created { .window = window, .daemon = daemon, .label = std::move(label), .mode = mode });
        request(window, daemon, mode, contour::session::UserSwitch, std::move(done));
    }

    void cancel(vtworkspace::WorkspaceId window, contour::session::SwitchMode mode) override
    {
        cancels.push_back(window);
        cancelledModes.push_back(mode);
    }

    /// Tells the newest request @p outcome.
    void answer(std::expected<SwitchOutcome, std::string> outcome)
    {
        REQUIRE_FALSE(asked.empty());
        answerAt(asked.size() - 1, std::move(outcome));
    }

    /// Tells request @p index (in the order asked) @p outcome.
    void answerAt(std::size_t index, std::expected<SwitchOutcome, std::string> outcome)
    {
        REQUIRE(index < asked.size());
        REQUIRE(asked.at(index).done);
        std::exchange(asked.at(index).done, {})(std::move(outcome));
    }

    /// One createWorkspace() call.
    struct Created
    {
        vtworkspace::WorkspaceId window;
        contour::session::SwitchTarget daemon;
        std::string label;
        contour::session::SwitchMode mode;
    };

    std::vector<Asked> asked;
    std::vector<Created> created; ///< Every createWorkspace(), in order.
    std::vector<vtworkspace::WorkspaceId> cancels;
    std::vector<contour::session::SwitchMode> cancelledModes; ///< The mode each cancel named, in order.
};

/// One thing a jump said.
struct Said
{
    JumpEvent event;
    SidebarTarget target;
    std::string reason;
};

/// One landing a jump told.
struct Landed
{
    std::optional<SidebarTarget> from;
    SidebarTarget to;
    HistoryCause cause;
};

/// A sidebar's jumps over a held switching service, with what they said and how often they changed.
struct JumpRig
{
    HeldSwitching switching;
    std::vector<Said> said;
    int changes = 0;
    std::vector<Landed> landed;           ///< Every landing told, in order.
    std::vector<SidebarTarget> unreached; ///< Every jump back's place that got nowhere, in order.
    SidebarJumps jumps { switching,
                         [this](JumpEvent event, SidebarTarget const& target, std::string const& reason) {
                             said.push_back(Said { .event = event, .target = target, .reason = reason });
                         },
                         [this] { ++changes; },
                         [this](
                             std::optional<SidebarTarget> from, SidebarTarget const& to, HistoryCause cause) {
                             landed.push_back(Landed { .from = std::move(from), .to = to, .cause = cause });
                         },
                         [this](SidebarTarget const& place) { unreached.push_back(place); } };
};

/// starfive's ci workspace, its build tab and pane, in starfive's run 9.
[[nodiscard]] SidebarTarget ciOnStarfive()
{
    return SidebarTarget { .daemon = contour::window::hostDaemonRef("starfive"),
                           .instance = contour::window::DaemonInstance { 9 },
                           .workspace = 5,
                           .tab = 50,
                           .pane = contour::window::PaneKey { 51 } };
}

} // namespace

TEST_CASE("a jump's outcome is said as the table says", "[sidebar][jump]")
{
    using Outcome = std::expected<SwitchOutcome, std::string>;
    CHECK(jumpEventOf(Outcome { SwitchOutcome::Switched }, JumpCancel::None) == JumpEvent::Landed);
    CHECK(jumpEventOf(Outcome { SwitchOutcome::FocusedExisting }, JumpCancel::None) == JumpEvent::Landed);
    CHECK(jumpEventOf(Outcome { SwitchOutcome::PickerShown }, JumpCancel::None) == std::nullopt);
    CHECK(jumpEventOf(Outcome { std::unexpected(std::string { "x" }) }, JumpCancel::None)
          == JumpEvent::Failed);
    CHECK(jumpEventOf(Outcome { std::unexpected(std::string { "x" }) }, JumpCancel::Requested)
          == JumpEvent::Cancelled);
}

TEST_CASE("a pending jump marks its target until it lands, and says Connecting then Landed once",
          "[sidebar][jump]")
{
    auto rig = JumpRig {};
    rig.jumps.jumpTo(Window, ciOnStarfive(), JumpMode::Here, ControlId, std::nullopt, HistoryCause::Jump);

    REQUIRE(rig.switching.asked.size() == 1);
    CHECK(rig.jumps.pending().here == ciOnStarfive());
    REQUIRE(rig.said.size() == 1);
    CHECK(rig.said[0].event == JumpEvent::Connecting);
    CHECK(rig.changes == 1);

    rig.switching.answer(SwitchOutcome::Switched);

    CHECK(rig.jumps.pending() == PendingJumps {});
    REQUIRE(rig.said.size() == 2);
    CHECK(rig.said[1].event == JumpEvent::Landed);
    CHECK(rig.said[1].target == ciOnStarfive());
    CHECK(rig.changes == 2);
}

TEST_CASE("a sidebar jump's switch carries the row's tab, pane and run", "[sidebar][jump][switch]")
{
    auto rig = JumpRig {};
    rig.jumps.jumpTo(Window, ciOnStarfive(), JumpMode::Here, ControlId, std::nullopt, HistoryCause::Jump);

    REQUIRE(rig.switching.asked.size() == 1);
    auto const& asked = rig.switching.asked.front();
    CHECK(asked.window == Window);
    CHECK(asked.mode == contour::session::SwitchMode::Switch);
    CHECK(asked.target.daemon == "starfive");
    CHECK(asked.target.instance == contour::window::DaemonInstance { 9 });
    CHECK(asked.target.focus.tab == std::optional<uint64_t> { 50 });
    CHECK(asked.target.focus.pane == contour::window::PaneKey { 51 });
    auto const* const workspace = std::get_if<vtcontrol::ObjectRef>(&asked.target.workspace);
    REQUIRE(workspace != nullptr);
    CHECK(workspace->uuid.bytes == ControlId);
}

TEST_CASE("a jump to a restarted daemon's old workspace switches nothing", "[sidebar][jump]")
{
    // Review Focus 4: a restarted daemon may reuse the workspace id for another workspace; the row names
    // nothing any more, and that is said.
    auto rig = JumpRig {};
    rig.jumps.jumpTo(Window,
                     ciOnStarfive(),
                     JumpMode::Here,
                     std::unexpected(JumpRefusal::StaleRun),
                     std::nullopt,
                     HistoryCause::Jump);

    CHECK(rig.switching.asked.empty());
    CHECK(rig.jumps.pending() == PendingJumps {});
    REQUIRE(rig.said.size() == 1);
    CHECK(rig.said[0].event == JumpEvent::Failed);
    CHECK(rig.said[0].reason == "the daemon restarted");
}

TEST_CASE("Cancel ends the pending jump through the switching service, and says so once", "[sidebar][jump]")
{
    auto rig = JumpRig {};
    rig.jumps.jumpTo(Window, ciOnStarfive(), JumpMode::Here, ControlId, std::nullopt, HistoryCause::Jump);

    rig.jumps.cancel(Window, JumpMode::Here);

    REQUIRE(rig.switching.cancels.size() == 1);
    CHECK(rig.switching.cancels.front() == Window);
    CHECK(rig.switching.cancelledModes.front() == contour::session::SwitchMode::Switch);
    // The service ends the switch as cancelled.
    rig.switching.answer(std::unexpected(std::string { "the user cancelled it" }));
    CHECK(rig.jumps.pending() == PendingJumps {});
    REQUIRE(rig.said.size() == 2);
    CHECK(rig.said[1].event == JumpEvent::Cancelled);
}

TEST_CASE("Cancel of an Open in New Window cancels that Open, not a switch in place", "[sidebar][jump]")
{
    auto rig = JumpRig {};
    rig.jumps.jumpTo(
        Window, ciOnStarfive(), JumpMode::NewWindow, ControlId, std::nullopt, HistoryCause::Jump);

    rig.jumps.cancel(Window, JumpMode::NewWindow);

    REQUIRE(rig.switching.cancelledModes.size() == 1);
    CHECK(rig.switching.cancelledModes.front() == contour::session::SwitchMode::Open);
}

TEST_CASE("Cancel with no jump under way asks nothing", "[sidebar][jump]")
{
    auto rig = JumpRig {};
    rig.jumps.cancel(Window, JumpMode::Here);
    CHECK(rig.switching.cancels.empty());
    CHECK(rig.said.empty());
}

TEST_CASE("a failed jump says why", "[sidebar][jump]")
{
    auto rig = JumpRig {};
    rig.jumps.jumpTo(Window, ciOnStarfive(), JumpMode::Here, ControlId, std::nullopt, HistoryCause::Jump);
    rig.switching.answer(std::unexpected(std::string { "cannot connect to the daemon" }));

    REQUIRE(rig.said.size() == 2);
    CHECK(rig.said[1].event == JumpEvent::Failed);
    CHECK(rig.said[1].reason == "cannot connect to the daemon");
}

TEST_CASE("a superseded jump says nothing", "[sidebar][jump]")
{
    // A second click while the first jump connects: the first ends for it, silently; the second speaks.
    auto rig = JumpRig {};
    rig.jumps.jumpTo(Window, ciOnStarfive(), JumpMode::Here, ControlId, std::nullopt, HistoryCause::Jump);
    auto second = ciOnStarfive();
    second.workspace = 6;
    rig.jumps.jumpTo(Window, second, JumpMode::Here, ControlId, std::nullopt, HistoryCause::Jump);

    REQUIRE(rig.said.size() == 2);
    CHECK(rig.said[0].event == JumpEvent::Connecting);
    CHECK(rig.said[1].event == JumpEvent::Connecting);
    CHECK(rig.jumps.pending().here == second);

    rig.switching.answer(SwitchOutcome::Switched);
    REQUIRE(rig.said.size() == 3);
    CHECK(rig.said[2].event == JumpEvent::Landed);
    CHECK(rig.said[2].target == second);
}

TEST_CASE("a jump's then runs once it landed here", "[sidebar][jump]")
{
    auto rig = JumpRig {};
    auto told = std::vector<SwitchOutcome> {};
    rig.jumps.jumpTo(Window,
                     ciOnStarfive(),
                     JumpMode::Here,
                     ControlId,
                     std::nullopt,
                     HistoryCause::Jump,
                     [&](SwitchOutcome outcome) {
                         // Run once the jump is over: no jump is pending any more.
                         CHECK(rig.jumps.pending() == PendingJumps {});
                         told.push_back(outcome);
                     });
    CHECK(told.empty());
    rig.switching.answer(SwitchOutcome::Switched);
    CHECK(told == std::vector { SwitchOutcome::Switched });
}

TEST_CASE("a jump's then is told when another window shows the target", "[sidebar][jump]")
{
    auto rig = JumpRig {};
    auto told = std::vector<SwitchOutcome> {};
    rig.jumps.jumpTo(Window,
                     ciOnStarfive(),
                     JumpMode::Here,
                     ControlId,
                     std::nullopt,
                     HistoryCause::Jump,
                     [&](SwitchOutcome outcome) { told.push_back(outcome); });
    rig.switching.answer(SwitchOutcome::FocusedExisting);
    CHECK(told == std::vector { SwitchOutcome::FocusedExisting });
}

TEST_CASE("a superseded or cancelled jump runs no then", "[sidebar][jump]")
{
    auto rig = JumpRig {};
    auto runs = 0;
    auto const then = [&](SwitchOutcome) {
        ++runs;
    };
    SECTION("superseded")
    {
        rig.jumps.jumpTo(
            Window, ciOnStarfive(), JumpMode::Here, ControlId, std::nullopt, HistoryCause::Jump, then);
        auto other = ciOnStarfive();
        other.workspace = 6;
        rig.jumps.jumpTo(Window, other, JumpMode::Here, ControlId, std::nullopt, HistoryCause::Jump);
        // The first was told it ended as the second started; the second lands.
        rig.switching.answer(SwitchOutcome::Switched);
    }
    SECTION("cancelled")
    {
        rig.jumps.jumpTo(
            Window, ciOnStarfive(), JumpMode::Here, ControlId, std::nullopt, HistoryCause::Jump, then);
        rig.jumps.cancel(Window, JumpMode::Here);
        rig.switching.answer(std::unexpected(std::string { "the user cancelled it" }));
    }
    SECTION("failed")
    {
        rig.jumps.jumpTo(
            Window, ciOnStarfive(), JumpMode::Here, ControlId, std::nullopt, HistoryCause::Jump, then);
        rig.switching.answer(std::unexpected(std::string { "no route" }));
    }
    SECTION("only the picker shown")
    {
        rig.jumps.jumpTo(
            Window, ciOnStarfive(), JumpMode::Here, ControlId, std::nullopt, HistoryCause::Jump, then);
        rig.switching.answer(SwitchOutcome::PickerShown);
    }
    CHECK(runs == 0);
}

TEST_CASE("New Workspace on another daemon creates there and is the pending jump", "[sidebar][jump]")
{
    auto rig = JumpRig {};
    auto const daemonRow = SidebarTarget { .daemon = contour::window::hostDaemonRef("starfive"),
                                           .instance = contour::window::DaemonInstance { 9 },
                                           .workspace = 0,
                                           .tab = std::nullopt,
                                           .pane = std::nullopt };
    rig.jumps.createAndJump(Window, daemonRow, std::nullopt);

    REQUIRE(rig.switching.created.size() == 1);
    auto const& created = rig.switching.created.front();
    CHECK(created.window == Window);
    CHECK(created.daemon.kind == vthost::proto::SwitchDaemonKind::Host);
    CHECK(created.daemon.daemon == "starfive");
    CHECK(created.label.empty());
    CHECK(created.mode == contour::session::SwitchMode::Switch);
    CHECK(rig.jumps.pending().here == daemonRow);
    REQUIRE(rig.said.size() == 1);
    CHECK(rig.said[0].event == JumpEvent::Connecting);

    rig.switching.answer(SwitchOutcome::Switched);
    CHECK(rig.jumps.pending() == PendingJumps {});
    CHECK(rig.said.back().event == JumpEvent::Landed);
}

TEST_CASE("a jump in place and an Open in New Window are pending side by side, and either may end first",
          "[sidebar][jump]")
{
    auto build = ciOnStarfive();
    build.workspace = 6;
    for (auto const openFirst: { JumpMode::NewWindow, JumpMode::Here })
    {
        INFO("first " << static_cast<int>(openFirst));
        auto rig = JumpRig {};
        auto const second = openFirst == JumpMode::NewWindow ? JumpMode::Here : JumpMode::NewWindow;
        rig.jumps.jumpTo(Window, ciOnStarfive(), openFirst, ControlId, std::nullopt, HistoryCause::Jump);
        rig.jumps.jumpTo(Window, build, second, ControlId, std::nullopt, HistoryCause::Jump);

        // Neither ended the other.
        auto const pending = rig.jumps.pending();
        CHECK(pending.of(openFirst) == ciOnStarfive());
        CHECK(pending.of(second) == build);
        REQUIRE(rig.said.size() == 2);

        // The older fails: said, and the newer goes on.
        rig.switching.answerAt(0, std::unexpected(std::string { "cannot connect to the daemon" }));
        REQUIRE(rig.said.size() == 3);
        CHECK(rig.said[2].event == JumpEvent::Failed);
        CHECK(rig.said[2].target == ciOnStarfive());
        CHECK_FALSE(rig.jumps.pending().of(openFirst).has_value());
        CHECK(rig.jumps.pending().of(second) == build);

        rig.switching.answerAt(1, SwitchOutcome::Switched);
        REQUIRE(rig.said.size() == 4);
        CHECK(rig.said[3].event == JumpEvent::Landed);
        CHECK(rig.said[3].target == build);
        CHECK(rig.jumps.pending() == PendingJumps {});
    }
}

TEST_CASE("Escape's cancel ends the jump in place and leaves an Open in New Window", "[sidebar][jump]")
{
    auto rig = JumpRig {};
    rig.jumps.jumpTo(
        Window, ciOnStarfive(), JumpMode::NewWindow, ControlId, std::nullopt, HistoryCause::Jump);

    // No jump in place: nothing to end.
    rig.jumps.cancel(Window, JumpMode::Here);
    CHECK(rig.switching.cancels.empty());

    auto build = ciOnStarfive();
    build.workspace = 6;
    rig.jumps.jumpTo(Window, build, JumpMode::Here, ControlId, std::nullopt, HistoryCause::Jump);
    rig.jumps.cancel(Window, JumpMode::Here);

    REQUIRE(rig.switching.cancelledModes.size() == 1);
    CHECK(rig.switching.cancelledModes.front() == contour::session::SwitchMode::Switch);
    rig.switching.answerAt(1, std::unexpected(std::string { "the user cancelled it" }));
    CHECK(rig.said.back().event == JumpEvent::Cancelled);
    CHECK(rig.jumps.pending().newWindow == ciOnStarfive());
}

TEST_CASE("supersede ends the jump without a word", "[sidebar][jump]")
{
    auto rig = JumpRig {};
    auto runs = 0;
    rig.jumps.jumpTo(
        Window, ciOnStarfive(), JumpMode::NewWindow, ControlId, std::nullopt, HistoryCause::Jump);
    auto build = ciOnStarfive();
    build.workspace = 6;
    rig.jumps.jumpTo(
        Window, build, JumpMode::Here, ControlId, std::nullopt, HistoryCause::Jump, [&](SwitchOutcome) {
            ++runs;
        });
    REQUIRE(rig.said.size() == 2);
    auto const changes = rig.changes;

    rig.jumps.supersede(Window);

    // The switch in place is ended; the Open in New Window goes on.
    REQUIRE(rig.switching.cancels == std::vector { Window });
    CHECK(rig.switching.cancelledModes == std::vector { contour::session::SwitchMode::Switch });
    CHECK(rig.jumps.pending() == PendingJumps { .here = std::nullopt, .newWindow = ciOnStarfive() });
    CHECK(rig.changes == changes + 1);
    // The service ends the switch as cancelled, or it lands after all: neither says a word nor runs then.
    SECTION("cancelled")
    {
        rig.switching.answerAt(1, std::unexpected(std::string { "the user cancelled it" }));
    }
    SECTION("landed anyway")
    {
        rig.switching.answerAt(1, SwitchOutcome::Switched);
    }
    CHECK(rig.said.size() == 2);
    CHECK(runs == 0);

    // With none under way in place, nothing is asked.
    rig.jumps.supersede(Window);
    CHECK(rig.switching.cancels.size() == 1);
}

// {{{ The back stack's landings (JumpBack)
namespace
{
/// The window's own place it jumps from: its docs workspace's second tab.
[[nodiscard]] SidebarTarget docsHere()
{
    return SidebarTarget { .daemon = contour::window::localDaemonRef("contour"),
                           .instance = contour::window::DaemonInstance { 1 },
                           .workspace = 2,
                           .tab = 20,
                           .pane = std::nullopt };
}
} // namespace

TEST_CASE("a jump that landed here is recorded with its cause", "[sidebar][jump][history]")
{
    auto rig = JumpRig {};
    rig.jumps.jumpTo(Window, ciOnStarfive(), JumpMode::Here, ControlId, docsHere(), HistoryCause::Attention);
    CHECK(rig.landed.empty());

    rig.switching.answer(SwitchOutcome::Switched);

    REQUIRE(rig.landed.size() == 1);
    CHECK(rig.landed.front().from == docsHere());
    CHECK(rig.landed.front().to == ciOnStarfive());
    CHECK(rig.landed.front().cause == HistoryCause::Attention);
}

TEST_CASE("a jump that raised another window is not recorded", "[sidebar][jump][history]")
{
    auto rig = JumpRig {};
    rig.jumps.jumpTo(Window, ciOnStarfive(), JumpMode::Here, ControlId, docsHere(), HistoryCause::Jump);
    rig.switching.answer(SwitchOutcome::FocusedExisting);
    CHECK(rig.landed.empty());
    // Nor one that failed.
    rig.jumps.jumpTo(Window, ciOnStarfive(), JumpMode::Here, ControlId, docsHere(), HistoryCause::Jump);
    rig.switching.answer(std::unexpected(std::string { "unreachable" }));
    CHECK(rig.landed.empty());
}

TEST_CASE("Open in New Window records nothing", "[sidebar][jump][history]")
{
    auto rig = JumpRig {};
    rig.jumps.jumpTo(Window, ciOnStarfive(), JumpMode::NewWindow, ControlId, docsHere(), HistoryCause::Jump);
    // The new window is shown: Switched, as the switching service tells an Open that made its window.
    rig.switching.answer(SwitchOutcome::Switched);
    CHECK(rig.landed.empty());
}

// Phase 7 review M5: a jump back that got nowhere leaves its place on the stack.
TEST_CASE("a jump back that failed or was cancelled tells its place", "[sidebar][jump][history]")
{
    auto rig = JumpRig {};
    rig.jumps.jumpTo(Window, ciOnStarfive(), JumpMode::Here, ControlId, docsHere(), HistoryCause::Back);
    rig.switching.answer(std::unexpected(std::string { "unreachable" }));
    CHECK(rig.unreached == std::vector { ciOnStarfive() });

    rig.jumps.jumpTo(Window, ciOnStarfive(), JumpMode::Here, ControlId, docsHere(), HistoryCause::Back);
    rig.jumps.cancel(Window, JumpMode::Here);
    rig.switching.answer(std::unexpected(std::string { "cancelled" }));
    CHECK(rig.unreached.size() == 2);
}

TEST_CASE("only a jump back that got nowhere tells its place", "[sidebar][jump][history]")
{
    auto rig = JumpRig {};
    // Another cause's failure: no place was popped for it.
    rig.jumps.jumpTo(Window, ciOnStarfive(), JumpMode::Here, ControlId, docsHere(), HistoryCause::Jump);
    rig.switching.answer(std::unexpected(std::string { "unreachable" }));
    // A jump back that landed.
    rig.jumps.jumpTo(Window, ciOnStarfive(), JumpMode::Here, ControlId, docsHere(), HistoryCause::Back);
    rig.switching.answer(SwitchOutcome::Switched);
    // A jump back a newer navigation replaced: the newer one is where the user went.
    rig.jumps.jumpTo(Window, ciOnStarfive(), JumpMode::Here, ControlId, docsHere(), HistoryCause::Back);
    rig.jumps.supersede(Window);
    rig.switching.answer(std::unexpected(std::string { "superseded" }));
    CHECK(rig.unreached.empty());
}

// Phase 7 review M10: a switch the sidebar did not ask for replaces its jump, which then says nothing.
TEST_CASE("a released jump ends without a word", "[sidebar][jump]")
{
    auto rig = JumpRig {};
    rig.jumps.jumpTo(Window, ciOnStarfive(), JumpMode::Here, ControlId, docsHere(), HistoryCause::Jump);
    auto const changes = rig.changes;

    rig.jumps.release();

    CHECK_FALSE(rig.jumps.pending().here.has_value());
    CHECK(rig.changes == changes + 1);
    // Released, not cancelled: the newer request is what ends it.
    CHECK(rig.switching.cancels.empty());
    rig.switching.answer(std::unexpected(std::string { "superseded" }));
    REQUIRE(rig.said.size() == 1);
    CHECK(rig.said.front().event == JumpEvent::Connecting);
    // Nothing under way: nothing to release.
    rig.jumps.release();
    CHECK(rig.changes == changes + 1);
}
// }}}
