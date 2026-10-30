// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A ServiceBackend that registers nothing: it reports a scripted status and counts what it was asked.

#include <expected>
#include <memory>
#include <string>
#include <utility>

#include <vthost/ServiceControl.hpp>

namespace vthost::testing
{

/// What a FakeServiceBackend was asked, shared with the test that scripted it: the backend itself is moved
/// into the code under test.
struct FakeServiceLog
{
    int starts = 0; ///< How often start() was called.
    int stops = 0;  ///< How often stop() was called.
};

/// A ServiceBackend answering from a script. Operations succeed; status() reports @p state.
class FakeServiceBackend final: public ServiceBackend
{
  public:
    /// @param state What status() reports.
    /// @param log Where the calls are counted; outlives the backend.
    /// @param style What launchStyle() reports.
    /// @param commandLine What status() reports the registration to run.
    FakeServiceBackend(ServiceRunState state,
                       FakeServiceLog& log,
                       DaemonLaunchStyle style = DaemonLaunchStyle::Foreground,
                       std::string commandLine = {}):
        _state { state }, _log { log }, _style { style }, _commandLine { std::move(commandLine) }
    {
    }

    [[nodiscard]] std::expected<void, ServiceError> install(ServiceInstallRequest const&) override
    {
        return {};
    }

    [[nodiscard]] std::expected<void, ServiceError> uninstall() override { return {}; }

    [[nodiscard]] std::expected<void, ServiceError> start() override
    {
        ++_log.starts;
        return {};
    }

    [[nodiscard]] std::expected<void, ServiceError> stop() override
    {
        ++_log.stops;
        return {};
    }

    [[nodiscard]] std::expected<ServiceStatus, ServiceError> status() const override
    {
        return ServiceStatus { .state = _state, .commandLine = _commandLine };
    }

    [[nodiscard]] DaemonLaunchStyle launchStyle() const noexcept override { return _style; }

  private:
    ServiceRunState _state;
    FakeServiceLog& _log;
    DaemonLaunchStyle _style;
    std::string _commandLine;
};

} // namespace vthost::testing
