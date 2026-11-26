// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SidebarJumps.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using contour::window::JumpCancel;
using contour::window::JumpEvent;
using contour::window::JumpMode;
using contour::window::JumpRefusal;
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
                 contour::session::SwitchLandBy /*landBy*/,
                 contour::session::SwitchCompletion done) override
    {
        // As the switcher does for the user's newer request: the one under way ends first, told so.
        if (!asked.empty() && asked.back().done)
            std::exchange(asked.back().done, {})(std::unexpected(std::string { "superseded" }));
        asked.push_back(
            Asked { .window = window, .target = std::move(target), .mode = mode, .done = std::move(done) });
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
        REQUIRE(asked.back().done);
        std::exchange(asked.back().done, {})(std::move(outcome));
    }

    std::vector<Asked> asked;
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

/// A sidebar's jumps over a held switching service, with what they said and how often they changed.
struct JumpRig
{
    HeldSwitching switching;
    std::vector<Said> said;
    int changes = 0;
    SidebarJumps jumps { switching,
                         [this](JumpEvent event, SidebarTarget const& target, std::string const& reason) {
                             said.push_back(Said { .event = event, .target = target, .reason = reason });
                         },
                         [this] { ++changes; } };
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
    rig.jumps.jumpTo(Window, ciOnStarfive(), JumpMode::Here, ControlId, std::nullopt);

    REQUIRE(rig.switching.asked.size() == 1);
    CHECK(rig.jumps.pending() == ciOnStarfive());
    REQUIRE(rig.said.size() == 1);
    CHECK(rig.said[0].event == JumpEvent::Connecting);
    CHECK(rig.changes == 1);

    rig.switching.answer(SwitchOutcome::Switched);

    CHECK_FALSE(rig.jumps.pending().has_value());
    REQUIRE(rig.said.size() == 2);
    CHECK(rig.said[1].event == JumpEvent::Landed);
    CHECK(rig.said[1].target == ciOnStarfive());
    CHECK(rig.changes == 2);
}

TEST_CASE("a sidebar jump's switch carries the row's tab, pane and run", "[sidebar][jump][switch]")
{
    auto rig = JumpRig {};
    rig.jumps.jumpTo(Window, ciOnStarfive(), JumpMode::Here, ControlId, std::nullopt);

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
    rig.jumps.jumpTo(
        Window, ciOnStarfive(), JumpMode::Here, std::unexpected(JumpRefusal::StaleRun), std::nullopt);

    CHECK(rig.switching.asked.empty());
    CHECK_FALSE(rig.jumps.pending().has_value());
    REQUIRE(rig.said.size() == 1);
    CHECK(rig.said[0].event == JumpEvent::Failed);
    CHECK(rig.said[0].reason == "the daemon restarted");
}

TEST_CASE("Cancel ends the pending jump through the switching service, and says so once", "[sidebar][jump]")
{
    auto rig = JumpRig {};
    rig.jumps.jumpTo(Window, ciOnStarfive(), JumpMode::Here, ControlId, std::nullopt);

    rig.jumps.cancel(Window);

    REQUIRE(rig.switching.cancels.size() == 1);
    CHECK(rig.switching.cancels.front() == Window);
    CHECK(rig.switching.cancelledModes.front() == contour::session::SwitchMode::Switch);
    // The service ends the switch as cancelled.
    rig.switching.answer(std::unexpected(std::string { "the user cancelled it" }));
    CHECK_FALSE(rig.jumps.pending().has_value());
    REQUIRE(rig.said.size() == 2);
    CHECK(rig.said[1].event == JumpEvent::Cancelled);
}

TEST_CASE("Cancel of an Open in New Window cancels that Open, not a switch in place", "[sidebar][jump]")
{
    auto rig = JumpRig {};
    rig.jumps.jumpTo(Window, ciOnStarfive(), JumpMode::NewWindow, ControlId, std::nullopt);

    rig.jumps.cancel(Window);

    REQUIRE(rig.switching.cancelledModes.size() == 1);
    CHECK(rig.switching.cancelledModes.front() == contour::session::SwitchMode::Open);
}

TEST_CASE("Cancel with no jump under way asks nothing", "[sidebar][jump]")
{
    auto rig = JumpRig {};
    rig.jumps.cancel(Window);
    CHECK(rig.switching.cancels.empty());
    CHECK(rig.said.empty());
}

TEST_CASE("a failed jump says why", "[sidebar][jump]")
{
    auto rig = JumpRig {};
    rig.jumps.jumpTo(Window, ciOnStarfive(), JumpMode::Here, ControlId, std::nullopt);
    rig.switching.answer(std::unexpected(std::string { "cannot connect to the daemon" }));

    REQUIRE(rig.said.size() == 2);
    CHECK(rig.said[1].event == JumpEvent::Failed);
    CHECK(rig.said[1].reason == "cannot connect to the daemon");
}

TEST_CASE("a superseded jump says nothing", "[sidebar][jump]")
{
    // A second click while the first jump connects: the first ends for it, silently; the second speaks.
    auto rig = JumpRig {};
    rig.jumps.jumpTo(Window, ciOnStarfive(), JumpMode::Here, ControlId, std::nullopt);
    auto second = ciOnStarfive();
    second.workspace = 6;
    rig.jumps.jumpTo(Window, second, JumpMode::Here, ControlId, std::nullopt);

    REQUIRE(rig.said.size() == 2);
    CHECK(rig.said[0].event == JumpEvent::Connecting);
    CHECK(rig.said[1].event == JumpEvent::Connecting);
    CHECK(rig.jumps.pending() == second);

    rig.switching.answer(SwitchOutcome::Switched);
    REQUIRE(rig.said.size() == 3);
    CHECK(rig.said[2].event == JumpEvent::Landed);
    CHECK(rig.said[2].target == second);
}
