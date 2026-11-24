// SPDX-License-Identifier: Apache-2.0
//
// The daemon world end to end: the agent orchestrator's real module on the real DaemonWorldExtensions, its
// declared commands answered from the orchestrator's recorded fixtures, granted by a NativeClient that proved
// the daemon's consent key -- and still running, summary and all, once no window is left.
#include <vtbackend/screen/Settings.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Environment.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/testing/CoroTestSupport.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <vtcontrol/testing/FakeControlApi.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/control/ControlService.hpp>

#ifdef VTHOST_TEST_ORCHESTRATOR_WASM

    #include <vthost/testing/OrchestratorWorld.hpp>

using core::async::Task;
using vthost::testing::ClientKind;
using vthost::testing::Connection;
using vthost::testing::OrchestratorWorld;
using namespace std::string_view_literals;

namespace
{

/// The longest the test waits, in 1 ms loop ticks, for what it counts on.
constexpr auto WaitTicks = 60'000;

/// The indicator status line every hosted session shows: the orchestrator's summary alone.
constexpr auto IndicatorTemplate = "[{DaemonExtension:orchestrator}]"sv;

/// @return Settings whose indicator status line shows the orchestrator's summary.
[[nodiscard]] vtbackend::Settings showingSummary()
{
    auto settings = vtbackend::Settings {};
    settings.statusDisplayType = vtbackend::StatusDisplayType::Indicator;
    settings.indicatorStatusLine.left = std::string(IndicatorTemplate);
    settings.indicatorStatusLine.middle.clear();
    settings.indicatorStatusLine.right.clear();
    return settings;
}

/// @return The indicator row @p screen mirrors, as UTF-8, trailing blanks trimmed; none if it mirrors none.
[[nodiscard]] std::optional<std::string> indicatorRowOf(vthost::client::RemoteScreen const& screen)
{
    if (screen.statusLinesDisplay != std::to_underlying(vtbackend::StatusDisplayType::Indicator)
        || screen.statusLines.empty())
        return std::nullopt;
    auto text = std::string {};
    for (auto const& cell: screen.statusLines.front().cells)
    {
        if (cell.codepoint == 0)
            text += ' ';
        else
            vthost::client::appendCluster(text, cell);
    }
    while (text.ends_with(' '))
        text.pop_back();
    return text;
}

/// A daemon host whose daemon world is the orchestrator on the real DaemonWorldExtensions
/// (OrchestratorWorld), and a control service on it.
struct DaemonWorldHarness
{
    DaemonWorldHarness():
        host { std::make_unique<vthost::SessionHost>(
            loop,
            [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                return std::make_unique<vtpty::MockPty>(size);
            },
            showingSummary(),
            core::defaultEnvironment(),
            vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                        .daemonWorld = services.factory() }) }
    {
        std::ignore = host->createTab();
        control = std::make_unique<vthost::control::ControlService>(
            *host,
            vthost::control::ControlServiceConfig { .version = "9.8.7",
                                                    .socketPath = "/run/test.sock",
                                                    .clock = services.wallClock,
                                                    .extensions = host->daemonExtensionControl() });
    }

    DaemonWorldHarness(DaemonWorldHarness const&) = delete;
    DaemonWorldHarness(DaemonWorldHarness&&) = delete;
    DaemonWorldHarness& operator=(DaemonWorldHarness const&) = delete;
    DaemonWorldHarness& operator=(DaemonWorldHarness&&) = delete;

    ~DaemonWorldHarness()
    {
        control.reset();
        host.reset();
    }

    /// @return A new connection to the host, of a client of @p kind.
    [[nodiscard]] std::unique_ptr<Connection> connect(ClientKind kind)
    {
        return services.connect(loop, *host, kind);
    }

    /// @return What `extension.status orchestrator` answers, through the control service.
    [[nodiscard]] vtcontrol::ExtensionStatusReply status()
    {
        auto reply =
            std::optional<std::expected<vtcontrol::ExtensionStatusReply, vtcontrol::ControlError>> {};
        control->handle(context,
                        vtcontrol::ExtensionStatusRequest { .name = "orchestrator" },
                        [&](std::expected<vtcontrol::ExtensionStatusReply, vtcontrol::ControlError> answer) {
                            reply = std::move(answer);
                        });
        REQUIRE(reply.has_value());
        REQUIRE(reply->has_value());
        return std::move(**reply);
    }

