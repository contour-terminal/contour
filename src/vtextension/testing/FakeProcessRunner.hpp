// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ProcessRunner.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vtextension::testing
{

/// A process runner that runs nothing: it records each ProcessSpec it is given, and completes a process
/// when the test says so, on the test's thread.
class FakeProcessRunner final: public vtpty::IProcessRunner
{
  public:
    /// One start() the runner accepted.
    struct Started
    {
        vtpty::ProcessToken token;     ///< What start() returned.
        vtpty::ProcessSpec spec;       ///< What it was asked to run.
        vtpty::CompletionHandler done; ///< Called by complete(); empty once it was.
    };

    /// Called by cancel(), on the canceller's thread, after recording the token.
    std::function<void(vtpty::ProcessToken)> onCancel;

    /// What start() answers instead of starting, until reset to std::nullopt.
    std::optional<vtpty::ProcessStartError> startError;

    [[nodiscard]] std::expected<vtpty::ProcessToken, vtpty::ProcessStartError> start(
        vtpty::ProcessSpec spec, vtpty::CompletionHandler done) override
    {
        if (startError)
            return std::unexpected(*startError);
        auto const token = vtpty::ProcessToken { ++_nextToken };
        _started.push_back(Started { .token = token, .spec = std::move(spec), .done = std::move(done) });
        return token;
    }

    void cancel(vtpty::ProcessToken token) override
    {
        _cancelled.push_back(token);
        if (onCancel)
            onCancel(token);
    }

    /// Completes the @p index'th process started, as the runner's thread would.
    /// @param index Which start(), counted from 0.
    /// @param exit How it ended.
    void complete(std::size_t index, vtpty::ProcessExit exit)
    {
        auto done = std::exchange(_started.at(index).done, {});
        done(_started.at(index).token, std::move(exit));
    }

    /// Completes the @p index'th process started as having exited 0 with the given output.
    void exitWith(std::size_t index, std::string standardOutput, std::string standardError = {})
    {
        complete(index,
                 vtpty::ProcessExit { .completion = vtpty::ProcessCompletion::Exited,
                                      .exitCode = 0,
                                      .signal = std::nullopt,
                                      .standardOutput = { .bytes = std::move(standardOutput) },
                                      .standardError = { .bytes = std::move(standardError) } });
    }

    /// @return Every start() accepted, in order.
    [[nodiscard]] std::vector<Started> const& started() const noexcept { return _started; }

    /// @return Every token cancel() was called with, in order.
    [[nodiscard]] std::vector<vtpty::ProcessToken> const& cancelled() const noexcept { return _cancelled; }

  private:
    uint64_t _nextToken = 0;
    std::vector<Started> _started;
    std::vector<vtpty::ProcessToken> _cancelled;
};

} // namespace vtextension::testing
