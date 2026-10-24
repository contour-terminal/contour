// SPDX-License-Identifier: Apache-2.0
#include <vthost/client/DaemonRestart.hpp>

#include <utility>

namespace vthost::client
{

std::expected<void, DaemonRestartFailure> restartDaemon(DaemonControlPort& control, DaemonStarter& starter)
{
    auto const stopped = control.stop(proto::StopReason::UpgradeRestart);
    if (!stopped && stopped.error().code != ControlErrorCode::NotRunning)
        return std::unexpected(DaemonRestartFailure { .code = DaemonRestartError::StopFailed,
                                                      .detail = stopped.error().toString() });
    return starter.start().transform_error([](std::string detail) {
        return DaemonRestartFailure { .code = DaemonRestartError::StartFailed, .detail = std::move(detail) };
    });
}

} // namespace vthost::client
