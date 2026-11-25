// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Test-only: remote backends whose hosts are real extension host processes, composed as a hosting process
/// composes them -- a supervisor over the system's launcher, recorded, and a process connector -- for the
/// tests that start the real helper, or the hostile one.

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <vtextension/wasm/BackendConfig.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/remote/RemoteBackend.hpp>
#include <vtextension/wasm/remote/process/ExtensionHostSupervisor.hpp>
#include <vtextension/wasm/remote/process/ProcessHostConnector.hpp>
#include <vtextension/wasm/remote/process/testing/RecordingLauncher.hpp>

namespace vtextension::wasm::remote::process::testing
{

/// How long a helper that was let go may take to exit on its own, its reports written (D57's grace).
inline constexpr auto HelperTeardownGrace = std::chrono::milliseconds { 2000 };

/// @return A helper's environment: the launcher's default, and this process's sanitizer variables, so that
///         the helper reports as the tests do.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> helperEnvironment();

/// Which helper a composition's backend starts, and what it holds it to.
struct HelperBackendConfig
{
    std::filesystem::path helper;                                          ///< The executable.
    std::vector<std::string> extraArguments {};                            ///< Appended to its command line.
    LockdownRequirement lockdown = LockdownRequirement::AcceptUnsandboxed; ///< Which helpers are driven.
    HostTimeouts timeouts {};                                              ///< Its budgets.
};

/// A supervisor over the system's launcher, recorded, waiting HelperTeardownGrace for each helper let go,
/// and the kind every remote backend built over it shares.
class HelperComposition
{
  public:
    /// Starts the supervisor; aborts the test run if it cannot start.
    HelperComposition();

    /// @return What started every helper, and when.
    [[nodiscard]] RecordingLauncher& recording() const noexcept { return *_recording; }

    /// @return The supervisor.
    [[nodiscard]] ExtensionHostSupervisor& supervisor() const noexcept { return *_supervisor; }

    /// @return The kind of every backend built over it.
    [[nodiscard]] std::shared_ptr<RemoteKindState> const& kind() const noexcept { return _kind; }

    /// @return A connector starting the helper @p config names, through the supervisor.
    [[nodiscard]] std::shared_ptr<ProcessHostConnector> connector(HelperBackendConfig const& config) const;

    /// @return A remote backend whose hosts are the helper @p config names.
    [[nodiscard]] std::unique_ptr<IWasmBackend> backend(HelperBackendConfig const& config) const;

    /// @return How many kills the supervisor sent for @p reason so far, as of everything it was told and
    ///         everything that fell due by now.
    [[nodiscard]] std::size_t killsFor(KillReason reason) const;

    /// Waits until every helper was reaped -- those let go exit on their own -- or the teardown grace and
    /// some passed.
    /// @return Whether none is left.
    [[nodiscard]] bool awaitEveryHelperReaped() const;

  private:
    std::shared_ptr<RecordingLauncher> _recording;
    std::unique_ptr<ExtensionHostSupervisor> _supervisor;
    std::shared_ptr<RemoteKindState> _kind;
};

/// @return The composition the whole test binary shares, as a hosting process has one supervisor.
[[nodiscard]] HelperComposition& binaryComposition();

} // namespace vtextension::wasm::remote::process::testing
