// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/proto/RunRefs.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include <orchestrator/core/Finished.hpp>
#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Overloaded.hpp>

using namespace std::string_view_literals;

namespace orchestrator::proto
{

namespace
{
    /// One action: its verb, the runs it takes, and what it says it does.
    struct ActionRow
    {
        RunAction action;
        std::string_view verb;
        bool (*applies)(Run const&) noexcept;
        std::string_view takes; ///< The runs it takes, as the user reads them.
        std::string_view doing; ///< Precedes the run's id in the confirmation.
        std::string_view then;  ///< Follows it.
    };

    constexpr auto ActionRows = std::array {
        ActionRow {
            .action = RunAction::End,
            .verb = "end"sv,
            .applies =
                [](Run const& run) noexcept {
                    return std::holds_alternative<Running>(run.state)
                           || std::holds_alternative<Blocked>(run.state)
                           || std::holds_alternative<Done>(run.state);
                },
            .takes = "a running, blocked or done run"sv,
            .doing = "ending"sv,
            .then = ": closing its tab, then checking whether its worktree can be removed"sv,
        },
        ActionRow {
            .action = RunAction::Retry,
            .verb = "retry"sv,
            .applies =
                [](Run const& run) noexcept {
                    auto const* const finished = std::get_if<Finished>(&run.state);
                    return finished != nullptr && finished->fate == Fate::Kept;
                },
            .takes = "a finished run whose worktree was kept"sv,
            .doing = "checking"sv,
            .then =
                " again: its kept worktree goes through the removal checks, and is removed if they pass"sv,
        },
        ActionRow {
            .action = RunAction::Dismiss,
            .verb = "dismiss"sv,
            .applies =
                [](Run const& run) noexcept {
                    auto const* const finished = std::get_if<Finished>(&run.state);
                    return finished != nullptr && finished->fate == Fate::Removed;
                },
            .takes = "a finished run whose worktree was removed"sv,
            .doing = "dismissing"sv,
            .then = ": it leaves the list and the store"sv,
        },
    };

    [[nodiscard]] ActionRow const& rowOf(RunAction action) noexcept
    {
        // Every action has its row; the table is the enum's only reader.
        return *std::ranges::find(ActionRows, action, &ActionRow::action);
    }

    /// Which items an item spelling's prefix names.
    enum class ItemKind : uint8_t
    {
        Any,           ///< `#1847`: a change request or an issue.
        ChangeRequest, ///< `pr…`.
        Issue,         ///< `issue…`.
    };

    struct ItemPrefix
    {
        std::string_view prefix; ///< Lower case.
        ItemKind kind;
    };

    /// Tried in order, so a longer prefix comes before the one it starts with.
    constexpr auto ItemPrefixes = std::array {
        ItemPrefix { .prefix = "pr#"sv, .kind = ItemKind::ChangeRequest },
        ItemPrefix { .prefix = "pr-"sv, .kind = ItemKind::ChangeRequest },
        ItemPrefix { .prefix = "pr"sv, .kind = ItemKind::ChangeRequest },
        ItemPrefix { .prefix = "issue#"sv, .kind = ItemKind::Issue },
        ItemPrefix { .prefix = "issue-"sv, .kind = ItemKind::Issue },
        ItemPrefix { .prefix = "issue"sv, .kind = ItemKind::Issue },
        ItemPrefix { .prefix = "#"sv, .kind = ItemKind::Any },
    };

    /// An item spelling, read.
    struct ItemSpelling
    {
        ItemKind kind;
        uint64_t number;
    };

    [[nodiscard]] std::optional<uint64_t> numberOf(std::string_view text) noexcept
    {
        auto number = uint64_t { 0 };
        auto const* const first = std::to_address(text.begin());
        auto const* const last = std::to_address(text.end());
        auto const [end, error] = std::from_chars(first, last, number);
        if (text.empty() || error != std::errc {} || end != last)
            return std::nullopt;
        return number;
    }

    [[nodiscard]] std::optional<ItemSpelling> itemSpelling(std::string_view lowered) noexcept
    {
        for (auto const& prefix: ItemPrefixes)
        {
            if (!lowered.starts_with(prefix.prefix))
                continue;
            if (auto const number = numberOf(lowered.substr(prefix.prefix.size())))
                return ItemSpelling { .kind = prefix.kind, .number = *number };
        }
        return std::nullopt;
    }

    [[nodiscard]] std::string lowered(std::string_view text)
    {
        auto lower = std::string(text);
        std::ranges::transform(lower, lower.begin(), [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        });
        return lower;
    }

