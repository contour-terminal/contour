// SPDX-License-Identifier: Apache-2.0
//
// The daemon world, composed: every daemon-scoped extension the configuration declares, read, checked and
// resolved, on an engine, a lane, a store and declared processes of its own, and the boot-time ones started.
#pragma once

#include <vtbackend/screen/LocalTime.hpp>

#include <vtpty/DirectoryHandle.hpp>
#include <vtpty/ProcessRunner.hpp>

#include <core/platform/Clock.hpp>
#include <core/platform/FileSystem.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/DaemonExtensionRunner.hpp>
#include <vtextension/DaemonLimits.hpp>
#include <vtextension/ExtensionAutostart.hpp>
#include <vtextension/ExtensionConsent.hpp>
#include <vtextension/ExtensionGrantStore.hpp>
#include <vtextension/ExtensionHost.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/ExtensionPresence.hpp>
#include <vtextension/ExtensionWorkerPool.hpp>
#include <vtextension/FileLocker.hpp>
#include <vtextension/IHostServices.hpp>
#include <vtextension/RootResolution.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension
{

/// The side file, beside the configuration, that remembers the user's answers for daemon-scoped extensions.
/// Never the status-line extensions' file of the same daemon: two stores rewriting one file whole would undo
/// each other's answers. A daemon qualifies it as daemonExtensionGrantsFileName() qualifies its own.
inline constexpr auto DaemonScopedGrantsFileName = std::string_view { "extension-grants.daemon-scoped.txt" };

/// Whether an extension's store holds anything.
enum class StoreContents : uint8_t
{
    Empty,   ///< No key.
    HasKeys, ///< At least one key.
};

/// Whether an extension is started as the daemon starts.
enum class BootStart : uint8_t
{
    No,  ///< It waits until something asks for it.
    Yes, ///< It is started with the daemon.
};

/// @param autostart What the configuration says.
/// @param store What the extension's store holds.
/// @return Yes for AtDaemonStart, and for OnDemand with a store that holds keys: there may be runs to
///         reconcile, and the host cannot read the store to tell. No otherwise.
[[nodiscard]] constexpr BootStart bootStartOf(ExtensionAutostart autostart, StoreContents store) noexcept
{
    if (autostart == ExtensionAutostart::AtDaemonStart || store == StoreContents::HasKeys)
        return BootStart::Yes;
    return BootStart::No;
}

/// Why a declared daemon-scoped extension does not run.
enum class DaemonExtensionProblem : uint8_t
{
    ModuleUnreadable,   ///< No module was found, or it could not be read or loaded.
    ManifestInvalid,    ///< The module's manifest does not parse.
    NotADaemonModule,   ///< The module is a status-line extension: it has no `placement daemon`.
    RootUnresolvable,   ///< A root cannot be resolved from the extension's configuration.
    StoreUnavailable,   ///< Its store's directory cannot be named, or opened as its single owner.
    RuntimeUnavailable, ///< The WebAssembly runtime could not be brought up.
};

/// One daemon-scoped extension as the configuration declares it (`daemon.extensions.<name>`), in no
/// configuration's own types.
struct DaemonExtensionDeclaration
{
    std::string name; ///< The key under `daemon.extensions`.
    std::filesystem::path
        modulePath; ///< The module on disk; empty names a bundled extension, found by @ref name.
    ExtensionAutostart autostart = ExtensionAutostart::OnDemand; ///< When it is started.
    std::string configJson = "{}";        ///< Its `config` subtree, as JSON: what `load` is given.
    std::filesystem::path storeDirectory; ///< Its durable store's directory; empty when none can be named.
};

/// Why one declaration does not run.
struct DeclarationProblem
{
    std::string name;               ///< The extension.
    DaemonExtensionProblem problem; ///< What is wrong.
    std::string message;            ///< For the daemon's log; names `daemon.extensions.<name>`.
};

/// A daemon-scoped extension waiting for the user's answer, for the manifest and roots it resolved.
struct PendingConsent
{
    std::string name;         ///< What the configuration calls it.
    std::string grantDigest;  ///< What the answer is recorded for (grantDigestOf()).
    ConsentQuestion question; ///< What to ask (daemonConsentQuestion()).
};

/// One daemon-scoped extension that composed.
struct DaemonExtensionEntry
{
    std::string name;                                           ///< What the configuration calls it.
    DaemonExtensionPhase phase = DaemonExtensionPhase::Stopped; ///< Where it stands.
    std::string grantDigest;                                    ///< What its grant is recorded for.
    /// Why it could not run, neutralized for display, while it is Failed; empty otherwise.
    std::string problem;
};

/// What a DaemonExtensionRuntime is: fixed for its whole life. A changed configuration takes effect at the
/// next daemon start.
struct DaemonExtensionRuntimeConfig
{
    std::vector<DaemonExtensionDeclaration> declarations; ///< In the configuration's order.
    std::filesystem::path
        grantsFile;   ///< Where the user's answers are kept (@see DaemonScopedGrantsFileName).
    std::string home; ///< The user's home directory: what `~` in a root stands for.
    std::vector<std::string> inheritedEnvironment; ///< The daemon's environment, as `NAME=value`.
    std::vector<std::filesystem::path>
        bundledDirectories;                       ///< Where a declaration without a module is looked for.
    ExtensionLimits limits = daemonWorldLimits(); ///< What each extension may cost.
};

/// What a DaemonExtensionRuntime borrows; each must outlive it.
struct DaemonExtensionRuntimeServices
{
    wasm::IWasmBackend& backend;                    ///< What the engine is created from.
    vtpty::IProcessRunner& runner;                  ///< Runs the declared processes.
    vtpty::IDirectoryOpener& directories;           ///< Opens their working directories.
    core::platform::FileSystem const& fileSystem;   ///< Where modules, grants and stores are.
    IFileLocker const& locker;                      ///< Takes each store's directory lock.
    core::platform::IWallClock const& wallClock;    ///< The wall-clock time `now` answers with.
    vtbackend::LocalTimeConverter const& localTime; ///< The time zone each `load` reads its UTC offset in.
    IExtensionPresence& presence;                   ///< Where summaries are shown.
    /// Called for every message an extension logs, on its lane; must be cheap.
    std::function<void(std::string_view name, LogLevel level, std::string_view message)> onLog;
};

/// The daemon world of one daemon: owns its EngineProvider, a one-worker ExtensionWorkerPool (its lane), the
/// grants, and a store and a DeclaredProcesses per extension.
///
/// Constructed complete: every declaration is read, checked and resolved, and the boot-time extensions
/// (bootStartOf()) that the user granted are started, before create() returns. An extension is granted for
/// its manifest and the directories its roots resolved to (grantDigestOf()). Only a granted extension that is
/// started acquires the engine and has its module loaded (on the calling thread, prepared for the runtime);
/// one not granted holds neither, and waits for the user's consent.
///
/// An extension that was to start but is not granted waits for the user's answer (pending()); answer()
/// records it, and starts the extension once granted. revoke() forgets the answer and asks again.
///
/// start(), status(), pending(), answer(), revoke() and stop() are for the owner's thread.
class DaemonExtensionRuntime
{
    struct Extension;

    /// Lets only create() construct one, through std::make_unique.
    struct Key
    {
        explicit Key() = default;
    };

  public:
    /// The runtime, and why the declarations that do not run do not.
    struct Composed
    {
        std::unique_ptr<DaemonExtensionRuntime> runtime; ///< Never null.
        std::vector<DeclarationProblem> problems;        ///< One per declaration that does not run.
    };

    /// Composes the daemon world.
    /// @param config What it is.
    /// @param services What it borrows.
    /// @return The runtime, its boot-time extensions started, and the declarations' problems -- a boot-time
    ///         extension whose module cannot be loaded among them.
    [[nodiscard]] static Composed create(DaemonExtensionRuntimeConfig config,
                                         DaemonExtensionRuntimeServices services);

    /// For create() only.
    DaemonExtensionRuntime(Key key,
                           DaemonExtensionRuntimeConfig const& config,
                           DaemonExtensionRuntimeServices services);

    /// Stops, as stop() does.
    ~DaemonExtensionRuntime();

    DaemonExtensionRuntime(DaemonExtensionRuntime const&) = delete;
    DaemonExtensionRuntime(DaemonExtensionRuntime&&) = delete;
    DaemonExtensionRuntime& operator=(DaemonExtensionRuntime const&) = delete;
    DaemonExtensionRuntime& operator=(DaemonExtensionRuntime&&) = delete;

    /// Starts an extension on demand: one the user granted is run, a disabled one restarted; one not granted
    /// waits for the user's consent. Does nothing for a name that does not run, or once stopped. One whose
    /// module cannot be loaded is logged, and Failed until it is started again (entries()).
    /// @param name What the configuration calls it.
    void start(std::string_view name);

    /// @param name What the configuration calls an extension.
    /// @return Where its runner stands; nullopt while it does not run.
    [[nodiscard]] std::optional<DaemonRunnerStatus> status(std::string_view name) const;

    /// @return Every extension that composed, in the configuration's order. None once stopped.
    [[nodiscard]] std::vector<DaemonExtensionEntry> entries() const;

    /// @return The extensions that were to start and wait for the user's answer, in the configuration's
    ///         order. None once stopped.
    [[nodiscard]] std::vector<PendingConsent> pending() const;

    /// Records the user's answer to a pending() question, and starts the extension once it is granted. An
    /// answer to no pending question -- another digest, a name that waits for nothing, a dismissal
    /// (GrantState::Pending) -- is ignored, as everything is once stopped. A module that cannot be loaded is
    /// reported as start() reports it.
    /// @param name What the configuration calls the extension.
    /// @param grantDigest The PendingConsent::grantDigest the user answered.
    /// @param answer What the user answered.
    void answer(std::string_view name, std::string_view grantDigest, GrantState answer);

    /// Forgets the user's answer for the extension's current module and roots. One that runs is stopped --
    /// its instance destroyed, the processes it started cancelled and their results dropped -- and is
    /// pending() again; one that does not run is asked only when it is next started. Does nothing for a name
    /// that does not compose, or once stopped.
    /// @param name What the configuration calls the extension.
    void revoke(std::string_view name);

    /// Stops every extension for good: detaches the runners, joins the lane (each runner's last job runs),
    /// then cancels the processes still in flight and releases what the runners retired. Idempotent.
    void stop() noexcept;

  private:
    /// Reads, checks and resolves @p declaration.
    /// @return The extension, ready to run; or why it does not.
    [[nodiscard]] std::expected<std::unique_ptr<Extension>, DeclarationProblem> compose(
        DaemonExtensionDeclaration declaration, DaemonExtensionRuntimeConfig const& config);

    /// Runs @p extension if the user granted it, loading its module first; otherwise leaves it waiting for
    /// consent, holding no engine and no loaded module.
    /// @return Nothing; or why the granted extension cannot run.
    [[nodiscard]] std::expected<void, DeclarationProblem> run(Extension& extension);

    /// Runs @p extension as run() does; why it cannot run is logged, and kept as the Failed phase's problem.
    /// Where it then stands is reported.
    void runOrReport(Extension& extension);

    /// @return Where @p extension stands.
    [[nodiscard]] DaemonExtensionPhase phaseOf(Extension const& extension) const;

    /// Tells the presence where @p extension stands, as its current run, unless a runner runs it: a runner
    /// reports its own run's phases.
    void report(Extension const& extension);

    /// @return The extension the configuration calls @p name; null for none.
    [[nodiscard]] Extension* find(std::string_view name) const;

    /// Makes @p extension's declared processes from Extension::processesConfig.
    /// @return Nothing; or why its roots do not fit them.
    [[nodiscard]] std::expected<void, RootError> makeProcesses(Extension& extension);

    /// Loads @p extension's module on the daemon world's engine, on this thread prepared for the runtime.
    /// @return Nothing once it is loaded; or why it cannot be.
    [[nodiscard]] std::expected<void, DeclarationProblem> load(Extension& extension);

    /// Whether the runtime still runs extensions.
    enum class Lifecycle : uint8_t
    {
        Running,
        Stopped,
    };

    DaemonExtensionRuntimeServices _services;
    std::string _home;
    ExtensionLimits _limits;
    EngineProvider _provider;    ///< The daemon world's engine; and where runners retire to.
    ExtensionGrantStore _grants; ///< The user's answers.
    ExtensionWorkerPool _pool;   ///< The lane every extension runs on: one worker, so jobs run in post order.
    LaneId _lane;                ///< The pool's lane, for the runtime's own jobs behind the runners'.
    std::vector<std::unique_ptr<Extension>> _extensions; ///< Last: destroyed before what they reach.
    Lifecycle _lifecycle = Lifecycle::Running;
};

} // namespace vtextension
