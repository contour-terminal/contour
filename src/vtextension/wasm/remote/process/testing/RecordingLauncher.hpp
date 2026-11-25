// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A launcher that decorates another: it records what was launched, how it ended, and from which threads its
/// processes were killed, and can rewrite the command line of every launch after the first -- how a test
/// makes a respawned helper behave unlike the first one.

#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>

namespace vtextension::wasm::remote::process::testing
{

/// Rewrites a launch's command line.
using ArgumentRewrite = std::function<std::vector<std::string>(std::vector<std::string>)>;

/// Records the launches, kills and exits of another launcher. Thread-safe: every record is guarded by one
/// mutex.
class RecordingLauncher final: public IProcessLauncher
{
  public:
    /// @param inner What starts the processes.
    /// @param laterArguments Applied to the command line of every launch after the first; none to leave
    ///        them as they are.
    explicit RecordingLauncher(std::shared_ptr<IProcessLauncher> inner, ArgumentRewrite laterArguments = {}):
        _inner { std::move(inner) },
        _records { std::make_shared<Records>() },
        _laterArguments { std::move(laterArguments) }
    {
    }

    [[nodiscard]] std::expected<std::unique_ptr<IProcessHandle>, LaunchError> launch(
        LaunchSpec const& spec) override
    {
        auto const isLater = [&] {
            auto const lock = std::scoped_lock { _records->mutex };
            return _records->launches++ > 0;
        }();
        auto launched = [&] {
            if (!isLater || !_laterArguments)
                return _inner->launch(spec);
            auto rewritten = spec;
            rewritten.arguments = [original = spec.arguments, rewrite = _laterArguments](
                                      std::span<transport::NativeHandle const> childHandles) {
                return rewrite(original(childHandles));
            };
            return _inner->launch(rewritten);
        }();
        if (!launched)
            return std::unexpected(launched.error());
        auto const lock = std::scoped_lock { _records->mutex };
        _records->processIds.push_back((*launched)->id());
        return std::make_unique<RecordingHandle>(*std::move(launched), _records);
    }

    [[nodiscard]] std::expected<std::unique_ptr<IExitWaitSet>, LaunchError> makeWaitSet(
        core::IClock& clock) override
    {
        return _inner->makeWaitSet(clock);
    }

    /// @return How many launches were asked for, failed ones included.
    [[nodiscard]] std::size_t launches() const
    {
        auto const lock = std::scoped_lock { _records->mutex };
        return _records->launches;
    }

    /// @return The id of every process started, in order.
    [[nodiscard]] std::vector<ProcessId> processIds() const
    {
        auto const lock = std::scoped_lock { _records->mutex };
        return _records->processIds;
    }

    /// @return The thread each kill() ran on, in call order.
    [[nodiscard]] std::vector<std::thread::id> killThreads() const
    {
        auto const lock = std::scoped_lock { _records->mutex };
        return _records->killThreads;
    }

    /// @return How each process reaped so far ended, in reaping order.
    [[nodiscard]] std::vector<ExitStatus> exitStatuses() const
    {
        auto const lock = std::scoped_lock { _records->mutex };
        return _records->exitStatuses;
    }

  private:
    /// What the launcher and its handles record, which a handle may outlive the launcher with.
    struct Records
    {
        mutable std::mutex mutex;
        std::size_t launches = 0;
        std::vector<ProcessId> processIds;
        std::vector<std::thread::id> killThreads;
        std::vector<ExitStatus> exitStatuses;
    };

    /// Forwards to the inner launcher's handle, recording each kill.
    class RecordingHandle final: public IProcessHandle
    {
      public:
        RecordingHandle(std::unique_ptr<IProcessHandle> inner, std::shared_ptr<Records> records) noexcept:
            _inner { std::move(inner) }, _records { std::move(records) }
        {
        }

        [[nodiscard]] ProcessId id() const noexcept override { return _inner->id(); }

        void kill() noexcept override
        {
            {
                auto const lock = std::scoped_lock { _records->mutex };
                _records->killThreads.push_back(std::this_thread::get_id());
            }
            _inner->kill();
        }

        [[nodiscard]] std::expected<std::optional<ExitStatus>, LaunchError> tryReap() noexcept override
        {
            auto status = _inner->tryReap();
            if (status && *status && !_reaped)
            {
                _reaped = *status;
                auto const lock = std::scoped_lock { _records->mutex };
                _records->exitStatuses.push_back(**status);
            }
            return status;
        }

        [[nodiscard]] Waitable waitable() const noexcept override { return _inner->waitable(); }

      private:
        std::unique_ptr<IProcessHandle> _inner;
        std::shared_ptr<Records> _records;
        std::optional<ExitStatus> _reaped; ///< Recorded once: tryReap() returns it again on every later call.
    };

    std::shared_ptr<IProcessLauncher> _inner;
    std::shared_ptr<Records> _records;
    ArgumentRewrite _laterArguments;
};

} // namespace vtextension::wasm::remote::process::testing
