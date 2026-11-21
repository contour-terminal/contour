// SPDX-License-Identifier: Apache-2.0
//
// Test-only: a process runner that answers the agent orchestrator's declared commands with the recorded
// output of the tools they run (extensions/orchestrator/core/testdata), when the test says so.
#pragma once

#include <vtpty/ProcessRunner.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vtextension::testing
{

/// What a declared command of the orchestrator answers, by its first two argv words.
struct FixtureAnswer
{
    std::string_view program; ///< argv[0].
    std::string_view verb;    ///< argv[1].
    std::string_view fixture; ///< Its standard output: a file of the orchestrator's testdata; empty: nothing.
};

/// The answers: `gh` logged in, the agents' hooks installed, the default branch, five pull requests (the
/// recorded list, in its `--jq` projection), the issues, and the worktree list. Every other command exits 0
/// with nothing.
inline constexpr auto OrchestratorFixtureAnswers = std::array {
    FixtureAnswer { .program = "gh", .verb = "auth", .fixture = {} },
    FixtureAnswer { .program = "contour", .verb = "agent", .fixture = "contour/setup-hooks-present.json" },
    FixtureAnswer { .program = "gh", .verb = "repo", .fixture = "github/default-branch.json" },
    FixtureAnswer { .program = "gh", .verb = "pr", .fixture = "github/prs-open.json" },
    FixtureAnswer { .program = "gh", .verb = "issue", .fixture = "github/issues-open.json" },
    FixtureAnswer { .program = "git", .verb = "worktree", .fixture = "git/worktree-list.porcelain-z" },
};

/// What every process whose argv begins with some words answers, ahead of the fixtures: a command the poll
/// does not run, or one whose answer a test changes.
struct ScriptedAnswer
{
    std::vector<std::string> prefix; ///< The words its argv begins with.
    std::string standardOutput {};   ///< What it prints.
    int32_t exitCode = 0;            ///< How it exits.
    /// What the command does to the world besides printing, given its argv: run on the answering thread,
    /// before its completion is reported.
    std::function<void(std::vector<std::string> const&)> effect {};
    std::optional<std::size_t> times {}; ///< How many processes it answers before it is dropped; none: all.
};

/// @param mainCheckout The repository's main checkout, whose `.git` a linked worktree's git names.
/// @return What the removal checks' commands answer in a clean worktree of @p mainCheckout that nothing
///         filters, whose HEAD one ref holds, with no operation in progress and nothing changed: what lets
///         an ending run's worktree be removed. `git-clean` and `git-worktree-remove` print nothing.
[[nodiscard]] inline std::vector<ScriptedAnswer> cleanWorktreeAnswers(std::string_view mainCheckout)
{
    auto const printing = [](std::vector<std::string> prefix, std::string output) {
        return ScriptedAnswer { .prefix = std::move(prefix),
                                .standardOutput = std::move(output),
                                .exitCode = 0,
                                .effect = {},
                                .times = std::nullopt };
    };
    return {
        printing({ "git", "rev-parse", "--path-format=absolute", "--git-common-dir" },
                 std::format("{}/.git\n", mainCheckout)),
        printing({ "git", "ls-files", "-v", "--stage" },
                 "H 100644 e69de29bb2d1d6434b8b29ae775ad8c2e48c5391 0\ta.txt\n"),
        printing({ "git", "for-each-ref", "--count=1", "--contains", "HEAD" },
                 "e6e8346832d4ba7a25fbaa38301c781b5cbb5ebe commit\trefs/remotes/origin/master\n"),
        printing({ "git", "-c", "status.displayCommentPrefix=false" },
                 "HEAD detached at origin/master\nnothing to commit, working tree clean\n"),
        printing({ "git", "status", "--porcelain" }, {}),
    };
}

/// Starts nothing: records each process it is given, and completes the waiting ones with their scripted
/// answer or fixture when answerWaiting() is called -- but for the programs it forwards to a runner that
/// starts them. Thread-safe: start() may run on an extension's lane while the test answers.
class FixtureProcessRunner final: public vtpty::IProcessRunner
{
  public:
    /// Which processes answerWaiting() leaves waiting.
    using HoldBack = std::function<bool(std::vector<std::string> const& argv)>;

    /// @param testdata The orchestrator's testdata directory, which the fixtures are read from.
    explicit FixtureProcessRunner(std::filesystem::path testdata): _testdata { std::move(testdata) } {}

    /// Answers every process @p answer's prefix matches from now on, ahead of every answer scripted before it
    /// and of the fixtures.
    void script(ScriptedAnswer answer)
    {
        auto const lock = std::scoped_lock { _mutex };
        _scripted.push_back(std::move(answer));
    }

    /// Starts every process of @p program on @p runner from now on, instead of answering it.
    /// @param program The argv[0] it starts.
    /// @param runner What starts it; it must outlive this runner.
    /// @param environment Laid over each process's own environment, a name of its replacing one there: what
    ///        keeps the test's machine out of it (a home, a configuration of the test's own).
    void forward(std::string program,
                 vtpty::IProcessRunner& runner,
                 std::vector<std::pair<std::string, std::string>> environment = {})
    {
        auto const lock = std::scoped_lock { _mutex };
        _forwarded = Forwarded { .program = std::move(program),
                                 .runner = &runner,
                                 .environment = std::move(environment) };
    }

    /// @return How many forwarded processes have not ended yet.
    [[nodiscard]] std::size_t forwardedRunning() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _forwardedTokens.size();
    }

    [[nodiscard]] std::expected<vtpty::ProcessToken, vtpty::ProcessStartError> start(
        vtpty::ProcessSpec spec, vtpty::CompletionHandler done) override
    {
        auto const lock = std::scoped_lock { _mutex };
        auto const token = vtpty::ProcessToken { ++_nextToken };
        _started.push_back(spec.argv);
        _startedIn.push_back(spec.workingDirectory ? spec.workingDirectory->resolvedPath() : std::string {});
        if (_forwarded && !spec.argv.empty() && spec.argv.front() == _forwarded->program)
        {
            for (auto const& [name, value]: _forwarded->environment)
            {
                if (auto const same = std::ranges::find(spec.environment, name, &EnvironmentEntry::first);
                    same != spec.environment.end())
                    same->second = value;
                else
                    spec.environment.emplace_back(name, value);
            }
            // Reported under this runner's token, which the forwarded runner's could collide with; forgotten
            // once it ended.
            auto started = _forwarded->runner->start(
                std::move(spec),
                [this, token, done = std::move(done)](vtpty::ProcessToken, vtpty::ProcessExit exit) {
                    {
                        auto const lock = std::scoped_lock { _mutex };
                        std::erase_if(_forwardedTokens,
                                      [token](TokenPair const& pair) { return pair.first == token; });
                    }
                    done(token, std::move(exit));
                });
            if (!started)
                return std::unexpected(started.error());
            _forwardedTokens.emplace_back(token, *started);
            return token;
        }
        _waiting.push_back(Waiting { .token = token, .argv = std::move(spec.argv), .done = std::move(done) });
        return token;
    }

    void cancel(vtpty::ProcessToken token) override
    {
        auto done = vtpty::CompletionHandler {};
        {
            auto const lock = std::scoped_lock { _mutex };
            if (auto const forwarded = std::ranges::find(_forwardedTokens, token, &TokenPair::first);
                forwarded != _forwardedTokens.end())
            {
                _forwarded->runner->cancel(forwarded->second);
                return;
            }
            auto const found = std::ranges::find(_waiting, token, &Waiting::token);
            if (found == _waiting.end())
                return;
            done = std::move(found->done);
            _waiting.erase(found);
        }
        done(token,
             vtpty::ProcessExit { .completion = vtpty::ProcessCompletion::Cancelled,
                                  .exitCode = std::nullopt,
                                  .signal = std::nullopt,
                                  .standardOutput = {},
                                  .standardError = {} });
    }

    /// Completes every waiting process @p holdBack does not hold back, on the caller's thread, with exit 0
    /// and its fixture as its standard output.
    /// @return How many it completed.
    std::size_t answerWaiting(HoldBack const& holdBack = {})
    {
        auto answering = std::vector<Waiting> {};
        {
            auto const lock = std::scoped_lock { _mutex };
            auto kept = std::vector<Waiting> {};
            for (auto& waiting: _waiting)
                (holdBack && holdBack(waiting.argv) ? kept : answering).push_back(std::move(waiting));
            _waiting = std::move(kept);
        }
        for (auto& waiting: answering)
        {
            auto answer = answerFor(waiting.argv);
            if (answer.effect)
                answer.effect(waiting.argv);
            waiting.done(waiting.token,
                         vtpty::ProcessExit { .completion = vtpty::ProcessCompletion::Exited,
                                              .exitCode = answer.exitCode,
                                              .signal = std::nullopt,
                                              .standardOutput = { .bytes = std::move(answer.standardOutput) },
                                              .standardError = {} });
        }
        return answering.size();
    }

    /// @return How many processes were started whose argv begins with @p program @p verb.
    [[nodiscard]] std::size_t startsOf(std::string_view program, std::string_view verb) const
    {
        auto const lock = std::scoped_lock { _mutex };
        return static_cast<std::size_t>(std::ranges::count_if(_started, [&](auto const& argv) {
            return argv.size() >= 2 && argv[0] == program && argv[1] == verb;
        }));
    }

    /// @return The argv of every process started, in order.
    [[nodiscard]] std::vector<std::vector<std::string>> started() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _started;
    }

    /// @return The directory every process was started in, in the order of started(); empty for one started
    ///         in the runner's own.
    [[nodiscard]] std::vector<std::string> startedIn() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _startedIn;
    }

    /// @return How many processes wait for an answer.
    [[nodiscard]] std::size_t waiting() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _waiting.size();
    }

  private:
    struct Waiting
    {
        vtpty::ProcessToken token {};
        std::vector<std::string> argv;
        vtpty::CompletionHandler done;
    };

    using EnvironmentEntry = std::pair<std::string, std::string>;

    struct Forwarded
    {
        std::string program;
        vtpty::IProcessRunner* runner = nullptr;
        std::vector<EnvironmentEntry> environment;
    };

    /// This runner's token of a forwarded process, and the forwarded runner's.
    using TokenPair = std::pair<vtpty::ProcessToken, vtpty::ProcessToken>;

    /// @return What @p argv answers: the latest scripted answer whose prefix it begins with (used up once its
    ///         times are), else its fixture, else nothing.
    [[nodiscard]] ScriptedAnswer answerFor(std::vector<std::string> const& argv)
    {
        {
            auto const lock = std::scoped_lock { _mutex };
            auto const scripted =
                std::ranges::find_if(_scripted | std::views::reverse, [&](ScriptedAnswer const& answer) {
                    // std::ranges::starts_with, which libstdc++ 14 lacks.
                    return std::ranges::mismatch(argv, answer.prefix).in2 == answer.prefix.end();
                });
            if (scripted != std::ranges::end(_scripted | std::views::reverse))
            {
                auto answer = *scripted;
                if (scripted->times && --*scripted->times == 0)
                    _scripted.erase(std::next(scripted).base());
                return answer;
            }
        }
        auto const row = std::ranges::find_if(OrchestratorFixtureAnswers, [&](FixtureAnswer const& answer) {
            return argv.size() >= 2 && argv[0] == answer.program && argv[1] == answer.verb;
        });
        if (row == OrchestratorFixtureAnswers.end() || row->fixture.empty())
            return {};
        auto file = std::ifstream(_testdata / row->fixture, std::ios::binary);
        REQUIRE(file);
        return ScriptedAnswer { .prefix = {},
                                .standardOutput = { std::istreambuf_iterator<char>(file),
                                                    std::istreambuf_iterator<char>() },
                                .exitCode = 0,
                                .effect = {},
                                .times = std::nullopt };
    }

    std::filesystem::path _testdata;
    mutable std::mutex _mutex;
    uint64_t _nextToken = 0;
    std::vector<Waiting> _waiting;
    std::vector<std::vector<std::string>> _started;
    std::vector<std::string> _startedIn;     ///< Where each of @ref _started runs; empty: the runner's own.
    std::vector<ScriptedAnswer> _scripted;   ///< In the order they were scripted; the latest wins.
    std::optional<Forwarded> _forwarded;     ///< The program started for real, if any.
    std::vector<TokenPair> _forwardedTokens; ///< Every forwarded process, by this runner's token.
};

} // namespace vtextension::testing
