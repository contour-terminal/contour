// SPDX-License-Identifier: Apache-2.0
//
// The composition a hosting process runs its extension hosts under: one supervisor for every default backend
// while one lives, none once the last is gone, and no helper started where none is installed.
#include <core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/testing/TestModules.hpp>
#include <vtextension/wasm/BackendConfig.hpp>
#include <vtextension/wasm/BuildInstrumentation.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/remote/HostRefusals.hpp>
#include <vtextension/wasm/remote/process/HelperLocation.hpp>
#include <vtextension/wasm/remote/process/HostComposition.hpp>
#include <vtextension/wasm/remote/process/testing/FakeProcessLauncher.hpp>

namespace process = vtextension::wasm::remote::process;
using vtextension::ExtensionError;
using vtextension::wasm::BackendConfig;
using vtextension::wasm::BuildInstrumentation;
using vtextension::wasm::LockdownRequirement;
using vtextension::wasm::ModuleIdentity;
using vtextension::wasm::RuntimeLimits;
using vtextension::wasm::ThisBuildInstrumentation;
using vtextension::wasm::remote::HostRefusal;
using vtextension::wasm::remote::refusalLine;

namespace
{

/// A helper no case starts: nothing here loads a module through a composition that has one.
[[nodiscard]] std::filesystem::path unstartedHelper()
{
    return std::filesystem::temp_directory_path() / "contour-unstarted-extension-host";
}

/// @return A refusal log writer that drops every line: no case here that uses it starts a host.
[[nodiscard]] vtextension::wasm::remote::HostRefusalLog::Writer ignoredLines()
{
    return [](vtextension::wasm::remote::LineSeverity /*severity*/, std::string_view /*line*/) {
    };
}

/// @return A writer appending to @p lines.
[[nodiscard]] vtextension::wasm::remote::HostRefusalLog::Writer recordingInto(
    std::shared_ptr<std::vector<std::string>> lines)
{
    return [lines = std::move(lines)](vtextension::wasm::remote::LineSeverity /*severity*/,
                                      std::string_view line) {
        lines->emplace_back(line);
    };
}

/// Loads a module three times through @p backend, and checks that every load fails `HostUnavailable`.
void checkEveryLoadIsHostUnavailable(vtextension::wasm::IWasmBackend& backend)
{
    auto const runtime =
        backend.createRuntime(RuntimeLimits { .memoryBytes = 65536, .stackBytes = 65536 }, {});
    REQUIRE(runtime.has_value());
    for ([[maybe_unused]] auto const attempt: { 1, 2, 3 })
    {
        auto const loaded = (*runtime)->load(
            ModuleIdentity {}, vtextension::testing::toModule(vtextension::testing::MinimalModule));
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error() == ExtensionError::HostUnavailable);
    }
}

} // namespace

TEST_CASE("HostComposition.backendsShareOneSupervisor", "[vtextension]")
{
    auto made = std::weak_ptr<process::HostComposition> {};
    auto const* firstSupervisor = static_cast<process::ExtensionHostSupervisor const*>(nullptr);
    auto first = std::unique_ptr<vtextension::wasm::IWasmBackend> {};
    auto second = std::unique_ptr<vtextension::wasm::IWasmBackend> {};
    {
        auto const composition = process::sharedHostComposition(
            unstartedHelper(), LockdownRequirement::RequireLockedDown, ignoredLines());
        REQUIRE(composition->supervisor() != nullptr);
        made = composition;
        firstSupervisor = composition->supervisor();
        first = composition->backend(BackendConfig {});
    }
    {
        // While the first backend lives, the second shares its composition, and so its supervisor.
        auto const composition = process::sharedHostComposition(
            unstartedHelper(), LockdownRequirement::RequireLockedDown, ignoredLines());
        CHECK(composition == made.lock());
        CHECK(composition->supervisor() == firstSupervisor);
        second = composition->backend(BackendConfig {});
    }

    // The composition lives as long as the last backend: dropping it stops the supervisor.
    first.reset();
    CHECK_FALSE(made.expired());
    second.reset();
    CHECK(made.expired());

    // A third backend gets a composition, and a supervisor, of its own.
    auto const third = process::sharedHostComposition(
        unstartedHelper(), LockdownRequirement::RequireLockedDown, ignoredLines());
    CHECK(third->supervisor() != nullptr);
    CHECK(made.expired());
}

TEST_CASE("HostComposition.missingHelperRefusesEveryLoad", "[vtextension]")
{
    auto const fake = std::make_shared<process::testing::FakeProcessLauncher>();
    auto clock = core::ManualClock {};
    auto lines = std::make_shared<std::vector<std::string>>();
    {
        auto const composition = process::HostComposition::create(process::HostCompositionConfig {
            .launcher = fake,
            .clock = &clock,
            .helper = std::unexpected(process::HelperLocationError::NotFound),
            .refusals = recordingInto(lines),
            .teardownGrace = std::chrono::milliseconds { 0 },
        });
        CHECK(composition->supervisor() == nullptr);
        auto const backend = composition->backend(BackendConfig {});
        checkEveryLoadIsHostUnavailable(*backend);
    }
    // Nothing was started, and why is said once.
    CHECK(fake->launches().empty());
    CHECK(*lines == std::vector<std::string> { refusalLine(HostRefusal::HelperNotFound) });
}

TEST_CASE("HostComposition.teardownGraceFollowsTheBuild", "[vtextension]")
{
    // A helper of an instrumented build is given the time to write its report; one of any other build has
    // nothing to write, and is killed at once.
    STATIC_CHECK(process::teardownGraceFor(BuildInstrumentation::None) == std::chrono::milliseconds { 0 });
    STATIC_CHECK(process::teardownGraceFor(BuildInstrumentation::Coverage) > std::chrono::milliseconds { 0 });
    STATIC_CHECK(process::teardownGraceFor(BuildInstrumentation::Sanitizers)
                 > std::chrono::milliseconds { 0 });

    auto const expected = process::teardownGraceFor(ThisBuildInstrumentation);
    CHECK(process::HostCompositionConfig {}.teardownGrace == expected);
    CHECK(process::sharedHostComposition(
              unstartedHelper(), LockdownRequirement::RequireLockedDown, ignoredLines())
              ->teardownGrace()
          == expected);
}

TEST_CASE("HostComposition.eachLockdownRequirementHasItsOwn", "[vtextension]")
{
    // A refusal kept for good under one requirement -- a host running unconfined -- must not reach the
    // other's.
    auto const locked = process::sharedHostComposition(
        unstartedHelper(), LockdownRequirement::RequireLockedDown, ignoredLines());
    auto const unsandboxed = process::sharedHostComposition(
        unstartedHelper(), LockdownRequirement::AcceptUnsandboxed, ignoredLines());
    CHECK(locked != unsandboxed);
    CHECK(locked->supervisor() != unsandboxed->supervisor());
    CHECK(process::sharedHostComposition(
              unstartedHelper(), LockdownRequirement::RequireLockedDown, ignoredLines())
          == locked);
}

TEST_CASE("HostComposition.defaultBackendWithoutHelperRefusesEveryLoad", "[vtextension]")
{
    auto lines = std::make_shared<std::vector<std::string>>();
    {
        auto const backend = process::makeProcessBackend(
            BackendConfig {}, std::unexpected(process::HelperLocationError::NotFound), recordingInto(lines));
        checkEveryLoadIsHostUnavailable(*backend);
    }
    CHECK(*lines == std::vector<std::string> { refusalLine(HostRefusal::HelperNotFound) });
}
