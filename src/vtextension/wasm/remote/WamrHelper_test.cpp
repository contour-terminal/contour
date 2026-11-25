// SPDX-License-Identifier: Apache-2.0
//
// The extension host on WAMR's engine, started as Contour starts it: what it says of itself.
#include <catch2/catch_test_macros.hpp>

#include <utility>

#include <vtextension/wasm/host/Sandbox.hpp>
#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>
#include <vtextension/wasm/remote/process/testing/HelperComposition.hpp>
#include <vtextension/wasm/remote/process/testing/LaunchedHelper.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/PduStream.hpp>

namespace proto = vtextension::wasm::remote::proto;
namespace process = vtextension::wasm::remote::process;
using vtextension::wasm::remote::transport::PduStream;

TEST_CASE("Helper.wamrHelperSaysCopy", "[vtextension]")
{
    // It copies memory wherever it runs, as WAMR places a linear memory where it chooses; and it locks itself
    // down where the build has a sandbox, its engine opening nothing once it did.
    constexpr auto Lockdown =
        vtextension::wasm::host::ThisPlatformSandbox == vtextension::wasm::host::PlatformSandbox::None
            ? proto::LockdownState::Unsandboxed
            : proto::LockdownState::LockedDown;
    auto helper = process::testing::launchHelper(
        process::makeSystemProcessLauncher(),
        process::testing::HelperLaunch { .executable = VTEXTENSION_HELPER_PATH,
                                         .environment = process::testing::helperEnvironment() });
    REQUIRE(helper.has_value());
    auto control = PduStream { std::move(helper->control),
                               proto::ChannelKind::Control,
                               proto::Direction::ToParent,
                               proto::PduLimits {} };
    auto const hello = control.receive();
    REQUIRE(hello.has_value());
    CHECK(*hello
          == proto::Pdu { proto::Hello { .version = proto::ProtocolVersion,
                                         .memoryMode = proto::MemoryMode::Copy,
                                         .lockdown = Lockdown } });

    // Contour closes the connection: the helper exits cleanly.
    {
        auto const closed = std::move(control);
    }
    CHECK(process::testing::awaitReap(*helper->launcher, *helper->process)
          == process::ExitStatus { .kind = process::ExitKind::Exited, .value = 0 });
}
