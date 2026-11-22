// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/Fleet.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <orchestrator/core/Finished.hpp>
#include <orchestrator/core/Overloaded.hpp>

using namespace std::string_view_literals;

namespace orchestrator::view
{

namespace
{
    constexpr auto Groups = std::array {
        FleetGroupRow { .group = FleetGroup::NeedsYou,
                        .key = "fleet/needs-you"sv,
                        .name = "NEEDS YOU"sv,
                        .role = Role::Warning,
                        .opensOn = FocusTab::Transcript },
        FleetGroupRow { .group = FleetGroup::Working,
                        .key = "fleet/working"sv,
                        .name = "WORKING"sv,
                        .role = Role::Working,
                        .opensOn = FocusTab::Board },
        FleetGroupRow { .group = FleetGroup::ToReview,
                        .key = "fleet/to-review"sv,
                        .name = "TO REVIEW"sv,
                        .role = Role::Passing,
                        .opensOn = FocusTab::Board },
        FleetGroupRow { .group = FleetGroup::Finished,
                        .key = "fleet/finished"sv,
                        .name = "FINISHED"sv,
                        .role = Role::Dim,
                        .opensOn = FocusTab::Details,
                        .foldDefault = FoldDefault::Folded },
    };

    static_assert(
        std::ranges::all_of(std::views::iota(std::size_t { 0 }, Groups.size()), [](std::size_t index) {
            return static_cast<std::size_t>(std::to_underlying(Groups[index].group)) == index;
        }));

    /// How an agent that reported looks, indexed by AgentStatus.
    constexpr auto ReportedLooks = std::array {
        RunLook { .mark = "…"sv, .role = Role::Warning, .word = "waiting"sv }, // Idle
        RunLook { .mark = "⠿"sv, .role = Role::Working, .word = "working"sv }, // Working
        RunLook { .mark = "✓"sv, .role = Role::Passing, .word = "done"sv },    // Done
        RunLook { .mark = "…"sv, .role = Role::Warning, .word = "waiting"sv }, // Waiting
        RunLook { .mark = "✗"sv, .role = Role::Failing, .word = "error"sv },   // Error
        RunLook { .mark = "?"sv, .role = Role::Warning, .word = "asking"sv },  // Asking
    };
    static_assert(ReportedLooks.size() == std::to_underlying(AgentStatus::Asking) + 1U);

    /// The row state of an agent that reported, indexed by AgentStatus.
    constexpr auto ReportedRowStates = std::array {
        RowState::Waiting, // Idle
        RowState::Working, // Working
        RowState::Done,    // Done
        RowState::Waiting, // Waiting
        RowState::Errored, // Error
        RowState::Asking,  // Asking
    };
    static_assert(ReportedRowStates.size() == ReportedLooks.size());

    constexpr auto StartingLook = RunLook { .mark = "◌"sv, .role = Role::Working, .word = "starting"sv };
    constexpr auto PreparingLook = RunLook { .mark = "◌"sv, .role = Role::Working, .word = "preparing"sv };
    constexpr auto EndingLook = RunLook { .mark = "◌"sv, .role = Role::Dim, .word = "ending"sv };
    constexpr auto CheckingLook = RunLook { .mark = "◌"sv, .role = Role::Dim, .word = "checking"sv };
    constexpr auto FailedLook = RunLook { .mark = "✗"sv, .role = Role::Failing, .word = "failed"sv };
    constexpr auto KeptLook = RunLook { .mark = "⚑"sv, .role = Role::Warning, .word = "kept"sv };
    constexpr auto RemovedLook = RunLook { .mark = "·"sv, .role = Role::Dim, .word = "removed"sv };

