// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Finished.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <orchestrator/core/FailureWords.hpp>
#include <orchestrator/core/Overloaded.hpp>
#include <orchestrator/core/Run.hpp>
#include <orchestrator/core/Sanitize.hpp>

using namespace std::string_view_literals;

namespace orchestrator
{

namespace
{
    /// One term of the short text after the run count, omitted while its count is zero.
    struct Term
    {
        uint32_t Counts::* count; ///< The number it shows.
        std::string_view symbol;  ///< What precedes the number.
        std::string_view noun;    ///< What follows it; may be empty.
    };

    constexpr auto Terms = std::array {
        Term { .count = &Counts::failingCi, .symbol = "✗"sv, .noun = ""sv },
        Term { .count = &Counts::blocked, .symbol = "●"sv, .noun = " blocked"sv },
        Term { .count = &Counts::newFinished, .symbol = "✓"sv, .noun = " new"sv },
    };

    // The words each state's line uses, indexed by the enumerator. They are kept apart from Run.cpp's tables
    // on purpose: those spell the store's and the tags' names, a format that must not change when a word the
    // user reads does ("ended-by-you" against "ended by you"), and EndSteps' labels name a step inside a
    // kept run's entry ("the reachability check"), where these name it on its own ("reachable").
    constexpr auto PrepareStepNames = std::array { "fetch"sv, "worktree"sv, "checkout"sv, "pane"sv };
    constexpr auto EndStepNames = std::array {
        "closing tab"sv,  "waiting for the main checkout"sv,
        "verify"sv,       "common dir"sv,
        "index"sv,        "reachable"sv,
        "operation"sv,    "status"sv,
        "status board"sv, "remove"sv,
    };
    constexpr auto EndedNames = std::array {
        "completed"sv, "ended by you"sv, "preparation failed"sv, "never reported"sv, "worktree gone"sv,
    };
    constexpr auto FateNames = std::array { "worktree removed"sv, "worktree kept"sv };
    constexpr auto SeenNames = std::array { "new"sv, "seen"sv };

    static_assert(PrepareStepNames.size() == std::to_underlying(PrepareStep::OpenPane) + 1U);
    static_assert(EndStepNames.size() == std::to_underlying(EndStep::Remove) + 1U);
    static_assert(EndedNames.size() == std::to_underlying(Ended::WorktreeGone) + 1U);
    static_assert(FateNames.size() == std::to_underlying(Fate::Kept) + 1U);
    static_assert(SeenNames.size() == std::to_underlying(Seen::Opened) + 1U);

    /// The word for @p value from @p names, indexed by its enumerator.
    template <typename Enum, std::size_t N>
    [[nodiscard]] constexpr std::string_view nameOf(std::array<std::string_view, N> const& names,
                                                    Enum value) noexcept
    {
        auto const index = static_cast<std::size_t>(std::to_underlying(value));
        return index < names.size() ? names[index] : "?"sv;
    }

    /// The bytes the line counting the runs left out may take, its line end included.
    constexpr auto MaxTrailerBytes = std::size_t { 32 };

    /// How a run's failure is spelled.
    enum class FailureWording : uint8_t
    {
        Code = 0, ///< As the host call answered it, such as `not-supported-yet`: for diagnosis.
        Plain,    ///< In plain words (plainFailure()): for the summary.
    };

    /// How a finished run ended, what became of its worktree, whether it was seen, and why.
    [[nodiscard]] std::string describeFinished(Run const& run,
                                               Finished const& finished,
                                               std::string_view again,
                                               FailureWording wording)
    {
        auto text = std::format("{}, {}, {}{}",
                                nameOf(EndedNames, finished.ended),
                                nameOf(FateNames, finished.fate),
                                nameOf(SeenNames, finished.seen),
                                again);
        if (run.failure)
            text += std::format(": {}: {}",
                                nameOf(PrepareStepNames, run.failure->step),
                                wording == FailureWording::Plain ? plainFailure(run.failure->why)
                                                                 : std::string_view { run.failure->why });
        else if (finished.fate == Fate::Kept && !finished.entries.empty())
            text += std::format(": {}", finished.entries.front());
        return text;
    }

