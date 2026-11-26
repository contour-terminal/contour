// SPDX-License-Identifier: Apache-2.0
//
// The extension host on WAMR's engine, started as Contour starts it: what it says of itself.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string>
#include <utility>
#include <variant>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/testing/TestModules.hpp>
#include <vtextension/wasm/BackendConfig.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>
#include <vtextension/wasm/remote/process/testing/HelperComposition.hpp>
#include <vtextension/wasm/remote/process/testing/LaunchedHelper.hpp>
#include <vtextension/wasm/remote/proto/LockdownSteps.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/PduStream.hpp>
#include <vtextension/wasm/testing/BackendContract.hpp>
#include <vtextension/wasm/testing/Wine.hpp>

namespace proto = vtextension::wasm::remote::proto;
namespace process = vtextension::wasm::remote::process;
using vtextension::wasm::remote::transport::PduStream;

TEST_CASE("Helper.wamrHelperSaysCopy", "[vtextension]")
{
    // It copies memory wherever it runs, as WAMR places a linear memory where it chooses; and it locks itself
    // down where the build has a sandbox, its engine opening nothing once it did.
    auto const lockdown =
        process::testing::helperLockdown() == vtextension::wasm::LockdownRequirement::RequireLockedDown
            ? proto::LockdownState::LockedDown
            : proto::LockdownState::Unsandboxed;
    auto helper = process::testing::launchHelper(
        process::makeSystemProcessLauncher(),
        process::testing::HelperLaunch { .executable = VTEXTENSION_HELPER_PATH,
                                         .environment = process::testing::helperEnvironment(),
                                         .isolation = process::testing::helperIsolation() });
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
                                         .lockdown = lockdown } });

    // Contour closes the connection: the helper exits cleanly.
    {
        auto const closed = std::move(control);
    }
    CHECK(process::testing::awaitReap(*helper->launcher, *helper->process)
          == process::ExitStatus { .kind = process::ExitKind::Exited, .value = 0 });
}

#ifdef _WIN32
namespace contract = vtextension::wasm::testing::contract;

namespace
{

/// Starts the helper as Contour does, but in @p isolation, its stderr captured.
/// @return Its Hello; fails the case, saying how the helper ended and what it wrote, if none arrives.
[[nodiscard]] proto::Hello helloOfHelperIn(process::ProcessIsolation isolation)
{
    auto standardError = process::testing::CapturedStandardError::create();
    REQUIRE(standardError.has_value());
    auto helper = process::testing::launchHelper(
        process::makeSystemProcessLauncher(),
        process::testing::HelperLaunch { .executable = VTEXTENSION_HELPER_PATH,
                                         .environment = process::testing::helperEnvironment(),
                                         .standardError = standardError->handle(),
                                         .isolation = isolation });
    INFO("launch: " << (helper ? std::string { "started" }
                               : std::string { process::launchErrorText(helper.error()) }));
    REQUIRE(helper.has_value());
    auto control = PduStream { std::move(helper->control),
                               proto::ChannelKind::Control,
                               proto::Direction::ToParent,
                               proto::PduLimits {} };
    auto const hello = control.receive();
    auto const* const said = hello ? std::get_if<proto::Hello>(&*hello) : nullptr;
    if (said == nullptr)
        FAIL("no Hello: " << process::testing::endOf(
                 *helper->launcher, *helper->process, [&] { return standardError->text(); }));
    // What the helper said, whatever it was: what a failing Windows run needs first.
    UNSCOPED_INFO(std::format("Hello: lockdown={} failed step: {} code=0x{:08X}",
                              std::to_underlying(said->lockdown),
                              proto::lockdownStepText(said->failedStep),
                              said->failureCode));
    auto const result = *said;
    {
        auto const closed = std::move(control);
    }
    process::testing::checkEndedCleanly(
        *helper->launcher, *helper->process, [&] { return standardError->text(); });
    return result;
}

} // namespace

TEST_CASE("Helper.windowsHelperIsLockedDown", "[vtextension][appcontainer]")
{
    // Started in its AppContainer, as Contour starts it, the helper finds its token contained, turns on its
    // mitigations, finds its job, and says LockedDown. Under wine, which stubs AppContainers, it is started
    // uncontained, and must say so: Unsandboxed, never a lockdown it does not have.
    auto const hello = helloOfHelperIn(process::testing::helperIsolation());
    if (vtextension::wasm::testing::runsOnWine())
    {
        CHECK(hello.lockdown == proto::LockdownState::Unsandboxed);
        WARN("wine: the uncontained helper says Unsandboxed, as it must; containment is proved on Windows");
    }
    else
        CHECK(hello.lockdown == proto::LockdownState::LockedDown);
}

TEST_CASE("Helper.windowsUncontainedHelperIsRefused", "[vtextension][appcontainer]")
{
    // Started in no container, the helper says Unsandboxed -- what only its parent can give it is never
    // pretended -- and a backend that requires a lockdown drives no such helper.
    CHECK(helloOfHelperIn(process::ProcessIsolation::None).lockdown == proto::LockdownState::Unsandboxed);

    {
        auto const backend =
            process::testing::binaryComposition().backend(process::testing::HelperBackendConfig {
                .helper = VTEXTENSION_HELPER_PATH,
                .extraArguments = {},
                .lockdown = vtextension::wasm::LockdownRequirement::RequireLockedDown,
                .timeouts = {},
                .isolation = process::ProcessIsolation::None });
        auto const runtime = contract::createRuntime(*backend);
        auto const loaded = runtime->load(vtextension::wasm::ModuleIdentity {},
                                          vtextension::testing::toModule(contract::InstanceModule));
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error() == vtextension::ExtensionError::HostUnavailable);
    }
    CHECK(process::testing::binaryComposition().awaitEveryHelperReaped());
}
#endif
