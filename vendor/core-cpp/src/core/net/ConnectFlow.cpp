// SPDX-License-Identifier: Apache-2.0
#include <core/net/ConnectFlow.hpp>

#include <core/async/WhenAny.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/SleepUntil.hpp>

#include <cassert>
#include <cstdint>
#include <format>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace core::net::detail
{

namespace
{

    /// The deadline this whole call must finish inside.
    ///
    /// A non-positive budget means the caller did not ask for one, and `SteadyTimePoint::max()`
    /// is how "no deadline" is spelled everywhere here. A default-constructed time point would
    /// mean *already expired*, which turns an opt-out into an instant failure.
    /// @param clock The time source, or null for no budget.
    /// @param connectTimeout What the caller asked for.
    /// @return The absolute deadline, or `SteadyTimePoint::max()`.
    [[nodiscard]] platform::SteadyTimePoint budgetDeadline(platform::IClock* clock,
                                                           std::chrono::milliseconds connectTimeout) noexcept
    {
        if (clock == nullptr || connectTimeout <= std::chrono::milliseconds::zero())
            return platform::SteadyTimePoint::max();
        return clock->now() + connectTimeout;
    }

    /// The lookup arm of @c resolveWithin: awaits the resolver and stores what it answered.
    /// @param resolver The name-resolution seam.
    /// @param host The host to look up. By value: this is a coroutine.
    /// @param port The port to pair it with.
    /// @param loop Where the lookup resumes.
    /// @param out Receives the answer; left empty if the arm is cancelled first.
    async::Task<void> lookUp(IAsyncAddressResolver* resolver,
                             std::string host,
                             std::uint16_t port,
                             EventLoop* loop,
                             std::optional<ResolveResult>* out)
    {
        *out = co_await resolver->resolve(std::move(host), port, loop);
    }

    /// The deadline arm of @c resolveWithin: parks on @p loop's deadline heap until @p deadline.
    /// @param loop The loop whose clock the deadline is on.
    /// @param deadline When the budget runs out.
    async::Task<void> sleepToDeadline(EventLoop* loop, platform::SteadyTimePoint deadline)
    {
        co_await sleepUntil(loop, deadline);
    }

    /// Resolves @p host, giving up at @p deadline.
    ///
    /// **Raced rather than checked afterwards, because a lookup does not have to come back.** A
    /// nameserver that drops packets holds `getaddrinfo` for the platform's whole retry schedule
    /// — about 30 s with glibc's defaults — and a budget looked at once the lookup returns is not
    /// applied at all while it has not. The race cancels the losing lookup through its stop token,
    /// which is why @c IAsyncAddressResolver makes honouring that token a duty: a race resumes its
    /// caller only once every arm has finished, so a lookup that ignored it would hold the answer
    /// back anyway.
    ///
    /// No race where there is nothing to race: with no loop the resolver never suspends, and with
    /// no deadline there is nothing to lose to.
    /// @param resolver The name-resolution seam.
    /// @param loop Where the lookup resumes and the deadline is armed; may be null.
    /// @param host The host to look up.
    /// @param port The port to pair it with.
    /// @param deadline When to give up; `SteadyTimePoint::max()` for never.
    /// @param budget What the caller asked for, for the message.
    /// @return The resolver's answer, or @c NetErrorCode::Timeout if the deadline came first.
    /// @throws async::OperationCancelled if the awaiting flow is stopped first.
    async::Task<ResolveResult> resolveWithin(IAsyncAddressResolver* resolver,
                                             EventLoop* loop,
                                             std::string host,
                                             std::uint16_t port,
                                             platform::SteadyTimePoint deadline,
                                             std::chrono::milliseconds budget)
    {
        if (loop == nullptr || deadline == platform::SteadyTimePoint::max())
            co_return co_await resolver->resolve(std::move(host), port, loop);

        auto answer = std::optional<ResolveResult> {};
        auto arms = std::vector<async::Task<void>> {};
        arms.reserve(2);
        arms.push_back(lookUp(resolver, host, port, loop, &answer));
        arms.push_back(sleepToDeadline(loop, deadline));
        // Which arm won is not the question: an answer that arrived is used even if the deadline
        // fired in the same turn, and the per-candidate check in the caller then decides whether
        // there is budget left to dial it.
        std::ignore = co_await async::whenAny(std::move(arms));
        if (answer.has_value())
            co_return std::move(*answer);
        co_return std::unexpected(
            makeNetError(NetErrorCode::Timeout,
                         0,
                         std::format("connect to {}:{} timed out after {} ms while resolving the name",
                                     host,
                                     port,
                                     budget.count())));
    }

} // namespace

async::Task<SocketResult> runConnectFlow(IAsyncAddressResolver* resolver,
                                         EventLoop* loop,
                                         platform::IClock* clock,
                                         std::string host,
                                         std::uint16_t port,
                                         DialOptions options,
                                         DialStep dial,
                                         void* dialState)
{
    // Refused before the resolver is touched. An empty host resolves to the wildcard address,
    // which is a bind target and not a dial target — and connecting to it reaches localhost on
    // Linux rather than failing, so the mistake would be silent.
    //
    // `AddressError` — "address resolution or parsing failed" — because an empty host is a name
    // that cannot be resolved into something dialable. `AddressNotAvail` is a bind's code.
    if (host.empty())
        co_return std::unexpected(
            makeNetError(NetErrorCode::AddressError, 0, std::format("no host to dial for port {}", port)));

    // The deadline is taken from @p clock and armed on @p loop, so the two must be one clock; a
    // connector passes `&loop.clock()`, and a case that injects a `ManualClock` builds its loop on it.
    assert((clock == nullptr || loop == nullptr || clock == &loop->clock())
           && "runConnectFlow: the budget's clock and the loop's clock differ, so the deadline "
              "raced against resolution would be measured on the wrong one");

    auto const deadline = budgetDeadline(clock, options.connectTimeout);

    auto resolved = co_await resolveWithin(resolver, loop, host, port, deadline, options.connectTimeout);
    if (!resolved.has_value())
        co_return std::unexpected(resolved.error());
    if (resolved->empty())
        co_return std::unexpected(resolveFailure(host, port, "no usable address"));

    // Seeded so a resolver that hands back an empty list — which the guard above makes
    // unreachable, but which a future resolver could — still produces an error naming the
    // endpoint rather than a default-constructed one.
    auto failure =
        makeNetError(NetErrorCode::AddressError, 0, std::format("no usable address for {}:{}", host, port));

    auto remainingCandidates = resolved->size();
    auto const socketOptions =
        StreamSocketOptions { .keepAlive = options.keepAlive, .buffers = options.buffers };
    for (auto const& candidate: *resolved)
    {
        // Checked PER CANDIDATE rather than once, which is what makes the budget a total:
        // without this the second candidate would start a fresh attempt after the first had
        // already consumed the whole allowance.
        if (clock != nullptr && clock->now() >= deadline)
        {
            failure = makeNetError(
                NetErrorCode::Timeout,
                0,
                std::format(
                    "connect to {}:{} timed out after {} ms", host, port, options.connectTimeout.count()));
            break;
        }

        // Each candidate gets an equal share of what is LEFT, not the whole of it. Both halves
        // matter and they pull against each other:
        //
        // - Handing every candidate the full budget means a caller asking for two seconds can
        //   wait four. A bound that multiplies by the number of addresses a name happens to have
        //   is not a bound.
        // - Handing the FIRST candidate the whole remaining budget defeats the fallback entirely
        //   whenever that candidate black-holes rather than refuses — the ordinary case for an
        //   AAAA on a machine with no IPv6 route, and the exact situation trying every candidate
        //   exists for.
        //
        // Dividing gives the caller the total it asked for and still leaves every candidate a
        // real chance. A candidate that finishes early hands what it did not use to the ones
        // after it, because the share is recomputed from the clock each time rather than fixed up
        // front.
        auto candidateDeadline = deadline;
        if (clock != nullptr && deadline != platform::SteadyTimePoint::max() && remainingCandidates > 1)
        {
            auto const left = deadline - clock->now();
            candidateDeadline = clock->now() + (left / static_cast<std::int64_t>(remainingCandidates));
        }
        --remainingCandidates;

        auto attempt = co_await dial(dialState, candidate, candidateDeadline, socketOptions);
        if (attempt.has_value())
            co_return std::move(*attempt);

        // The LAST failure wins: an AAAA that cannot be routed followed by an A that can is a
        // healthy host, and reporting the first would describe this machine's routing rather than
        // the peer.
        failure = std::move(attempt.error());
    }

    co_return std::unexpected(std::move(failure));
}

} // namespace core::net::detail
