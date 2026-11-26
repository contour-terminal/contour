// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How a hosting process -- Contour, `contour daemon`, bench-headless -- runs its extensions in extension
/// host processes: one supervisor over the system's launcher, one kind every remote backend shares, and one
/// connector starting the helper. The composition lives as long as the last backend made from it, or anything
/// of theirs that still reaches a host: it ends with them, not with the process.

#include <core/Clock.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>

#include <vtextension/wasm/BackendConfig.hpp>
#include <vtextension/wasm/BuildInstrumentation.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/remote/HostConnector.hpp>
#include <vtextension/wasm/remote/HostRefusals.hpp>
#include <vtextension/wasm/remote/RemoteBackend.hpp>
#include <vtextension/wasm/remote/process/ExtensionHostSupervisor.hpp>
#include <vtextension/wasm/remote/process/HelperLocation.hpp>
#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>

namespace vtextension::wasm::remote::process
{

/// @param instrumentation How a build is instrumented.
/// @return How long a helper that was let go may take to exit on its own in such a build, before it is
///         killed: long enough for a sanitizer's or coverage's report to be written where there is one; no
///         time at all where there is none, as such a helper has nothing to finish.
[[nodiscard]] constexpr std::chrono::milliseconds teardownGraceFor(
    BuildInstrumentation instrumentation) noexcept
{
    switch (instrumentation)
    {
        case BuildInstrumentation::None: return std::chrono::milliseconds { 0 };
        case BuildInstrumentation::Coverage:
        case BuildInstrumentation::Sanitizers: return std::chrono::milliseconds { 2000 };
    }
    return std::chrono::milliseconds { 0 };
}

/// How a hosting process composes its extension hosts.
struct HostCompositionConfig
{
    /// What starts and waits for the helpers: makeSystemProcessLauncher() in Contour.
    std::shared_ptr<IProcessLauncher> launcher;
    core::IClock* clock = nullptr; ///< What the supervisor times by; non-null, outlives the composition.
    /// The extension host executable; or why it was not found, in which case no helper is ever started and
    /// every load fails `HostUnavailable`.
    std::expected<std::filesystem::path, HelperLocationError> helper;
    /// Where why no host serves is logged: logWriter() in Contour.
    remote::HostRefusalLog::Writer refusals;
    /// How long a helper let go may take to exit on its own: as this build is instrumented.
    std::chrono::milliseconds teardownGrace = teardownGraceFor(ThisBuildInstrumentation);
};

/// A supervisor over the configured launcher, the kind every backend made from it shares, and the connector
/// that starts the helper through the supervisor.
class HostComposition: public std::enable_shared_from_this<HostComposition>
{
    struct Passkey
    {
        explicit Passkey() = default;
    };

  public:
    /// Starts the supervisor, unless there is no helper to supervise. A supervisor that cannot start leaves
    /// every load failing `HostUnavailable`, and says why, once.
    /// @param config Its launcher, clock, helper, log and teardown grace.
    /// @return The composition.
    [[nodiscard]] static std::shared_ptr<HostComposition> create(HostCompositionConfig const& config);

    /// Use create(). Public for std::make_shared only.
    HostComposition(Passkey,
                    std::chrono::milliseconds teardownGrace,
                    std::unique_ptr<ExtensionHostSupervisor> supervisor,
                    std::unique_ptr<remote::IHostConnector> connector);

    HostComposition(HostComposition const&) = delete;
    HostComposition(HostComposition&&) = delete;
    HostComposition& operator=(HostComposition const&) = delete;
    HostComposition& operator=(HostComposition&&) = delete;
    ~HostComposition();

    /// Creates a remote backend of this composition's kind, which holds the composition for as long as it,
    /// or anything of it, may start a host.
    /// @param config Its budgets and lockdown requirement; a connector it names is used instead of the
    ///        composition's, which it otherwise is given.
    /// @return The backend.
    [[nodiscard]] std::unique_ptr<IWasmBackend> backend(BackendConfig config);

    /// Test hook.
    /// @return The supervisor; null where none runs, as there is no helper or it could not start.
    [[nodiscard]] ExtensionHostSupervisor const* supervisor() const noexcept { return _supervisor.get(); }

    /// Test hook.
    /// @return How long a helper let go may take to exit on its own.
    [[nodiscard]] std::chrono::milliseconds teardownGrace() const noexcept { return _teardownGrace; }

  private:
    std::chrono::milliseconds _teardownGrace;
    std::shared_ptr<RemoteKindState> _kind;
    std::unique_ptr<ExtensionHostSupervisor> _supervisor;
    std::unique_ptr<remote::IHostConnector> _connector;
};

/// The composition every default backend of this process with the same lockdown requirement shares while one
/// lives: the one made for @p helper before, if a backend still holds it; a new one, over the system's
/// launcher and clock, otherwise. One per requirement, so that a refusal kept for good under one -- a host
/// running unconfined -- does not stop the backends of the other. It caches one helper path per requirement:
/// a process finds its one helper beside itself, at the same path every time. Thread-safe.
/// @param helper The extension host executable; an absolute path.
/// @param lockdown Which hosts its backends drive (BackendConfig::lockdown).
/// @param refusals Where a new composition logs why no host serves; one shared already keeps its own.
/// @return The composition.
[[nodiscard]] std::shared_ptr<HostComposition> sharedHostComposition(std::filesystem::path helper,
                                                                     LockdownRequirement lockdown,
                                                                     remote::HostRefusalLog::Writer refusals);

/// The default backend of a program running its extensions out of process: the remote backend over the
/// shared composition (sharedHostComposition()) of @p helper; with no helper found, one that fails every load
/// `HostUnavailable` without starting anything, and says why once.
/// @param config Its budgets and lockdown requirement; a connector it names is used instead of the
///        composition's.
/// @param helper The extension host executable; or why it was not found.
/// @param refusals Where why no host serves is logged: logWriter() in Contour.
/// @return The backend.
[[nodiscard]] std::unique_ptr<IWasmBackend> makeProcessBackend(
    BackendConfig config,
    std::expected<std::filesystem::path, HelperLocationError> helper,
    remote::HostRefusalLog::Writer refusals);

} // namespace vtextension::wasm::remote::process
