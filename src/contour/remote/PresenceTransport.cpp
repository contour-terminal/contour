// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/PresenceTransport.hpp>

#include <core/net/EventLoop.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <ranges>
#include <string_view>
#include <utility>

#include <vthost/KnownDaemons.hpp>

namespace contour::remote
{

namespace
{
    using vthost::TransportErrorKind;
    using window::DaemonLinkState;

    using Dialled = std::expected<std::shared_ptr<vthost::ITransportFactory>, DialRefusal>;

    /// What a transport failure says of the link; nullopt for one that is retried.
    struct FailureRow
    {
        TransportErrorKind kind;
        std::optional<DaemonLinkState> state;
    };

    constexpr auto FailureRows = std::to_array<FailureRow>({
        { .kind = TransportErrorKind::Unreachable, .state = std::nullopt },
        { .kind = TransportErrorKind::Refused, .state = DaemonLinkState::Failed },
        { .kind = TransportErrorKind::AuthenticationRequired, .state = DaemonLinkState::NeedsInput },
        { .kind = TransportErrorKind::HostKeyUnknown, .state = DaemonLinkState::NeedsInput },
        { .kind = TransportErrorKind::RemoteProgramMissing, .state = DaemonLinkState::Failed },
        { .kind = TransportErrorKind::ProgramNotFound, .state = DaemonLinkState::Failed },
        { .kind = TransportErrorKind::Protocol, .state = std::nullopt },
        { .kind = TransportErrorKind::Cancelled, .state = DaemonLinkState::Failed },
        { .kind = TransportErrorKind::DaemonAbsent, .state = DaemonLinkState::NotRunning },
        { .kind = TransportErrorKind::Misconfigured, .state = DaemonLinkState::Failed },
        { .kind = TransportErrorKind::HostKeyChanged, .state = DaemonLinkState::Failed },
        { .kind = TransportErrorKind::RemoteProgramTooOld, .state = DaemonLinkState::TooOld },
    });

    static_assert(FailureRows.size() == vthost::TransportErrorKindCount,
                  "FailureRows needs a row for every TransportErrorKind");
    static_assert(std::ranges::all_of(std::views::iota(size_t { 0 }, FailureRows.size()),
                                      [](size_t i) { return FailureRows[i].kind == TransportErrorKind(i); }),
                  "FailureRows must list the kinds in enumerator order");

    /// Whether a dial that may ask no one checks first that a tls host's daemon can be verified unasked.
    enum class UnaskedTrust : uint8_t
    {
        /// Not checked: the dial may ask, or the daemon was vouched for by the dial before it, and a daemon
        /// that cannot be verified fails the handshake without a question.
        Unchecked = 0,
        Required, ///< Checked: a daemon only the user could vouch for is refused as NeedsInput, undialled.
    };

    /// How a dial reaches its daemon.
    struct DialPolicy
    {
        askpass::AskpassRouteId route; ///< Where its prompts are shown.
        /// Whether anyone may be asked: Batch also dials a tls host only when its daemon can be verified
        /// without a question.
        cli::SshPrompts prompts;
        cli::RemoteStart remoteStart; ///< Whether an ssh host's relay may start the daemon there.
        /// Whether a tls host is dialled only once its daemon can be verified without a question.
        UnaskedTrust trust = UnaskedTrust::Unchecked;
    };

    [[nodiscard]] std::unexpected<DialRefusal> refused(DaemonLinkState state, std::string reason)
    {
        return std::unexpected { DialRefusal { .state = state, .reason = std::move(reason) } };
    }

