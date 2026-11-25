// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/host/HostProcess.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <print>
#include <ranges>
#include <utility>
#include <vector>

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
    [[nodiscard]] int endWith(HostProcessExit exit)
    {
        auto const& row = rowOf(exit);
        if (exit != HostProcessExit::Served)
            std::println(stderr, "contour-extension-host: {}", row.text);
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
    _server { engine, std::move(endpoints.lanes), std::move(endpoints.control), std::move(config) }
{
}

ServerExit HostProcess::run()
{
    return _server.run();
}

void HostProcess::stop() noexcept
{
    _server.stop();
}

HostConfinement systemConfinement()
{
    return HostConfinement {
        .bindToParent = &bindToParent,
        .confine =
            [](uint64_t dataBytes) {
                if constexpr (BuildCapPolicy == CapPolicy::Skip)
                    std::println(stderr, "contour-extension-host: resource caps are off in this build");
                return capResources(dataBytes, BuildCapPolicy);
            },
    };
}

int runExtensionHost(std::span<std::string_view const> arguments,
                     EngineFactory const& engines,
                     HostConfinement const& confinement)
{
    auto const parsed = transport::parseHelperArguments(arguments);
    if (!parsed)
        return endWith(HostProcessExit::BadArguments);
    // First, so that the host never outlives Contour, whatever happens next.
    if (!confinement.bindToParent(parsed->parentProcessId))
        return endWith(HostProcessExit::ParentGone);
    auto endpoints = adoptEndpoints(*parsed);
    if (!endpoints)
        return endWith(endpoints.error());
    // Declared before the host, so that every object the host made of it goes first.
    auto const engine = engines();
    if (!engine)
        return endWith(HostProcessExit::EngineUnavailable);
    auto config = HostServerConfig {};
    config.confine = [&confinement, dataBytes = parsed->dataBytes] {
        return confinement.confine(dataBytes);
    };
    auto host = HostProcess { *engine, *std::move(endpoints), std::move(config) };
    return endWith(hostProcessExitOf(host.run()));
}

int runExtensionHost(std::span<std::string_view const> arguments, EngineFactory const& engines)
{
    return runExtensionHost(arguments, engines, systemConfinement());
}

} // namespace vtextension::wasm::host
