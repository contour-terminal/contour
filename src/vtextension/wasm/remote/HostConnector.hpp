// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How Contour reaches an extension host process: a connector starts one, and hands back its channels and a
/// handle on it. A process supervisor implements it in production; tests connect to a host in this process.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>

namespace vtextension::wasm::remote
{

/// How many lanes a host serves: one per thread that may call into its instances at once.
inline constexpr std::size_t MaxLaneChannels = 4;

/// One incarnation of a host process, as its parent holds it.
class IHostHandle
{
  public:
    IHostHandle() = default;
    IHostHandle(IHostHandle const&) = delete;
    IHostHandle(IHostHandle&&) = delete;
    IHostHandle& operator=(IHostHandle const&) = delete;
    IHostHandle& operator=(IHostHandle&&) = delete;
    /// Terminates the host, if it still runs, and waits until it is gone.
    virtual ~IHostHandle() = default;

    /// Interrupts one instance's running call, for good. Never blocks on the host.
    /// @param instanceId The parent-chosen id of the instance; an id the host does not know is ignored.
    virtual void interrupt(uint32_t instanceId) noexcept = 0;

    /// Makes the host stop serving: its channels close, and every receive on the parent's ends fails. Does
    /// not wait for it to be gone.
    virtual void terminate() noexcept = 0;
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
