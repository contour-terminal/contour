// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `detail::runConnectFlow` — the platform-free half of every connector.
///
/// Imported from fastcached's `Net/ConnectFlow.{hpp,cpp}` at
/// `0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`.

#include <core/async/Task.hpp>
#include <core/net/IAsyncAddressResolver.hpp>
#include <core/net/IConnector.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/KeepAlive.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/detail/StreamSocketOptions.hpp>
#include <core/platform/Clock.hpp>

#include <cstdint>
#include <string>

namespace core::net
{

class EventLoop;

namespace detail
{

    /// One candidate dial attempt, supplied by the platform half.
    ///
    /// @p endpoint is taken **by value**: this produces a coroutine, whose frame outlives the
    /// call expression, so a `ResolvedEndpoint const&` could dangle at the first suspend. A
    /// 144-byte copy per attempt is not worth a hazard.
    ///
    /// A function pointer plus an opaque state pointer rather than a `std::function`, matching
    /// @c ReadinessHandler and @c ResultAwaitable's arm callback — the house shape, and no
    /// allocation on a path that runs per dial.
    ///
    /// The socket options travel as a parameter rather than inside @p state: the state is the
    /// connector's own, built once, and folding a per-call answer into it is how a per-call
    /// option quietly becomes a per-connector one — the design @c DialOptions exists to rule out.
    using DialStep = async::Task<SocketResult> (*)(void* state,
                                                   ResolvedEndpoint endpoint,
                                                   platform::SteadyTimePoint deadline,
                                                   StreamSocketOptions options);

    /// Guards the host, budgets the whole call, resolves, tries every candidate in preference
    /// order, and reports the LAST failure.
    ///
    /// Each of those rules was a defect somewhere before it was a rule:
    ///
    /// - **An empty host is refused before the resolver is touched.** The blocking resolver is
    ///   bind-shaped — it passes `AI_PASSIVE` and turns an empty host into the wildcard address,
    ///   which is exactly right for a bind and is not something you can dial. Connecting to
    ///   `0.0.0.0` reaches localhost on Linux, so the mistake would not even be loud.
    /// - **The budget covers the whole call**, resolution and every candidate included. A host
    ///   with both an AAAA and an A record used to be able to take twice what the caller asked
    ///   for, which is a bound that is not one. Resolution is RACED against the deadline rather
    ///   than checked once it returns, because a lookup against a dead nameserver does not
    ///   return for about 30 s, and a budget applied only afterwards is not applied at all.
    /// - **Every candidate is tried, and the LAST failure is what is reported.** A peer whose
    ///   name has both records, on a machine with no IPv6 route, is reachable through the second
    ///   — and a dial that gave up after the first would report a healthy peer as down for a
    ///   reason that is about this machine.
    ///
    /// @param resolver The name-resolution seam; must not be null.
    /// @param loop Where a suspended resolution resumes. Null means the caller is on a thread
    ///        that may block, and nothing here will suspend.
    /// @param clock The source of the deadline. Injected so the total-budget rule is a
    ///        @c platform::ManualClock unit test rather than a sleep. Null means no budget at all.
    ///        With a @p loop it must be that loop's own clock: the deadline is armed there.
    /// @param host The target host, unbracketed. By value, for the coroutine-frame reason
    ///        @c IConnector::connect documents.
    /// @param port The target port in host byte order.
    /// @param options The total budget — non-positive means no deadline of ours, leaving the
    ///        platform's own — and whether each candidate socket is armed with keepalive.
    /// @param dial The per-candidate attempt.
    /// @param dialState The opaque pointer handed to @p dial.
    /// @return The connected socket, or why no candidate produced one: @c NetErrorCode::Timeout
    ///         when the budget ran out, during resolution included.
    /// @throws async::OperationCancelled if the awaiting flow's stop token is stopped, while
    ///         resolving or while dialling. A cancel from the flow's own token unwinds.
    [[nodiscard]] async::Task<SocketResult> runConnectFlow(IAsyncAddressResolver* resolver,
                                                           EventLoop* loop,
                                                           platform::IClock* clock,
                                                           std::string host,
                                                           std::uint16_t port,
                                                           DialOptions options,
                                                           DialStep dial,
                                                           void* dialState);

} // namespace detail

} // namespace core::net
