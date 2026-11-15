// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Fixtures.hpp>
#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Reconcile.hpp>
#include <orchestrator/core/Run.hpp>
#include <orchestrator/proto/Panes.hpp>

using namespace std::string_view_literals;

namespace
{

using namespace orchestrator;
using namespace orchestrator::proto;

/// The fixture's tab holds two panes: the first reported `blocked` through the agent's integration, the
/// second reported nothing.
constexpr auto ReportingPane = "01a1df08-04b3-717f-b69a-d342bf66080e"sv;
constexpr auto SilentPane = "01a1df08-0501-70bb-910e-eede51df1d71"sv;
constexpr auto UnlistedPane = "01a1df08-0000-7000-8000-000000000000"sv;

/// A recorded `contour api call workspace.list` reply.
std::string workspaceListFixture()
{
    return readFixture(FixtureRoot::Testdata, "contour/workspace-list.json");
}

/// A listing of one pane whose program-status records are @p records.
std::string listingWith(std::string_view records)
{
    return std::format(R"({{"workspaces":[{{"id":"workspace:w","tabs":[{{"id":"tab:t","panes":[)"
                       R"({{"id":"pane:{}","programStatus":[{}]}}]}}]}}]}})",
                       ReportingPane,
                       records);
}

} // namespace

TEST_CASE("Panes: the pane ids the engine and the control API spell", "[orchestrator][proto]")
{
    struct Row
    {
        std::string_view control;
        std::optional<std::string_view> engine;
    };
    static constexpr auto Rows = std::array {
        Row { .control = "pane:01a1df08-04b3-717f-b69a-d342bf66080e",
              .engine = "01a1df08-04b3-717f-b69a-d342bf66080e" },
        Row { .control = "pane:", .engine = std::nullopt },
        Row { .control = "tab:01a1df08", .engine = std::nullopt },
        Row { .control = "01a1df08", .engine = std::nullopt },
        Row { .control = "", .engine = std::nullopt },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(row.control);
        CHECK(enginePaneId(row.control) == row.engine);
        if (row.engine)
            CHECK(controlPaneId(*row.engine) == row.control);
    }
}

TEST_CASE("Panes: only the owned panes of a recorded listing are facts", "[orchestrator][proto]")
{
    auto const listing = workspaceListFixture();
    struct Row
    {
        std::string_view name;
        std::vector<OwnedPane> owned;
        std::vector<PaneFact> expected;
    };
    auto const rows = std::array {
        Row { .name = "the pane tagged by the driver's map",
              .owned = { OwnedPane { .pane = std::string(ReportingPane), .tag = "run-a" } },
              .expected = { PaneFact { .pane = std::string(ReportingPane),
                                       .tag = "run-a",
                                       .status = AgentStatus::Blocked,
                                       .program = ProgramLiveness::Alive } } },
        Row { .name = "both panes, in listing order",
              .owned = { OwnedPane { .pane = std::string(SilentPane), .tag = "run-b" },
                         OwnedPane { .pane = std::string(ReportingPane), .tag = "run-a" } },
              .expected = { PaneFact { .pane = std::string(ReportingPane),
                                       .tag = "run-a",
                                       .status = AgentStatus::Blocked,
                                       .program = ProgramLiveness::Alive },
                            PaneFact { .pane = std::string(SilentPane),
                                       .tag = "run-b",
                                       .status = std::nullopt,
                                       .program = ProgramLiveness::Alive } } },
        Row { .name = "nothing owned", .owned = {}, .expected = {} },
        Row { .name = "an owned pane no longer listed",
              .owned = { OwnedPane { .pane = std::string(UnlistedPane), .tag = "run-c" } },
              .expected = {} },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.name);
        auto const facts = parseWorkspaceList(listing, row.owned);
        REQUIRE(facts.has_value());
        CHECK(*facts == row.expected);
    }
}

TEST_CASE("Panes: the agent's program-status record is its status", "[orchestrator][proto]")
{
    struct Row
    {
        std::string_view records;
        std::optional<AgentStatus> status;
    };
    static constexpr auto Rows = std::array {
        Row { .records = R"({"id":"claude-code","state":"idle"})", .status = AgentStatus::Idle },
        Row { .records = R"({"id":"claude-code","state":"working"})", .status = AgentStatus::Working },
        Row { .records = R"({"id":"claude-code","state":"done"})", .status = AgentStatus::Done },
        Row { .records = R"({"id":"claude-code","state":"blocked","kind":"permission"})",
              .status = AgentStatus::Blocked },
        Row { .records = R"({"id":"claude-code","state":"error"})", .status = AgentStatus::Error },
        Row { .records = R"({"id":"claude-code","state":"sleeping"})", .status = std::nullopt },
        Row { .records = R"({"id":"make","state":"error"})", .status = std::nullopt },
        Row { .records = R"({"id":"make","state":"error"},{"id":"claude-code","state":"done"})",
              .status = AgentStatus::Done },
        Row { .records = "", .status = std::nullopt },
    };
    auto const owned = std::array { OwnedPane { .pane = std::string(ReportingPane), .tag = "run-a" } };
    for (auto const& row: Rows)
    {
        CAPTURE(row.records);
        auto const facts = parseWorkspaceList(listingWith(row.records), owned);
        REQUIRE(facts.has_value());
        REQUIRE(facts->size() == 1);
        CHECK(facts->front().status == row.status);
        CHECK(facts->front().program == ProgramLiveness::Alive);
    }
}

TEST_CASE("Panes: a listing that cannot be read in full is refused", "[orchestrator][proto]")
{
    auto const listing = workspaceListFixture();
    auto const cut = listing.substr(0, listing.size() / 2);
    struct Row
    {
        std::string_view name;
        std::string_view json;
        ParseError error;
    };
    auto const rows = std::array {
        Row { .name = "empty", .json = "", .error = ParseError::NotJson },
        Row { .name = "cut midway", .json = cut, .error = ParseError::NotJson },
        Row { .name = "an error text", .json = "Error: no daemon", .error = ParseError::NotJson },
        Row {
            .name = "workspaces not a list", .json = R"({"workspaces":3})", .error = ParseError::WrongShape },
        Row { .name = "a pane not an object",
              .json = R"({"workspaces":[{"tabs":[{"panes":[7]}]}]})",
              .error = ParseError::WrongShape },
        Row { .name = "not an object", .json = "[]", .error = ParseError::WrongShape },
    };
    auto const owned = std::array { OwnedPane { .pane = std::string(ReportingPane), .tag = "run-a" } };
    for (auto const& row: rows)
    {
        CAPTURE(row.name);
        auto const facts = parseWorkspaceList(row.json, owned);
        REQUIRE_FALSE(facts.has_value());
        CHECK(facts.error() == row.error);
    }
}
