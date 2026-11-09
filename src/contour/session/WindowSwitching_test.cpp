// SPDX-License-Identifier: Apache-2.0

#include <contour/session/WindowSwitching.hpp>

#include <catch2/catch_test_macros.hpp>

#include <variant>

using namespace contour::session;

TEST_CASE("windowSwitching.defaultsMeanSwitchToTheSameDaemon", "[session]")
{
    auto const target = SwitchTarget {};
    CHECK(target.kind == vthost::proto::SwitchDaemonKind::Same);
    CHECK(target.daemon.empty());
    CHECK(std::holds_alternative<AnyWorkspace>(target.workspace));
    CHECK(target == SwitchTarget {});
    CHECK(SwitchMode {} == SwitchMode::Switch);
}

TEST_CASE("windowSwitching.aSpecAsAProgramSpellsItNamesTheWorkspace", "[session]")
{
    CHECK(workspaceTargetOf("") == WorkspaceTarget { AnyWorkspace {} });
    CHECK(workspaceTargetOf(NewWorkspaceSpec) == WorkspaceTarget { NewWorkspace {} });
    CHECK(workspaceTargetOf("2") == WorkspaceTarget { WorkspaceNamed { .spec = "2" } });
    CHECK(switchTargetOf(vthost::proto::SwitchTargetWire {
              .kind = vthost::proto::SwitchDaemonKind::Host, .daemon = "home", .workspace = "ops" })
          == SwitchTarget { .kind = vthost::proto::SwitchDaemonKind::Host,
                            .daemon = "home",
                            .workspace = WorkspaceNamed { .spec = "ops" } });
}
