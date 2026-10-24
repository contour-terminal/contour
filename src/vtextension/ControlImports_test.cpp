// SPDX-License-Identifier: Apache-2.0
//
// The control interface's imports: their table, what one does with a guest's request -- lift, check, call,
// store -- and what it refuses to do, against a fake linear memory; then the same through an Instance, on the
// fake backend (to drive the deadline) and on the real one (to link and call an import).
#include <core/platform/Clock.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include <vtcontrol/Api.hpp>
#include <vtcontrol/Catalog.hpp>
#include <vtcontrol/ControlError.hpp>
#include <vtcontrol/Wit.hpp>
#include <vtextension/CompiledExtension.hpp>
#include <vtextension/ControlCodec.hpp>
#include <vtextension/ControlImports.hpp>
#include <vtextension/Engine.hpp>
#include <vtextension/HostBindings.hpp>
#include <vtextension/Instance.hpp>
#include <vtextension/testing/FakeGuestMemory.hpp>
#include <vtextension/testing/FakeWasmBackend.hpp>
#include <vtextension/testing/Harness.hpp>
#include <vtextension/testing/TestModules.hpp>
#include <vtextension/wasm/DefaultBackend.hpp>

using namespace std::chrono_literals;
using namespace vtextension;
namespace abi = vtextension::abi;
using vtcontrol::ControlErrorCode;
using vtcontrol::ControlGrants;
using vtcontrol::Permission;
using vtextension::testing::errorOf;
using vtextension::testing::FakeHostServices;
using vtextension::testing::surface;
using vtextension::testing::toModule;
using FakeMemory = vtextension::testing::FakeGuestMemory;

namespace
{

/// The index of the control import named @p name in controlHostFunctions().
[[nodiscard]] std::size_t importIndexOf(std::string_view name)
{
    auto const functions = controlHostFunctions();
    auto const found = std::ranges::find(functions, name, &wasm::HostFunction::name);
    REQUIRE(found != functions.end());
    return static_cast<std::size_t>(found - functions.begin());
}

/// What a guest sets aside for an import's result: more than any of them takes.
constexpr auto ResultSlotBytes = uint32_t { 256 };

/// A guest's call of one import, against a fake memory: the request lowered flat, a result slot allocated.
template <typename Request>
struct GuestCall
{
    explicit GuestCall(Request const& request)
    {
        REQUIRE(abi::Codec<Request>::lowerFlat(memory, request, arguments).has_value());
        auto const address = memory.allocate(ResultSlotBytes, 4);
        REQUIRE(address.has_value());
        resultAddress = *address;
        arguments.push_back(resultAddress);
    }

    /// Calls the import @p name with the call's arguments.
    [[nodiscard]] abi::Result<void> call(std::string_view name,
                                         FakeHostServices& services,
                                         ControlGrants declared,
                                         std::chrono::milliseconds remaining = 40ms)
    {
        return callControlImport(importIndexOf(name),
                                 memory,
                                 arguments,
                                 services,
                                 declared,
                                 ControlCallBudget { .remaining = remaining, .deadline = deadline });
    }

    /// @return The result the import stored, as the guest would read it.
    template <typename Reply>
    [[nodiscard]] std::expected<Reply, vtcontrol::ControlError> result()
    {
        auto loaded = abi::Codec<std::expected<Reply, vtcontrol::ControlError>>::load(memory, resultAddress);
        REQUIRE(loaded.has_value());
        return std::move(*loaded);
    }

    /// @return Whether the result slot is untouched: all zeros, as it was allocated.
    [[nodiscard]] bool resultUntouched()
    {
        auto const bytes = memory.view(resultAddress, ResultSlotBytes);
        REQUIRE(bytes.has_value());
        return std::ranges::all_of(*bytes, [](std::byte byte) { return byte == std::byte { 0 }; });
    }

    FakeMemory memory;
    abi::FlatValues arguments;
    abi::Address resultAddress = 0;
    std::atomic<DeadlineState> deadline = DeadlineState::Running;
};

[[nodiscard]] vtcontrol::SplitRequest splitRequest()
{
    return vtcontrol::SplitRequest { .pane = "pane:x",
                                     .direction = vtcontrol::SplitDirection::Below,
                                     .ratioPercent = 30,
                                     .cwd = "",
                                     .command = { "tail", "-f", "log" },
                                     .label = "",
                                     .focus = vtcontrol::FocusPolicy::Keep };
}

/// What splitRequest() needs declared: it names a command, so `input` beside the method's `write`.
constexpr auto SplitWithCommand = ControlGrants::of({ Permission::Write, Permission::Input });

[[nodiscard]] std::unexpected<vtcontrol::ControlError> controlError(ControlErrorCode code,
                                                                    std::string message)
{
    return std::unexpected(
        vtcontrol::ControlError { .code = code, .message = std::move(message), .candidates = {} });
}

} // namespace

