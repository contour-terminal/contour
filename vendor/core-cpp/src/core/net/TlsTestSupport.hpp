// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `TlsLoopback` -- a TLS socket under test on one end of a real socket pair, a
/// `testing::StrictTlsPeer` on the other, and the loop both run on -- the bounded handshake the
/// `net_tls` cases drive it with, and `ScriptedChainVerifier`, an `IChainVerifier` whose verdict
/// the case chooses.
///
/// Test-only, beside the tests, because it needs Catch2: it is compiled into no module.

#include <core/async/IExecutor.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/ITlsSession.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/NetError.hpp>
#include <core/net/Tls.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/net/testing/StrictTlsPeer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace core::net::testing
{

/// What the peer of a socket-pair end hears when that end is closed with bytes still unread.
enum class UnreadClose : std::uint8_t
{
    Ends,   ///< An end of stream: an AF_UNIX pair on macOS and the BSDs.
    Resets, ///< A reset: an AF_UNIX pair on Linux, and Windows' pair, which is TCP.
};

/// What closing with bytes unread is on this system's socket pair.
#if defined(__linux__) || defined(_WIN32)
inline constexpr auto NativeUnreadClose = UnreadClose::Resets;
#else
inline constexpr auto NativeUnreadClose = UnreadClose::Ends;
#endif

/// How long one handshake may take before a case fails rather than hangs: a regression in a
/// handshake does not fail, it parks a peer waiting for a flight nobody sends.
inline constexpr auto TlsExchangeBound = std::chrono::milliseconds { 10'000 };

/// A real socket pair on a loop: @c transport for the code under test to wrap, @c wire for the
/// strict peer to speak TLS over.
struct TlsLoopback
{
    std::unique_ptr<IoBackend> backend = makeDefaultBackend();
    EventLoop loop { *backend };
    std::unique_ptr<ISocket> transport;  ///< The end the context under test wraps; moved out by it.
    std::unique_ptr<ISocket> wire;       ///< The strict peer's plaintext end.
    std::unique_ptr<StrictTlsPeer> peer; ///< The other end of the TLS conversation.
};

/// Builds a loopback around @p peer, connected and not yet handshaken.
/// @param peer The strict peer, as its factory answered.
/// @return The loopback; the case fails here if the pair or the peer could not be made.
[[nodiscard]] inline std::unique_ptr<TlsLoopback> makeTlsLoopback(
    std::expected<std::unique_ptr<StrictTlsPeer>, std::string> peer)
{
    REQUIRE(peer.has_value());
    auto loopback = std::make_unique<TlsLoopback>();
    auto pair = makeSocketPair(loopback->loop);
    REQUIRE(pair.has_value());
    loopback->transport = std::move(pair->first);
    loopback->wire = std::move(pair->second);
    loopback->peer = std::move(*peer);
    return loopback;
}

/// What each end of one handshake answered.
struct HandshakeOutcome
{
    bool finished = false; ///< Both ends answered in time.
    std::expected<void, NetError> local = std::unexpected(makeNetError(NetErrorCode::Timeout));
    std::expected<void, std::string> peer = std::unexpected(std::string { "never answered" });
};

/// Runs @p socket's handshake against the loopback's peer, both at once, bounded.
/// @param loopback The loopback @p socket was wrapped from.
/// @param socket The TLS socket under test.
/// @return What each end answered.
[[nodiscard]] inline HandshakeOutcome handshakeBoth(TlsLoopback& loopback, ISocket& socket)
{
    auto outcome = HandshakeOutcome {};
    auto const local = [](ISocket* tls, std::expected<void, NetError>* out) -> async::Task<void> {
        *out = co_await tls->handshakeIfNeeded();
    };
    auto const remote =
        [](StrictTlsPeer* peer, ISocket* wire, std::expected<void, std::string>* out) -> async::Task<void> {
        *out = co_await peer->handshake(wire);
    };
    outcome.finished = blockOnWithin(loopback.loop,
                                     allOf(local(&socket, &outcome.local),
                                           remote(loopback.peer.get(), loopback.wire.get(), &outcome.peer)),
                                     TlsExchangeBound);
    return outcome;
}

/// What a @c ScriptedChainVerifier answers.
enum class ChainVerdict : std::uint8_t
{
    Refuse = 0,       ///< Every chain is refused, as a platform verifier refuses an untrusted one.
    RefuseAsProtocol, ///< Refused, but misclassified as @c TlsFailureKind::Protocol.
    Accept,           ///< Every chain is accepted.
};

/// An @c IChainVerifier whose verdict the case chooses, and which records what it was asked.
///
/// It answers from the BLOCKING executor, and finishes there, as the platform verifiers do:
/// a case can therefore hold the verification open -- the blocking executor's @c pending() is 1 --
/// and look at what the socket did meanwhile.
class ScriptedChainVerifier final: public IChainVerifier
{
  public:
    /// The message a refusal carries.
    static constexpr auto RefusalMessage = std::string_view { "scripted refusal" };

    /// @param verdict What every verification answers.
    /// @param blocking Where it runs; the case drains it.
    ScriptedChainVerifier(ChainVerdict verdict, async::IExecutor& blocking) noexcept:
        _verdict { verdict }, _blocking { &blocking }
    {
    }

    [[nodiscard]] async::Task<std::expected<void, TlsFailure>> verify(std::vector<DerCertificate> chain,
                                                                      std::string serverName) override
    {
        co_await async::ResumeOn { *_blocking };
        chains.push_back(std::move(chain));
        serverNames.push_back(std::move(serverName));
        co_return answer();
    }

    std::vector<std::vector<DerCertificate>> chains; ///< Every chain it was asked about, in order.
    std::vector<std::string> serverNames;            ///< The name each one was asked for.

  private:
    [[nodiscard]] std::expected<void, TlsFailure> answer() const
    {
        if (_verdict == ChainVerdict::Accept)
            return {};
        return std::unexpected(TlsFailure { .kind = _verdict == ChainVerdict::RefuseAsProtocol
                                                        ? TlsFailureKind::Protocol
                                                        : TlsFailureKind::Certificate,
                                            .alert = std::nullopt,
                                            .message = std::string { RefusalMessage } });
    }

    ChainVerdict _verdict;
    async::IExecutor* _blocking;
};

} // namespace core::net::testing
