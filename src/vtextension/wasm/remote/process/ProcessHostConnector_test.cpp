// SPDX-License-Identifier: Apache-2.0
//
// The connector that starts each host as a process: a host that cannot be started is said once per reason.
#include <core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/remote/HostRefusals.hpp>
#include <vtextension/wasm/remote/process/ExtensionHostSupervisor.hpp>
#include <vtextension/wasm/remote/process/ProcessHostConnector.hpp>
#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>

namespace process = vtextension::wasm::remote::process;
using vtextension::ExtensionError;
using vtextension::wasm::remote::HostRefusal;
using vtextension::wasm::remote::hostRefusals;

TEST_CASE("ProcessHostConnector.launchFailureIsLoggedOncePerReason", "[vtextension]")
{
    auto supervisor = process::ExtensionHostSupervisor::start(
        process::SupervisorConfig { .launcher = process::makeSystemProcessLauncher(),
                                    .clock = &core::defaultSteadyClock(),
                                    .teardownGrace = {} });
    REQUIRE(supervisor.has_value());
    // Read only after the connector, and with it its writer, is gone: the connector writes on this thread.
    auto lines = std::make_shared<std::vector<std::string>>();
    {
        auto connector = process::ProcessHostConnector {
            (*supervisor)->mailbox(),
            process::ProcessHostConfig { .helper = std::filesystem::temp_directory_path()
                                                   / "contour-no-such-extension-host",
                                         .environment = process::defaultHelperEnvironment(),
                                         .extraArguments = {},
                                         .caps = {},
                                         .standardError = std::nullopt },
            [lines](std::string_view line) { lines->emplace_back(line); }
        };
        // Every attempt fails; the first says why.
        for ([[maybe_unused]] auto const attempt: { 1, 2, 3 })
        {
            auto const connected = connector.connect();
            REQUIRE_FALSE(connected.has_value());
            CHECK(connected.error() == ExtensionError::HostUnavailable);
        }
    }
    CHECK(*lines
          == std::vector<std::string> {
              std::string { hostRefusals()[static_cast<std::size_t>(HostRefusal::HelperNotFound)].text } });
}