TEST_CASE("the control imports are the everywhere-verbs, named and shaped as the WIT says",
          "[vtextension][control]")
{
    auto const functions = controlHostFunctions();
    CHECK(functions.size() == ControlImportCount);
    CHECK(ControlImportCount == 13);
    CHECK(std::ranges::all_of(functions, [](wasm::HostFunction const& function) {
        return function.module == ControlImportModule;
    }));
    // A request's flat values, then the result's address.
    CHECK(functions[importIndexOf("pane-split")].parameterCount == 13);
    CHECK(functions[importIndexOf("daemon-info")].parameterCount == 1);
    CHECK(functions[importIndexOf("object-set-label")].parameterCount == 5);
    CHECK(functions[importIndexOf("workspace-list")].parameterCount == 3);
    for (auto const socketOnly: { "block-wait", "events-subscribe", "events-unsubscribe" })
        CHECK(std::ranges::find(functions, std::string_view { socketOnly }, &wasm::HostFunction::name)
              == functions.end());
}

TEST_CASE("each control import is named as the WIT renders its verb", "[vtextension][control]")
{
    // The imports spell their names at compile time, the WIT at run time (vtcontrol::kebabCase()): two
    // spellings of one rule, held to each other here for every verb an extension may call.
    auto const functions = controlHostFunctions();
    auto names = std::vector<std::string> {};
    std::apply(
        [&](auto const&... row) {
            ((row.exposure == vtcontrol::Exposure::Everywhere
                  ? names.push_back(vtcontrol::kebabCase(row.name))
                  : void()),
             ...);
        },
        vtcontrol::Methods);
    REQUIRE(names.size() == functions.size());
    for (auto const& [function, name]: std::views::zip(functions, names))
        CHECK(function.name == name);
}

TEST_CASE("a control import lifts, checks and stores", "[vtextension][control]")
{
    auto services = FakeHostServices {};
    services.controlAnswer = R"({"tab":"tab:t","pane":"pane:n"})";
    auto call = GuestCall { splitRequest() };

    REQUIRE(call.call("pane-split", services, SplitWithCommand, 37ms).has_value());

    REQUIRE(services.controlCalls.size() == 1);
    auto const& seen = services.controlCalls.front();
    CHECK(seen.method == "pane.split");
    CHECK(seen.params.contains(R"("direction":"below")"));
    CHECK(seen.params.contains(R"("command":["tail","-f","log"])"));
    CHECK(seen.granted == SplitWithCommand);
    CHECK(seen.timeout == 37ms);

    auto const result = call.result<vtcontrol::SplitReply>();
    REQUIRE(result.has_value());
    CHECK(result->tab == "tab:t");
    CHECK(result->pane == "pane:n");
}

TEST_CASE("a control error is the guest's result, not a trap", "[vtextension][control]")
{
    auto services = FakeHostServices {};
    auto call = GuestCall { splitRequest() };

    SECTION("the host's error")
    {
        services.controlAnswer = controlError(ControlErrorCode::NotFound, "no pane 'x'");
        REQUIRE(call.call("pane-split", services, SplitWithCommand).has_value());
        auto const result = call.result<vtcontrol::SplitReply>();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ControlErrorCode::NotFound);
        CHECK(result.error().message == "no pane 'x'");
    }
    SECTION("a reply that does not decode")
    {
        services.controlAnswer = R"({"tab":7})";
        REQUIRE(call.call("pane-split", services, SplitWithCommand).has_value());
        auto const result = call.result<vtcontrol::SplitReply>();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ControlErrorCode::Failed);
        CHECK(result.error().message.contains("does not decode"));
    }
}

