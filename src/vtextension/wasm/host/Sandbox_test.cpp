// SPDX-License-Identifier: Apache-2.0
//
// The sandbox seam: a system without a sandbox runs the host unsandboxed, and a step that failed is phrased
// from the one table both ends share.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <ranges>
#include <set>
#include <string>
#include <utility>

#include <vtextension/wasm/host/DescriptorRights.hpp>
#include <vtextension/wasm/host/HostBootstrap.hpp>
#include <vtextension/wasm/host/Sandbox.hpp>
#include <vtextension/wasm/remote/proto/LockdownSteps.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>

using vtextension::wasm::host::ConfinementFailure;
using vtextension::wasm::host::confinementFailureText;
using vtextension::wasm::host::DescriptorRight;
using vtextension::wasm::host::DescriptorRole;
using vtextension::wasm::host::descriptorRoleRights;
using vtextension::wasm::host::GuestCode;
using vtextension::wasm::host::makeSystemSandbox;
using vtextension::wasm::host::NullSandbox;
using vtextension::wasm::host::PlatformSandbox;
using vtextension::wasm::host::rightsOf;
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
    auto const sandbox = makeSystemSandbox(SandboxDescriptors { .control = {} }, GuestCode::Interpreted);
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

TEST_CASE("Sandbox.rightsTableCoversEveryRole", "[vtextension][host]")
{
    // One row per role, at its index, up to the last; each names what its role is used for, once each.
    auto const rows = descriptorRoleRights();
    REQUIRE(rows.size() == std::to_underlying(DescriptorRole::StopEvent) + std::size_t { 1 });
    for (auto const index: std::views::iota(std::size_t { 0 }, rows.size()))
    {
        auto const& row = rows[index];
        INFO("role " << index);
        auto const roleIndex = std::size_t { std::to_underlying(row.role) };
        CHECK(roleIndex == index);
        CHECK_FALSE(row.rights.empty());
        CHECK(std::set<DescriptorRight>(row.rights.begin(), row.rights.end()).size() == row.rights.size());
        CHECK(std::ranges::equal(rightsOf(row.role), row.rights));
    }

    // What the host does with each after the lockdown: it talks over its channels and waits on its wake end,
    // and never writes to the page nor reads its standard error. The engine's device takes what a guest's
    // stdio does: writes, seeks, and its flags.
    auto const has = [](DescriptorRole role, DescriptorRight right) {
        return std::ranges::find(rightsOf(role), right) != rightsOf(role).end();
    };
    for (auto const role: { DescriptorRole::Control, DescriptorRole::Lane })
        for (auto const right: { DescriptorRight::Receive, DescriptorRight::Send, DescriptorRight::Event })
            CHECK(has(role, right));
    CHECK(has(DescriptorRole::Wake, DescriptorRight::Event));
    CHECK_FALSE(has(DescriptorRole::Page, DescriptorRight::Write));
    CHECK_FALSE(has(DescriptorRole::StandardError, DescriptorRight::Read));
    for (auto const right: { DescriptorRight::Write, DescriptorRight::Seek, DescriptorRight::GetStatusFlags })
        CHECK(has(DescriptorRole::EngineDevice, right));
    // Standard input and output are only asked what they are; the stop pipe is waited on at one end and
    // written to at the other.
    CHECK(std::ranges::equal(rightsOf(DescriptorRole::IdleStream), std::array { DescriptorRight::Fstat }));
    CHECK(has(DescriptorRole::StopReceiver, DescriptorRight::Event));
    CHECK_FALSE(has(DescriptorRole::StopReceiver, DescriptorRight::Write));
    CHECK(std::ranges::equal(rightsOf(DescriptorRole::StopSender), std::array { DescriptorRight::Write }));
    // The stop event is both ends at once: what each of them may, and no more.
    CHECK(std::ranges::equal(
        rightsOf(DescriptorRole::StopEvent),
        std::array { DescriptorRight::Read, DescriptorRight::Write, DescriptorRight::Event }));
}
