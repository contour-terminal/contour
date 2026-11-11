// SPDX-License-Identifier: Apache-2.0
//
// `contour agent report`: when it calls the daemon, with what, and that it never fails.

#include <contour/cli/AgentReport.hpp>

#include <core/platform/Clock.hpp>
#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

#ifndef _WIN32
    #include <unistd.h>
#endif

#include <vtcontrol/Json.hpp>

using namespace std::string_literals;
using namespace std::string_view_literals;
using contour::cli::ControlChannel;
using vthost::client::ControlClientError;
using vthost::client::ControlFailure;

namespace
{

struct Call
{
    std::string method;
    std::string params;
    std::string context;
    std::optional<std::chrono::milliseconds> timeout;
};

class FakeChannel final: public ControlChannel
{
  public:
    explicit FakeChannel(std::vector<Call>& calls, bool fails = false): _calls { calls }, _fails { fails } {}

    [[nodiscard]] std::expected<std::string, ControlClientError> call(
        std::string_view method,
        std::string_view params,
        std::string_view context,
        std::optional<std::chrono::milliseconds> timeout) override
    {
        _calls.emplace_back(
            Call { std::string { method }, std::string { params }, std::string { context }, timeout });
        if (_fails)
            return std::unexpected(ControlClientError {
                .failure = ControlFailure::Unreachable, .message = "no such pane", .remote = std::nullopt });
        return "{}"s;
    }

    [[nodiscard]] std::expected<void, ControlClientError> stream(
        std::string_view,
        std::string_view,
        std::function<vthost::client::StreamStep(std::string_view)>,
        core::async::StopToken const&) override
    {
        return {};
    }

  private:
    std::vector<Call>& _calls;
    bool _fails;
};

/// A payload from a string, as standard input would deliver it; it can also take time on the clock.
class TextPayload final: public contour::cli::PayloadReader
{
  public:
    TextPayload(std::optional<std::string> text,
                core::platform::ManualClock& clock,
                std::chrono::milliseconds takes):
        _text { std::move(text) }, _clock { clock }, _takes { takes }
    {
    }

    [[nodiscard]] std::optional<std::string> read(std::size_t maxBytes,
                                                  core::platform::SteadyTimePoint deadline) override
    {
        askedMax = maxBytes;
        askedDeadline = deadline;
        _clock.advance(_takes);
        if (_text && _text->size() > maxBytes)
            return std::nullopt;
        return _text;
    }

    std::size_t askedMax = 0;
    core::platform::SteadyTimePoint askedDeadline {};

  private:
    std::optional<std::string> _text;
    core::platform::ManualClock& _clock;
    std::chrono::milliseconds _takes;
};

constexpr auto StopPayload = R"({"hook_event_name":"Stop","last_assistant_message":"done it"})"sv;

core::testing::FakeEnvironment inPane()
{
    return core::testing::FakeEnvironment { { { "CONTOUR_DAEMON", "/run/c.sock" },
                                              { "CONTOUR_PANE", "pane:abc" } } };
}

struct Harness
{
    std::vector<Call> calls;
    int opened = 0;
    bool fails = false;

    core::platform::ManualClock clock { core::platform::SteadyTimePoint {} };
    std::chrono::milliseconds readTakes { 0 };
    std::size_t askedMax = 0;
    core::platform::SteadyTimePoint askedDeadline {};

    int run(std::optional<std::string> payload,
            core::Environment const& env,
            std::string_view agent = "claude-code")
    {
        auto const factory = contour::cli::ChannelFactory { [this] {
            ++opened;
            return std::make_unique<FakeChannel>(calls, fails);
        } };
        auto reader = TextPayload { std::move(payload), clock, readTakes };
        auto const status =
            contour::cli::runAgentReport(*contour::cli::findIntegration(agent), reader, env, factory, clock);
        askedMax = reader.askedMax;
        askedDeadline = reader.askedDeadline;
        return status;
    }
};

} // namespace

TEST_CASE("agentReport.noDaemonEnvIsSilentSuccess", "[contour][cli][agent]")
{
    auto harness = Harness {};
    auto const noDaemon = core::testing::FakeEnvironment { { { "CONTOUR_PANE", "pane:abc" } } };
    CHECK(harness.run(std::string { StopPayload }, noDaemon) == 0);
    CHECK(harness.opened == 0);
    CHECK(harness.calls.empty());
    auto const noPane = core::testing::FakeEnvironment { { { "CONTOUR_DAEMON", "/x" } } };
    CHECK(harness.run(std::string { StopPayload }, noPane) == 0);
    CHECK(harness.opened == 0);
}

TEST_CASE("agentReport.callerErrorIsSwallowed", "[contour][cli][agent]")
{
    auto harness = Harness {};
    harness.fails = true;
    CHECK(harness.run(std::string { StopPayload }, inPane()) == 0);
    CHECK(harness.calls.size() == 1);
}

TEST_CASE("agentReport.sendsReportForCallerPane", "[contour][cli][agent]")
{
    auto harness = Harness {};
    CHECK(harness.run(std::string { StopPayload }, inPane()) == 0);
    REQUIRE(harness.calls.size() == 1);
    auto const& call = harness.calls.front();
    CHECK(call.method == "pane.reportStatus");
    CHECK(call.context == "pane:abc");
    auto const request = vtcontrol::decodeJson<vtcontrol::ReportStatusRequest>(call.params);
    REQUIRE(request.has_value());
    CHECK(request->pane == "pane:abc");
    CHECK(request->integration == "claude-code");
    CHECK(request->state == "done");
    CHECK(request->message == "done it");
}

