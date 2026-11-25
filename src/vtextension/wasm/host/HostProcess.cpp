// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/host/HostProcess.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <format>
#include <iterator>
#include <memory>
#include <ranges>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <vtextension/wasm/host/InterruptListener.hpp>

namespace vtextension::wasm::host
{

namespace transport = remote::transport;

namespace
{
    /// Every way the host process ends. Its codes stay clear of 1 (a generic failure), 23 (LeakSanitizer)
    /// and 66 (AddressSanitizer, ThreadSanitizer), so that a sanitizer's report cannot pass for one of them.
    constexpr auto ExitTable = std::array {
        HostProcessExitRow {
            .exit = HostProcessExit::Served, .code = 0, .text = "Contour ended the connection" },
        HostProcessExitRow { .exit = HostProcessExit::BadArguments,
                             .code = 70,
                             .text = "its command line is not what it takes: Contour starts it, not a user" },
        HostProcessExitRow { .exit = HostProcessExit::HandlesUnusable,
                             .code = 71,
                             .text = "the channels it was started with cannot be used" },
        HostProcessExitRow { .exit = HostProcessExit::ParentGone,
                             .code = 72,
                             .text = "the Contour process that started it is gone" },
        HostProcessExitRow { .exit = HostProcessExit::EngineUnavailable,
                             .code = 73,
                             .text = "no extension runtime can be created" },
        HostProcessExitRow { .exit = HostProcessExit::ProtocolViolation,
                             .code = 74,
                             .text = "Contour sent what the protocol does not allow" },
        HostProcessExitRow { .exit = HostProcessExit::ExecutorUnavailable,
                             .code = 75,
                             .text = "a thread to run extensions on cannot be started" },
        HostProcessExitRow { .exit = HostProcessExit::MemoryUnusable,
                             .code = 76,
                             .text = "memory Contour granted an extension could not be opened" },
    };

    /// @return @p exit's row.
    [[nodiscard]] HostProcessExitRow const& rowOf(HostProcessExit exit) noexcept
    {
        // Every enumerator has its row, at its own index.
        return ExitTable[static_cast<std::size_t>(exit)];
    }

    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, ExitTable.size()),
                                      [](std::size_t index) {
                                          return static_cast<std::size_t>(ExitTable[index].exit) == index;
                                      }),
                  "ExitTable lists every HostProcessExit at its own index");

    /// Writes why the host ends to stderr, unless it served.
    /// @return @p exit's code.
    [[nodiscard]] int endWith(HostProcessExit exit) noexcept
    {
        auto const& row = rowOf(exit);
        if (exit != HostProcessExit::Served)
            tellOnStandardError(row.text);
        return row.code;
    }
} // namespace

std::span<HostProcessExitRow const> hostProcessExits() noexcept
{
    return ExitTable;
}

int exitCodeOf(HostProcessExit exit) noexcept
{
    return rowOf(exit).code;
}

HostProcessExit hostProcessExitOf(ServerExit exit) noexcept
{
    switch (exit)
    {
        case ServerExit::PeerClosed:
        case ServerExit::Stopped: return HostProcessExit::Served;
        case ServerExit::ProtocolViolation: return HostProcessExit::ProtocolViolation;
        case ServerExit::ExecutorUnavailable: return HostProcessExit::ExecutorUnavailable;
        case ServerExit::MemoryUnusable: return HostProcessExit::MemoryUnusable;
    }
    return HostProcessExit::ProtocolViolation;
}

std::expected<HostEndpoints, HostProcessExit> adoptEndpoints(transport::HelperArguments const& arguments)
{
    auto const unusable = [](auto) {
        return HostProcessExit::HandlesUnusable;
    };
    auto control = transport::Channel::adopt(arguments.control);
    if (!control)
        return std::unexpected(HostProcessExit::HandlesUnusable);
    auto lanes = std::vector<transport::Channel> {};
    lanes.reserve(arguments.lanes.size());
    for (auto const handle: arguments.lanes)
    {
        auto lane = transport::Channel::adopt(handle);
        if (!lane)
            return std::unexpected(HostProcessExit::HandlesUnusable);
        lanes.push_back(*std::move(lane));
    }
    return transport::ControlPage::adopt(arguments.controlPage)
        .transform_error(unusable)
        .and_then([&](transport::ControlPage page) {
            return transport::WakeReceiver::adopt(arguments.wake)
                .transform_error(unusable)
                .transform([&](transport::WakeReceiver wake) {
                    return HostEndpoints { .control = *std::move(control),
                                           .lanes = std::move(lanes),
                                           .controlPage = std::move(page),
                                           .wake = std::move(wake) };
                });
        });
}

HostProcess::HostProcess(IHostedEngine& engine, HostEndpoints endpoints, HostServerConfig config):
    _controlPage { std::move(endpoints.controlPage) },
    _wake { std::move(endpoints.wake) },
    _startThread { config.startExecutor },
    _server { engine, std::move(endpoints.lanes), std::move(endpoints.control), [&] {
                 config.isInterruptRaised = [page = &_controlPage](uint32_t slot) {
                     return page->isRaised(slot);
                 };
                 return std::move(config);
             }() }
{
}

ServerExit HostProcess::run()
{
    // Listening before the server confines the host, which may forbid new threads from then on; stopped and
    // joined once the server is done.
    auto const listener = InterruptListener::start(
        _controlPage, _wake, [this](uint32_t slot) { _server.interruptSlot(slot); }, _startThread);
    if (!listener)
        return ServerExit::ExecutorUnavailable;
    return _server.run();
}

