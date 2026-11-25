// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How Contour reaches an extension host process: a connector starts one, and hands back its channels and a
/// handle on it. A process supervisor implements it in production; tests connect to a host in this process.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/HelperArguments.hpp>

namespace vtextension::wasm::remote
{

/// A step of a host's that is not a call, each with its own budget (HostTimeouts).
enum class HostStep : uint8_t
{
    Handshake,   ///< From its start until its Hello is read and its runtime is created.
    Load,        ///< Loading a module.
    Instantiate, ///< Instantiating one, its start function included.
};

/// A host step's timeout, armed while it lives: a host that overstays it is stopped. Disarms its timeout when
/// destroyed. Move-only; default-constructed it is inert.
class StepWatch
{
  public:
    StepWatch() noexcept = default;

    /// @param disarm Called once, when the watch is destroyed; disarms its timeout.
    explicit StepWatch(std::function<void()> disarm) noexcept: _disarm { std::move(disarm) } {}

    StepWatch(StepWatch const&) = delete;
    StepWatch& operator=(StepWatch const&) = delete;

    StepWatch(StepWatch&& other) noexcept: _disarm { std::exchange(other._disarm, nullptr) } {}

    StepWatch& operator=(StepWatch&& other) noexcept
    {
        if (this != &other)
        {
            release();
            _disarm = std::exchange(other._disarm, nullptr);
        }
        return *this;
    }

    ~StepWatch() { release(); }

  private:
    /// Disarms the timeout, once.
    void release() noexcept
    {
        if (auto const disarm = std::exchange(_disarm, nullptr))
            disarm();
    }

    std::function<void()> _disarm;
};

/// What the parent wants to hear of a host's end.
struct HostAttachment
{
    /// Called once when the host is gone or was stopped, from whichever thread observed it; at once if it was
    /// gone before it was attached. It must not block.
    std::function<void()> onDeath;
};

/// One incarnation of a host process, as its parent holds it.
class IHostHandle
{
  public:
    IHostHandle() = default;
    IHostHandle(IHostHandle const&) = delete;
    IHostHandle(IHostHandle&&) = delete;
    IHostHandle& operator=(IHostHandle const&) = delete;
    IHostHandle& operator=(IHostHandle&&) = delete;
    /// Lets the host go: it is stopped if it still runs. A host in this process is waited for; a process is
    /// reaped by its supervisor, after the handle is gone.
    virtual ~IHostHandle() = default;

    /// Interrupts one instance's running call, for good. Never blocks on the host.
    /// @param instanceId The parent-chosen id of the instance; an id the host does not know is ignored.
    virtual void interrupt(uint32_t instanceId) noexcept = 0;

    /// Makes the host stop serving: its channels close, and every receive on the parent's ends fails. Does
    /// not wait for it to be gone.
    virtual void terminate() noexcept = 0;

    /// Arms @p step's timeout: should the host still be at it once @p budget passed, it is stopped, and the
    /// attachment's onDeath runs.
    /// @param step The step about to begin.
    /// @param budget How long it may take, from now.
    /// @return What disarms the timeout when destroyed; inert where no host can overstay.
    [[nodiscard]] virtual StepWatch watch(HostStep step, std::chrono::milliseconds budget) = 0;

    /// Tells the handle what the parent wants to hear of the host's end. Called once, right after the
    /// connection is made.
    /// @param attachment Its onDeath runs once the host is gone, at once if it is gone already.
    virtual void attach(HostAttachment attachment) = 0;
};

/// A host process started and connected: the parent's ends of its channels, and the handle on it.
struct HostConnection
{
    transport::Channel control;            ///< The control channel's parent end.
    std::vector<transport::Channel> lanes; ///< The lanes' parent ends; MaxLaneChannels of them.
    std::unique_ptr<IHostHandle> handle;   ///< The host itself.
};

/// Starts host processes.
class IHostConnector
{
  public:
    IHostConnector() = default;
    IHostConnector(IHostConnector const&) = delete;
    IHostConnector(IHostConnector&&) = delete;
    IHostConnector& operator=(IHostConnector const&) = delete;
    IHostConnector& operator=(IHostConnector&&) = delete;
    virtual ~IHostConnector() = default;

    /// Starts a host and connects to it. The host's first control PDU is its Hello.
    /// @return The connection; `HostUnavailable` if no host could be started or connected to.
    [[nodiscard]] virtual std::expected<HostConnection, ExtensionError> connect() = 0;
};

} // namespace vtextension::wasm::remote
