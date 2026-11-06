// SPDX-License-Identifier: Apache-2.0
#include <vthost/handover/HandoverCapture.hpp>

#include <vtbackend/state/TerminalStateCodec.hpp>

#ifdef _WIN32
    #include <vtpty/ConPty.hpp>
#else
    #include <vtpty/UnixPty.hpp>
#endif

#include <core/Assert.hpp>

#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

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
        vtpty::AdoptedChild child;                        ///< None for SessionDevice::PtyOnly.
        SessionDevice device = SessionDevice::ChildOnPty; ///< Which of the two cross.
    };

    /// @return What of @p pty crosses: its descriptors on POSIX, its pseudoconsole's handles on Windows; or
    ///         UnsupportedSession for a device that is no PTY of this process, ForeignConsoleHost for a
    ///         pseudoconsole no successor could rebuild.
    [[nodiscard]] std::expected<vtpty::InheritedPty, HandoverError> crossingPtyOf(vtpty::Pty const& pty)
    {
#ifdef _WIN32
        auto const* const conpty = dynamic_cast<vtpty::ConPty const*>(&pty);
        if (conpty == nullptr)
            return std::unexpected(HandoverError::UnsupportedSession);
        return conpty->handoverHandles()
            .transform([&](vtpty::InheritedConPty const& handles) {
                return vtpty::InheritedPty { .pageSize = conpty->pageSize(),
                                             .pixels = conpty->pixelSize(),
                                             .conpty = handles };
            })
            .transform_error([](vtpty::AdoptError error) {
                return error == vtpty::AdoptError::ForeignConsoleHost ? HandoverError::ForeignConsoleHost
                                                                      : HandoverError::UnsupportedSession;
            });
#else
        auto const* const unix = dynamic_cast<vtpty::UnixPty const*>(&pty);
        if (unix == nullptr)
            return std::unexpected(HandoverError::UnsupportedSession);
        return unix->handoverDescriptors();
#endif
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
                                             .device = SessionDevice::PtyOnly };
                auto child = process->handoverChild();
                if (!child)
                    return std::unexpected(HandoverError::UnsupportedSession);
                return SessionCrossing { .pty = std::move(pty),
                                         .child = *std::move(child),
                                         .device = SessionDevice::ChildOnPty };
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

    /// @return Session @p quiesced of @p host as it crosses the exec.
    [[nodiscard]] HandoverSession handoverSessionOf(SessionHost& host,
                                                    QuiescedHostSession quiesced,
                                                    SessionCrossing crossing)
    {
        auto& hosted = *host.hostedSession(quiesced.id);
        // Every hosted session is bound as it is spawned: a missing UUID is the zero one, never a refusal.
        auto const uuid = host.objects().uuidOf(quiesced.id).value_or(vtcontrol::Uuid {});
        auto statedSettings = [&] {
            auto& terminal = hosted.terminal();
            auto const _ = std::lock_guard { terminal };
            return toWireSessionSettings(terminal.settings());
        }();
        return HandoverSession {
            .id = quiesced.id,
            .uuid = uuid,
            .label = std::string { host.objects().labelOf(
                vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Pane, .uuid = uuid }) },
            .profileName = hosted.profileName(),
            .origin = hosted.settingsOrigin(),
            .statedSettings = std::move(statedSettings),
            .pty = std::move(crossing.pty),
            .child = std::move(crossing.child),
            .terminalState = vtbackend::encodeTerminalState(quiesced.state.terminal),
            .carried = std::move(quiesced.state.carried),
            .colorPreference = host.colorPreferenceOf(quiesced.id),
            .interruption = host.interruptionOf(quiesced.id),
            .device = crossing.device,
        };
    }
} // namespace

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
                                                            int leaseFd,
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
        auto image = HandoverImage {
            .daemonInstance = daemonInstance,
            .fromRelease = std::string { release },
            .tree = std::move(quiesced.tree),
            .modelIds = quiesced.modelIds,
            .nextSessionId = quiesced.nextSessionId,
            .objects = host.objects().snapshot(),
            .sessions = {},
            .listeners = { listeners.begin(), listeners.end() },
            .leaseFd = leaseFd,
            .defaultColorPreference = host.defaultColorPreference(),
        };
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
