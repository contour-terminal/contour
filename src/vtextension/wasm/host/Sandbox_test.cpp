// SPDX-License-Identifier: Apache-2.0
//
// The sandbox seam: a system without a sandbox runs the host unsandboxed, and a step that failed is phrased
// from the one table both ends share.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include <vtextension/wasm/host/HostBootstrap.hpp>
#include <vtextension/wasm/host/Sandbox.hpp>
#include <vtextension/wasm/remote/proto/LockdownSteps.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>

using vtextension::wasm::host::ConfinementFailure;
using vtextension::wasm::host::confinementFailureText;
using vtextension::wasm::host::makeSystemSandbox;
using vtextension::wasm::host::NullSandbox;
using vtextension::wasm::host::PlatformSandbox;
using vtextension::wasm::host::SandboxDescriptors;
using vtextension::wasm::host::systemErrorText;
using vtextension::wasm::host::ThisPlatformSandbox;

namespace proto = vtextension::wasm::remote::proto;

TEST_CASE("Sandbox.nullSandboxTakesBothStepsUnsandboxed", "[vtextension][host]")
{
    auto sandbox = NullSandbox {};
    CHECK(sandbox.restrictBeforeThreads().has_value());
    CHECK(sandbox.lockDown().has_value());
    CHECK(sandbox.lockdownState() == proto::LockdownState::Unsandboxed);
}

TEST_CASE("Sandbox.systemWithoutOneRunsUnsandboxed", "[vtextension][host]")
{
    // A system without a sandbox makes the null one, which confines nothing. Where there is one, it would
    // confine this very process: the probe proves it in a child of its own (SandboxProbe_test.cpp).
    if constexpr (ThisPlatformSandbox != PlatformSandbox::None)
        SKIP("this system has a sandbox");
    auto const sandbox = makeSystemSandbox(SandboxDescriptors { .control = {} });
    REQUIRE(sandbox != nullptr);
    CHECK(sandbox->restrictBeforeThreads().has_value());
    CHECK(sandbox->lockDown().has_value());
    CHECK(sandbox->lockdownState() == proto::LockdownState::Unsandboxed);
}

TEST_CASE("Sandbox.failureSaysTheStepAndWhatTheSystemSaid", "[vtextension][host]")
{
    // The error code is read, not printed as a bare number: the line carries the system's word on it.
    auto const failure = ConfinementFailure { .step = proto::LockdownStep::SystemCalls, .code = 1 };
    auto const text = confinementFailureText(failure);
    INFO(text);
    CHECK(text
          == std::string { proto::lockdownStepText(proto::LockdownStep::SystemCalls) } + ": "
                 + systemErrorText(1));
    CHECK_FALSE(systemErrorText(1).empty());
}
