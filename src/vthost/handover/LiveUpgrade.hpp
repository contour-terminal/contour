// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The live upgrade of a daemon: it hands a new release's binary every hosted program's PTY and child, its
/// listeners and its whole state, so that no program notices -- on POSIX by exec'ing it in its own process,
/// on Windows by starting it as a successor process that takes over (ImageHandoff).

#include <vtpty/CrossingHandle.hpp>
#include <vtpty/SandboxInfo.hpp>

#include <core/async/Task.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vthost/handover/HandoverImage.hpp>
#include <vthost/handover/HandoverMemory.hpp>
#include <vthost/handover/ImageHandoff.hpp>
#include <vthost/handover/Readiness.hpp>
#include <vthost/handover/UpgradeError.hpp>
#include <vthost/handover/UpgradeTarget.hpp>

namespace core::net
{
class EventLoop;
}

namespace vthost
{
class ConnectionAcceptor;
class DaemonImageStore;
class ImageLease;
class ImageLeases;
class SessionHost;
} // namespace vthost

namespace vthost::handover
{

class ReadinessProbe;

/// The daemon's side of an upgrade that is not the host's: its clients and its workspace snapshot. An
/// interface, because the connections and the snapshot writer belong to the daemon's runtime, which wires
/// them.
class UpgradeHooks
{
  public:
    UpgradeHooks() = default;
    UpgradeHooks(UpgradeHooks const&) = delete;
    UpgradeHooks(UpgradeHooks&&) = delete;
    UpgradeHooks& operator=(UpgradeHooks const&) = delete;
    UpgradeHooks& operator=(UpgradeHooks&&) = delete;
    virtual ~UpgradeHooks() = default;

    /// Tells every client the daemon is about to become @p toRelease (`DaemonUpgrading`), so that it waits
    /// for the same daemon to come back rather than starting one.
    /// @param toRelease The release the target reported.
    virtual void announceUpgrade(std::string_view toRelease) = 0;

    /// Writes the workspace snapshot now: the safety net should everything after fail. Does not seal the
    /// snapshot writer, since a daemon whose upgrade fails keeps serving and keeps writing it.
    virtual void writeWorkspaceSnapshot() = 0;

    /// Closes every client connection. Each unwinds on the loop; the upgrade waits until the host counts no
    /// client attached (SessionHost::connectedClients()).
    virtual void closeClients() = 0;

    /// @return How many connections closeClients() asked to end in an orderly way that have not ended yet.
    ///         Counts what no attached client does: a Control connection, such as the one the upgrade was
    ///         requested over, whose last answer must reach its peer before the exec takes the descriptor.
    [[nodiscard]] virtual std::size_t endingConnections() const = 0;

