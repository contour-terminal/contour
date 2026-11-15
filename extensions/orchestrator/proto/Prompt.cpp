// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/proto/Prompt.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

#include <orchestrator/core/Finished.hpp>
#include <orchestrator/core/Overloaded.hpp>
#include <orchestrator/core/Reconcile.hpp>
#include <orchestrator/proto/Host.hpp>
#include <orchestrator/proto/Spawn.hpp>
#include <orchestrator/proto/Store.hpp>

using namespace std::string_view_literals;

namespace orchestrator::proto
{

namespace
{
    /// How much one read of the prompt's input takes.
    constexpr auto ReadChunk = std::size_t { 4096 };

    /// @return @p item as the user reads it.
    [[nodiscard]] std::string itemText(ItemRef const& item)
    {
        return std::visit(Overloaded {
                              [](ChangeRequestRef const& ref) { return std::format("PR #{}", ref.number); },
                              [](IssueRef const& ref) { return std::format("issue #{}", ref.number); },
                              [](JiraRef const& ref) { return ref.key; },
                          },
                          item);
    }

    /// @return @p text as a number from 1 on; std::nullopt for anything else.
    [[nodiscard]] std::optional<std::size_t> rowNumber(std::string_view text) noexcept
    {
        auto number = std::size_t { 0 };
        auto const* const first = std::to_address(text.begin());
        auto const* const last = std::to_address(text.end());
        auto const [end, error] = std::from_chars(first, last, number);
        if (error != std::errc {} || end != last || number == 0)
            return std::nullopt;
        return number;
    }
} // namespace

std::span<Prompt::Verb const> Prompt::verbs() noexcept
{
    static constexpr auto Verbs = std::array {
        Verb { .name = "list", .minWords = 1, .usage = "list", .handler = &Prompt::list },
        Verb { .name = "dispatch",
               .minWords = 3,
               .usage = "dispatch <n> <task-id> [model] [note…]",
               .handler = &Prompt::dispatch },
        Verb { .name = "end", .minWords = 2, .usage = "end <row|run>", .handler = &Prompt::end },
        Verb { .name = "runs", .minWords = 1, .usage = "runs", .handler = &Prompt::runs },
        Verb { .name = "retry", .minWords = 2, .usage = "retry <row|run>", .handler = &Prompt::retry },
        Verb { .name = "dismiss", .minWords = 2, .usage = "dismiss <row|run>", .handler = &Prompt::dismiss },
        Verb { .name = "refresh", .minWords = 1, .usage = "refresh", .handler = &Prompt::refresh },
        Verb { .name = "quit", .minWords = 1, .usage = "quit", .handler = &Prompt::quit },
    };
    return Verbs;
}

Continue Prompt::readAvailable(int fd)
{
    auto chunk = std::array<char, ReadChunk> {};
    auto const got = ::read(fd, chunk.data(), chunk.size());
    if (got < 0)
        return errno == EINTR ? Continue::Yes : Continue::Failed;
    if (got == 0)
        return Continue::Quit;
    _buffer.append(chunk.data(), static_cast<std::size_t>(got));
    auto newline = _buffer.find('\n');
    while (newline != std::string::npos)
    {
        auto const line = _buffer.substr(0, newline);
        _buffer.erase(0, newline + 1);
        if (auto const next = handle(line); next != Continue::Yes)
            return next;
        newline = _buffer.find('\n');
    }
    return Continue::Yes;
}

Continue Prompt::handle(std::string_view line)
{
    auto words = std::vector<std::string_view> {};
    for (auto const word: std::views::split(line, ' '))
        if (!word.empty())
            words.emplace_back(word.begin(), word.end());
    if (words.empty())
        return Continue::Yes;
    auto const verbTable = verbs();
    auto const verb = std::ranges::find(verbTable, words.front(), &Verb::name);
    if (verb == verbTable.end() || words.size() < verb->minWords)
    {
        std::println("verbs: {}", usage());
        return Continue::Yes;
    }
    return std::invoke(verb->handler, this, std::span<std::string_view const>(words).subspan(1));
}

std::string Prompt::usage()
{
    auto text = std::string {};
    for (auto const& verb: verbs())
        text += std::format("{}{}", text.empty() ? "" : " | ", verb.usage);
    return text;
}

Continue Prompt::feed(Input input)
{
    return _driver.feed(std::move(input)) == DriverHealth::Healthy ? Continue::Yes : Continue::Failed;
}

Continue Prompt::list(Words /*words*/)
{
    _rows.clear();
    auto const& engine = _driver.engine();
    for (auto const& repo: _driver.config() ? _driver.config()->repos() : std::span<Repo const> {})
    {
        for (auto const& change: engine.changeRequests(repo.name))
        {
            _rows.push_back(
                ListRow { .repo = repo.name, .item = ChangeRequestRef { change.number }, .url = change.url });
            auto const failing = change.ci == CiState::Failing ? " [CI failing]"sv : ""sv;
            std::println(
                "{:3}  {}  PR #{}{}  {}", _rows.size(), repo.name, change.number, failing, change.title);
        }
        for (auto const& issue: engine.unclaimedIssues(repo.name))
        {
            _rows.push_back(ListRow { .repo = repo.name, .item = issue.item, .url = issue.url });
            std::println("{:3}  {}  {}  {}", _rows.size(), repo.name, itemText(issue.item), issue.title);
        }
        for (auto const& orphan: engine.orphans(repo.name))
            std::println("     orphan: {}", orphanText(orphan));
    }
    if (_rows.empty())
        std::println("nothing listed yet: the first poll may still run (try `refresh`)");
    for (auto const& problem: engine.problems())
        std::println("problem: {}", problem);
    return Continue::Yes;
}

Continue Prompt::dispatch(Words words)
{
    auto const index = rowNumber(words[0]);
    if (!index || *index > _rows.size())
    {
        std::println("no row {} in the last `list`", words[0]);
        return Continue::Yes;
    }
    auto const session = newSessionUuid();
    if (!session)
    {
        std::println("no session UUID could be read");
        return Continue::Yes;
    }
    // The word after the task is the model when it is one of `models`; the rest is the note.
    auto rest = words.subspan(2);
    auto model = std::string {};
    auto const models = _driver.config() ? _driver.config()->models() : std::span<std::string const> {};
    if (!rest.empty() && std::ranges::contains(models, rest.front()))
    {
        model = std::string(rest.front());
        rest = rest.subspan(1);
    }
    auto note = std::optional<std::string> {};
    for (auto const word: rest)
        note = std::format("{}{}{}", note.value_or(""), note ? " " : "", word);
    auto const& row = _rows[*index - 1];
    return feed(Dispatch { .repo = row.repo,
                           .item = row.item,
                           .url = row.url,
                           .task = std::string(words[1]),
                           .model = std::move(model),
                           .note = std::move(note),
                           .session = *session,
                           .wallMs = wallNowMs(),
                           .monoMs = monotonicNowMs() });
}

Continue Prompt::runs(Words /*words*/)
{
    _runRows.clear();
    auto const& engine = _driver.engine();
    if (engine.runs().empty())
    {
        std::println("no runs");
        return Continue::Yes;
    }
    // The core's own run lines, numbered for `end`, `retry` and `dismiss`.
    for (auto const& run: engine.runs())
    {
        _runRows.push_back(run.id);
        std::println("{:3}  {}", _runRows.size(), runLine(run));
    }
    for (auto const& run: engine.runs())
    {
        // OwnRecordsOnly never tells a run whose tab it did not record that the tab is gone.
        auto const* const ending = std::get_if<Ending>(&run.state);
        if (ending != nullptr && ending->step == EndStep::ClosePane && !run.pane)
            std::println(
                "  {} needs you: the prototype lists only the tabs it recorded (Contour's listing has no "
                "tag), so this run stays in ending: close-pane. Close any tab left for it, then remove its "
                "worktree {} and delete the store file {} by hand.",
                run.id,
                run.worktree,
                escapeKey(runKey(run.id)));
    }
    return Continue::Yes;
}

Continue Prompt::act(RunAction action, std::string_view word)
{
    auto const runs = _driver.engine().runs();
    auto run = resolveRun(word, action, _runRows, runs);
    if (!run)
    {
        std::println("{}", refusalText(word, action, run.error(), runs));
        return Continue::Yes;
    }
    // resolveRun() returns the id of one of these runs.
    auto const& taken = *std::ranges::find(runs, *run, &Run::id);
    auto const checkout = checkoutKnowledge(_driver.engine().unknownCheckout(taken));
    if (auto const why = deferralText(action, taken, checkout))
    {
        std::println("{}", *why);
        return Continue::Yes;
    }
    std::println("{}", confirmationText(action, *run));
    auto const monoMs = monotonicNowMs();
    switch (action)
    {
        case RunAction::End: return feed(EndRun { .run = std::move(*run), .monoMs = monoMs });
        case RunAction::Retry: return feed(RetryRemovalOf { .run = std::move(*run), .monoMs = monoMs });
        case RunAction::Dismiss: return feed(DismissRun { .run = std::move(*run), .monoMs = monoMs });
    }
    return Continue::Yes;
}

Continue Prompt::end(Words words)
{
    return act(RunAction::End, words[0]);
}

Continue Prompt::retry(Words words)
{
    return act(RunAction::Retry, words[0]);
}

Continue Prompt::dismiss(Words words)
{
    return act(RunAction::Dismiss, words[0]);
}

Continue Prompt::refresh(Words /*words*/)
{
    return feed(RefreshNow { .monoMs = monotonicNowMs() });
}

Continue Prompt::quit(Words /*words*/)
{
    return Continue::Quit;
}

} // namespace orchestrator::proto
