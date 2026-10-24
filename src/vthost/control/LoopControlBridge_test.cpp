// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>

#include <vtcontrol/Api.hpp>
#include <vtcontrol/Catalog.hpp>
#include <vtcontrol/Json.hpp>
#include <vtcontrol/testing/FakeControlApi.hpp>
#include <vthost/control/LoopControlBridge.hpp>
#include <vthost/testing/ControlHarness.hpp>
#include <vthost/testing/FakeExtensionHosting.hpp>

using namespace std::chrono_literals;
using vtcontrol::ControlErrorCode;
using vtcontrol::ControlGrants;
using vthost::control::ExtensionCaller;
using vthost::control::LoopControlBridge;
using vthost::testing::ControlHarness;

namespace
{

using Reply = std::expected<std::string, vtcontrol::ControlError>;

/// Makes a call through @p bridge from a thread of its own -- an extension's worker -- while this thread runs
/// @p loop, as the daemon's loop thread does, until the call returned.
[[nodiscard]] Reply callFromWorker(LoopControlBridge const& bridge,
                                   core::net::EventLoop& loop,
                                   std::string_view method,
                                   std::string_view params,
                                   ControlGrants granted = ControlGrants::all(),
                                   std::chrono::milliseconds timeout = 10s)
{
    auto reply = std::optional<Reply> {};
    auto done = std::atomic<bool> { false };
    auto worker = std::thread { [&] {
        reply = bridge.call(method, params, granted, timeout);
        done.store(true, std::memory_order_release);
    } };
    while (!done.load(std::memory_order_acquire))
        std::ignore = loop.runOnce(1ms);
    worker.join();
    return std::move(*reply);
}

/// A bridge whose every call is served by @p api, as @p pane.
[[nodiscard]] LoopControlBridge bridgeTo(core::net::EventLoop& loop,
                                         vtcontrol::ControlApi& api,
                                         std::string pane)
{
    return LoopControlBridge { loop, [&api, pane = std::move(pane)] {
                                  return std::optional { ExtensionCaller { .api = api, .pane = pane } };
                              } };
}

} // namespace

TEST_CASE("LoopControlBridge serves a call from another thread", "[vthost][control][extension][threading]")
{
    auto h = ControlHarness {};
    std::ignore = h.openTab();
    auto const bridge = bridgeTo(h.loop, h.service, "");

    auto const reply = callFromWorker(bridge, h.loop, "workspace.list", "");

    REQUIRE(reply.has_value());
    auto const list = vtcontrol::decodeJson<vtcontrol::ListReply>(*reply);
    REQUIRE(list.has_value());
    REQUIRE(list->windows.size() == 1);
    CHECK(list->windows.front().tabs.size() == 1);
}

TEST_CASE("LoopControlBridge calls as the extension's pane", "[vthost][control][extension][threading]")
{
    // A target left empty means the extension's pane, not the active one.
    auto h = ControlHarness {};
    auto const first = h.openTab();
    std::ignore = h.openTab();
    auto const bridge = bridgeTo(h.loop, h.service, h.paneId(first));

    auto const reply = callFromWorker(bridge, h.loop, "pane.capture", R"({"pane":""})");

    REQUIRE(reply.has_value());
    auto const capture = vtcontrol::decodeJson<vtcontrol::CaptureReply>(*reply);
    REQUIRE(capture.has_value());
    CHECK(capture->pane == h.paneId(first));
}

TEST_CASE("LoopControlBridge times out when the loop is stalled", "[vthost][control][extension][threading]")
{
    // Nothing runs the loop while the call waits. The call is not cancelled: once the loop runs, it is
    // served, and its reply goes into the slot nobody reads any more.
    auto api = vtcontrol::testing::FakeControlApi {};
    api.daemonInfoReply.hostname = "late";
    auto loop = ControlHarness {};
    auto const bridge = bridgeTo(loop.loop, api, "pane:x");

    auto const reply = bridge.call("daemon.info", "", ControlGrants::all(), 30ms);

    REQUIRE_FALSE(reply.has_value());
    CHECK(reply.error().code == ControlErrorCode::Failed);
    CHECK(reply.error().message == "timed out");
    CHECK_FALSE(api.lastDaemonInfo.has_value());

    loop.drain();
    CHECK(api.lastDaemonInfo.has_value());
}

TEST_CASE("LoopControlBridge refuses socket-only verbs", "[vthost][control][extension][threading]")
{
    auto h = ControlHarness {};
    std::ignore = h.openTab();
    auto const bridge = bridgeTo(h.loop, h.service, "");

    auto const reply = callFromWorker(bridge, h.loop, "block.wait", "");

    REQUIRE_FALSE(reply.has_value());
    CHECK(reply.error().code == ControlErrorCode::Unsupported);
}

TEST_CASE("LoopControlBridge passes the extension's grants to dispatch",
          "[vthost][control][extension][threading]")
{
    auto api = vtcontrol::testing::FakeControlApi {};
    auto h = ControlHarness {};
    auto const bridge = bridgeTo(h.loop, api, "pane:x");

    auto const reply = callFromWorker(
        bridge, h.loop, "pane.split", R"({"pane":""})", ControlGrants::of({ vtcontrol::Permission::Read }));

    REQUIRE_FALSE(reply.has_value());
    CHECK(reply.error().code == ControlErrorCode::Forbidden);
    CHECK_FALSE(api.lastSplit.has_value());
}