    /// Closes every client connection at once, whatever it has queued for its peer: what the upgrade does to
    /// those still attached half way through LiveUpgradeConfig::settleTimeout, since a peer that stopped
    /// reading would hold an orderly close up for as long as it lives.
    virtual void abortClients() = 0;
};

/// How a live upgrade runs.
struct LiveUpgradeConfig
{
    std::string release;         ///< The release this daemon is: what the image says wrote it.
    uint64_t daemonInstance = 0; ///< Which run of the daemon this is, kept across the exec.
    /// How many live upgrades this run has completed, which the image hands on
    /// (HandoverImage::upgradeGeneration).
    uint64_t upgradeGeneration = 0;
    /// This process's command line; the target runs it again, its program replaced by the target's and the
    /// handover argument (HandoverPrefix) appended in place of any it carried.
    std::vector<std::string> argv;
    /// The executable an empty UpgradeTarget names: the daemon's own installed binary, which a package
    /// upgrade replaced; empty for none.
    std::filesystem::path installedExecutable;
    vtpty::SandboxState sandbox = vtpty::SandboxState::Host; ///< The sandbox the daemon runs in.
    /// The configuration file the daemon runs on, which the target must load before anything stops
    /// (ReadinessQuery::configFile); empty to have it load none.
    std::filesystem::path configFile {};
    /// The profile the daemon's sessions run, which the target must resolve; empty for the default one.
    std::string profile {};
    /// Whether the daemon loaded its configuration file. A target that cannot is refused (ConfigUnreadable)
    /// only then: a daemon already on built-in defaults gains nothing from refusing, the target falls back
    /// the same way.
    RunningConfig runningConfig = RunningConfig::LoadedFromFile;
    /// The TLS identities the daemon made up for its listeners, by listener name: each crosses with its
    /// listener (InheritedListener::tlsIdentity), so that the new image presents the very same certificate.
    std::map<std::string, TlsIdentity, std::less<>> tlsIdentities {};
    /// Handles the target keeps that the image does not name: the PTY keeper's channel, which is
    /// close-on-exec for every other child of the daemon. Kept across the exec only.
    std::vector<vtpty::CrossingHandle> alsoCrossing {};
    /// How many times a capture refused for a session still starting is tried in all.
    unsigned sessionStartAttempts = 20;
    /// How long the upgrade waits between two such tries.
    std::chrono::milliseconds sessionStartRetryDelay { 50 };
    /// How long the clients may take to go, and the sessions' pumps to settle, once the upgrade stopped them.
    /// Clients still attached half way through are closed at once (UpgradeHooks::abortClients()).
    std::chrono::milliseconds settleTimeout { 5000 };
    /// How often the upgrade looks whether they have, while it waits.
    std::chrono::milliseconds settlePollInterval { 2 };
    /// The largest image the upgrade writes; a larger one is refused (ImageTooLarge). Never above
    /// MaxHandoverImageSize, which is what the new image reads: a lower ceiling only lets a test reach it.
    uint64_t maxImageSize = MaxHandoverImageSize;
};

/// Whether a LiveUpgrade is under way.
enum class UpgradeState : uint8_t
{
    Idle = 0, ///< None is.
    Running,  ///< run() is under way.
};

/// Upgrades a daemon live: hands the target binary everything it needs in an anonymous memory object
/// (HandoverMemory), and lets it take over (ImageHandoff) -- on POSIX by exec'ing it in this very process,
/// which keeps every hosted program's parent, process group and controlling terminal, on Windows by starting
/// it as a successor process.
///
/// The sequence, every refusal before anything is stopped or announced:
///  1. refuse while another upgrade runs (InProgress), inside a sandbox (Sandboxed);
///  2. resolve the target inside the store and lease its image (NotStaged);
///  3. probe the target, off the loop (ReadinessProbe; TargetUnresponsive, FormatUnreadable, and
///     ConfigUnreadable for a target that cannot load the daemon's configuration or resolve its profile,
///     when the daemon itself loaded it: RunningConfig; TargetCannotAdoptTerminals for a target that says it
///     cannot adopt pseudoconsoles, when a session runs on one);
///  4. refuse a session that could not cross (UnsupportedSession, ForeignConsoleHost, InFlightTailTooLarge;
///     checkCapturable());
///  5. announce the upgrade, write the workspace snapshot, hold every listener, close every client;
///  6. stop every session's pump (SessionHost::stopAllPumps()), wait until no client is attached and the
///     pumps' events are handled, then capture (captureHandover(), the one quiesce that captures each
///     terminal); a session still starting is waited for and tried again
///     (SessionStarting once the tries are spent), a host that does not settle in time is QuiesceTimedOut;
///  7. encode the image, refused when it outgrew what the new image reads (ImageTooLarge), and hand it over
///     (ImageHandoff::handOver()).
///
/// Whatever ends run() from step 5 on but a handoff that succeeded -- a refusal, the handoff's failure, an
/// exception -- is rolled back once, by one guard: every session resumed with what was handed to it
/// meanwhile, and every listener accepts again; the handoff has undone its own part by then. A handoff that
/// succeeded does not return.
class LiveUpgrade
{
  public:
    /// @param loop The daemon's loop, which run() runs on and waits on. Not owned.
    /// @param host The host whose sessions cross. Not owned.
    /// @param store The daemon's image store, where the target lies. Not owned.
    /// @param leases The leases that guard the store's images. Not owned.
    /// @param handoff What hands the image over to the target. Not owned.
    /// @param probe What asks the target whether it can take over. Not owned.
    /// @param hooks The daemon's clients and snapshot. Not owned.
    /// @param listeners Every listener the daemon serves on; each crosses under its name. Not owned.
    /// @param config How the upgrade runs.
    LiveUpgrade(core::net::EventLoop& loop,
                SessionHost& host,
                DaemonImageStore& store,
                ImageLeases& leases,
                ImageHandoff& handoff,
                ReadinessProbe& probe,
                UpgradeHooks& hooks,
                std::span<ConnectionAcceptor* const> listeners,
                LiveUpgradeConfig config);

