// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/process/testing/HelperComposition.hpp>

#include <core/Clock.hpp>
#include <core/Environment.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace vtextension::wasm::remote::process::testing
{

namespace
{
    /// The sanitizer variables a helper is handed when this process has them (D39).
    constexpr auto SanitizerVariables =
        std::array<std::string_view, 4> { "ASAN_OPTIONS", "LSAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS" };

    /// How long past the teardown grace awaitEveryHelperReaped() waits: a killed helper's reap included.
    constexpr auto ReapSlack = std::chrono::seconds { 3 };
} // namespace

std::vector<std::pair<std::string, std::string>> helperEnvironment()
{
    auto environment = defaultHelperEnvironment();
    auto const live = core::LiveEnvironment {};
    for (auto const name: SanitizerVariables)
        if (auto value = live.get(name))
            environment.emplace_back(std::string { name }, *std::move(value));
    return environment;
}

HelperComposition::HelperComposition():
    _recording { std::make_shared<RecordingLauncher>(makeSystemProcessLauncher()) },
    _supervisor { [this] {
        auto started =
            ExtensionHostSupervisor::start(SupervisorConfig { .launcher = _recording,
                                                              .clock = &core::defaultSteadyClock(),
                                                              .teardownGrace = HelperTeardownGrace });
        if (!started)
        {
            std::fputs("HelperComposition: the supervisor could not start\n", stderr);
            std::abort();
        }
        return std::move(*started);
    }() },
    _kind { makeRemoteKindState() }
{
}

std::shared_ptr<ProcessHostConnector> HelperComposition::connector(HelperBackendConfig const& config) const
{
    return std::make_shared<ProcessHostConnector>(_supervisor->mailbox(),
                                                  ProcessHostConfig { .helper = config.helper,
                                                                      .environment = helperEnvironment(),
                                                                      .extraArguments = config.extraArguments,
                                                                      .caps = {} });
}

std::unique_ptr<IWasmBackend> HelperComposition::backend(HelperBackendConfig const& config) const
{
    return makeRemoteBackend(_kind,
                             BackendConfig { .helper = config.helper,
                                             .timeouts = config.timeouts,
                                             .connector = connector(config),
                                             .lockdown = config.lockdown });
}

std::size_t HelperComposition::killsFor(KillReason reason) const
{
    // A kill's onDeath runs before its batch's counters are published: sync() first.
    _supervisor->sync();
    return _supervisor->counters().kills.at(std::to_underlying(reason));
}

bool HelperComposition::awaitEveryHelperReaped() const
{
    using namespace std::chrono_literals;
    auto const deadline = std::chrono::steady_clock::now() + HelperTeardownGrace + ReapSlack;
    while (_supervisor->liveProcesses() != 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    return _supervisor->liveProcesses() == 0;
}

HelperComposition& binaryComposition()
{
    static auto composition = HelperComposition {};
    return composition;
}

} // namespace vtextension::wasm::remote::process::testing