    /// Whether the daemon of a tls host can be verified without asking the user: by its `tls_ca`, or by a
    /// pinned fingerprint. Any other host has nothing to verify ahead of the dial.
    [[nodiscard]] std::expected<void, DialRefusal> verifiableUnasked(std::string_view name,
                                                                     config::HostEntry const& host,
                                                                     vthost::KnownDaemons const* knownDaemons)
    {
        if (host.url.scheme != config::HostScheme::Tls || !host.tlsCa.empty())
            return {};
        if (knownDaemons == nullptr)
            return refused(DaemonLinkState::Failed,
                           std::format("{}: no file to keep pinned daemons in is available", name));
        auto const pin = knownDaemons->pinOf(vthost::pinKey(host.url.host, host.url.port.value_or(0)));
        if (!pin)
            return refused(DaemonLinkState::Failed, std::format("{}: {}", name, pin.error()));
        if (!*pin)
            return refused(DaemonLinkState::NeedsInput,
                           std::format("{}: the daemon's certificate has not been trusted yet", name));
        return {};
    }

    [[nodiscard]] Dialled dialHost(std::string const& name,
                                   DialPolicy const& policy,
                                   PresenceTransportDeps const& deps)
    {
        auto const* host = deps.savedHost(name);
        if (host == nullptr)
            return refused(DaemonLinkState::Failed, std::format("{} is not a saved host", name));
        if (policy.trust == UnaskedTrust::Required)
            if (auto const verifiable = verifiableUnasked(name, *host, deps.knownDaemons); !verifiable)
                return std::unexpected { verifiable.error() };
        return deps.openHost(name, *host, policy.route, policy.prompts, policy.remoteStart)
            .transform([](cli::HostTransport transport) { return std::move(transport.factory); })
            .transform_error([](std::string reason) {
                return DialRefusal { .state = DaemonLinkState::Failed, .reason = std::move(reason) };
            });
    }

    [[nodiscard]] Dialled dial(window::DaemonRef const& daemon,
                               DialPolicy const& policy,
                               PresenceTransportDeps const& deps)
    {
        auto const parsed = window::parseDaemonRef(daemon);
        if (!parsed)
            return refused(DaemonLinkState::Failed, std::format("'{}' names no daemon", daemon.value));
        auto const name = std::string { parsed->second };
        switch (parsed->first)
        {
            case window::DaemonRefKind::Local:
                // No daemon start: an observer only watches, and starting one is a verb of its own.
                return std::make_shared<vthost::UnixTransportFactory>(deps.localSocketOf(name), nullptr);
            case window::DaemonRefKind::Host: return dialHost(name, policy, deps);
            case window::DaemonRefKind::Link: break;
        }
        return refused(DaemonLinkState::Failed,
                       std::format("'{}' is reached only through the window that is on it", daemon.value));
    }

    static_assert(vthost::TransportErrorKindCount < 0xFF, "a kind plus one must fit a FailureCell");

    /// The dial itself, owning what it records into for as long as it takes.
    /// @param holdUntil The daemon-start hold in force as the dial began: an absent daemon is waited for
    ///        while it lasts.
    [[nodiscard]] core::async::Task<
        std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>>
    recordedConnect(std::shared_ptr<vthost::ITransportFactory> inner,
                    std::shared_ptr<std::atomic<std::uint8_t>> lastFailure,
                    core::platform::SteadyTimePoint holdUntil,
                    core::net::EventLoop* loop)
    {
        auto result = co_await inner->connect(*loop);
        lastFailure->store(result ? std::uint8_t { 0 }
                                  : static_cast<std::uint8_t>(std::to_underlying(result.error().kind) + 1),
                           std::memory_order_relaxed);
        // An observer neither starts a daemon nor waits for one to come back: an absent daemon ends its
        // reconnects (Refused is never retried), and its row reads the absence recorded above. Only a daemon
        // that announced its upgrade is waited for, as it comes back by itself.
        if (!result && result.error().kind == TransportErrorKind::DaemonAbsent
            && loop->clock().now() >= holdUntil)
            result.error().kind = TransportErrorKind::Refused;
        co_return result;
    }

