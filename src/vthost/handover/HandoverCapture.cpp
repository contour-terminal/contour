// SPDX-License-Identifier: Apache-2.0
#include <vthost/handover/HandoverCapture.hpp>

#include <vtbackend/state/TerminalStateCodec.hpp>

#include <vtpty/InheritedPty.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vthost/Logging.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/SessionSettings.hpp>

namespace vthost::handover
{

namespace
{
    /// What of a session's device crosses the exec as it is: the PTY's descriptors, and the child on it.
    struct SessionCrossing
    {
        vtpty::InheritedPty pty;
        vtpty::AdoptedChild child;                        ///< None but for SessionDevice::ChildOnPty.
        SessionDevice device = SessionDevice::ChildOnPty; ///< What crosses.
        uint64_t keeperSession = 0;                       ///< For SessionDevice::KeeperChild.
    };

    /// What a PTY's refusal to be handed over makes of a capture.
    struct AdoptRefusal
    {
        vtpty::AdoptError adopt; ///< Why the PTY cannot be handed over.
        HandoverError capture;   ///< Why the capture is refused for it.
    };

    /// Each refusal a capture reports as its own; any other is a session that cannot cross
    /// (HandoverError::UnsupportedSession).
    constexpr auto AdoptRefusals = std::array {
        // Distinct, for the user to learn which conpty.dll a pseudoconsole needs.
        AdoptRefusal { .adopt = vtpty::AdoptError::ForeignConsoleHost,
                       .capture = HandoverError::ForeignConsoleHost },
    };

    /// @return Why a capture is refused, for why a session's PTY cannot be handed over (AdoptRefusals).
    [[nodiscard]] HandoverError handoverErrorOfAdopt(vtpty::AdoptError error) noexcept
    {
        auto const row = std::ranges::find(AdoptRefusals, error, &AdoptRefusal::adopt);
        return row != AdoptRefusals.end() ? row->capture : HandoverError::UnsupportedSession;
    }

    /// @return What of @p pty crosses (vtpty::Pty::handover()); or UnsupportedSession for a device that is no
    ///         PTY of this process, ForeignConsoleHost for a pseudoconsole no successor could rebuild.
    [[nodiscard]] std::expected<vtpty::InheritedPty, HandoverError> crossingPtyOf(vtpty::Pty const& pty)
    {
        return pty.handover().transform_error(handoverErrorOfAdopt);
    }

    /// @return What a session whose child crosses as @p kind is to the image that takes it over.
    [[nodiscard]] constexpr SessionDevice deviceOf(vtpty::CrossingKind kind) noexcept
    {
        switch (kind)
        {
            case vtpty::CrossingKind::Adoptable: return SessionDevice::ChildOnPty;
            case vtpty::CrossingKind::Keeper: return SessionDevice::KeeperChild;
            case vtpty::CrossingKind::None: break;
        }
        return SessionDevice::PtyOnly;
    }

    /// Reads what of @p session crosses the exec, giving up nothing: the host keeps owning it.
    /// @return It; or UnsupportedSession for a session whose device is no PTY of this process, or a process
    /// on
    ///         one whose child is gone, ForeignConsoleHost for one on a pseudoconsole no successor could
    ///         rebuild.
    [[nodiscard]] std::expected<SessionCrossing, HandoverError> crossingOf(HostedSession const& session)
    {
        return crossingPtyOf(session.pty())
            .and_then([&](vtpty::InheritedPty pty) -> std::expected<SessionCrossing, HandoverError> {
                // A PTY served with no process on it: what a handover served on without its child, which
                // crosses the same way again.
                auto const* const process = session.process();
                if (process == nullptr)
                    return SessionCrossing { .pty = std::move(pty),
                                             .child = {},
                                             .device = SessionDevice::PtyOnly,
                                             .keeperSession = 0 };
                auto crossing = process->crossing();
                if (crossing.kind == vtpty::CrossingKind::Adoptable && !crossing.child)
                    return std::unexpected(HandoverError::UnsupportedSession);
                // A keeper's child crosses by the keeper's name for it and its pid, which the new image names
                // again rather than adopts.
                auto const device = deviceOf(crossing.kind);
                return SessionCrossing {
                    .pty = std::move(pty),
                    .child = device == SessionDevice::PtyOnly
                                 ? vtpty::AdoptedChild {}
                                 : std::move(crossing.child).value_or(vtpty::AdoptedChild {}),
                    .device = device,
                    .keeperSession = device == SessionDevice::KeeperChild ? crossing.keeperSession : 0,
                };
            });
    }

    /// @return Whether @p session ended, its PTY closed: the quiesce drops it (QuiesceError::Ended), so
    /// nothing
    ///         of it crosses, and nothing of it refuses the capture. On Windows a program's exit closes its
    ///         pseudoconsole and its handles with it; on POSIX the master stays open until the session goes.
    [[nodiscard]] bool hasEnded(HostedSession const& session)
    {
        return session.pty().isClosed();
    }

    /// @return Why a capture was refused, for why the host's sessions could not be quiesced.
    [[nodiscard]] HandoverError handoverErrorOf(HostQuiesceError error) noexcept
    {
        switch (error)
        {
            case HostQuiesceError::SessionStarting: return HandoverError::SessionStarting;
            case HostQuiesceError::InFlightTailOverflowed: break;
        }
        return HandoverError::InFlightTailOverflowed;
    }