    /// Upgrades the daemon to @p target. Loop thread; one at a time.
    ///
    /// A coroutine rather than a call that blocks, because the upgrade waits on the loop itself -- for the
    /// clients to go, for the pumps' events to be handled, before a retry -- which a call running inside a
    /// turn of that loop could not.
    /// @param target What to upgrade to.
    /// @return On success it does not complete: the process is the new image, or has ended. Otherwise why not
    ///         -- a refusal, or the handoff's failure -- with the daemon serving as before.
    [[nodiscard]] core::async::Task<std::expected<void, UpgradeError>> run(UpgradeTarget target);

    /// @return Whether an upgrade is under way.
    [[nodiscard]] UpgradeState state() const noexcept { return _state; }

    /// Forgets every listener, which the daemon is about to destroy as it shuts down: an upgrade under way,
    /// unwound only after them, then holds and resumes none, and a later one hands none over. Loop thread.
    void forgetListeners() noexcept;

  private:
    /// The target, resolved and leased.
    struct ResolvedTarget
    {
        std::filesystem::path executable;
        std::unique_ptr<ImageLease> lease; ///< Null for the installed executable, which no image holds.
    };

    /// @return The executable @p target names, its image leased; NotStaged for none.
    [[nodiscard]] std::expected<ResolvedTarget, UpgradeRefusal> resolve(UpgradeTarget const& target) const;

    /// Stops every session's pump, waits for the host to settle and captures it, trying again while a session
    /// is still starting. The caller rolls back what a refusal leaves stopped.
    /// @param lease The target image's lease descriptor or handle, which the image names; none for none.
    /// @return The image, or why not.
    [[nodiscard]] core::async::Task<std::expected<HandoverImage, UpgradeRefusal>> quiesceAndCapture(
        vtpty::CrossingHandle lease);

    /// Waits until no client is attached and the loop has handled what the stopped pumps posted, closing the
    /// clients at once that are still attached half way through.
    /// @return Nothing; QuiesceTimedOut when that took longer than LiveUpgradeConfig::settleTimeout.
    [[nodiscard]] core::async::Task<std::expected<void, UpgradeRefusal>> awaitSettled();

    /// @return Every listener that has a descriptor to hand over, by name.
    [[nodiscard]] std::vector<InheritedListener> crossingListeners() const;

    /// @return The command line the target runs with, but for the handover argument, which the handoff
    ///         appends.
    [[nodiscard]] std::vector<std::string> argvFor(std::filesystem::path const& executable) const;

    core::net::EventLoop& _loop;
    SessionHost& _host;
    DaemonImageStore& _store;
    ImageLeases& _leases;
    ImageHandoff& _handoff;
    ReadinessProbe& _probe;
    UpgradeHooks& _hooks;
    std::vector<ConnectionAcceptor*> _listeners;
    LiveUpgradeConfig _config;
    UpgradeState _state = UpgradeState::Idle;
};

} // namespace vthost::handover
