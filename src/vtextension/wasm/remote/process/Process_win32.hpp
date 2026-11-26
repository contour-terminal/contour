// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What only Windows' launcher has: the system functions its AppContainers are ensured through, and where it
/// says what the system refused, handed in, so that a test reads the refusals a launch logs.

#include <functional>
#include <memory>
#include <string_view>

#include <vtextension/wasm/remote/process/AppContainer_win32.hpp>
#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>

namespace vtextension::wasm::remote::process
{

/// @param api What isolated launches' containers are ensured through.
/// @param diagnostics Where a refusal whose launch error cannot say what the system refused
///        (DetailedLaunchErrors) is said, with its step and code (launchRefusalLine()), once per step and
///        code; and a directory this user may not grant (AppContainerProvision).
/// @return The launcher of this system's processes. makeSystemProcessLauncher() makes it with
///         systemAppContainerApi() and Contour's error log.
[[nodiscard]] std::shared_ptr<IProcessLauncher> makeWin32ProcessLauncher(
    AppContainerApi api, std::function<void(std::string_view line)> diagnostics);

} // namespace vtextension::wasm::remote::process