void HostProcess::stop() noexcept
{
    _server.stop();
}

void tellOnStandardError(std::string_view line) noexcept
{
    constexpr auto Prefix = std::string_view { "contour-extension-host: " };
    std::ignore = std::fwrite(Prefix.data(), 1, Prefix.size(), stderr);
    std::ignore = std::fwrite(line.data(), 1, line.size(), stderr);
    std::ignore = std::fputc('\n', stderr);
}

HostConfinement systemConfinement()
{
    return HostConfinement {
        .bindToParent = &bindToParent,
        .beforeThreads = [] { return hardenProcess(); },
        .adoptEndpoints = &adoptEndpoints,
        .sandbox = &makeSystemSandbox,
        .startThread = &ExecutorThread::start,
        .confine =
            [](uint64_t dataBytes) {
                if constexpr (BuildCapPolicy == CapPolicy::Skip)
                    tellOnStandardError("resource caps are off in this build");
                return capResources(dataBytes, BuildCapPolicy);
            },
        .tell = &tellOnStandardError,
    };
}

SandboxDescriptors sandboxDescriptorsOf(HostEndpoints const& endpoints, IHostedEngine const& engine)
{
    auto const devices = engine.heldDevices();
    return sandboxDescriptorsOf(endpoints, devices);
}

SandboxDescriptors sandboxDescriptorsOf(HostEndpoints const& endpoints,
                                        std::span<transport::NativeHandle const> engineDevices)
{
    auto held = std::vector<HeldDescriptor> {};
    for (auto const& lane: endpoints.lanes)
        held.push_back(HeldDescriptor { .role = DescriptorRole::Lane, .handle = lane.nativeHandle() });
    held.push_back(
        HeldDescriptor { .role = DescriptorRole::Page, .handle = endpoints.controlPage.nativeHandle() });
    held.push_back(HeldDescriptor { .role = DescriptorRole::Wake, .handle = endpoints.wake.nativeHandle() });
    // Two ends of a pipe on POSIX; on Windows one event in both roles, held once.
    auto const stop = endpoints.wake.stopHandles();
    if (stop.waitedOn == stop.signalled)
        held.push_back(HeldDescriptor { .role = DescriptorRole::StopEvent, .handle = stop.waitedOn });
    else
    {
        held.push_back(HeldDescriptor { .role = DescriptorRole::StopReceiver, .handle = stop.waitedOn });
        held.push_back(HeldDescriptor { .role = DescriptorRole::StopSender, .handle = stop.signalled });
    }
    std::ranges::copy(standardStreams(), std::back_inserter(held));
    for (auto const device: engineDevices)
        held.push_back(HeldDescriptor { .role = DescriptorRole::EngineDevice, .handle = device });
    return SandboxDescriptors { .control = endpoints.control.nativeHandle(), .held = std::move(held) };
}

int runExtensionHost(std::span<std::string_view const> arguments,
                     EngineFactory const& engines,
                     HostConfinement const& confinement)
{
    using Confined = std::expected<void, ConfinementFailure>;
    // Tells of a step that failed, after what it was a step of.
    auto const tellOf = [&confinement](std::string_view what, Confined const& step) {
        if (!step)
            confinement.tell(std::format("{} failed: {}", what, confinementFailureText(step.error())));
    };

    auto const parsed = transport::parseHelperArguments(arguments);
    if (!parsed)
        return endWith(HostProcessExit::BadArguments);
    // First, so that the host never outlives Contour, whatever happens next.
    if (!confinement.bindToParent(parsed->parentProcessId))
        return endWith(HostProcessExit::ParentGone);
    // While the main thread is the only one: what is set per thread then reaches every thread started later.
    auto const hardened = confinement.beforeThreads();
    tellOf("hardening", hardened);
    auto endpoints = confinement.adoptEndpoints(*parsed);
    if (!endpoints)
        return endWith(endpoints.error());
    // Declared before the host, so that every object the host made of it goes first.
    auto const engine = engines();
    if (!engine)
        return endWith(HostProcessExit::EngineUnavailable);
    // Still before any thread: what the sandbox restricts now, every thread inherits.
    auto const sandbox = confinement.sandbox(sandboxDescriptorsOf(*endpoints, *engine), engine->guestCode());
    auto const restricted = sandbox->restrictBeforeThreads();
    tellOf("sandbox", restricted);

    auto config = HostServerConfig {};
    config.startExecutor = confinement.startThread;
    // Once every thread runs and was prepared: caps first, which the locked-down host could not set.
    config.confine = [&confinement, &sandbox, &tellOf, hardened, restricted, dataBytes = parsed->dataBytes]()
        -> std::expected<remote::proto::LockdownState, ConfinementFailure> {
        auto const capped = confinement.confine(dataBytes);
        tellOf("resource caps", capped);
        auto const lockedDown = sandbox->lockDown();
        // One line for the sandbox: what failed first.
        if (restricted)
            tellOf("sandbox", lockedDown);
        for (auto const& step: std::array { hardened, restricted, capped, lockedDown })
            if (!step)
                return std::unexpected(step.error());
        return sandbox->lockdownState();
    };
    auto host = HostProcess { *engine, *std::move(endpoints), std::move(config) };
    return endWith(hostProcessExitOf(host.run()));
}

int runExtensionHost(std::span<std::string_view const> arguments, EngineFactory const& engines)
{
    return runExtensionHost(arguments, engines, systemConfinement());
}

} // namespace vtextension::wasm::host