    /// @return Session @p quiesced of @p host as it crosses the exec: its structure, and what of it crosses.
    [[nodiscard]] HandoverSession handoverSessionOf(SessionHost const& host,
                                                    QuiescedHostSession quiesced,
                                                    SessionCrossing crossing)
    {
        auto session = sessionStructureOf(host, quiesced.id);
        session.pty = std::move(crossing.pty);
        session.child = std::move(crossing.child);
        session.terminalState = vtbackend::encodeTerminalState(quiesced.state.terminal);
        session.carried = std::move(quiesced.state.carried);
        session.device = crossing.device;
        session.keeperSession = crossing.keeperSession;
        return session;
    }
} // namespace

HandoverSession sessionStructureOf(SessionHost const& host, vtworkspace::SessionId id)
{
    auto const& hosted = *host.hostedSession(id);
    // Every hosted session is bound as it is spawned: a missing UUID is the zero one, never a refusal.
    auto const uuid = host.objects().uuidOf(id).value_or(vtcontrol::Uuid {});
    auto session = HandoverSession {
        .id = id,
        .uuid = uuid,
        .label = std::string { host.objects().labelOf(
            vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Pane, .uuid = uuid }) },
        .profileName = hosted.profileName(),
        .origin = hosted.settingsOrigin(),
        .statedSettings = hosted.statedSettings(),
        .pty = {},
        .child = {},
        .terminalState = {},
        .carried = {},
        .colorPreference = host.colorPreferenceOf(id),
        .interruption = host.interruptionOf(id),
        .device = SessionDevice::PtyOnly,
        .keeperSession = 0,
    };
    if (auto const* const process = hosted.process(); process != nullptr)
    {
        auto crossing = process->crossing();
        session.device = deviceOf(crossing.kind);
        if (session.device == SessionDevice::KeeperChild)
        {
            session.keeperSession = crossing.keeperSession;
            session.child = std::move(crossing.child).value_or(vtpty::AdoptedChild {});
        }
    }
    return session;
}

HandoverImage hostStructureOf(SessionHost const& host)
{
    auto image = HandoverImage {
        .daemonInstance = 0,
        .fromRelease = {},
        .tree = host.model().captureTree(),
        .modelIds = host.model().idState(),
        .nextSessionId = host.nextSessionId(),
        .defaultWorkspace = host.workspaceId().value,
        .objects = host.objects().snapshot(),
        .sessions = {},
        .listeners = {},
        .lease = {},
        .defaultColorPreference = host.defaultColorPreference(),
        .upgradeGeneration = 0,
        .cellPixelSize = host.learnedCellPixelSize(),
    };
    for (auto const id: host.sessionIds())
        image.sessions.push_back(sessionStructureOf(host, id));
    return image;
}

std::expected<void, HandoverError> checkCapturable(SessionHost& host)
{
    // Found now rather than by the capture, which would refuse it only after every client was closed.
    if (host.isAnySessionStarting())
        return std::unexpected(HandoverError::SessionStarting);
    for (auto const id: host.sessionIds())
    {
        auto& session = *host.hostedSession(id);
        if (hasEnded(session))
            continue;
        if (auto const crossing = crossingOf(session); !crossing)
            return std::unexpected(crossing.error());
        auto& terminal = session.terminal();
        auto const _ = std::lock_guard { terminal };
        if (terminal.inFlightTailCompleteness() == vtbackend::TailCompleteness::Overflowed)
            return std::unexpected(HandoverError::InFlightTailOverflowed);
    }
    return {};
}

std::expected<HandoverImage, HandoverError> captureHandover(SessionHost& host,
                                                            std::span<InheritedListener const> listeners,
                                                            vtpty::CrossingHandle lease,
                                                            uint64_t daemonInstance,
                                                            std::string_view release)
{
    Require(host.connectedClients() == 0);
    Require(host.pumpEventsInFlight() == 0);

    // Before anything stops: a session that cannot cross refuses the whole capture, the daemon untouched.
    auto crossings = std::unordered_map<uint64_t, SessionCrossing> {};
    for (auto const id: host.sessionIds())
    {
        if (hasEnded(*host.hostedSession(id)))
            continue; // the quiesce drops it
        auto crossing = crossingOf(*host.hostedSession(id));
        if (!crossing)
        {
            sessionLog()(
                "handover: session {} runs on no PTY a new image could adopt, or its child is gone; it "
                "cannot cross",
                id.value);
            return std::unexpected(crossing.error());
        }
        crossings.emplace(id.value, *std::move(crossing));
    }

    return host.quiesceAll().transform_error(handoverErrorOf).transform([&](QuiescedHost quiesced) {
        // The structure as of the quiesce, which the sessions below are captured at.
        auto image = hostStructureOf(host);
        image.daemonInstance = daemonInstance;
        image.fromRelease = std::string { release };
        image.tree = std::move(quiesced.tree);
        image.modelIds = quiesced.modelIds;
        image.nextSessionId = quiesced.nextSessionId;
        image.defaultWorkspace = quiesced.defaultWorkspace.value;
        image.sessions.clear();
        image.listeners = { listeners.begin(), listeners.end() };
        image.lease = lease;
        // A session that ended meanwhile is not among the quiesced ones: its crossing is dropped.
        for (auto& session: quiesced.sessions)
        {
            auto crossing = std::move(crossings.at(session.id.value));
            image.sessions.push_back(handoverSessionOf(host, std::move(session), std::move(crossing)));
        }
        return image;
    });
}

void rollbackCapture(SessionHost& host)
{
    host.resumeAll();
}

} // namespace vthost::handover