TEST_CASE("agentReport.sendsClearOnSessionEnd", "[contour][cli][agent]")
{
    auto harness = Harness {};
    CHECK(harness.run(R"({"hook_event_name":"SessionEnd"})", inPane()) == 0);
    REQUIRE(harness.calls.size() == 1);
    CHECK(harness.calls.front().method == "pane.clearStatus");
    auto const request = vtcontrol::decodeJson<vtcontrol::ClearStatusRequest>(harness.calls.front().params);
    REQUIRE(request.has_value());
    CHECK(request->integration == "claude-code");
}

TEST_CASE("agentReport.timeoutIs2s", "[contour][cli][agent]")
{
    auto harness = Harness {};
    CHECK(harness.run(std::string { StopPayload }, inPane()) == 0);
    REQUIRE(harness.calls.size() == 1);
    REQUIRE(harness.calls.front().timeout.has_value());
    CHECK(*harness.calls.front().timeout <= std::chrono::milliseconds { 2000 });
}

TEST_CASE("agentReport.readingCountsAgainstTheBudget", "[contour][cli][agent]")
{
    auto harness = Harness {};
    auto const start = harness.clock.now();
    harness.readTakes = std::chrono::milliseconds { 1500 };
    CHECK(harness.run(std::string { StopPayload }, inPane()) == 0);
    // The reader is told the cap and the whole report's deadline; the call gets what reading left.
    CHECK(harness.askedMax == contour::cli::MaxAgentPayloadBytes);
    CHECK(harness.askedDeadline == start + contour::cli::AgentReportTimeout);
    REQUIRE(harness.calls.size() == 1);
    CHECK(harness.calls.front().timeout == std::chrono::milliseconds { 500 });

    // A read that used it all up sends nothing.
    harness.readTakes = contour::cli::AgentReportTimeout;
    CHECK(harness.run(std::string { StopPayload }, inPane()) == 0);
    CHECK(harness.calls.size() == 1);
}

TEST_CASE("agentReport.anUnreadablePayloadReportsNothing", "[contour][cli][agent]")
{
    auto harness = Harness {};
    CHECK(harness.run(std::nullopt, inPane()) == 0); // too large, or cut off by the deadline
    CHECK(harness.opened == 0);
    // Outside a pane it is not even read.
    CHECK(harness.run(std::string { StopPayload }, core::testing::FakeEnvironment {}) == 0);
    CHECK(harness.askedMax == 0);
}

#ifndef _WIN32
namespace
{
/// Writes all of @p text to @p fd.
bool writeAll(int fd, std::string_view text)
{
    return ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
}
} // namespace

TEST_CASE("agentReport.stdinReaderNeverWaitsPastItsDeadline", "[contour][cli][agent]")
{
    auto ends = std::array<int, 2> {};
    REQUIRE(::pipe(ends.data()) == 0);
    auto const clock = core::platform::SteadyClock {};
    auto const reader = contour::cli::makeHandlePayloadReader(ends[0], clock);

    // An agent that wrote half its payload and hangs: given up on at the deadline.
    REQUIRE(writeAll(ends[1], "{\"hook_"));
    auto const started = clock.now();
    CHECK_FALSE(reader->read(contour::cli::MaxAgentPayloadBytes, started + std::chrono::milliseconds { 50 })
                    .has_value());
    CHECK(clock.now() - started < std::chrono::seconds { 1 });

    // More than the cap: not taken.
    REQUIRE(writeAll(ends[1], "event_name\":\"Stop\"}"));
    CHECK_FALSE(reader->read(8, clock.now() + std::chrono::seconds { 1 }).has_value());

    // The whole payload, then the end of the input.
    auto const second = contour::cli::makeHandlePayloadReader(ends[0], clock);
    REQUIRE(writeAll(ends[1], "{}"));
    ::close(ends[1]);
    CHECK(second->read(contour::cli::MaxAgentPayloadBytes, clock.now() + std::chrono::seconds { 1 })
          == std::optional<std::string> { "{}" });
    ::close(ends[0]);
}
#endif

TEST_CASE("agentReport.malformedPayloadIsIgnored", "[contour][cli][agent]")
{
    auto harness = Harness {};
    CHECK(harness.run("{ not json", inPane()) == 0);
    CHECK(harness.run(R"({"hook_event_name":"SomethingNew"})", inPane()) == 0);
    CHECK(harness.run("", inPane()) == 0);
    CHECK(harness.opened == 0);
}

TEST_CASE("agentReport.oddlyFormattedPayloadStillReports", "[contour][cli][agent]")
{
    // Valid JSON an agent may well send: pretty-printed, the event name last, a unicode escape in the
    // message, and a number past 2^53 that only a double can hold.
    auto harness = Harness {};
    CHECK(harness.run("{\n  \"last_assistant_message\" : \"d\\u00f6ne\",\n"
                      "  \"tokens\": 18446744073709551616,\n"
                      "  \"hook_event_name\":\n    \"Stop\"\n}\n",
                      inPane())
          == 0);
    REQUIRE(harness.calls.size() == 1);
    auto const request = vtcontrol::decodeJson<vtcontrol::ReportStatusRequest>(harness.calls.front().params);
    REQUIRE(request.has_value());
    CHECK(request->state == "done");
    CHECK(request->message == "d\xC3\xB6ne");
}
