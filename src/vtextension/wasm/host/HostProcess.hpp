// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The extension host process: the endpoints it inherited from Contour, the server it runs over them, and the
/// way it ends, as one exit code per reason.

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <vtextension/wasm/host/HostBootstrap.hpp>
#include <vtextension/wasm/host/HostServer.hpp>
#include <vtextension/wasm/host/IHostedEngine.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/ControlPage.hpp>
#include <vtextension/wasm/remote/transport/HelperArguments.hpp>
#include <vtextension/wasm/remote/transport/WakeSignal.hpp>

namespace vtextension::wasm::host
{

/// How the extension host process ended.
enum class HostProcessExit : uint8_t
{
    Served,              ///< It served until Contour closed the connection, or it was stopped.
    BadArguments,        ///< Its command line is not what it takes.
    HandlesUnusable,     ///< A handle it was told of cannot be adopted.
    ParentGone,          ///< It could not bind itself to its parent, or that one ended already.
    EngineUnavailable,   ///< No engine could be created.
    ProtocolViolation,   ///< Contour sent what the protocol does not allow.
    ExecutorUnavailable, ///< A lane's thread could not be started.
};

/// One way the host process ends.
struct HostProcessExitRow
{
    HostProcessExit exit;  ///< The reason.
    int code;              ///< Its exit code: 0, or 70 and up -- never 1, 23 or 66, which sanitizers use.
    std::string_view text; ///< What it writes to stderr on the way out; user-readable, naming no engine.
};

/// @return Every way the host process ends, one row each.
[[nodiscard]] std::span<HostProcessExitRow const> hostProcessExits() noexcept;

/// @param exit How the host ended.
/// @return Its exit code.
[[nodiscard]] int exitCodeOf(HostProcessExit exit) noexcept;

/// @param exit Why the server stopped serving.
/// @return How the host process ends because of it: `Served` once Contour closed it or it was stopped.
[[nodiscard]] HostProcessExit hostProcessExitOf(ServerExit exit) noexcept;

/// The helper's ends of everything Contour handed it.
struct HostEndpoints
{
    remote::transport::Channel control;            ///< The control channel.
    std::vector<remote::transport::Channel> lanes; ///< MaxLaneChannels lanes.
    remote::transport::ControlPage controlPage;    ///< The interrupt page, read-only.
    remote::transport::WakeReceiver wake;          ///< The wake signal's receiving end.
};

/// Takes ownership of every handle @p arguments name.
/// @param arguments The parsed command line.
/// @return The endpoints; `HandlesUnusable` if any handle cannot be adopted.
[[nodiscard]] std::expected<HostEndpoints, HostProcessExit> adoptEndpoints(
    remote::transport::HelperArguments const& arguments);

/// A host's whole serving side over its endpoints: the server, and the listener that hands it the interrupts
/// raised on the page.
class HostProcess
{
  public:
    /// @param engine What runtimes are created on; must outlive the host.
    /// @param endpoints What it serves over.
    /// @param config How its server runs; whether a slot is raised is the page's to say.
    HostProcess(IHostedEngine& engine, HostEndpoints endpoints, HostServerConfig config);

    /// Starts listening for interrupts, serves until Contour closes the connection, it violates the protocol,
    /// or stop() is called; then releases every engine object, and stops listening. Called once.
    /// @return Why it stopped serving; `ExecutorUnavailable` if the listener's thread could not be started,
    ///         nothing being served then.
    [[nodiscard]] ServerExit run();

    /// Thread-safe, idempotent: ends run().
    void stop() noexcept;

  private:
    remote::transport::ControlPage
        _controlPage; ///< Where Contour raises an instance's interrupt; held while it serves.
    remote::transport::WakeReceiver _wake; ///< What tells it to look at the page; held while it serves.
    HostServer _server;
};

/// What the host process does to itself, as the operating system does it (systemConfinement()) or, in a test
/// that runs the host in its own process, as a double does it.
struct HostConfinement
{
    /// Binds the process to the parent it is given, as bindToParent() does.
    std::function<std::expected<void, BootstrapError>(int64_t parentProcessId)> bindToParent;
    /// Hardens the process while its main thread is its only one, as hardenProcess() does: right after it
    /// bound itself to its parent, before it adopts its endpoints, makes its engine or starts any thread.
    /// What is set per thread and must reach every thread is set here.
    /// @return Nothing if it hardened itself wholly; the first step that failed otherwise, which the host
    ///         tells of through tell and which makes Hello state `Failed` whatever confine returns.
    std::function<std::expected<void, HardeningFailure>()> beforeThreads;
    /// Caps its resources once its threads run, to the data bytes it is given, as capResources() does.
    /// @return What Hello states, unless beforeThreads failed.
    std::function<remote::proto::LockdownState(uint64_t dataBytes)> confine;
    /// Tells of a confinement the host could not complete, one line without its end: on stderr, after
    /// `contour-extension-host: `, as tellOnStandardError() does.
    std::function<void(std::string_view line)> tell;
};

/// Writes `contour-extension-host: <line>` to stderr. Never throws: a failed write is no reason to end.
/// @param line The line, without its end.
void tellOnStandardError(std::string_view line) noexcept;

/// @return bindToParent(), hardenProcess(), and capResources() under BuildCapPolicy, which under `Skip` says
///         so once on stderr; tellOnStandardError().
[[nodiscard]] HostConfinement systemConfinement();

/// The executable's body: parse, bind to the parent, harden, adopt, make the engine, serve. Every exit but
/// `Served` writes one line, `contour-extension-host: <text>`, to stderr.
/// @param arguments The command line after the program's name.
/// @param engines Makes the engine; called only once everything before it succeeded.
/// @param confinement How it binds to its parent, hardens and caps itself.
/// @return The exit code.
[[nodiscard]] int runExtensionHost(std::span<std::string_view const> arguments,
                                   EngineFactory const& engines,
                                   HostConfinement const& confinement);

/// runExtensionHost() with systemConfinement(): what the executable runs.
/// @param arguments The command line after the program's name.
/// @param engines Makes the engine.
/// @return The exit code.
[[nodiscard]] int runExtensionHost(std::span<std::string_view const> arguments, EngineFactory const& engines);

} // namespace vtextension::wasm::host
