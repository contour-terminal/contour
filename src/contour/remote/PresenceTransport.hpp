// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How the sidebar dials a listed daemon. An observer -- a daemon no window is on -- is dialled without
/// anyone being asked anything: ssh in batch mode with a relay that never starts a daemon, a tls host only
/// when its certificate can be verified without a question, a local daemon without starting it. A dial the
/// user asked for (Connect, a jump, Open in New Window) may ask, in the window that acted.

#include <contour/askpass/AskpassPromptRouter.hpp>
#include <contour/cli/HostTransport.hpp>
#include <contour/config/HostUrl.hpp>
#include <contour/window/DaemonLinkLook.hpp>
#include <contour/window/DaemonSection.hpp>

#include <core/async/Task.hpp>

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <vthost/Transport.hpp>
#include <vtworkspace/Primitives.hpp>

namespace vthost
{
class KnownDaemons;
}

namespace contour::remote
{

/// Why a daemon was not dialled, and what its row says about it.
struct DialRefusal
{
    window::DaemonLinkState state; ///< NeedsInput when the dial would have had to ask; Failed otherwise.
    std::string reason;            ///< For the user, shown verbatim.
};

/// What dialling a listed daemon needs of the application. Every member is called on the GUI thread.
struct PresenceTransportDeps
{
    /// The saved host of that name (`hosts:`); null for none.
    std::function<config::HostEntry const*(std::string const& host)> savedHost;
    /// Maps a saved host onto its transport (ContourGuiApp::transportOf).
    std::function<std::expected<cli::HostTransport, std::string>(std::string const& host,
                                                                 config::HostEntry const& entry,
                                                                 askpass::AskpassRouteId route,
                                                                 cli::SshPrompts prompts,
                                                                 cli::RemoteStart remoteStart)>
        openHost;
    /// The control socket of the local daemon of that label (vthost::muxSocketPath()).
    std::function<std::filesystem::path(std::string const& label)> localSocketOf;
    /// Where tls daemons are pinned; borrowed, and must outlive every dial. Null pins nothing.
    vthost::KnownDaemons const* knownDaemons = nullptr;
    /// The askpass route of the window that shows a workspace (its window id).
    std::function<askpass::AskpassRouteId(vtworkspace::WorkspaceId)> routeOf;
};

/// The transport an observer dials @p daemon with: one that never asks the user anything and never starts
/// a daemon.
///
/// A local daemon is connected without being started; a saved ssh host is dialled in batch mode with
/// cli::RemoteStart::Never (see cli::makeHostTransport() for a host's own `remote_command:`); a saved tls
/// host only when its `tls_ca` or a pinned fingerprint verifies the daemon.
/// @param daemon The daemon.
/// @param deps What dialling needs.
/// @return The transport; or NeedsInput for a tls host whose daemon only the user could vouch for, without
///         anything dialled; or Failed for a daemon that cannot be dialled as configured, such as a pin file
///         that cannot be read, or a `link:` daemon, which only the window on it reaches.
[[nodiscard]] std::expected<std::shared_ptr<vthost::ITransportFactory>, DialRefusal> observerTransport(
    window::DaemonRef const& daemon, PresenceTransportDeps const& deps);

/// The transport the observer of a daemon the user asked to connect to (Connect) goes through: its first dial
/// may ask, the host's prompts shown in the window that shows @p askingWindow; every later dial -- a
/// reconnect after the connection was lost -- is an observer's, as observerTransport() makes one, and asks
/// no one. No dial of it starts a daemon: an observer never does, and a local one is started by a verb of
/// its own.
/// @param daemon The daemon.
/// @param askingWindow A workspace of the window the user acted in.
/// @param deps What dialling needs.
/// @return The transport; or Failed for a daemon that cannot be dialled as configured.
[[nodiscard]] std::expected<std::shared_ptr<vthost::ITransportFactory>, DialRefusal> interactiveTransport(
    window::DaemonRef const& daemon,
    vtworkspace::WorkspaceId askingWindow,
    PresenceTransportDeps const& deps);

/// What a failed connection's transport failure says of the link.
/// @param kind The last transport failure.
/// @return NeedsInput for a credential or an unknown host key; NotRunning for an absent daemon; Failed for
///         what the user must fix first; nullopt for a failure that may pass and is retried.
[[nodiscard]] std::optional<window::DaemonLinkState> linkStateOfFailure(
    vthost::TransportErrorKind kind) noexcept;

/// An observer's transport: wraps the one it dials through, and remembers how its last dial failed, so that a
/// link that gave up can say why.
///
/// A dial that finds no daemon is handed on as Refused, which ends the reconnects (vthost::retryOf()), and
/// recorded as DaemonAbsent, which the row reads as NotRunning: an observer whose daemon stopped dials it no
/// more by itself. Only while the daemon's upgrade is waited out (holdDaemonStart()) is an absent daemon
/// dialled again.
///
/// Threading: connect() and holdDaemonStart() run on the loop's thread; lastFailure() may be read from any
/// thread.
class FailureRecordingTransport final: public vthost::ITransportFactory
{
  public:
    /// @param inner The transport every dial goes through.
    explicit FailureRecordingTransport(std::shared_ptr<vthost::ITransportFactory> inner);

    /// Dials through the inner transport, and records the outcome.
    /// @param loop The loop the connection runs on.
    /// @return What the inner transport returned.
    [[nodiscard]] core::async::Task<
        std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>>
    connect(core::net::EventLoop& loop) override;

    /// @return The inner transport's description.
    [[nodiscard]] std::string describe() const override;

    /// Forwards to the inner transport, and waits for an absent daemon until then.
    /// @param until When the hold ends.
    void holdDaemonStart(core::platform::SteadyTimePoint until) override;

    /// @return How the last dial failed; nullopt before any dial, and after one that succeeded.
    [[nodiscard]] std::optional<vthost::TransportErrorKind> lastFailure() const noexcept;

  private:
    /// The last failure's kind plus one; zero for none. Shared with each dial, which may outlive this object.
    using FailureCell = std::atomic<std::uint8_t>;

    std::shared_ptr<vthost::ITransportFactory> _inner;
    std::shared_ptr<FailureCell> _lastFailure;
    /// When the daemon-start hold in force ends; the loop's thread only.
    core::platform::SteadyTimePoint _holdUntil = core::platform::SteadyTimePoint::min();
};

} // namespace contour::remote
