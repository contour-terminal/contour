// SPDX-License-Identifier: Apache-2.0
#include <contour/session/SidebarJump.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>

using contour::session::AnyWorkspace;
using contour::session::SwitchTarget;
using contour::session::switchTargetOf;
using vthost::proto::SwitchDaemonKind;

TEST_CASE("a daemon ref becomes the switch target that names it", "[sidebar][switch]")
{
    auto const workspace =
        vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Workspace, .uuid = { .bytes = { 1, 2, 3 } } };

    CHECK(switchTargetOf(contour::window::localDaemonRef("work"), workspace, {}, {})
          == SwitchTarget { .kind = SwitchDaemonKind::Name,
                            .daemon = "work",
                            .workspace = workspace,
                            .focus = {},
                            .instance = {} });
    CHECK(switchTargetOf(contour::window::hostDaemonRef("starfive"), AnyWorkspace {}, {}, {})
          == SwitchTarget { .kind = SwitchDaemonKind::Host,
                            .daemon = "starfive",
                            .workspace = AnyWorkspace {},
                            .focus = {},
                            .instance = {} });
    // A daemon reached by an ad-hoc link has no name a switch could go by.
    CHECK(switchTargetOf(contour::window::linkDaemonRef(4), AnyWorkspace {}, {}, {}) == std::nullopt);
    CHECK(switchTargetOf(contour::window::DaemonRef { "bogus" }, AnyWorkspace {}, {}, {}) == std::nullopt);
}

TEST_CASE("a sidebar target carries its focus and instance into the switch", "[sidebar][switch]")
{
    // The row's tab and pane are what the window activates once it lands, and the run the row was listed in
    // is what the switch checks the daemon is still on.
    auto const focus = contour::session::LandingFocus { .tab = 12, .pane = contour::window::PaneKey { 34 } };
    auto const target = switchTargetOf(contour::window::hostDaemonRef("starfive"),
                                       AnyWorkspace {},
                                       focus,
                                       contour::window::DaemonInstance { 9 });
    REQUIRE(target.has_value());
    CHECK(target->focus == focus);
    CHECK(target->instance == contour::window::DaemonInstance { 9 });
}