TEST_CASE("an undeclared or ungranted permission is Forbidden without a call", "[vtextension][control]")
{
    auto services = FakeHostServices {};
    services.controlAnswer = R"({"tab":"tab:t","pane":"pane:n"})";
    auto call = GuestCall { splitRequest() };
    auto declared = ControlGrants::of({ Permission::Write });
    auto expected = std::string {};

    SECTION("not in the manifest")
    {
        declared = ControlGrants::of({ Permission::Read, Permission::Input });
        expected = "method 'pane.split' needs the manifest line 'control write'";
    }
    SECTION("not granted yet")
    {
        services.consent = GrantState::Pending;
        expected = "method 'pane.split' needs grant 'write': not granted yet";
    }
    SECTION("denied")
    {
        services.consent = GrantState::Denied;
        expected = "method 'pane.split' needs grant 'write': denied";
    }

    REQUIRE(call.call("pane-split", services, declared).has_value());
    CHECK(services.controlCalls.empty());
    auto const result = call.result<vtcontrol::SplitReply>();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ControlErrorCode::Forbidden);
    CHECK(result.error().message == expected);
}

TEST_CASE("running a command needs the manifest line 'control input' beside 'control write'",
          "[vtextension][control]")
{
    // A guest granted `write` may rearrange the workspace, not run what it likes in it: the catalog's
    // requestGrant rule, which dispatch() applies to a socket's grants, applies to the manifest's here.
    auto services = FakeHostServices {};
    services.controlAnswer = R"({"tab":"tab:t","pane":"pane:n"})";
    auto const write = ControlGrants::of({ Permission::Write });

    SECTION("a split naming a command, without input")
    {
        auto call = GuestCall { splitRequest() };
        REQUIRE(call.call("pane-split", services, write).has_value());
        CHECK(services.controlCalls.empty());
        auto const result = call.result<vtcontrol::SplitReply>();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ControlErrorCode::Forbidden);
        CHECK(result.error().message
              == "method 'pane.split': running a command needs the manifest line 'control input'");
    }
    SECTION("a new tab naming a command, without input")
    {
        auto call =
            GuestCall { vtcontrol::CreateTabRequest { .window = "",
                                                      .placement = vtcontrol::WindowPlacement::Existing,
                                                      .cwd = "",
                                                      .command = { "make" },
                                                      .label = "",
                                                      .focus = vtcontrol::FocusPolicy::Keep } };
        REQUIRE(call.call("tab-create", services, write).has_value());
        CHECK(services.controlCalls.empty());
        auto const result = call.result<vtcontrol::CreateTabReply>();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ControlErrorCode::Forbidden);
    }
    SECTION("a split naming no command needs write alone")
    {
        auto request = splitRequest();
        request.command.clear();
        auto call = GuestCall { request };
        REQUIRE(call.call("pane-split", services, write).has_value());
        CHECK(services.controlCalls.size() == 1);
        CHECK(call.result<vtcontrol::SplitReply>().has_value());
    }
    SECTION("a split naming a command, with input")
    {
        auto call = GuestCall { splitRequest() };
        auto const writeAndInput = ControlGrants::of({ Permission::Write, Permission::Input });
        REQUIRE(call.call("pane-split", services, writeAndInput).has_value());
        REQUIRE(services.controlCalls.size() == 1);
        CHECK(services.controlCalls.front().granted == writeAndInput);
        CHECK(call.result<vtcontrol::SplitReply>().has_value());
    }
}

TEST_CASE("the control API's limitReached reaches a guest as its own error code", "[vtextension][control]")
{
    auto services = FakeHostServices {};
    services.controlAnswer = controlError(ControlErrorCode::LimitReached, "too many");
    auto call = GuestCall { vtcontrol::DaemonInfoRequest {} };
    REQUIRE(call.call("daemon-info", services, ControlGrants::of({ Permission::Read })).has_value());
    auto const result = call.result<vtcontrol::DaemonInfoReply>();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ControlErrorCode::LimitReached);
    CHECK(result.error().message == "too many");
}

TEST_CASE("send with wait is refused for extensions", "[vtextension][control]")
{
    auto services = FakeHostServices {};
    services.controlAnswer = R"({})";
    auto const input = ControlGrants::of({ Permission::Input });

    auto waiting = GuestCall { vtcontrol::SendRequest {
        .pane = "", .text = "make\n", .wait = vtcontrol::SendWait::CommandFinished } };
    REQUIRE(waiting.call("pane-send", services, input).has_value());
    CHECK(services.controlCalls.empty());
    auto const refused = waiting.result<vtcontrol::SendReply>();
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ControlErrorCode::BadRequest);
    CHECK(refused.error().message.contains("'wait' must be 'none'"));

    auto plain = GuestCall { vtcontrol::SendRequest {
        .pane = "", .text = "make\n", .wait = vtcontrol::SendWait::None } };
    REQUIRE(plain.call("pane-send", services, input).has_value());
    REQUIRE(services.controlCalls.size() == 1);
    CHECK(plain.result<vtcontrol::SendReply>().has_value());
}

