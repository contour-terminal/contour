// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/Overloaded.hpp>
#include <orchestrator/proto/StepQueue.hpp>

using namespace std::string_view_literals;

namespace
{

using namespace orchestrator;
using namespace orchestrator::proto;

/// What an input's effects produce, as the driver would carry them out.
enum class FollowUp : uint8_t
{
    None,         ///< Nothing.
    PaneOpened,   ///< An OpenPane effect, whose tab opened: a PaneOpenedFor.
    FileReadBack, ///< A ReadFile effect: a FileRead.
};

/// @return @p input's name in a trace.
std::string_view nameOf(Input const& input)
{
    return std::visit(Overloaded {
                          [](Wake const&) { return "wake"sv; },
                          [](ProcessFinished const&) { return "process-finished"sv; },
                          [](FileRead const&) { return "file-read"sv; },
                          [](PanesListed const&) { return "panes-listed"sv; },
                          [](PaneOpenedFor const&) { return "pane-opened"sv; },
                          [](RefreshNow const&) { return "refresh"sv; },
                          [](auto const&) { return "other"sv; },
                      },
                      input);
}

/// Runs the driver's loop over @p queue, with what a ProcessFinished and a Wake produce.
/// @return Each input fed by name, and "list" where the panes were listed.
std::vector<std::string_view> trace(StepQueue& queue, FollowUp afterProcess, FollowUp afterWake)
{
    auto const followUp = [](FollowUp kind) -> std::optional<Input> {
        switch (kind)
        {
            case FollowUp::None: return std::nullopt;
            case FollowUp::PaneOpened: return PaneOpenedFor { .run = "r", .pane = "p", .monoMs = 2 };
            case FollowUp::FileReadBack:
                return FileRead { .tag = "r/board", .content = std::string {}, .monoMs = 2 };
        }
        return std::nullopt;
    };
    auto steps = std::vector<std::string_view> {};
    while (queue.next() != NextStep::Idle)
    {
        if (queue.next() == NextStep::ListPanes)
        {
            steps.push_back("list"sv);
            queue.listingTaken();
            queue.push(
                PanesListed { .panes = {}, .requestedMonoMs = 3, .listing = PaneListing::OwnRecordsOnly });
            continue;
        }
        auto const input = queue.take();
        steps.push_back(nameOf(input));
        auto produced = std::optional<Input> {};
        if (std::holds_alternative<ProcessFinished>(input))
            produced = followUp(afterProcess);
        else if (std::holds_alternative<Wake>(input))
            produced = followUp(afterWake);
        if (produced)
            queue.push(*produced);
        queue.carriedOut(input);
    }
    return steps;
}

ProcessFinished finished()
{
    return ProcessFinished { .tag = "r/checkout#3", .outcome = {}, .monoMs = 1 };
}

} // namespace

TEST_CASE("StepQueue: a Wake's listing waits for every queued input", "[orchestrator][proto]")
{
    struct Row
    {
        std::string_view name;
        std::vector<Input> queued;
        FollowUp afterProcess = FollowUp::None;
        FollowUp afterWake = FollowUp::None;
        std::vector<std::string_view> expected;
    };
    auto const rows = std::array {
        // The review's interleaving: the command's reply opens the tab, whose PaneOpenedFor lands behind the
        // Wake; listing right after the Wake would predate the tab and close it.
        Row { .name = "a tab opened while the wake waited",
              .queued = { finished(), Wake { .monoMs = 1, .wallMs = 1 } },
              .afterProcess = FollowUp::PaneOpened,
              .expected = { "process-finished"sv, "wake"sv, "pane-opened"sv, "list"sv, "panes-listed"sv } },
        Row { .name = "a wake alone",
              .queued = { Wake { .monoMs = 1, .wallMs = 1 } },
              .expected = { "wake"sv, "list"sv, "panes-listed"sv } },
        Row { .name = "the wake's own effects come first",
              .queued = { Wake { .monoMs = 1, .wallMs = 1 } },
              .afterWake = FollowUp::FileReadBack,
              .expected = { "wake"sv, "file-read"sv, "list"sv, "panes-listed"sv } },
        Row { .name = "two wakes, one listing",
              .queued = { Wake { .monoMs = 1, .wallMs = 1 }, Wake { .monoMs = 2, .wallMs = 2 } },
              .expected = { "wake"sv, "wake"sv, "list"sv, "panes-listed"sv } },
        Row { .name = "no wake, no listing",
              .queued = { RefreshNow { .monoMs = 1 }, finished() },
              .afterProcess = FollowUp::PaneOpened,
              .expected = { "refresh"sv, "process-finished"sv, "pane-opened"sv } },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.name);
        auto queue = StepQueue {};
        for (auto const& input: row.queued)
            queue.push(input);
        CHECK(trace(queue, row.afterProcess, row.afterWake) == row.expected);
    }
}
