// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/host/InterruptListener.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <ranges>
#include <system_error>
#include <utility>

namespace vtextension::wasm::host
{

namespace transport = remote::transport;

namespace
{
    /// Waits for wakes until the receiver is stopped or its sender is gone; after each, hands every slot
    /// raised on @p page to @p interruptSlot. A wake is taken before the page is scanned, so that a slot
    /// raised after the scan began comes with a wake of its own, which the next wait returns.
    void listen(transport::ControlPage const& page,
                transport::WakeReceiver& wake,
                std::function<void(uint32_t)> const& interruptSlot) noexcept
    {
        while (wake.wait() == transport::WakeWait::Woken)
            for (auto const slot: std::views::iota(uint32_t { 0 }, transport::InterruptSlotCount))
                if (page.isRaised(slot))
                    interruptSlot(slot);
    }
} // namespace

std::expected<InterruptListener, std::error_code> InterruptListener::start(
    transport::ControlPage const& page,
    transport::WakeReceiver& wake,
    std::function<void(uint32_t slot)> interruptSlot,
    ExecutorStarter const& startThread)
{
    return startThread(InterruptListenerStackBytes,
                       [&page, &wake, interruptSlot = std::move(interruptSlot)] {
                           listen(page, wake, interruptSlot);
                       })
        .transform([&](ExecutorThread thread) { return InterruptListener { wake, std::move(thread) }; });
}

InterruptListener::InterruptListener(transport::WakeReceiver& wake, ExecutorThread thread) noexcept:
    _wake { &wake }, _thread { std::move(thread) }
{
}

InterruptListener::InterruptListener(InterruptListener&& other) noexcept:
    _wake { std::exchange(other._wake, nullptr) }, _thread { std::move(other._thread) }
{
}

InterruptListener& InterruptListener::operator=(InterruptListener&& other) noexcept
{
    if (this != &other)
    {
        release();
        _wake = std::exchange(other._wake, nullptr);
        _thread = std::move(other._thread);
    }
    return *this;
}

InterruptListener::~InterruptListener()
{
    release();
}

void InterruptListener::release() noexcept
{
    if (auto* const wake = std::exchange(_wake, nullptr))
    {
        wake->stop();
        _thread.join();
    }
}

} // namespace vtextension::wasm::host
