// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The extension host's ear for interrupts: a thread of its own that waits on the wake signal and, on every
/// wake, scans the interrupt page and hands each raised slot on -- to the server, which interrupts the
/// instance holding it. Not the control thread, which waits on the control channel.

#include <cstdint>
#include <expected>
#include <functional>
#include <system_error>

#include <vtextension/wasm/host/ExecutorThread.hpp>
#include <vtextension/wasm/remote/transport/ControlPage.hpp>
#include <vtextension/wasm/remote/transport/WakeSignal.hpp>

namespace vtextension::wasm::host
{

/// The stack the listener's thread runs on: it scans a page and calls a function, nothing deeper.
inline constexpr std::size_t InterruptListenerStackBytes = std::size_t { 256 } * 1024;

/// Listens for interrupts until it is destroyed. Move-only.
class InterruptListener
{
  public:
    /// Starts listening on a thread of its own.
    /// @param page The interrupt page; outlives the listener.
    /// @param wake The wake signal's receiving end; outlives the listener, and is stopped by its destruction.
    /// @param interruptSlot Called with every slot raised, on every wake; on the listener's thread.
    /// @param startThread How its thread is started, with InterruptListenerStackBytes.
    /// @return The listener; why its thread could not be started.
    [[nodiscard]] static std::expected<InterruptListener, std::error_code> start(
        remote::transport::ControlPage const& page,
        remote::transport::WakeReceiver& wake,
        std::function<void(uint32_t slot)> interruptSlot,
        ExecutorStarter const& startThread);

    InterruptListener(InterruptListener&& other) noexcept;
    InterruptListener& operator=(InterruptListener&& other) noexcept;
    InterruptListener(InterruptListener const&) = delete;
    InterruptListener& operator=(InterruptListener const&) = delete;

    /// Stops the wake signal's receiver, and joins the thread.
    ~InterruptListener();

  private:
    InterruptListener(remote::transport::WakeReceiver& wake, ExecutorThread thread) noexcept;

    /// Stops and joins, unless moved from.
    void release() noexcept;

    remote::transport::WakeReceiver* _wake; ///< Null once moved from.
    ExecutorThread _thread;
};

} // namespace vtextension::wasm::host
