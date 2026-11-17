// SPDX-License-Identifier: Apache-2.0
//
// Test-only: a process runner that answers the agent orchestrator's declared commands with the recorded
// output of the tools they run (extensions/orchestrator/core/testdata), when the test says so.
#pragma once

#include <vtpty/ProcessRunner.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
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

/// Starts nothing: records each process it is given, and completes the waiting ones with their fixture when
/// answerWaiting() is called. Thread-safe: start() may run on an extension's lane while the test answers.
class FixtureProcessRunner final: public vtpty::IProcessRunner
{
  public:
    /// Which processes answerWaiting() leaves waiting.
    using HoldBack = std::function<bool(std::vector<std::string> const& argv)>;

    /// @param testdata The orchestrator's testdata directory, which the fixtures are read from.
    explicit FixtureProcessRunner(std::filesystem::path testdata): _testdata { std::move(testdata) } {}

    [[nodiscard]] std::expected<vtpty::ProcessToken, vtpty::ProcessStartError> start(
        vtpty::ProcessSpec spec, vtpty::CompletionHandler done) override
    {
        auto const lock = std::scoped_lock { _mutex };
        auto const token = vtpty::ProcessToken { ++_nextToken };
        _started.push_back(spec.argv);
        _waiting.push_back(Waiting { .token = token, .argv = std::move(spec.argv), .done = std::move(done) });
        return token;
    }

    void cancel(vtpty::ProcessToken token) override
    {
        auto done = vtpty::CompletionHandler {};
        {
            auto const lock = std::scoped_lock { _mutex };
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
            waiting.done(waiting.token,
                         vtpty::ProcessExit { .completion = vtpty::ProcessCompletion::Exited,
                                              .exitCode = 0,
                                              .signal = std::nullopt,
                                              .standardOutput = { .bytes = answerFor(waiting.argv) },
                                              .standardError = {} });
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

    [[nodiscard]] std::string answerFor(std::vector<std::string> const& argv) const
    {
        auto const row = std::ranges::find_if(OrchestratorFixtureAnswers, [&](FixtureAnswer const& answer) {
            return argv.size() >= 2 && argv[0] == answer.program && argv[1] == answer.verb;
        });
        if (row == OrchestratorFixtureAnswers.end() || row->fixture.empty())
            return {};
        auto file = std::ifstream(_testdata / row->fixture, std::ios::binary);
        REQUIRE(file);
        return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    }

    std::filesystem::path _testdata;
    mutable std::mutex _mutex;
    uint64_t _nextToken = 0;
    std::vector<Waiting> _waiting;
    std::vector<std::vector<std::string>> _started;
};

} // namespace vtextension::testing
