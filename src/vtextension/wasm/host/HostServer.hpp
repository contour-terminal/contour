// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The extension host's side of the protocol: serves runtimes, modules and instances of one engine to the
/// parent at the other end of its channels.
///
/// The control channel creates and releases runtimes, modules and instances, on the thread that runs the
/// server; each lane runs one parent thread's calls, host calls and memory accesses, on an executor thread of
/// its own. A host call a guest makes is sent on the lane the call came in on, and the parent may call back
/// in, on that lane, before it answers. Ids are the parent's: one the parent never created, or creates twice,
/// is a protocol violation, as is any PDU out of place. A violation ends the connection.

#include <cstdint>
#include <memory>
#include <vector>

#include <vtextension/wasm/host/IHostedEngine.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>

namespace vtextension::wasm::host
{

/// How a host server runs.
struct HostServerConfig
{
    remote::proto::PduLimits limits {}; ///< The caps of the PDUs it receives, and sends.
    /// How far the host confined itself, as its Hello states.
    remote::proto::LockdownState lockdown = remote::proto::LockdownState::Unsandboxed;
};

/// Why a host server stopped serving.
enum class ServerExit : uint8_t
{
    ProtocolViolation, ///< The parent sent what the protocol does not allow, on any channel.
    PeerClosed,        ///< The parent closed a channel, or a channel failed.
    Stopped,           ///< stop() was called.
};

/// Serves one engine over a control channel and its lanes.
class HostServer
{
  public:
    /// @param engine What runtimes are created on; must outlive the server.
    /// @param lanes The helper's ends of the lanes, each served by an executor thread of its own.
    /// @param control The helper's end of the control channel.
    /// @param config How it runs.
    HostServer(IHostedEngine& engine,
               std::vector<remote::transport::Channel> lanes,
               remote::transport::Channel control,
               HostServerConfig config);
    ~HostServer();
    HostServer(HostServer const&) = delete;
    HostServer(HostServer&&) = delete;
    HostServer& operator=(HostServer const&) = delete;
    HostServer& operator=(HostServer&&) = delete;

    /// Sends Hello, starts one executor jthread per lane, serves control on the calling thread until EOF,
    /// a violation or stop(); then stops, joins the executors, releases every instance, module, runtime.
    /// Called once.
    /// @return Why it stopped serving: the first of the reasons that came up, on any channel.
    [[nodiscard]] ServerExit run();

    /// Thread-safe, idempotent: interrupts every live instance, shuts every channel down. Callable before,
    /// during and after run(); run() then returns `Stopped`, unless it had stopped for another reason first.
    void stop() noexcept;

    /// Interrupts one instance; a brief mutex over the instance map, no I/O. Unknown id: ignored.
    /// @param instanceId The parent-chosen id of the instance.
    void interrupt(uint32_t instanceId) noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace vtextension::wasm::host
