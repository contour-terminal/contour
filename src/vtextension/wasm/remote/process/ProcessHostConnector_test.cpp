// SPDX-License-Identifier: Apache-2.0
//
// The connector that starts each host as a process: a host that cannot be started is said once per reason,
// and a host refused for a reason that lasts is the last it starts.
#include <core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
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
#include <vtextension/wasm/remote/process/testing/FakeProcessLauncher.hpp>

namespace process = vtextension::wasm::remote::process;
using vtextension::ExtensionError;
using vtextension::wasm::remote::HostRefusal;
using vtextension::wasm::remote::refusalLine;
using vtextension::wasm::remote::RefusalPersistence;

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
    CHECK(*lines == std::vector<std::string> { refusalLine(HostRefusal::HelperNotFound) });
}

TEST_CASE("ProcessHostConnector.lastingRefusalsStartNoOtherHost", "[vtextension]")
{
    struct Expectation
    {
        HostRefusal refusal;
        RefusalPersistence persistence; ///< Lasting: no host is started after it.
    };
    auto const expectation = GENERATE(
        Expectation { .refusal = HostRefusal::HelperNotFound, .persistence = RefusalPersistence::Transient },
        Expectation { .refusal = HostRefusal::OutOfResources, .persistence = RefusalPersistence::Transient },
        Expectation { .refusal = HostRefusal::Unsupported, .persistence = RefusalPersistence::Transient },
        Expectation { .refusal = HostRefusal::LaunchRefused, .persistence = RefusalPersistence::Transient },
        Expectation { .refusal = HostRefusal::ProtocolVersion, .persistence = RefusalPersistence::Lasting },
        Expectation { .refusal = HostRefusal::LockdownFailed, .persistence = RefusalPersistence::Lasting },
        Expectation { .refusal = HostRefusal::Unsandboxed, .persistence = RefusalPersistence::Lasting });
    INFO(refusalLine(expectation.refusal));
    CHECK(vtextension::wasm::remote::refusalPersistence(expectation.refusal) == expectation.persistence);

    auto const fake = std::make_shared<process::testing::FakeProcessLauncher>();
    auto clock = core::ManualClock {};
    auto supervisor = process::ExtensionHostSupervisor::start(process::SupervisorConfig {
        .launcher = fake, .clock = &clock, .teardownGrace = std::chrono::milliseconds { 0 } });
    REQUIRE(supervisor.has_value());
    {
        auto connector =
            process::ProcessHostConnector { (*supervisor)->mailbox(),
                                            process::ProcessHostConfig { .helper = "contour-extension-host",
                                                                         .environment = {},
                                                                         .extraArguments = {},
                                                                         .caps = {},
                                                                         .standardError = std::nullopt },
                                            [](std::string_view /*line*/) {} };
        // The host of a first connection was refused, as the handshake tells the connector.
        REQUIRE(connector.connect().has_value());
        connector.refused(expectation.refusal);

        auto const again = connector.connect();
        if (expectation.persistence == RefusalPersistence::Lasting)
        {
            REQUIRE_FALSE(again.has_value());
            CHECK(again.error() == ExtensionError::HostUnavailable);
        }
        else
            CHECK(again.has_value());
    }
    auto const launched = fake->launches().size();
    auto const expectedLaunches = expectation.persistence == RefusalPersistence::Lasting ? 1U : 2U;
    CHECK(launched == expectedLaunches);
}
