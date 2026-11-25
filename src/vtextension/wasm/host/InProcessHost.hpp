// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A host serving in this process, on threads of its own, over real channels: what the tests connect to, and
/// what measures the protocol's cost without a process boundary.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/host/HostProcess.hpp>
#include <vtextension/wasm/host/HostServer.hpp>
#include <vtextension/wasm/host/IHostedEngine.hpp>
#include <vtextension/wasm/remote/HostConnector.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/ControlPage.hpp>
#include <vtextension/wasm/remote/transport/HandleGrant.hpp>
#include <vtextension/wasm/remote/transport/WakeSignal.hpp>

namespace vtextension::wasm::host
{

/// The hosts one connector started that are still alive; shared with each of them.
class InProcessHostRegistry;

/// One host serving in this process: an engine, the host process's serving side driving it over in-memory
/// endpoints, and the thread it runs on, in that order, so that the thread is joined before the host goes,
/// and the host before its engine. It is interrupted as a host process is: through the interrupt page and
/// the wake signal, which it holds the parent's ends of.
class InProcessHost final: public remote::IHostHandle
{
  public:
    /// Whether a host's server still serves.
    enum class ServingState : uint8_t
    {
        Ended,   ///< It was told to stop, or run() returned.
        Serving, ///< run() serves.
    };

    /// Starts serving.
    /// @param registry Where the host is listed while it lives.
    /// @param engine What it runs modules on.
    /// @param endpoints The helper's ends of the channels, the page and the wake signal.
    /// @param page The parent's mapping of the interrupt page.
    /// @param wake The wake signal's sending end.
    /// @param config How its server runs.
    InProcessHost(std::shared_ptr<InProcessHostRegistry> registry,
                  std::unique_ptr<IHostedEngine> engine,
                  HostEndpoints endpoints,
                  remote::transport::ControlPage page,
                  remote::transport::WakeSender wake,
                  HostServerConfig config);
    InProcessHost(InProcessHost const&) = delete;
    InProcessHost(InProcessHost&&) = delete;
    InProcessHost& operator=(InProcessHost const&) = delete;
    InProcessHost& operator=(InProcessHost&&) = delete;

    /// Terminates the host and joins its thread, on the thread that drops it -- unless that is its own
    /// thread, which an onDeath dropping the last reference to the host's parent ends on: the thread is
    /// then let go, as it touches nothing of the host after onDeath.
    ~InProcessHost() override;

    /// Raises @p interruptSlot and wakes the host's listener, as a host process is interrupted.
    void interrupt(uint32_t interruptSlot) noexcept override;

    /// Lowers @p interruptSlot.
    void clearInterrupt(uint32_t interruptSlot) noexcept override;

    /// Stops the server: its channels close, and its thread winds down.
    void terminate() noexcept override;

    /// Stops the server, as terminate() does; its thread is joined when the host is destroyed.
    void letGo() noexcept override;

    /// A host in this process cannot hang its parent's steps the way a process could: no timeout is armed.
    /// @return An inert watch.
    [[nodiscard]] remote::StepWatch watch(remote::HostStep step, std::chrono::milliseconds budget) override;

    /// Stores @p attachment; its onDeath runs on the host's thread once the server's run() returned, or at
    /// once if it returned already.
    void attach(remote::HostAttachment attachment) override;

    /// @return This process: the host serves in it.
    [[nodiscard]] remote::transport::HandleTarget const& handleTarget() const noexcept override
    {
        return _target;
    }

    /// @return Whether its server still serves.
    [[nodiscard]] ServingState state() const noexcept;

  private:
    /// Whether the server's run() returned.
    enum class RunState : uint8_t
    {
        Running,  ///< It serves, or was told to stop and has not returned yet.
        Returned, ///< It returned; onDeath ran or runs on attach.
    };

    /// Marks run() returned. @return The onDeath to call, if one was attached.
    [[nodiscard]] std::function<void()> runReturned();

    std::shared_ptr<InProcessHostRegistry> _registry;
    remote::transport::HandleTarget _target; ///< This process, which the host serves in.
    std::atomic<ServingState> _state { ServingState::Serving };
    std::mutex _attachmentMutex;       ///< Guards the two below.
    RunState _run = RunState::Running; ///< Guarded by _attachmentMutex.
    std::function<void()> _onDeath;    ///< Guarded by _attachmentMutex.
    std::unique_ptr<IHostedEngine> _engine;
    remote::transport::ControlPage _page; ///< The parent's mapping: raised and lowered here.
    remote::transport::WakeSender _wake;  ///< The parent's end.
    HostProcess _host;
    std::jthread _thread;
};

/// Starts each host as a HostServer in this process, on an engine of its own.
class InProcessHostConnector final: public remote::IHostConnector
{
  public:
    /// @param engines Creates each host's engine.
    /// @param config How each host's server runs.
    InProcessHostConnector(EngineFactory engines, HostServerConfig config);

    /// Starts a host serving MaxLaneChannels lanes and a control channel.
    /// @return The connection; `HostUnavailable` if no engine or no channel could be created.
    [[nodiscard]] std::expected<remote::HostConnection, ExtensionError> connect() override;

    /// Test hook.
    /// @return How many hosts connect() started.
    [[nodiscard]] std::size_t connects() const noexcept;

    /// Test hook.
    /// @return How many hosts this connector started still serve: neither terminated, nor stopped by their
    ///         parent, nor dropped.
    [[nodiscard]] std::size_t liveHosts() const noexcept;

    /// Test hook: a simulated crash of every live host. Each stops serving, and its channels close.
    /// @return How many hosts it terminated.
    std::size_t terminateAll() noexcept;

  private:
    EngineFactory _engines;
    HostServerConfig _config;
    std::shared_ptr<InProcessHostRegistry> _registry;
    std::atomic<std::size_t> _connects;
};

} // namespace vtextension::wasm::host
