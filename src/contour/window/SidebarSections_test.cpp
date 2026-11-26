// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SidebarSections.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <vector>

using namespace contour::window;

namespace
{
DaemonSection sectionOf(DaemonRef daemon,
                        std::string name,
                        DaemonLinkState state,
                        std::set<uint64_t> elsewhere = {})
{
    auto section = DaemonSection {};
    section.daemon = std::move(daemon);
    section.name = std::move(name);
    section.state = state;
    section.shownElsewhere = std::move(elsewhere);
    return section;
}

std::vector<std::string> namesOf(std::vector<DaemonSection> const& sections)
{
    return std::ranges::to<std::vector>(sections | std::views::transform(&DaemonSection::name));
}
} // namespace

TEST_CASE("the window's own section replaces the listed one in place", "[sidebar]")
{
    auto const listed = std::vector {
        sectionOf(localDaemonRef("default"), "default", DaemonLinkState::Connected),
        sectionOf(localDaemonRef("work"), "work (observed)", DaemonLinkState::Reconnecting),
        sectionOf(hostDaemonRef("starfive"), "starfive", DaemonLinkState::NotConnected),
    };
    auto const sections = windowSections(
        listed, sectionOf(localDaemonRef("work"), "work", DaemonLinkState::Connected), std::nullopt);
    CHECK(namesOf(sections) == std::vector<std::string> { "default", "work", "starfive" });
    CHECK(sections[1].state == DaemonLinkState::Connected);
}

TEST_CASE("an own daemon not yet listed is appended", "[sidebar]")
{
    auto const listed =
        std::vector { sectionOf(localDaemonRef("default"), "default", DaemonLinkState::Connected) };
    auto const sections = windowSections(
        listed, sectionOf(hostDaemonRef("starfive"), "starfive", DaemonLinkState::Connected), std::nullopt);
    CHECK(namesOf(sections) == std::vector<std::string> { "default", "starfive" });
}

TEST_CASE("the window's own workspace is not shown elsewhere", "[sidebar]")
{
    auto const own = sectionOf(localDaemonRef("work"), "work", DaemonLinkState::Connected, { 1, 2 });
    auto const sections = windowSections({}, own, 1);
    REQUIRE(sections.size() == 1);
    CHECK(sections[0].shownElsewhere == std::set<uint64_t> { 2 });
    CHECK(windowSections({}, own, std::nullopt)[0].shownElsewhere == std::set<uint64_t> { 1, 2 });
}

TEST_CASE("a jump names its workspace by the run the row was listed from", "[sidebar]")
{
    // A restarted daemon may give its new workspaces the ids the old run's had: a row one rebuild stale
    // would land on a stranger, so a jump naming another run is refused (spec §3).
    auto directory = WorkspaceDirectory {};
    auto workspace = DirectoryWorkspace {};
    workspace.id = 5;
    workspace.controlId = std::array<uint8_t, 16> { 1, 2, 3 };
    directory.workspaces.push_back(workspace);
    auto const target = SidebarTarget { .daemon = hostDaemonRef("starfive"),
                                        .instance = DaemonInstance { 9 },
                                        .workspace = 5,
                                        .tab = {},
                                        .pane = {} };
    CHECK(jumpControlIdOf(DaemonInstance { 9 }, directory, target) == workspace.controlId);
    CHECK(jumpControlIdOf(DaemonInstance { 10 }, directory, target)
          == std::unexpected(JumpRefusal::StaleRun));
    // A workspace the run does not list leaves the choice to the daemon.
    auto elsewhere = target;
    elsewhere.workspace = 6;
    CHECK(jumpControlIdOf(DaemonInstance { 9 }, directory, elsewhere) == std::array<uint8_t, 16> {});
}
