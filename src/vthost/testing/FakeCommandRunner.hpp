// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A CommandRunner that records what it was asked to run and answers from a script.

#include <expected>
#include <functional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <vthost/CommandRunner.hpp>

namespace vthost::testing
{

/// Records each argv and answers it through @ref responder; by default every command succeeds with no output.
class FakeCommandRunner final: public CommandRunner
{
  public:
    /// Decides the answer to one command.
    using Responder =
        std::function<std::expected<CommandOutcome, std::error_code>(std::span<std::string const>)>;

    /// Every command run so far, in order.
    std::vector<std::vector<std::string>> calls {};
    /// How commands are answered; empty means exit 0 with no output.
    Responder responder {};

    /// Answers every command with @p outcome.
    /// @param outcome What each command reports.
    void answerAll(CommandOutcome outcome)
    {
        responder = [outcome = std::move(outcome)](std::span<std::string const>) {
            return outcome;
        };
    }

    /// Fails every command to start, as a missing program does.
    /// @param error Why it could not be started.
    void failToStart(std::error_code error)
    {
        responder = [error](std::span<std::string const>) {
            return std::unexpected(error);
        };
    }

    [[nodiscard]] std::expected<CommandOutcome, std::error_code> run(
        std::span<std::string const> argv) override
    {
        calls.emplace_back(argv.begin(), argv.end());
        if (responder)
            return responder(argv);
        return CommandOutcome {};
    }
};

} // namespace vthost::testing