TEST_CASE("LoopControlBridge applies a request's grant rule as dispatch does",
          "[vthost][control][extension][threading]")
{
    // The extension's grants reach dispatch(), which applies the catalog's requestGrant rule to them: a
    // guest granted `write` but not `input` cannot run a command through tab.create, even past the import.
    auto h = ControlHarness {};
    std::ignore = h.openTab();
    auto const bridge = bridgeTo(h.loop, h.service, "");
    auto const write = ControlGrants::of({ vtcontrol::Permission::Read, vtcontrol::Permission::Write });
    auto const tabCount = [&] {
        auto const reply = callFromWorker(bridge, h.loop, "workspace.list", "");
        REQUIRE(reply.has_value());
        auto const list = vtcontrol::decodeJson<vtcontrol::ListReply>(*reply);
        REQUIRE(list.has_value());
        REQUIRE(list->windows.size() == 1);
        return list->windows.front().tabs.size();
    };

    auto const refused = callFromWorker(bridge, h.loop, "tab.create", R"({"command":["make"]})", write);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ControlErrorCode::Forbidden);
    CHECK(refused.error().message == "method 'tab.create': running a command needs grant 'input'");
    CHECK(tabCount() == 1);

    CHECK(callFromWorker(bridge, h.loop, "tab.create", R"({})", write).has_value());
    CHECK(tabCount() == 2);
}

TEST_CASE("LoopControlBridge leaves pane.send's wait to the import",
          "[vthost][control][extension][threading]")
{
    // An extension may not ask `pane.send` to wait, but that is the import's rule (vtextension's
    // callControlImport), not dispatch's: below the import, the request goes through as it is.
    auto api = vtcontrol::testing::FakeControlApi {};
    auto h = ControlHarness {};
    auto const bridge = bridgeTo(h.loop, api, "pane:x");

    auto const reply = callFromWorker(
        bridge, h.loop, "pane.send", R"({"pane":"","text":"make\n","wait":"commandFinished"})");

    CHECK(reply.has_value());
    REQUIRE(api.lastSend.has_value());
    CHECK(api.lastSend->wait == vtcontrol::SendWait::CommandFinished);
}

TEST_CASE("LoopControlBridge answers unsupported once the pane is gone",
          "[vthost][control][extension][threading]")
{
    auto h = ControlHarness {};
    auto const bridge = LoopControlBridge { h.loop, [] { return std::optional<ExtensionCaller> {}; } };

    auto const reply = callFromWorker(bridge, h.loop, "daemon.info", "");

    REQUIRE_FALSE(reply.has_value());
    CHECK(reply.error().code == ControlErrorCode::Unsupported);
}

TEST_CASE("an extension's control call reaches the attached service as its pane",
          "[vthost][control][extension][threading]")
{
    // The host hands each session's extensions a callControl hook: served on the loop by the control API
    // attached at the time, as the session's pane; with none attached, it is unsupported.
    auto source = core::net::testing::ScriptedBackend {};
    auto loop = core::net::EventLoop { source };
    auto wall = core::platform::ManualWallClock {};
    auto const extensions = std::make_shared<vthost::testing::FakeExtensionHosting>();
    auto host = vthost::SessionHost {
        loop,
        [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const& /*spawn*/) {
            return std::make_unique<vtpty::MockPty>(size);
        },
        vtbackend::Settings {},
        core::defaultEnvironment(),
        vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                    .extensions = extensions,
                                    .uuids = std::make_shared<vtcontrol::testing::SequentialUuidSource>() }
    };
    auto* tab = host.createTab();
    REQUIRE(tab != nullptr);
    std::ignore = host.createTab();
    REQUIRE(extensions->sessions.size() == 2);
    auto const& firstExtensions = *extensions->sessions.front();
    auto const firstPane = vtcontrol::toString(vtcontrol::ObjectRef {
        .kind = vtcontrol::ObjectKind::Pane, .uuid = *host.objects().uuidOf(tab->rootPane()->session()) });

    auto const callCapture = [&] {
        auto reply = std::optional<Reply> {};
        auto done = std::atomic<bool> { false };
        auto worker = std::thread { [&] {
            reply = firstExtensions.callControl("pane.capture", R"({"pane":""})", ControlGrants::all(), 10s);
            done.store(true, std::memory_order_release);
        } };
        while (!done.load(std::memory_order_acquire))
            std::ignore = loop.runOnce(1ms);
        worker.join();
        return std::move(*reply);
    };

    SECTION("served by the attached service")
    {
        auto const service = vthost::control::ControlService {
            host, vthost::control::ControlServiceConfig { .version = "1", .socketPath = "", .clock = wall }
        };
        auto const reply = callCapture();
        REQUIRE(reply.has_value());
        auto const capture = vtcontrol::decodeJson<vtcontrol::CaptureReply>(*reply);
        REQUIRE(capture.has_value());
        CHECK(capture->pane == firstPane);
    }
    SECTION("unsupported with none attached")
    {
        {
            auto const gone = vthost::control::ControlService {
                host,
                vthost::control::ControlServiceConfig { .version = "1", .socketPath = "", .clock = wall }
            };
        }
        auto const reply = callCapture();
        REQUIRE_FALSE(reply.has_value());
        CHECK(reply.error().code == ControlErrorCode::Unsupported);
    }
}