    /// Where @p run stands, its failure spelled as @p wording says.
    [[nodiscard]] std::string stateText(Run const& run, FailureWording wording)
    {
        return std::visit(Overloaded {
                              [](Preparing const& state) {
                                  return std::format("preparing: {}", nameOf(PrepareStepNames, state.step));
                              },
                              [](Running const&) { return std::string { "running" }; },
                              [](Blocked const&) { return std::string { "blocked" }; },
                              [](Done const&) { return std::string { "done" }; },
                              [&run, wording](Ending const& state) {
                                  if (state.resumedFrom)
                                      return describeFinished(
                                          run, *state.resumedFrom, ", checking again"sv, wording);
                                  return std::format("ending: {}", nameOf(EndStepNames, state.step));
                              },
                              [&run, wording](Finished const& state) {
                                  return describeFinished(run, state, ""sv, wording);
                              },
                          },
                          run.state);
    }

    [[nodiscard]] std::string shortTextOf(Counts const& counts)
    {
        auto text = std::format("⬢ agents {}/{}", counts.active, counts.max);
        for (auto const& term: Terms)
        {
            if (auto const count = counts.*term.count; count != 0)
                text += std::format(" · {}{}{}", term.symbol, count, term.noun);
        }
        return text;
    }
} // namespace

Finished const* finishedEntryOf(Run const& run) noexcept
{
    if (auto const* finished = std::get_if<Finished>(&run.state))
        return finished;
    if (auto const* ending = std::get_if<Ending>(&run.state); ending && ending->resumedFrom)
        return &*ending->resumedFrom;
    return nullptr;
}

std::string_view endedText(Ended ended) noexcept
{
    return nameOf(EndedNames, ended);
}

std::string runStateText(Run const& run)
{
    return stateText(run, FailureWording::Code);
}

std::string runLine(Run const& run)
{
    return displayText(
        std::format("{} · {} · {} · {}", run.id, run.repo, run.model, stateText(run, FailureWording::Plain)),
        MaxSummaryLineChars);
}

bool isPurgeable(Run const& run, uint64_t nowWallMs, std::chrono::milliseconds retention) noexcept
{
    auto const* finished = std::get_if<Finished>(&run.state);
    if (!finished || finished->fate != Fate::Removed || retention <= std::chrono::milliseconds::zero()
        || nowWallMs < finished->endedWallMs)
        return false;
    return nowWallMs - finished->endedWallMs > static_cast<uint64_t>(retention.count());
}

std::size_t newFinishedCount(std::span<Run const> runs) noexcept
{
    return static_cast<std::size_t>(std::ranges::count_if(runs, [](Run const& run) {
        auto const* finished = finishedEntryOf(run);
        return finished && finished->seen == Seen::New;
    }));
}

Counts countRuns(std::span<Run const> runs, uint32_t maxAgents, uint32_t failingCi) noexcept
{
    return Counts {
        .active = static_cast<uint32_t>(std::ranges::count_if(runs, countsAsActive)),
        .max = maxAgents,
        .failingCi = failingCi,
        .blocked = static_cast<uint32_t>(std::ranges::count_if(
            runs, [](Run const& run) { return std::holds_alternative<Blocked>(run.state); })),
        .newFinished = static_cast<uint32_t>(newFinishedCount(runs)),
    };
}

Summary summarize(Counts counts, std::span<Run const> runs)
{
    auto summary = Summary { .shortText = shortTextOf(counts), .detail = {} };
    summary.detail = summary.shortText;
    for (auto const index: std::views::iota(std::size_t { 0 }, runs.size()))
    {
        auto const line = runLine(runs[index]);
        auto const isLast = index + 1 == runs.size();
        auto const budget = MaxSummaryDetailBytes - (isLast ? 0 : MaxTrailerBytes);
        if (summary.detail.size() + 1 + line.size() > budget)
        {
            summary.detail += std::format("\n… {} more", runs.size() - index);
            break;
        }
        summary.detail += '\n';
        summary.detail += line;
    }
    return summary;
}

} // namespace orchestrator