    /// Whether @p item is what @p lower (the word, in lower case) spells.
    [[nodiscard]] bool itemMatches(ItemRef const& item, std::string_view lower)
    {
        auto const spelling = itemSpelling(lower);
        return std::visit(Overloaded {
                              [&](ChangeRequestRef const& ref) {
                                  return spelling && spelling->kind != ItemKind::Issue
                                         && spelling->number == ref.number;
                              },
                              [&](IssueRef const& ref) {
                                  return spelling && spelling->kind != ItemKind::ChangeRequest
                                         && spelling->number == ref.number;
                              },
                              [&](JiraRef const& ref) { return lowered(ref.key) == lower; },
                          },
                          item);
    }

    [[nodiscard]] std::vector<std::string> idsOf(std::span<Run const* const> runs)
    {
        auto ids = std::vector<std::string> {};
        for (auto const* const run: runs)
            ids.push_back(run->id);
        return ids;
    }

    [[nodiscard]] std::string joined(std::span<std::string const> texts)
    {
        auto text = std::string {};
        for (auto const& each: texts)
            text += std::format("{}{}", text.empty() ? "" : ", ", each);
        return text;
    }
} // namespace

bool actionApplies(RunAction action, Run const& run) noexcept
{
    return rowOf(action).applies(run);
}

std::expected<std::string, RunRefError> resolveRun(std::string_view word,
                                                   RunAction action,
                                                   std::span<std::string const> rows,
                                                   std::span<Run const> runs)
{
    auto const accept = [action](Run const& run) -> std::expected<std::string, RunRefError> {
        if (!actionApplies(action, run))
            return std::unexpected { RunRefError { .problem = RunRefProblem::NotApplicable,
                                                   .candidates = { run.id } } };
        return run.id;
    };

    if (auto const exact = std::ranges::find(runs, word, &Run::id); exact != runs.end())
        return accept(*exact);

    if (auto const row = numberOf(word))
    {
        if (*row == 0 || *row > rows.size())
            return std::unexpected { RunRefError { .problem = RunRefProblem::NoSuchRow, .candidates = {} } };
        auto const& id = rows[*row - 1];
        auto const listed = std::ranges::find(runs, id, &Run::id);
        if (listed == runs.end())
            return std::unexpected { RunRefError { .problem = RunRefProblem::RowGone,
                                                   .candidates = { id } } };
        return accept(*listed);
    }

    auto const lower = lowered(word);
    auto matches = std::vector<Run const*> {};
    for (auto const& run: runs)
        if (run.id.starts_with(lower) || itemMatches(run.item, lower))
            matches.push_back(&run);
    if (matches.empty())
        return std::unexpected { RunRefError { .problem = RunRefProblem::Unknown, .candidates = {} } };

    auto takeable = std::vector<Run const*> {};
    for (auto const* const run: matches)
        if (actionApplies(action, *run))
            takeable.push_back(run);
    if (takeable.size() == 1)
        return takeable.front()->id;
    if (takeable.empty())
        return std::unexpected { RunRefError { .problem = RunRefProblem::NotApplicable,
                                               .candidates = idsOf(matches) } };
    return std::unexpected { RunRefError { .problem = RunRefProblem::Ambiguous,
                                           .candidates = idsOf(takeable) } };
}

std::string refusalText(std::string_view word,
                        RunAction action,
                        RunRefError const& error,
                        std::span<Run const> runs)
{
    auto const& row = rowOf(action);
    switch (error.problem)
    {
        case RunRefProblem::Unknown:
            return std::format("no run matches {}: name it by its row in `runs`, its id or the start of it, "
                               "or its item (#1847)",
                               word);
        case RunRefProblem::NoSuchRow: return std::format("no row {} in the last `runs`", word);
        case RunRefProblem::RowGone:
            return std::format("row {} was {}, which is no longer listed: type `runs` again",
                               word,
                               joined(error.candidates));
        case RunRefProblem::Ambiguous:
            return std::format("{} matches {} runs: {}; name one by its row or its id",
                               word,
                               error.candidates.size(),
                               joined(error.candidates));
        case RunRefProblem::NotApplicable: {
            auto states = std::vector<std::string> {};
            for (auto const& id: error.candidates)
            {
                auto const run = std::ranges::find(runs, id, &Run::id);
                states.push_back(run != runs.end() ? std::format("{} ({})", id, runStateText(*run)) : id);
            }
            return std::format("`{}` takes {}; {} names {}", row.verb, row.takes, word, joined(states));
        }
    }
    return std::format("{}: not understood", word);
}

std::string confirmationText(RunAction action, std::string_view runId)
{
    auto const& row = rowOf(action);
    return std::format("{} {}{}", row.doing, runId, row.then);
}

} // namespace orchestrator::proto