    /// The share of a meter that is drawn done, in cells: at least one for any progress, all of it only when
    /// every box is ticked.
    [[nodiscard]] int doneCellsOf(BoardProgress const& progress, int cells) noexcept
    {
        if (progress.total == 0 || progress.done == 0)
            return 0;
        if (progress.done >= progress.total)
            return cells;
        auto const share =
            static_cast<int>((progress.done * static_cast<std::size_t>(cells)) / progress.total);
        return std::clamp(share, 1, cells - 1);
    }
} // namespace

std::span<FleetGroupRow const> fleetGroups() noexcept
{
    return Groups;
}

FleetGroupRow const& fleetGroupRow(FleetGroup group) noexcept
{
    return Groups[std::to_underlying(group)];
}

FleetGroup fleetGroupOf(Run const& run) noexcept
{
    if (finishedEntryOf(run) != nullptr)
        return FleetGroup::Finished;
    return std::visit(Overloaded {
                          [&](Running const&) {
                              return run.lastStatus == AgentStatus::Idle ? FleetGroup::NeedsYou
                                                                         : FleetGroup::Working;
                          },
                          [](Blocked const&) { return FleetGroup::NeedsYou; },
                          [](Done const&) { return FleetGroup::ToReview; },
                          [](auto const&) { return FleetGroup::Working; },
                      },
                      run.state);
}

RunLook runLookOf(Run const& run) noexcept
{
    if (auto const* const finished = finishedEntryOf(run))
    {
        if (!std::holds_alternative<Finished>(run.state))
            return CheckingLook;
        if (run.failure)
            return FailedLook;
        return finished->fate == Fate::Kept ? KeptLook : RemovedLook;
    }
    return std::visit(Overloaded {
                          [](Preparing const&) { return PreparingLook; },
                          [](Ending const&) { return EndingLook; },
                          // The state says done, whatever the agent last reported on the way there.
                          [](Done const&) { return ReportedLooks[std::to_underlying(AgentStatus::Done)]; },
                          [&](auto const&) {
                              return run.lastStatus ? ReportedLooks[std::to_underlying(*run.lastStatus)]
                                                    : StartingLook;
                          },
                      },
                      run.state);
}

RowState rowStateOf(Run const& run) noexcept
{
    if (auto const* const finished = finishedEntryOf(run))
        return finished->fate == Fate::Kept ? RowState::Kept : RowState::Finished;
    return std::visit(Overloaded {
                          [](Preparing const&) { return RowState::Starting; },
                          [](Ending const&) { return RowState::Ending; },
                          [](Done const&) { return RowState::Done; },
                          // A blocked run whose report was lost waits for the user all the same.
                          [&](Blocked const&) {
                              return run.lastStatus ? ReportedRowStates[std::to_underlying(*run.lastStatus)]
                                                    : RowState::Waiting;
                          },
                          [&](auto const&) {
                              return run.lastStatus ? ReportedRowStates[std::to_underlying(*run.lastStatus)]
                                                    : RowState::Starting;
                          },
                      },
                      run.state);
}

uint64_t waitingSinceOf(Run const& run) noexcept
{
    if (auto const* const finished = std::get_if<Finished>(&run.state);
        finished && finished->endedWallMs != 0)
        return finished->endedWallMs;
    return run.history.empty() ? run.startedWallMs : run.history.back().wallMs;
}

std::optional<BoardProgress> boardProgressOf(std::span<BoardLine const> lines)
{
    constexpr auto Open = "☐ "sv;
    constexpr auto Ticked = "☑ "sv;
    auto progress = BoardProgress {};
    for (auto const& line: lines)
    {
        auto const text = std::string_view { line.text };
        auto const box = text.substr(std::min(text.find_first_not_of(' '), text.size()));
        if (box.starts_with(Ticked))
        {
            ++progress.done;
            ++progress.total;
        }
        else if (box.starts_with(Open))
        {
            ++progress.total;
            if (progress.current.empty())
                progress.current = box.substr(Open.size());
        }
    }
    if (progress.total == 0)
        return std::nullopt;
    return progress;
}

std::string meterOf(BoardProgress const& progress, int cells)
{
    auto meter = std::string {};
    auto const done = doneCellsOf(progress, cells);
    for (auto const cell: std::views::iota(0, std::max(0, cells)))
        meter += cell < done ? "▰"sv : "▱"sv;
    return meter;
}

} // namespace orchestrator::view
