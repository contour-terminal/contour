// SPDX-License-Identifier: Apache-2.0
#include <vthost/handover/HandoverCapture.hpp>

#include <vtbackend/state/TerminalStateCodec.hpp>

#ifndef _WIN32
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

namespace vthost::handover
{

namespace
{
    /// Whether this platform execs a new image in place, which is what a capture is for.
#ifdef _WIN32
    constexpr auto HasInPlaceExec = false;
#else
    constexpr auto HasInPlaceExec = true;
#endif

    /// What of a session's device crosses the exec as it is: the PTY's descriptors and the child.
    struct SessionCrossing
    {
        vtpty::InheritedPty pty;
        vtpty::AdoptedChild child;
    };

    /// Reads what of @p session crosses the exec, giving up nothing: the host keeps owning it.
    /// @return It; or nullopt for a session whose device is no child process on a PTY of this process.
    [[nodiscard]] std::optional<SessionCrossing> crossingOf(HostedSession const& session)
    {
#ifdef _WIN32
        std::ignore = session;
        return std::nullopt;
#else
        auto const* const process = session.process();
        if (process == nullptr)
            return std::nullopt;
        auto const* const pty = dynamic_cast<vtpty::UnixPty const*>(&process->pty());
        auto child = process->handoverChild();
        if (pty == nullptr || !child)
            return std::nullopt;
        return SessionCrossing { .pty = pty->handoverDescriptors(), .child = *std::move(child) };
#endif
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
        auto settings = [&] {
            auto& terminal = hosted.terminal();
            auto const _ = std::lock_guard { terminal };
            return terminal.settings();
        }();
        return HandoverSession {
            .id = quiesced.id,
            .uuid = uuid,
            .label = std::string { host.objects().labelOf(
                vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Pane, .uuid = uuid }) },
            .profileName = hosted.profileName(),
            .origin = hosted.settingsOrigin(),
            .settings = std::move(settings),
            .pty = std::move(crossing.pty),
            .child = std::move(crossing.child),
            .terminalState = vtbackend::encodeTerminalState(quiesced.state.terminal),
            .carried = std::move(quiesced.state.carried),
        };
    }
} // namespace

std::expected<HandoverImage, HandoverError> captureHandover(SessionHost& host,
                                                            std::span<InheritedListener const> listeners,
                                                            int leaseFd,
                                                            uint64_t daemonInstance,
                                                            std::string_view release)
{
    if constexpr (!HasInPlaceExec)
        return std::unexpected(HandoverError::NotSupported);
    Require(host.connectedClients() == 0);
    Require(host.pumpEventsInFlight() == 0);

    // Before anything stops: a session that cannot cross refuses the whole capture, the daemon untouched.
    auto crossings = std::unordered_map<uint64_t, SessionCrossing> {};
    for (auto const id: host.sessionIds())
    {
        auto crossing = crossingOf(*host.hostedSession(id));
        if (!crossing)
        {
            sessionLog()("handover: session {} runs on no child process of a PTY; it cannot cross", id.value);
            return std::unexpected(HandoverError::UnsupportedSession);
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
