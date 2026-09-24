// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `IConnector` — how a component opens an **outbound** connection, and `DialOptions`, what one
/// dial asks for beyond the address.
///
/// The counterpart to @c IListener. Every server-side path in this library reaches the network
/// through `IListener`/`ISocket`; before this existed the one thing that dialled out did it
/// through a free function with the socket API inlined, which is the shape the dependency
/// injection rule exists to prevent.
///
/// ## Why the dial is a coroutine
///
/// Because **name resolution runs first and is bounded by nothing** — `getaddrinfo` takes no
/// timeout. A blocking dial's argument was always "the caller has nothing to do until the
/// connection exists"; the mistake in it is that the caller has nothing to do and the THREAD has
/// thousands of other connections. A loop thread may call this.
///
/// ## What is deliberately NOT here
///
/// **A per-call loop.** The socket handed back is pinned to one loop, so which loop is a property
/// of the CONNECTOR, chosen where it is constructed. Passing one per call would let a caller
/// obtain a socket wired to a loop other than the one its coroutine runs on — a data race with no
/// symptom until load.
///
/// **A post-connect I/O timeout.** It would be `SO_RCVTIMEO`/`SO_SNDTIMEO`, which bounds a
/// BLOCKING syscall and means nothing to a socket whose reads suspend. Keeping it would hand a
/// caller a bound that does not exist, which is worse than having none. A caller bounds the
/// transfer it actually cares about with @c ISocket::setReceiveDeadline or @c withTimeout.
///
/// Imported from fastcached's `Net/IConnector.hpp` at
/// `0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`.

#include <core/async/Task.hpp>
#include <core/net/IAsyncAddressResolver.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/KeepAlive.hpp>
#include <core/net/NetError.hpp>
#include <core/net/SocketBuffers.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace core::net
{

class EventLoop;

/// What one dial asks for beyond the address.
///
/// **A named descriptor rather than positional parameters, because the next option is not
/// speculative.** Threaded positionally, each new knob is a new argument through every signature
/// and every implementation, and the point at which two of them share a type is the point at
/// which a caller can transpose them silently.
///
/// **And it is per CALL rather than per connector**, which is the half that matters: fixing
/// keepalive at construction would be per-socket only by accident of how often a caller happens
/// to build a connector, and would become per-many-sockets, silently, the first time anyone
/// reused one.
struct DialOptions
{
    /// How long to allow for the **whole call**, name resolution and every candidate address
    /// included. A non-positive value leaves the platform default in place, which may be minutes.
    ///
    /// A total and not a per-candidate budget: a host with both an AAAA and an A record used to
    /// be able to take twice what the caller asked for, which is a bound that is not one.
    std::chrono::milliseconds connectTimeout { 0 };

    /// Whether the connection probes a silent peer.
    ///
    /// Off by default, so a caller that does not ask gets exactly the behaviour every connection
    /// in this library had before the option existed. See @c KeepAlive for what it does and does
    /// not detect.
    KeepAlive keepAlive { KeepAlive::No };

    /// The connected socket's kernel send and receive buffers; unset ones keep the kernel's
    /// value. See @c SocketBufferSizes for what a size does and does not promise.
    SocketBufferSizes buffers {};
};

/// How a component opens an outbound connection.
class IConnector
{
  public:
    IConnector() = default;
    virtual ~IConnector() = default;

    IConnector(IConnector const&) = delete;
    IConnector& operator=(IConnector const&) = delete;
    IConnector(IConnector&&) = delete;
    IConnector& operator=(IConnector&&) = delete;

    /// Opens a connection to @p host : @p port.
    ///
    /// The returned task is **lazy**: nothing is resolved and no descriptor is created until it
    /// is awaited, so discarding it unawaited costs nothing. Once awaited it must be awaited to
    /// completion — destroying a suspended task frees a frame the loop still points into, the
    /// same contract @c ResultAwaitable states for its buffers.
    ///
    /// @param host Hostname or literal address, IPv4 or IPv6, **unbracketed**. Taken by value:
    ///        this is a coroutine, its frame outlives the call expression, and a `string_view`
    ///        parameter would name storage the caller is entitled to destroy before the first
    ///        suspend. The value is also what a threaded resolver hands to a worker, so the copy
    ///        is one the implementation needed anyway.
    /// @param port TCP port in host byte order.
    /// @param options The budget, and whether the connection carries keepalive.
    /// @return The connected socket, or why the attempt did not succeed: @c NetErrorCode::Timeout
    ///         when @c DialOptions::connectTimeout ran out, name resolution included.
    /// @throws async::OperationCancelled if the awaiting flow's stop token is stopped while the dial
    ///         is outstanding, while resolving included.
    [[nodiscard]] virtual async::Task<SocketResult> connect(std::string host,
                                                            std::uint16_t port,
                                                            DialOptions options) = 0;
};

/// Builds the connector for this platform: the sockets it hands back are pinned to @p loop.
///
/// **One connector rather than one per backend**, where fastcached shipped an `EpollConnector`
/// and a `KqueueConnector` that were identical but for the reactor type they named. core-cpp has
/// one @c EventLoop over an @c IoBackend, so the readiness dial is written once and the platform
/// difference lives where it belongs — in the backend.
///
/// @param loop The loop the returned sockets are pinned to, and whose clock measures the budget.
///        Not owned; it must outlive the connector.
/// @param resolver The name-resolution seam. Not owned; it must outlive the connector. A caller
///        with none of its own passes @c defaultAsyncResolver().
/// @return The connector.
[[nodiscard]] std::unique_ptr<IConnector> makeConnector(EventLoop& loop, IAsyncAddressResolver& resolver);

} // namespace core::net
