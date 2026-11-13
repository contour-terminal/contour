// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How long an attach may take, in the two phases it has when a prompt can be open during the first.

#include <core/async/Task.hpp>
#include <core/async/WhenAny.hpp>
#include <core/net/EventLoop.hpp>
#include <core/platform/Clock.hpp>

#include <algorithm>
#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <vthost/PromptPause.hpp>
#include <vthost/Transport.hpp>
#include <vthost/WithDeadline.hpp>

namespace vthost
{

/// When the transport last handed out its connection, which is when the handshake phase of an AttachBudget
/// with no connect bound begins. Written by a ConnectObservingFactory and read by the attach's deadline, on
/// one loop's thread.
class TransportReturnMark
{
  public:
    /// Forgets the last return: a new attempt begins.
    void reset() noexcept { _at.reset(); }

    /// @param when The instant the transport returned its connection, on the loop's clock.
    void note(core::platform::SteadyTimePoint when) noexcept { _at = when; }

    /// @return When the transport last returned its connection; nullopt before, or since reset().
    [[nodiscard]] std::optional<core::platform::SteadyTimePoint> at() const noexcept { return _at; }

  private:
    std::optional<core::platform::SteadyTimePoint> _at;
};

/// How often a wait with no connect bound looks whether the transport has returned its connection.
inline constexpr auto TransportReturnPollInterval = std::chrono::milliseconds { 50 };

/// How long an attach may take: the connect up to the transport handing out its connection, and the
/// handshake after it.
struct AttachBudget
{
    /// The connect phase's bound; nullopt: none, as the transport keeps its own clock (ssh's deadline
    /// pauses while a prompt is open, and an outer clock cannot see prompts).
    std::optional<std::chrono::milliseconds> connect;
    /// The handshake phase's bound, from the moment the transport returned its connection. Used only when
    /// @c connect is nullopt; with a connect bound the whole attach is bound by it.
    std::chrono::milliseconds handshake;
    /// The time the user has spent answering prompts that this transport asks while it attaches, which the
    /// attach does not count against its bound (a passphrase asked during the handshake); null for none.
    std::shared_ptr<PromptPause const> pause = nullptr;
    /// What @c pause held when this attempt began: prompts answered before it, for an earlier attempt on the
    /// same transport, extend nothing of this one.
    std::chrono::milliseconds pausedBefore = std::chrono::milliseconds::zero();

    /// A single bound for the whole attach.
    /// @param total How long the attach may take in all.
    template <typename Rep, typename Period>
    constexpr AttachBudget(std::chrono::duration<Rep, Period> total) noexcept:
        connect(std::chrono::duration_cast<std::chrono::milliseconds>(total)),
        handshake(std::chrono::duration_cast<std::chrono::milliseconds>(total))
    {
    }

    /// @param prompts Where the time of the prompts asked during the attach is kept.
    /// @return This budget, extended by as long as prompts are open.
    [[nodiscard]] AttachBudget pausedBy(std::shared_ptr<PromptPause const> prompts) const
    {
        auto budget = *this;
        budget.pause = std::move(prompts);
        return budget;
    }

    /// @return This budget for an attempt that begins now: only prompts from now on extend it.
    [[nodiscard]] AttachBudget startingNow() const
    {
        auto budget = *this;
        budget.pausedBefore = pause ? pause->total() : std::chrono::milliseconds::zero();
        return budget;
    }

    /// @return How long prompts have been open since the attempt began (startingNow()), which the attach's
    ///         bound does not count.
    [[nodiscard]] std::chrono::milliseconds paused() const
    {
        return pause ? std::max(pause->total() - pausedBefore, std::chrono::milliseconds::zero())
                     : std::chrono::milliseconds::zero();
    }

    /// @param handshakeBound How long the handshake may take after the transport returned its connection.
    /// @return A budget with no bound on the connect, for a transport whose prompts may be open meanwhile.
    [[nodiscard]] static constexpr AttachBudget promptsOpen(std::chrono::milliseconds handshakeBound) noexcept
    {
        auto budget = AttachBudget { handshakeBound };
        budget.connect.reset();
        return budget;
    }
};

namespace detail
{
    /// Sleeps until the attach's deadline: @p budget's connect bound from now, or, without one, its
    /// handshake bound from the moment @p mark says the transport returned.
    inline core::async::Task<void> attachDeadline(core::net::EventLoop* loop,
                                                  AttachBudget budget,
                                                  TransportReturnMark const* mark)
    {
        auto const start = loop->clock().now();
        budget = budget.startingNow();
        while (true)
        {
            auto deadline = std::optional<core::platform::SteadyTimePoint> {};
            if (budget.connect)
                deadline = start + *budget.connect + budget.paused();
            else if (auto const returned = mark != nullptr ? mark->at() : std::nullopt)
                deadline = *returned + budget.handshake + budget.paused();
            if (!deadline)
            {
                co_await loop->delay(TransportReturnPollInterval);
                continue;
            }
            if (loop->clock().now() >= *deadline)
                co_return;
            co_await loop->sleepUntil(*deadline);
        }
    }

} // namespace detail

/// core::net::withTimeout over an AttachBudget.
/// @param loop The loop whose clock bounds the wait.
/// @param work The attach.
/// @param budget How long it may take.
/// @param mark When the transport returned its connection; null: never told, so a budget with no connect
///        bound would never end.
/// @return What @p work returned; nullopt when the budget ran out first.
template <typename T>
core::async::Task<std::optional<T>> withAttachBudget(core::net::EventLoop* loop,
                                                     core::async::Task<T> work,
                                                     AttachBudget budget,
                                                     TransportReturnMark const* mark)
{
    return withDeadline<T>(std::move(work), detail::attachDeadline(loop, budget, mark));
}

/// A transport that says when it has handed out a connection, which is when the handshake phase begins.
class ConnectObservingFactory final: public ITransportFactory
{
  public:
    /// @param inner The transport that connects.
    /// @param onConnected Called, on the loop's thread, each time @p inner returned a connection.
    ConnectObservingFactory(std::shared_ptr<ITransportFactory> inner, std::function<void()> onConnected):
        _inner(std::move(inner)), _onConnected(std::move(onConnected))
    {
    }

    [[nodiscard]] core::async::Task<std::expected<std::unique_ptr<Connection>, TransportError>> connect(
        core::net::EventLoop& loop) override
    {
        return connectOn(&loop);
    }

    [[nodiscard]] std::string describe() const override { return _inner->describe(); }
    void holdDaemonStart(core::platform::SteadyTimePoint until) override { _inner->holdDaemonStart(until); }

  private:
    /// The connect itself: a coroutine takes the loop by pointer, as its reference would outlive the call.
    [[nodiscard]] core::async::Task<std::expected<std::unique_ptr<Connection>, TransportError>> connectOn(
        core::net::EventLoop* loop)
    {
        auto connection = co_await _inner->connect(*loop);
        if (connection)
            _onConnected();
        co_return connection;
    }

    std::shared_ptr<ITransportFactory> _inner;
    std::function<void()> _onConnected;
};

} // namespace vthost
