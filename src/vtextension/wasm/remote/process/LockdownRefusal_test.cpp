// SPDX-License-Identifier: Apache-2.0
//
// A host that failed to confine itself is refused at the handshake, and its connector starts no other: the
// failure is the system's and the build's, and every host it started would fail alike.
#include <core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/BackendConfig.hpp>
#include <vtextension/wasm/remote/HelperIncarnation.hpp>
#include <vtextension/wasm/remote/HostRefusals.hpp>
#include <vtextension/wasm/remote/process/ExtensionHostSupervisor.hpp>
#include <vtextension/wasm/remote/process/ProcessHostConnector.hpp>
#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>
#include <vtextension/wasm/remote/process/testing/FakeProcessLauncher.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/PduStream.hpp>
#include <vtextension/wasm/remote/transport/testing/HandleCopies.hpp>

namespace process = vtextension::wasm::remote::process;
namespace proto = vtextension::wasm::remote::proto;
namespace transport = vtextension::wasm::remote::transport;
using vtextension::ExtensionError;
using vtextension::wasm::LockdownRequirement;
using vtextension::wasm::remote::HelperIncarnation;
using vtextension::wasm::remote::HelperPolicy;
using vtextension::wasm::remote::HostRefusal;
using vtextension::wasm::remote::HostRefusalLog;
using vtextension::wasm::remote::refusalLine;

namespace
{

/// The step the fake host says it failed at, and what the system said of it.
constexpr auto FailedStep = proto::LockdownStep::FileSystem;
constexpr auto FailureCode = uint32_t { 13 };

/// Plays the host a fake launch starts: says, on the control channel it inherited, that it failed to confine
/// itself, then ends its end.
void sayLockdownFailed([[maybe_unused]] std::size_t index, process::LaunchSpec const& spec)
{
    REQUIRE_FALSE(spec.inherited.empty());
    // The spec's handle is the connector's until the launch returns: the host speaks through a copy.
    auto const copy = transport::testing::duplicate(spec.inherited.front());
    REQUIRE(copy.has_value());
    auto channel = transport::Channel::adopt(*copy);
    REQUIRE(channel.has_value());
    auto host = transport::PduStream {
        *std::move(channel), proto::ChannelKind::Control, proto::Direction::ToHelper, proto::PduLimits {}
    };
    auto const sent = host.send(proto::Hello { .version = proto::ProtocolVersion,
                                               .memoryMode = proto::MemoryMode::Copy,
                                               .lockdown = proto::LockdownState::Failed,
                                               .failedStep = FailedStep,
                                               .failureCode = FailureCode });
    REQUIRE(sent.has_value());
}

} // namespace

TEST_CASE("ProcessHostConnector.lockdownFailureIsNotRelaunched", "[vtextension][remote]")
{
    auto const fake = std::make_shared<process::testing::FakeProcessLauncher>(&sayLockdownFailed);
    auto clock = core::ManualClock {};
    auto supervisor = process::ExtensionHostSupervisor::start(process::SupervisorConfig {
        .launcher = fake, .clock = &clock, .teardownGrace = std::chrono::milliseconds { 0 } });
    REQUIRE(supervisor.has_value());
    // Contour's error log, which the connector and the backend both write to; read once both are gone, as
    // they write on this thread.
    auto lines = std::make_shared<std::vector<std::string>>();
    auto const writer = [lines](std::string_view line) {
        lines->emplace_back(line);
    };
    {
        auto connector =
            process::ProcessHostConnector { (*supervisor)->mailbox(),
                                            process::ProcessHostConfig { .helper = "contour-extension-host",
                                                                         .environment = {},
                                                                         .extraArguments = {},
                                                                         .caps = {},
                                                                         .standardError = std::nullopt },
                                            writer };
        auto refusals = HostRefusalLog { writer };
        auto const runtime = proto::CreateRuntime { .runtimeId = HelperIncarnation::RuntimeId,
                                                    .memoryBytes = 65536,
                                                    .stackBytes = 65536,
                                                    .functions = {} };
        auto const policy =
            HelperPolicy { .timeouts = {}, .lockdown = LockdownRequirement::AcceptUnsandboxed };

        // The first host is started, says it failed, and is refused.
        auto const first = HelperIncarnation::connect(connector, runtime, policy, refusals);
        REQUIRE_FALSE(first.has_value());
        CHECK(first.error() == ExtensionError::HostUnavailable);
        CHECK(fake->launches().size() == 1);

        // No later attempt starts another.
        for ([[maybe_unused]] auto const attempt: { 1, 2, 3 })
        {
            auto const again = HelperIncarnation::connect(connector, runtime, policy, refusals);
            REQUIRE_FALSE(again.has_value());
            CHECK(again.error() == ExtensionError::HostUnavailable);
        }
        CHECK(fake->launches().size() == 1);
    }
    // Said once, naming the step the host failed at.
    CHECK(*lines == std::vector<std::string> { refusalLine(HostRefusal::LockdownFailed, FailedStep) });
}