    /// The first dial through one transport, every later one through another.
    class FirstDialTransport final: public vthost::ITransportFactory
    {
      public:
        FirstDialTransport(std::shared_ptr<vthost::ITransportFactory> first,
                           std::shared_ptr<vthost::ITransportFactory> later):
            _first { std::move(first) }, _later { std::move(later) }
        {
        }

        [[nodiscard]] core::async::Task<
            std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>>
        connect(core::net::EventLoop& loop) override
        {
            if (_firstDial.exchange(FirstDial::Made, std::memory_order_relaxed) == FirstDial::Pending)
                return _first->connect(loop);
            return _later->connect(loop);
        }

        [[nodiscard]] std::string describe() const override { return _first->describe(); }

        void holdDaemonStart(core::platform::SteadyTimePoint until) override
        {
            _first->holdDaemonStart(until);
            _later->holdDaemonStart(until);
        }

      private:
        /// Whether the first dial was made.
        enum class FirstDial : uint8_t
        {
            Pending = 0,
            Made,
        };

        std::shared_ptr<vthost::ITransportFactory> _first;
        std::shared_ptr<vthost::ITransportFactory> _later;
        std::atomic<FirstDial> _firstDial = FirstDial::Pending;
    };
} // namespace

Dialled observerTransport(window::DaemonRef const& daemon, PresenceTransportDeps const& deps)
{
    return dial(daemon,
                DialPolicy { .route = askpass::AnyWindow,
                             .prompts = cli::SshPrompts::Batch,
                             .remoteStart = cli::RemoteStart::Never,
                             .trust = UnaskedTrust::Required },
                deps);
}

Dialled interactiveTransport(window::DaemonRef const& daemon,
                             vtworkspace::WorkspaceId askingWindow,
                             PresenceTransportDeps const& deps)
{
    auto asking = dial(daemon,
                       DialPolicy { .route = deps.routeOf(askingWindow),
                                    .prompts = cli::SshPrompts::Askpass,
                                    .remoteStart = cli::RemoteStart::Never,
                                    .trust = UnaskedTrust::Unchecked },
                       deps);
    if (!asking)
        return asking;
    // Made now, while the GUI thread may read the configuration: a reconnect runs on the loop's thread. The
    // daemon this dial reaches is vouched for by the time a reconnect is made, so it is not checked first.
    return dial(daemon,
                DialPolicy { .route = askpass::AnyWindow,
                             .prompts = cli::SshPrompts::Batch,
                             .remoteStart = cli::RemoteStart::Never,
                             .trust = UnaskedTrust::Unchecked },
                deps)
        .transform([&](std::shared_ptr<vthost::ITransportFactory> unasked)
                       -> std::shared_ptr<vthost::ITransportFactory> {
            return std::make_shared<FirstDialTransport>(*std::move(asking), std::move(unasked));
        });
}

std::optional<window::DaemonLinkState> linkStateOfFailure(vthost::TransportErrorKind kind) noexcept
{
    return FailureRows[static_cast<size_t>(kind)].state;
}

FailureRecordingTransport::FailureRecordingTransport(std::shared_ptr<vthost::ITransportFactory> inner):
    _inner { std::move(inner) }, _lastFailure { std::make_shared<FailureCell>(std::uint8_t { 0 }) }
{
}

core::async::Task<std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>>
FailureRecordingTransport::connect(core::net::EventLoop& loop)
{
    return recordedConnect(_inner, _lastFailure, _holdUntil, &loop);
}

std::string FailureRecordingTransport::describe() const
{
    return _inner->describe();
}

void FailureRecordingTransport::holdDaemonStart(core::platform::SteadyTimePoint until)
{
    _holdUntil = until;
    _inner->holdDaemonStart(until);
}

std::optional<vthost::TransportErrorKind> FailureRecordingTransport::lastFailure() const noexcept
{
    auto const recorded = _lastFailure->load(std::memory_order_relaxed);
    if (recorded == 0)
        return std::nullopt;
    return static_cast<vthost::TransportErrorKind>(recorded - 1);
}

} // namespace contour::remote