TEST_CASE("a control import whose deadline passed while the host answered writes nothing",
          "[vtextension][control]")
{
    // The host's answer arrives after the watchdog marked the guest's deadline expired: the guest is being
    // interrupted, and the answer is dropped instead of being written into its memory.
    auto services = FakeHostServices {};
    services.controlAnswer = R"({"tab":"tab:t","pane":"pane:n"})";
    auto call = GuestCall { splitRequest() };
    services.beforeControlAnswer = [&] {
        call.deadline.store(DeadlineState::Expired);
    };

    CHECK(errorOf(call.call("pane-split", services, SplitWithCommand)) == ExtensionError::DeadlineExceeded);
    CHECK(services.controlCalls.size() == 1);
    CHECK(call.resultUntouched());
}

TEST_CASE("a control import with no time left calls nothing", "[vtextension][control]")
{
    auto services = FakeHostServices {};
    services.controlAnswer = R"({"tab":"tab:t","pane":"pane:n"})";
    auto call = GuestCall { splitRequest() };

    CHECK(errorOf(call.call("pane-split", services, SplitWithCommand, 0ms))
          == ExtensionError::DeadlineExceeded);
    CHECK(services.controlCalls.empty());
    CHECK(call.resultUntouched());
}

TEST_CASE("a control import the guest calls wrongly traps", "[vtextension][control]")
{
    auto services = FakeHostServices {};
    auto call = GuestCall { splitRequest() };
    auto const write = ControlGrants::of({ Permission::Write });

    SECTION("a string outside its memory")
    {
        call.arguments[0] = FakeMemory::Size - 2; // the pane's pointer; its length is 6
        CHECK(errorOf(call.call("pane-split", services, write)) == ExtensionError::Trapped);
    }
    SECTION("too few arguments")
    {
        call.arguments.pop_back();
        CHECK(errorOf(call.call("pane-split", services, write)) == ExtensionError::Trapped);
    }
    SECTION("a result address outside its memory")
    {
        services.controlAnswer = R"({"tab":"tab:t","pane":"pane:n"})";
        call.arguments.back() = FakeMemory::Size - 4;
        CHECK(errorOf(call.call("pane-split", services, write)) == ExtensionError::Trapped);
    }
    SECTION("no such import")
    {
        auto memory = FakeMemory {};
        auto deadline = std::atomic<DeadlineState> { DeadlineState::Running };
        CHECK(errorOf(callControlImport(ControlImportCount,
                                        memory,
                                        call.arguments,
                                        services,
                                        write,
                                        ControlCallBudget { .remaining = 40ms, .deadline = deadline }))
              == ExtensionError::Trapped);
    }
    CHECK(services.controlCalls.empty());
}

TEST_CASE("a module importing the control interface instantiates", "[vtextension][control]")
{
    auto const backend = wasm::makeDefaultBackend();
    auto const engine = Engine::create(*backend, ExtensionLimits {});
    REQUIRE(engine.has_value());
    auto const compiled =
        CompiledExtension::load(**engine, toModule(vtextension::testing::ControlImportingModule));
    REQUIRE(compiled.has_value());
    auto services = FakeHostServices {};
    auto clock = core::platform::ManualClock {};
    auto const instance = Instance::create(**engine, **compiled, services, clock, ExtensionLimits {});
    REQUIRE(instance.has_value());
    CHECK((*instance)->render(surface(80, 1)).has_value());
    CHECK(services.controlCalls.empty());
}

TEST_CASE("a guest calls a control verb through the runtime", "[vtextension][control]")
{
    // Thirteen i32s through the real runtime's trampoline: the widest import there is.
    auto guest = vtextension::testing::RunningGuest { vtextension::testing::withManifest(
        toModule(vtextension::testing::ControlCallingModule),
        "name = splitter\nversion = 1\ncontrol write\n") };
    guest.services.controlAnswer = R"({"tab":"tab:t","pane":"pane:n"})";
    REQUIRE(guest.instance().render(surface(80, 1)).has_value());
    REQUIRE(guest.services.controlCalls.size() == 1);
    CHECK(guest.services.controlCalls.front().method == "pane.split");
    CHECK(guest.services.controlCalls.front().params
          == R"({"pane":"","direction":"right","cwd":"","command":[],"label":"","focus":"take"})");
}

