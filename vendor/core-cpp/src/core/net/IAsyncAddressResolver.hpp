// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `IAsyncAddressResolver` — name resolution a coroutine can await, and the seam a dial injects.
///
/// **The seam is the deliverable, not the thread.** A resolver is the thing a test substitutes;
/// @c ThreadedAddressResolver is merely the implementation that ships. A dial written against
/// this interface can be exercised end to end with no thread and no name server anywhere.
///
/// Imported from fastcached's `Net/IAsyncAddressResolver.hpp` at
/// `0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`.

#include <core/async/Task.hpp>
#include <core/net/NetError.hpp>
#include <core/net/SocketAddress.hpp>

#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace core::net
{

class EventLoop;

/// The candidates for a host, or why there are none.
///
/// A @c NetError rather than the blocking seam's `std::string`, because this feeds a connector:
/// mapping a message onto a code once, here, is what keeps every dial path from inventing its own
/// answer.
using ResolveResult = std::expected<std::vector<ResolvedEndpoint>, NetError>;

/// Builds the one error a failed lookup produces.
///
/// A free function rather than each call site formatting its own, so the message an operator
/// reads is the same whichever dial produced it, and it names what was being looked up —
/// "resolution failed" without the host is the one thing the reader already knew.
/// @param host The host that could not be resolved.
/// @param port The port it was to be paired with.
/// @param why The underlying resolver's own message.
/// @return The structured error, coded @c NetErrorCode::AddressError — "address resolution or
///         parsing failed", in @c NetError.hpp's own words. Not @c NetErrorCode::AddressNotAvail,
///         which that header reserves for a BIND whose address is not local: a caller retrying a
///         DNS failure matches on this code, and one that read a lookup failure as a local bind
///         problem would stop retrying the thing that is worth retrying.
[[nodiscard]] inline NetError resolveFailure(std::string_view host, std::uint16_t port, std::string_view why)
{
    return makeNetError(
        NetErrorCode::AddressError, 0, std::format("cannot resolve {}:{}: {}", host, port, why));
}

/// Name resolution a coroutine can await.
///
/// A seam beside @c IAddressResolver rather than a replacement for it. Binding happens once, at
/// start-up, on a thread that is allowed to block, so the bind path keeps the blocking primitive;
/// this decorates it for the DIAL path, where the caller may be a loop thread carrying thousands
/// of other connections.
///
/// The distinction matters because `getaddrinfo` is the one genuinely unbounded step in opening a
/// connection — it takes no timeout, and a wedged resolver therefore parks whoever called it for
/// as long as the platform's resolver library feels like.
class IAsyncAddressResolver
{
  public:
    IAsyncAddressResolver() = default;
    virtual ~IAsyncAddressResolver() = default;

    IAsyncAddressResolver(IAsyncAddressResolver const&) = delete;
    IAsyncAddressResolver& operator=(IAsyncAddressResolver const&) = delete;
    IAsyncAddressResolver(IAsyncAddressResolver&&) = delete;
    IAsyncAddressResolver& operator=(IAsyncAddressResolver&&) = delete;

    /// Resolves @p host : @p port into candidate endpoints in preference order.
    ///
    /// **An implementation that suspends MUST honour the awaiting flow's stop token**, and that
    /// is a duty of this interface rather than a property of one implementation. A lookup is the
    /// one step of a dial that nothing else can bound — `getaddrinfo` takes no timeout, and a
    /// nameserver that drops packets holds it for the platform's full retry schedule, about 30 s
    /// with glibc's defaults. So a dial bounds resolution the only way it can: by stopping the
    /// flow that awaits it. @c DialOptions::connectTimeout does, and so does
    /// `whenAny(connect, delay)`. A resolver that ignored the stop would make both of them wait
    /// for the lookup anyway, because a race resumes its caller only once every child has
    /// finished.
    ///
    /// Honouring it means: when the token is stopped while the task is suspended, the task
    /// resumes — on @p loop's thread, like any other resumption — and throws
    /// @c async::OperationCancelled, and whatever the lookup later produces is discarded rather
    /// than delivered into a frame that no longer exists. A cancel from the flow's own token
    /// unwinds rather than returning a value, as every loop awaitable's does. An implementation
    /// that never suspends, such as @c InlineAddressResolver, meets the duty trivially.
    ///
    /// @param host Hostname or literal address, unbracketed. Taken **by value** because an
    ///        implementation may hand it to another thread, and because this produces a coroutine
    ///        whose frame outlives the call expression — a `string_view` parameter would name
    ///        storage the caller is entitled to destroy before the first suspend.
    /// @param port TCP port in host byte order.
    /// @param loop Where the result is delivered. A **null** pointer means "resolve inline on
    ///        this thread and never suspend", the same nullable-loop convention
    ///        @c core::net::DelayAwaiter uses, and what keeps an inline resolver drivable by
    ///        @c core::async::syncRun. When non-null the returned task resumes on that loop's
    ///        thread and nowhere else.
    /// @return The candidates, or why the lookup produced none.
    /// @throws async::OperationCancelled if the awaiting flow's stop token is stopped while the
    ///         lookup is outstanding.
    [[nodiscard]] virtual async::Task<ResolveResult> resolve(std::string host,
                                                             std::uint16_t port,
                                                             EventLoop* loop) = 0;
};

/// An async resolver that is not async: it calls the blocking seam inline.
///
/// For callers already on a thread allowed to block — a one-shot CLI, a start-up path, a test.
/// Because it never suspends, a task built on it is never left suspended, which is exactly what
/// @c core::async::syncRun requires.
///
/// Header-only deliberately: a `.cpp` row carrying a thread would put a `pthread_create`
/// reference into a binary that links no thread library.
class InlineAddressResolver final: public IAsyncAddressResolver
{
  public:
    /// @param inner The blocking resolver to delegate to. Must outlive this.
    explicit InlineAddressResolver(IAddressResolver& inner = defaultAddressResolver()) noexcept: _inner(inner)
    {
    }

    /// @copydoc IAsyncAddressResolver::resolve
    ///
    /// @p loop is accepted and ignored: this resolver has nothing to hand back FROM, so honouring
    /// it would mean suspending for no reason.
    [[nodiscard]] async::Task<ResolveResult> resolve(std::string host,
                                                     std::uint16_t port,
                                                     EventLoop* /*loop*/) override
    {
        auto resolved = _inner.resolve(host, port);
        if (!resolved.has_value())
            co_return std::unexpected(resolveFailure(host, port, resolved.error()));
        co_return std::move(*resolved);
    }

  private:
    IAddressResolver& _inner;
};

} // namespace core::net
