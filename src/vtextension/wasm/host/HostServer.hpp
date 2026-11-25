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
///
/// An engine that shares memory (MemoryMode::Shared) runs each instance in the object its Instantiate grants:
/// the server maps it before instantiating, and a grant it cannot map as that object is a violation. Growing
/// that memory asks the parent first, with a Grow answered by exactly one Grown: on the lane whose call grows
/// it, or on control while a start function runs. An engine that copies memory is granted nothing.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <system_error>
#include <vector>

#include <vtextension/wasm/host/ExecutorThread.hpp>
#include <vtextension/wasm/host/IHostedEngine.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/GuestMemoryMapping.hpp>

namespace vtextension::wasm::host
{

/// What one guest call on a lane may take of its thread's stack besides the guest's own: the engine's entry,
/// the host call's way out and the server's frames serving the lane meanwhile, sanitizers included.
inline constexpr std::size_t HostFrameBytes = std::size_t { 256 } * 1024;

/// The stack each lane's executor thread runs on: MaxCallDepth guest calls at their largest.
inline constexpr std::size_t ExecutorStackBytes =
    remote::proto::MaxCallDepth * (remote::proto::MaxRuntimeStackBytes + HostFrameBytes);

/// Starts the thread a lane's executor runs on, with the stack it asks for, as ExecutorThread::start does.
using ExecutorStarter =
    std::function<std::expected<ExecutorThread, std::error_code>(std::size_t, std::function<void()>)>;

/// Opens a guest memory mapping's first bytes, as GuestMemoryMapping::open() does.
using GuestMemoryOpener = std::function<std::expected<void, remote::transport::MappingError>(
    remote::transport::GuestMemoryMapping& memory, uint64_t bytes)>;

/// How a host server runs.
struct HostServerConfig
{
    remote::proto::PduLimits limits {}; ///< The caps of the PDUs it receives, and sends.
    /// How far the host confined itself, as its Hello states, when there is no confine.
    remote::proto::LockdownState lockdown = remote::proto::LockdownState::Unsandboxed;
    /// Confines the host: called once every lane's thread started, before Hello, whose lockdown is what it
    /// returns. Null: nothing is confined, and Hello states `lockdown`.
    std::function<remote::proto::LockdownState()> confine {};
    /// How each lane's executor thread is started: the operating system's threads, but in a test that
    /// makes one fail.
    ExecutorStarter startExecutor = &ExecutorThread::start;
    /// Whether an interrupt slot is raised on the interrupt page, as interruptSlot() asks before it
    /// interrupts: what tells the instance a slot was raised for from one that holds the slot since. Null:
    /// every interruptSlot() interrupts.
    std::function<bool(uint32_t slot)> isInterruptRaised {};
    /// How the bytes a guest's memory grew into, once the parent granted them, are opened: the operating
    /// system's way, but in a test that makes it fail.
    GuestMemoryOpener openGuestMemory = &remote::transport::GuestMemoryMapping::open;
};

/// Why a host server stopped serving.
enum class ServerExit : uint8_t
{
    ProtocolViolation,   ///< The parent sent what the protocol does not allow, on any channel.
    PeerClosed,          ///< The parent closed a channel, or a channel failed.
    Stopped,             ///< stop() was called.
    ExecutorUnavailable, ///< A lane's executor thread could not be started.
    MemoryUnusable,      ///< Guest memory the parent granted to grow into could not be opened.
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

    /// Starts one executor thread per lane (of ExecutorStackBytes), confines the host (HostServerConfig::
    /// confine), sends Hello, serves control on the calling thread until EOF, a violation or stop(); then
    /// stops, joins the executors, releases every instance, module, runtime. Called once.
    /// @return Why it stopped serving: the first of the reasons that came up, on any channel;
    ///         `ExecutorUnavailable` if a lane's thread could not be started, neither confining nor sending
    ///         anything then.
    [[nodiscard]] ServerExit run();

    /// Thread-safe, idempotent: interrupts every live instance, shuts every channel down. Callable before,
    /// during and after run(); run() then returns `Stopped`, unless it had stopped for another reason first.
    void stop() noexcept;

    /// Interrupts the live instance instantiated with an interrupt slot, if the slot is still raised
    /// (HostServerConfig::isInterruptRaised); a brief mutex over the instance map, no I/O. A slot no live
    /// instance holds is ignored.
    /// @param slot The slot, as Instantiate named it.
    void interruptSlot(uint32_t slot) noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace vtextension::wasm::host