TEST_CASE("a guest's control call is refused without the manifest line", "[vtextension][control]")
{
    auto guest = vtextension::testing::RunningGuest { toModule(vtextension::testing::ControlCallingModule) };
    guest.services.controlAnswer = R"({"tab":"tab:t","pane":"pane:n"})";
    REQUIRE(guest.instance().render(surface(80, 1)).has_value());
    CHECK(guest.services.controlCalls.empty());
    CHECK(guest.services.consentQuestions == 0);
}

TEST_CASE("an instance's deadline expiring during a control call faults it, writing nothing",
          "[vtextension][control][threading]")
{
    // The watchdog thread fires the guest's deadline while the host is still answering on the guest's thread:
    // the call fails as overrun and the answer, which arrives afterwards, never reaches the guest's memory.
    auto backend = vtextension::testing::FakeWasmBackend {};
    constexpr auto ResultAddress = uint32_t { 1024 };
    // Its index in the table every runtime is created with: after the statusline world's functions.
    auto const table = vtextension::detail::hostFunctions();
    auto const daemonInfo =
        static_cast<std::size_t>(std::ranges::find_if(table,
                                                      [](wasm::HostFunction const& function) {
                                                          return function.module == ControlImportModule
                                                                 && function.name == "daemon-info";
                                                      })
                                 - table.begin());
    backend.guest.exports["render"] =
        [&](std::span<uint32_t> argv) -> std::expected<void, wasm::CallFailure> {
        auto const arguments = std::array { ResultAddress };
        if (!backend.lastInstance->callHost(daemonInfo, arguments))
            return std::unexpected(wasm::CallFailure::HostFailure);
        argv.front() = 16;
        return {};
    };

    auto limits = ExtensionLimits {};
    limits.callDeadline = 20ms;
    auto const engine = Engine::create(backend, limits);
    REQUIRE(engine.has_value());
    auto const compiled = CompiledExtension::load(
        **engine,
        vtextension::testing::withManifest(toModule(vtextension::testing::MinimalModule),
                                           "name = reader\nversion = 1\ncontrol read\n"));
    REQUIRE(compiled.has_value());
    auto services = FakeHostServices {};
    services.controlAnswer =
        R"({"version":"1","protocolVersion":4,"platform":"linux","hostname":"h","uptimeSeconds":1})";
    // The host takes until the watchdog has interrupted the guest (bounded, so that a watchdog that never
    // fires fails the checks below rather than hanging the test).
    services.beforeControlAnswer = [&] {
        auto const giveUp = std::chrono::steady_clock::now() + std::chrono::seconds { 5 };
        while (backend.lastInstance->interrupts.load() == 0 && std::chrono::steady_clock::now() < giveUp)
            std::this_thread::sleep_for(1ms);
    };
    auto clock = core::platform::ManualClock {};
    auto const instance = Instance::create(**engine, **compiled, services, clock, limits);
    REQUIRE(instance.has_value());

    CHECK(errorOf((*instance)->render(surface(80, 1))) == ExtensionError::DeadlineExceeded);
    REQUIRE(services.controlCalls.size() == 1);
    CHECK(services.controlCalls.front().method == "daemon.info");
    CHECK(services.controlCalls.front().timeout <= 20ms);
    auto const& memory = backend.lastInstance->linearMemory;
    CHECK(std::ranges::all_of(std::span { memory }.subspan(ResultAddress, 64),
                              [](std::byte byte) { return byte == std::byte { 0 }; }));
}

TEST_CASE("a guest reads what a control verb answers it", "[vtextension][control]")
{
    auto guest = vtextension::testing::RunningGuest { vtextension::testing::withManifest(
        toModule(vtextension::testing::ControlReportingModule),
        "name = reporter\nversion = 1\ncontrol read\n") };
    guest.services.controlAnswer =
        R"({"version":"9.8.7","protocolVersion":4,"platform":"linux","hostname":"h","uptimeSeconds":1})";
    CHECK(guest.loadAndRender({}) == "9.8.7");

    guest.services.consent = GrantState::Pending;
    auto const refused = guest.instance().render(surface(80, 1));
    REQUIRE(refused.has_value());
    CHECK(*refused == "method 'daemon.info' needs grant 'read': not granted yet");
}