    // What the daemon world borrows: declared before the host, which stops it, so that they outlive it.
    OrchestratorWorld services { VTHOST_TEST_ORCHESTRATOR_WASM, VTHOST_TEST_ORCHESTRATOR_TESTDATA };

    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source };
    std::unique_ptr<vthost::SessionHost> host;
    std::unique_ptr<vthost::control::ControlService> control;
    vtcontrol::testing::FakeCallContext context;
};

/// The window's part: answers the question it is asked with Granted, waits until the daemon world runs the
/// extension, and closes its connection -- leaving no window.
Task<void> grantAndLeave(core::net::EventLoop* loop, DaemonWorldHarness* h, Connection* window)
{
    std::ignore = co_await core::net::testing::waitUntil(
        loop, [window] { return !window->log.requests.empty(); }, WaitTicks);
    if (window->log.requests.empty())
    {
        window->client->detach();
        co_return;
    }
    auto const& request = window->log.requests.front();
    window->client->answerDaemonExtension(request.name, request.grantDigest, vthost::GrantState::Granted);
    std::ignore = co_await core::net::testing::waitUntil(
        loop,
        [h] {
            auto const status = h->services.world()->status("orchestrator");
            return status && status->state == vthost::DaemonExtensionState::Running;
        },
        WaitTicks);
    window->client->detach();
}

/// @return Whether @p later mirrors the indicator row holding the summary the extension reports now.
[[nodiscard]] bool mirrorsSummary(DaemonWorldHarness& h, Connection const& later)
{
    auto const row = std::format("[{}]", h.status().summary);
    return std::ranges::any_of(later.client->screens(),
                               [&row](auto const& entry) { return indicatorRowOf(entry.second) == row; });
}

/// The later client's part: waits until it mirrors the row holding the current summary, and leaves. The
/// current one, not one read earlier: the guest goes on publishing after its first poll (`⬢ agents 0/4`
/// becomes `⬢ agents 0/4 · ✗2`), and with a fast interpreter does so before this client attaches.
Task<void> mirrorAndLeave(core::net::EventLoop* loop, DaemonWorldHarness* h, Connection* later)
{
    std::ignore = co_await core::net::testing::waitUntil(
        loop, [h, later] { return mirrorsSummary(*h, *later); }, WaitTicks);
    later->client->detach();
}

} // namespace

TEST_CASE("DaemonWorld: a window grants, every window closes, the summary stays", "[daemon][e2e]")
{
    auto h = DaemonWorldHarness {};
    REQUIRE(h.services.world() != nullptr);
    // Started at the daemon's start, it waits for its user: no command has run.
    auto const started = h.services.world()->status("orchestrator");
    REQUIRE(started.has_value());
    REQUIRE(started->state == vthost::DaemonExtensionState::Pending);
    CHECK(h.status().state == "pending");
    CHECK(h.services.processRunner.waiting() == 0);

    // A window that proved the daemon's consent key is asked, and grants; then it closes.
    auto window = h.connect(ClientKind::Window);
    h.loop.blockOn(core::net::testing::allOf(
        window->server->run(), window->client->run(), grantAndLeave(&h.loop, &h, window.get())));
    REQUIRE(window->log.requests.size() == 1);
    CHECK(window->log.requests.front().name == "orchestrator");
    window.reset();

    // No window is left. Within one poll -- every command it runs started once and answered -- it reports
    // running, with the run list it published. Waited for one 1 ms delay at a time: a turn of a loop that
    // nothing is parked on returns at once, so counting turns would not wait for the guest's lane at all.
    auto const polled = h.loop.blockOn(core::net::testing::waitUntil(
        &h.loop,
        [&h] {
            std::ignore = h.services.processRunner.answerWaiting();
            return h.services.pollDone() && h.status().summary.starts_with("⬢ agents");
        },
        WaitTicks));
    REQUIRE(polled);
    for (auto const& answer: vtextension::testing::OrchestratorFixtureAnswers)
    {
        CAPTURE(answer.program, answer.verb);
        CHECK(h.services.processRunner.startsOf(answer.program, answer.verb) == 1);
    }
    auto const status = h.status();
    CHECK(status.state == "running");
    CHECK(status.problem.empty());
    REQUIRE(status.summary.starts_with("⬢ agents"));
    // The detail is the short text and the run list, one line each.
    CHECK(status.detail.starts_with(status.summary));

    // A client attaching afterwards -- one that answers nothing -- mirrors the row holding the summary.
    auto later = h.connect(ClientKind::Cli);
    h.loop.blockOn(core::net::testing::allOf(
        later->server->run(), later->client->run(), mirrorAndLeave(&h.loop, &h, later.get())));
    CHECK(mirrorsSummary(h, *later));
    CHECK(later->log.requests.empty());
}

#endif
