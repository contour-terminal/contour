// SPDX-License-Identifier: Apache-2.0
//
// The control CLI verbs: their flags, requests, human output and exit statuses, over a channel that answers
// from a script instead of a daemon.

#include <contour/cli/ContourApp.hpp>
#include <contour/cli/ControlVerbs.hpp>
#include <contour/config/HostUrl.hpp>

#include <core/Environment.hpp>
#include <core/async/StopToken.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/Environment.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <expected>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <vtcontrol/Catalog.hpp>
#include <vtcontrol/Json.hpp>
#include <vtcontrol/Wit.hpp>
#include <vthost/StdioChild.hpp>

using namespace std::string_literals;
using contour::cli::ControlIo;
using contour::cli::ExitStatus;
using vthost::client::ControlClientError;
using vthost::client::ControlFailure;

namespace
{

/// A channel that records the call it was given and answers a scripted result.
class ScriptedChannel final: public contour::cli::ControlChannel
{
  public:
    explicit ScriptedChannel(std::expected<std::string, ControlClientError> answer):
        _answer { std::move(answer) }
    {
    }

    [[nodiscard]] std::expected<std::string, ControlClientError> call(
        std::string_view method,
        std::string_view params,
        std::string_view context,
        std::optional<std::chrono::milliseconds> /*timeout*/) override
    {
        calls.emplace_back(Call { std::string { method }, std::string { params }, std::string { context } });
        return _answer;
    }

    struct Call
    {
        std::string method;
        std::string params;
        std::string context;
    };

    [[nodiscard]] std::expected<void, ControlClientError> stream(
        std::string_view params,
        std::string_view context,
        std::function<vthost::client::StreamStep(std::string_view)> onEvent,
        core::async::StopToken const& /*stop*/) override
    {
        calls.emplace_back(Call { "events.subscribe", std::string { params }, std::string { context } });
        for (auto const& event: events)
        {
            ++delivered;
            if (onEvent(event) == vthost::client::StreamStep::Stop)
                break;
        }
        if (streamFailure)
            return std::unexpected(*streamFailure);
        return {};
    }

    std::vector<Call> calls;
    std::vector<std::string> events;                 ///< What a stream delivers, in order.
    std::optional<ControlClientError> streamFailure; ///< How a stream ends once its events are out.
    std::size_t delivered = 0;                       ///< How many events were handed to the verb.

  private:
    std::expected<std::string, ControlClientError> _answer;
};

/// @return A failed call the daemon answered with @p code.
ControlClientError remoteError(vtcontrol::ControlErrorCode code, std::string const& message)
{
    return ControlClientError { .failure = ControlFailure::Remote,
                                .message = message,
                                .remote = vtcontrol::ControlError {
                                    .code = code, .message = message, .candidates = {} } };
}

/// Everything a verb run touches, with its output captured.
struct Run
{
    core::testing::FakeEnvironment env { { { "CONTOUR_PANE",
                                             "pane:018f3c4e-9a1b-7c2d-8e3f-0123456789ad" } } };
    core::platform::testing::InMemoryFileSystem fs {};
    std::istringstream in {};
    std::ostringstream out {};
    std::ostringstream err {};
    core::platform::ManualWallClock clock { std::chrono::system_clock::time_point {}
                                            + std::chrono::hours { 12 } };
    core::async::StopSource stop {};

    [[nodiscard]] ControlIo io()
    {
        return ControlIo { .env = env,
                           .fs = fs,
                           .in = in,
                           .out = out,
                           .err = err,
                           .clock = clock,
                           .target = contour::cli::OutputTarget::Pipe,
                           .stop = stop.get_token() };
    }
};

/// Parses @p args (after `contour`) against the application's own syntax.
core::cli::FlagStore parseFlags(std::vector<std::string_view> const& args)
{
    static auto const app = contour::cli::ContourApp { core::defaultEnvironment() };
    auto line = core::cli::StringViewList { "contour" };
    line.insert(line.end(), args.begin(), args.end());
    auto parsed = core::cli::parse(app.parameterDefinition(), line);
    REQUIRE(parsed.has_value());
    return *std::move(parsed);
}

contour::cli::ControlVerb const& verbNamed(std::string_view path)
{
    auto const verbs = contour::cli::controlVerbs();
    auto const found = std::ranges::find(verbs, path, &contour::cli::ControlVerb::path);
    REQUIRE(found != verbs.end());
    return *found;
}

} // namespace

TEST_CASE("split builds its request from flags", "[contour][cli][control]")
{
    auto const flags =
        parseFlags({ "split", "--below", "--ratio", "30", "--no-focus", "--", "htop", "-d", "5" });
    auto const params = verbNamed("split").buildParams(flags);
    REQUIRE(params.has_value());
    CHECK(*params
          == vtcontrol::encodeJson(vtcontrol::SplitRequest { .direction = vtcontrol::SplitDirection::Below,
                                                             .ratioPercent = 30,
                                                             .command = { "htop", "-d", "5" },
                                                             .focus = vtcontrol::FocusPolicy::Keep }));
}

TEST_CASE("contradictory or malformed flags are bad usage", "[contour][cli][control]")
{
    auto run = Run {};
    auto channel = ScriptedChannel { "{}"s };
    auto const status = [&](std::vector<std::string_view> const& args) {
        auto const flags = parseFlags(args);
        return contour::cli::runControlVerb(verbNamed(args.front()), flags, channel, run.io());
    };

    CHECK(status({ "split", "--right", "--below" }) == 2);
    CHECK(status({ "split", "--ratio", "0" }) == 2);
    CHECK(status({ "split", "--ratio", "x" }) == 2);
    CHECK(status({ "split", "--ratio", "100" }) == 2);
    CHECK(status({ "new", "--window", "w", "--new-window" }) == 2);
    CHECK(status({ "close", "--pane", "p", "--tab", "t" }) == 2);
    CHECK(status({ "label", "only-one" }) == 2);
    CHECK(channel.calls.empty());
    CHECK(run.err.str().contains("--ratio"));
}

TEST_CASE("numeric flags past what their request holds are bad usage", "[contour][cli][control]")
{
    auto run = Run {};
    auto channel = ScriptedChannel { "{}"s };
    auto const status = [&](std::string_view path, std::vector<std::string_view> const& args) {
        return contour::cli::runControlVerb(verbNamed(path), parseFlags(args), channel, run.io());
    };

    // A float past what milliseconds hold would make the conversion undefined.
    for (auto const* timeout: { "1e300", "inf", "nan", "-1", "10000001" })
    {
        INFO(timeout);
        CHECK(status("wait", { "wait", "--timeout", timeout }) == 2);
    }
    // Past int32_t the line count would wrap; 4294967295 would read as --all.
    CHECK(status("pane.capture", { "pane", "capture", "--scrollback", "2147483648" }) == 2);
    CHECK(status("pane.capture", { "pane", "capture", "--scrollback", "4294967295" }) == 2);
    CHECK(channel.calls.empty());
    CHECK(run.err.str().contains("--timeout takes a number of seconds up to 10000000, 0 for none"));
    CHECK(run.err.str().contains("--scrollback takes at most 2147483647 lines"));

    CHECK(status("wait", { "wait", "--timeout", "10000000" }) == 0);
    CHECK(status("pane.capture", { "pane", "capture", "--scrollback", "2147483647" }) == 0);
    REQUIRE(channel.calls.size() == 2);
    CHECK(channel.calls.back().params.contains("2147483647"));
}

TEST_CASE("a wait whose reply does not decode fails rather than succeeds", "[contour][cli][control]")
{
    auto run = Run {};
    auto garbled = ScriptedChannel { R"({"block":"not a block"})"s };
    CHECK(contour::cli::runControlVerb(verbNamed("wait"), parseFlags({ "wait" }), garbled, run.io()) == 1);
    CHECK(run.err.str() == "Error: the daemon's reply could not be read: {\"block\":\"not a block\"}\n");
}

TEST_CASE("every ControlErrorCode and ControlFailure has an exit status", "[contour][cli][control]")
{
    using vtcontrol::ControlErrorCode;
    struct CodeRow
    {
        ControlErrorCode code;
        ExitStatus status;
    };
    constexpr auto CodeRows = std::array {
        CodeRow { .code = ControlErrorCode::BadRequest, .status = ExitStatus::BadUsage },
        CodeRow { .code = ControlErrorCode::MethodNotFound, .status = ExitStatus::Failed },
        CodeRow { .code = ControlErrorCode::NotFound, .status = ExitStatus::TargetNotFound },
        CodeRow { .code = ControlErrorCode::Ambiguous, .status = ExitStatus::TargetNotFound },
        CodeRow { .code = ControlErrorCode::Unsupported, .status = ExitStatus::Failed },
        CodeRow { .code = ControlErrorCode::Forbidden, .status = ExitStatus::Failed },
        CodeRow { .code = ControlErrorCode::Failed, .status = ExitStatus::Failed },
        CodeRow { .code = ControlErrorCode::LimitReached, .status = ExitStatus::Failed },
    };
    // Every code the API names has a row: a code added without one fails here, not at a script's exit.
    CHECK(CodeRows.size() == vtcontrol::EnumTable<ControlErrorCode>::Rows.size());
    for (auto const& row: CodeRows)
    {
        INFO(vtcontrol::nameOf(row.code));
        CHECK(contour::cli::exitStatusFor(row.code) == row.status);
    }

    struct FailureRow
    {
        ControlFailure failure;
        ExitStatus status;
    };
    constexpr auto FailureRows = std::array {
        FailureRow { .failure = ControlFailure::Unreachable, .status = ExitStatus::Unreachable },
        FailureRow { .failure = ControlFailure::Refused, .status = ExitStatus::Failed },
        FailureRow { .failure = ControlFailure::Unsupported, .status = ExitStatus::Failed },
        FailureRow { .failure = ControlFailure::Untrusted, .status = ExitStatus::Failed },
        FailureRow { .failure = ControlFailure::Disconnected, .status = ExitStatus::Unreachable },
        FailureRow { .failure = ControlFailure::TimedOut, .status = ExitStatus::Failed },
        FailureRow { .failure = ControlFailure::Remote, .status = ExitStatus::Failed },
        FailureRow { .failure = ControlFailure::Misconfigured, .status = ExitStatus::BadUsage },
    };
    // In value order, so the last row is the last enumerator and none is skipped.
    for (auto const [index, row]: std::views::enumerate(FailureRows))
    {
        CHECK(std::cmp_equal(std::to_underlying(row.failure), index));
        CHECK(contour::cli::exitStatusFor(row.failure) == row.status);
    }

    CHECK(contour::cli::exitStatusFor(remoteError(ControlErrorCode::NotFound, "gone"))
          == ExitStatus::TargetNotFound);
    CHECK(contour::cli::exitStatusFor(remoteError(ControlErrorCode::LimitReached, "too many subscriptions"))
          == ExitStatus::Failed);
    CHECK(contour::cli::exitStatusFor(ControlClientError {
              .failure = ControlFailure::TimedOut, .message = {}, .remote = std::nullopt })
          == ExitStatus::Failed);
}

TEST_CASE("exitStatusForBlock", "[contour][cli][control]")
{
    using vtcontrol::BlockInfo;
    using vtcontrol::BlockState;
    auto const finished = [](std::optional<int32_t> code, BlockState state = BlockState::Finished) {
        return BlockInfo { .state = state, .exitCode = code };
    };
    CHECK(contour::cli::exitStatusForBlock(finished(0)) == 0);
    CHECK(contour::cli::exitStatusForBlock(finished(3)) == 3);
    CHECK(contour::cli::exitStatusForBlock(finished(300)) == 255);
    CHECK(contour::cli::exitStatusForBlock(finished(-1)) == 1);
    CHECK(contour::cli::exitStatusForBlock(finished(std::nullopt)) == 1);
    CHECK(contour::cli::exitStatusForBlock(finished(0, BlockState::Superseded)) == 1);
    CHECK_FALSE(contour::cli::exitStatusNote(finished(3)).has_value());
    CHECK(contour::cli::exitStatusNote(finished(std::nullopt)).has_value());
}

TEST_CASE("ls prints a readable tree", "[contour][cli][control]")
{
    auto run = Run {};
    auto channel =
        ScriptedChannel { R"({"windows":[{"id":"window:018f3c4e-9a1b-7c2d-8e3f-0123456789ab","label":"",)"
                          R"("position":1,"tabs":[{"id":"tab:018f3c4e-9a1b-7c2d-8e3f-0123456789ac",)"
                          R"("label":"build","title":"zsh","position":1,"isActive":true,"panes":[)"
                          R"({"id":"pane:018f3c4e-9a1b-7c2d-8e3f-0123456789ad","label":"","title":"zsh",)"
                          R"("cwd":"/home/me","isActive":true,"size":{"columns":80,"lines":24},)"
                          R"("programStatus":[],"futureKey":1}]}]}]})"s };
    auto const flags = parseFlags({ "ls" });

    CHECK(contour::cli::runControlVerb(verbNamed("ls"), flags, channel, run.io()) == 0);

    CHECK(run.out.str()
          == "window 1  456789ab\n"
             "  * tab 1  456789ac  build\n"
             "    * pane  456789ad  zsh  80x24  /home/me\n");
    REQUIRE(channel.calls.size() == 1);
    CHECK(channel.calls.front().method == "workspace.list");
    CHECK(channel.calls.front().context == "pane:018f3c4e-9a1b-7c2d-8e3f-0123456789ad");
}

TEST_CASE("--json prints the reply as received", "[contour][cli][control]")
{
    auto run = Run {};
    auto channel = ScriptedChannel { R"({"tab":"tab:1","pane":"pane:2"})"s };
    auto const flags = parseFlags({ "split", "--json" });

    CHECK(contour::cli::runControlVerb(verbNamed("split"), flags, channel, run.io()) == 0);
    CHECK(run.out.str() == "{\"tab\":\"tab:1\",\"pane\":\"pane:2\"}\n");

    auto human = Run {};
    CHECK(contour::cli::runControlVerb(verbNamed("split"), parseFlags({ "split" }), channel, human.io())
          == 0);
    CHECK(human.out.str() == "pane:2\n");
}

TEST_CASE("close picks pane.close or tab.close", "[contour][cli][control]")
{
    auto run = Run {};
    auto channel = ScriptedChannel { "{}"s };
    CHECK(contour::cli::runControlVerb(
              verbNamed("close"), parseFlags({ "close", "--tab", "2" }), channel, run.io())
          == 0);
    CHECK(contour::cli::runControlVerb(verbNamed("close"), parseFlags({ "close" }), channel, run.io()) == 0);
    REQUIRE(channel.calls.size() == 2);
    CHECK(channel.calls[0].method == "tab.close");
    CHECK(channel.calls[0].params == R"({"tab":"2"})");
    CHECK(channel.calls[1].method == "pane.close");
}

TEST_CASE("send joins its words, reads stdin and exits with the block's status", "[contour][cli][control]")
{
    auto run = Run {};
    auto channel = ScriptedChannel {
        R"({"block":{"id":"block:x/1","pane":"pane:x","state":"finished","exitCode":3}})"s
    };

    CHECK(
        contour::cli::runControlVerb(
            verbNamed("send"), parseFlags({ "send", "--enter", "--wait", "make", "test" }), channel, run.io())
        == 3);
    REQUIRE(channel.calls.size() == 1);
    CHECK(channel.calls.front().params
          == vtcontrol::encodeJson(vtcontrol::SendRequest { .text = "make test\r",
                                                            .wait = vtcontrol::SendWait::CommandFinished }));

    run.in.str("from stdin");
    auto plain = ScriptedChannel { "{}"s };
    CHECK(contour::cli::runControlVerb(verbNamed("send"), parseFlags({ "send", "-" }), plain, run.io()) == 0);
    REQUIRE(plain.calls.size() == 1);
    CHECK(plain.calls.back().params
          == vtcontrol::encodeJson(vtcontrol::SendRequest { .text = "from stdin" }));
}

TEST_CASE("api call sends params verbatim and no context", "[contour][cli][control]")
{
    auto run = Run {};
    auto channel = ScriptedChannel { R"({"ok":true})"s };
    auto const flags = parseFlags({ "api", "call", "pane.send", R"({"pane":"x","text":"y"})" });

    CHECK(contour::cli::runControlVerb(verbNamed("api.call"), flags, channel, run.io()) == 0);

    REQUIRE(channel.calls.size() == 1);
    CHECK(channel.calls.front().method == "pane.send");
    CHECK(channel.calls.front().params == R"({"pane":"x","text":"y"})");
    CHECK(channel.calls.front().context.empty());
    CHECK(run.out.str() == "{\"ok\":true}\n");
}

TEST_CASE("api call reads its params from a file or stdin", "[contour][cli][control]")
{
    auto run = Run {};
    run.fs = core::platform::testing::InMemoryFileSystem { { .path = "/p.json", .content = R"({"a":1})" } };
    auto channel = ScriptedChannel { "{}"s };

    CHECK(contour::cli::runControlVerb(
              verbNamed("api.call"), parseFlags({ "api", "call", "m.x", "@/p.json" }), channel, run.io())
          == 0);
    run.in.str(R"({"b":2})");
    CHECK(contour::cli::runControlVerb(
              verbNamed("api.call"), parseFlags({ "api", "call", "m.x", "-" }), channel, run.io())
          == 0);
    CHECK(
        contour::cli::runControlVerb(
            verbNamed("api.call"), parseFlags({ "api", "call", "m.x", "@/missing.json" }), channel, run.io())
        == 2);

    REQUIRE(channel.calls.size() == 2);
    CHECK(channel.calls[0].params == R"({"a":1})");
    CHECK(channel.calls[1].params == R"({"b":2})");
}

TEST_CASE("a stale target exits 3 and names it", "[contour][cli][control]")
{
    auto run = Run {};
    auto channel = ScriptedChannel { std::unexpected(
        remoteError(vtcontrol::ControlErrorCode::NotFound, "pane 'pane:dead' not found")) };

    auto const status = contour::cli::runControlVerb(
        verbNamed("send"), parseFlags({ "send", "--pane", "pane:dead", "hi" }), channel, run.io());

    CHECK(status == 3);
    CHECK(run.err.str() == "Error: pane 'pane:dead' not found\n");
    CHECK(run.out.str().empty());
}

TEST_CASE("an ambiguous target lists its candidates", "[contour][cli][control]")
{
    auto run = Run {};
    auto error = remoteError(vtcontrol::ControlErrorCode::Ambiguous, "'logs' matches 2 panes");
    error.remote->candidates = { "pane:a", "pane:b" };
    auto channel = ScriptedChannel { std::unexpected(std::move(error)) };

    CHECK(
        contour::cli::runControlVerb(
            verbNamed("pane.capture"), parseFlags({ "pane", "capture", "--pane", "logs" }), channel, run.io())
        == 3);
    CHECK(run.err.str() == "Error: 'logs' matches 2 panes\n  pane:a\n  pane:b\n");
}

TEST_CASE("an unreachable daemon exits 4 naming the socket", "[contour][cli][control]")
{
    auto const temp = core::testing::ScopedTempDir { "contour-control-verbs" };
    auto const socket = (temp.path() / "nothing-listens-here").string();
    auto run = Run {};

    auto const status =
        contour::cli::runControlVerb(verbNamed("ls"), parseFlags({ "ls", "--daemon", socket }), run.io());

    CHECK(status == 4);
    INFO(run.err.str());
    CHECK(run.err.str().contains(socket));
}

TEST_CASE("a TCP daemon without --tls-ca is refused before connecting", "[contour][cli][control]")
{
    auto run = Run {};

    auto const status = contour::cli::runControlVerb(
        verbNamed("ls"), parseFlags({ "ls", "--daemon", "127.0.0.1:9" }), run.io());

    CHECK(status == 1);
    CHECK(run.err.str().contains("--tls-ca"));
}

TEST_CASE("an unreadable --tls-ca is bad usage", "[contour][cli][control]")
{
    auto run = Run {};
    CHECK(contour::cli::runControlVerb(
              verbNamed("ls"),
              parseFlags({ "ls", "--daemon", "127.0.0.1:9", "--tls-ca", "/no/such.pem" }),
              run.io())
          == 2);
    CHECK(run.err.str().contains("--tls-ca"));
}

TEST_CASE("a saved host named by --daemon is reached through its own transport",
          "[contour][cli][control][hosts]")
{
    using contour::config::HostEntry;
    using contour::config::HostScheme;
    using contour::config::HostUrl;

    /// Records what it was asked to start, and starts nothing.
    class RecordingSpawner final: public vthost::IStdioSpawner
    {
      public:
        std::vector<std::vector<std::string>> spawned;

        [[nodiscard]] std::expected<vthost::SpawnedStdioChild, vthost::StdioSpawnError> spawn(
            core::net::EventLoop& /*loop*/, std::span<std::string const> argv) override
        {
            spawned.emplace_back(argv.begin(), argv.end());
            return std::unexpected { vthost::StdioSpawnError { .kind = vthost::SpawnFailure::SystemError,
                                                               .message = "not in this test" } };
        }
    };

    // The saved hosts the configuration would hold.
    auto const lookup = [](std::string_view name) -> std::expected<HostEntry, std::string> {
        if (name == "studio")
            return HostEntry {
                .url = HostUrl { .scheme = HostScheme::Tls, .user = {}, .host = "studio", .port = 7297 },
                .tokenFile = "/home/me/studio.token",
                .tlsCa = {},
                .remoteCommand = {},
                .profile = {}
            };
        if (name == "homepc")
            return HostEntry {
                .url = HostUrl { .scheme = HostScheme::Ssh, .user = "me", .host = "homepc", .port = 0 },
                .tokenFile = {},
                .tlsCa = {},
                .remoteCommand = {},
                .profile = {}
            };
        return std::unexpected { std::format("no host named '{}' (saved hosts: homepc, studio)", name) };
    };
    auto run = Run {};
    run.fs.addFile("/home/me/studio.token", "secret\n");
    auto const spawner = std::make_shared<RecordingSpawner>();
    auto io = run.io();
    io.resolveSavedHost = contour::cli::makeSavedHostResolver(lookup, run.fs, spawner);

    SECTION("a tls host is the host's transport, refused for want of its tls_ca before connecting")
    {
        CHECK(contour::cli::runControlVerb(verbNamed("ls"), parseFlags({ "ls", "--daemon", "studio" }), io)
              == 1);
        CHECK(run.err.str().contains("tls://studio:7297"));
        CHECK(run.err.str().contains("tls_ca"));
    }

    SECTION("an ssh host starts ssh to it")
    {
        CHECK(contour::cli::runControlVerb(verbNamed("ls"), parseFlags({ "ls", "--daemon", "homepc" }), io)
              == 4);
        REQUIRE(spawner->spawned.size() == 1);
        CHECK(std::ranges::contains(spawner->spawned.front(), "me@homepc"s));
    }

    SECTION("a name no host has says so, rather than naming a socket")
    {
        CHECK(contour::cli::runControlVerb(verbNamed("ls"), parseFlags({ "ls", "--daemon", "nope" }), io)
              == 4);
        CHECK(run.err.str() == "Error: --daemon nope: no host named 'nope' (saved hosts: homepc, studio)\n");
        CHECK(spawner->spawned.empty());
    }

    SECTION("a socket path is still a socket path")
    {
        auto const temp = core::testing::ScopedTempDir { "contour-control-verbs" };
        auto const socket = (temp.path() / "nothing-listens-here").string();
        CHECK(contour::cli::runControlVerb(verbNamed("ls"), parseFlags({ "ls", "--daemon", socket }), io)
              == 4);
        CHECK(run.err.str().contains(socket));
        CHECK(spawner->spawned.empty());
    }
}

TEST_CASE("a local verb never resolves an endpoint", "[contour][cli][control]")
{
    auto const dummy = contour::cli::ControlVerb {
        .path = "dummy",
        .method = "",
        .usage = "dummy",
        .summary = "A verb that never reaches a daemon.",
        .options = [] { return core::cli::OptionList {}; },
        .verbatim = std::nullopt,
        .buildParams = [](core::cli::FlagStore const& flags) -> std::expected<std::string, std::string> {
            if (!flags.verbatim.empty())
                return std::unexpected("dummy takes no words"s);
            return std::string { "{}" };
        },
        .printHuman = [](std::string_view reply, std::ostream& out) { out << "local:" << reply << '\n'; },
    };
    auto run = Run {};
    auto flags = core::cli::FlagStore {};

    CHECK(contour::cli::runControlVerb(dummy, flags, run.io()) == 0);
    CHECK(run.out.str() == "local:\n");
    CHECK(run.err.str().empty());

    flags.verbatim = { "word" };
    CHECK(contour::cli::runControlVerb(dummy, flags, run.io()) == 2);
    CHECK(run.err.str() == "Error: dummy takes no words\n");
}

TEST_CASE("api wit prints the checked-in WIT package without a daemon", "[contour][cli][control]")
{
    // A local verb: no daemon runs here, and none is looked for.
    auto run = Run {};

    CHECK(contour::cli::runControlVerb(verbNamed("api.wit"), parseFlags({ "api", "wit" }), run.io()) == 0);
    CHECK(run.err.str().empty());
    // Byte for byte what vtcontrol's golden test holds wit/control/world.wit to, so that
    // `contour api wit > wit/control/world.wit` regenerates the file the README says it does.
    CHECK(run.out.str() == vtcontrol::renderControlWit());
    auto file = std::ifstream(CONTOUR_CONTROL_WIT, std::ios::binary);
    REQUIRE(file.good());
    CHECK(run.out.str() == std::string { std::istreambuf_iterator<char>(file), {} });

    // --json is not an option of a local verb.
    auto const app = contour::cli::ContourApp { core::defaultEnvironment() };
    CHECK_FALSE(core::cli::parse(app.parameterDefinition(),
                                 core::cli::StringViewList { "contour", "api", "wit", "--json" })
                    .has_value());
}

TEST_CASE("every ControlVerb with a method names a catalog row", "[contour][cli][control]")
{
    auto const inCatalog = [](std::string_view name) {
        return std::apply([&](auto const&... method) { return ((method.name == name) || ...); },
                          vtcontrol::Methods);
    };
    for (auto const& verb: contour::cli::controlVerbs())
    {
        INFO(verb.path);
        if (verb.methodSource == contour::cli::MethodSource::Fixed && !verb.method.empty())
            CHECK(inCatalog(verb.method));
        if (!verb.alternateMethod.empty())
            CHECK(inCatalog(verb.alternateMethod));
        CHECK(verb.options != nullptr);
        CHECK(!verb.usage.empty());
    }
}

TEST_CASE("wait and block read build their requests", "[contour][cli][control]")
{
    CHECK(*verbNamed("wait").buildParams(parseFlags({ "wait", "--pane", "build", "block:x/1" }))
          == vtcontrol::encodeJson(vtcontrol::WaitRequest { .block = "block:x/1", .pane = "build" }));
    CHECK(*verbNamed("block.read").buildParams(parseFlags({ "block", "read", "--output", "--pane", "p" }))
          == vtcontrol::encodeJson(
              vtcontrol::BlockReadRequest { .pane = "p", .part = vtcontrol::BlockPart::Output }));
    CHECK(*verbNamed("block.read").buildParams(parseFlags({ "block", "read", "--prompt", "b" }))
          == vtcontrol::encodeJson(
              vtcontrol::BlockReadRequest { .block = "b", .part = vtcontrol::BlockPart::Prompt }));
    CHECK(*verbNamed("block.list").buildParams(parseFlags({ "block", "list", "--limit", "5" }))
          == vtcontrol::encodeJson(vtcontrol::BlockListRequest { .limit = 5 }));
    CHECK_FALSE(verbNamed("block.read").buildParams(parseFlags({ "block", "read", "--prompt", "--output" })));
    CHECK_FALSE(verbNamed("wait").buildParams(parseFlags({ "wait", "a", "b" })));
    CHECK(
        *verbNamed("events").buildParams(parseFlags({ "events", "--kind", "block.finished,pane.bell" }))
        == vtcontrol::encodeJson(vtcontrol::SubscribeRequest { .kinds = { "block.finished", "pane.bell" } }));
}

TEST_CASE("wait exits with the block's status", "[contour][cli][control]")
{
    auto run = Run {};
    auto finished = ScriptedChannel {
        R"({"block":{"id":"block:x/1","pane":"pane:x","state":"finished","exitCode":7}})"s
    };
    CHECK(contour::cli::runControlVerb(verbNamed("wait"), parseFlags({ "wait" }), finished, run.io()) == 7);

    auto none = ScriptedChannel { "{}"s };
    CHECK(contour::cli::runControlVerb(verbNamed("wait"), parseFlags({ "wait" }), none, run.io()) == 0);

    auto superseded =
        ScriptedChannel { R"({"block":{"id":"block:x/1","pane":"pane:x","state":"superseded"}})"s };
    CHECK(contour::cli::runControlVerb(verbNamed("wait"), parseFlags({ "wait" }), superseded, run.io()) == 1);
    CHECK(run.err.str().contains("superseded"));
}

TEST_CASE("a timed out wait says so", "[contour][cli][control]")
{
    auto run = Run {};
    auto channel = ScriptedChannel { std::unexpected(ControlClientError {
        .failure = ControlFailure::TimedOut, .message = "deadline", .remote = std::nullopt }) };

    CHECK(contour::cli::runControlVerb(
              verbNamed("wait"), parseFlags({ "wait", "--timeout", "2" }), channel, run.io())
          == 1);
    CHECK(run.err.str() == "Error: timed out\n");
}

TEST_CASE("block list prints one line per block", "[contour][cli][control]")
{
    auto run = Run {};
    auto channel = ScriptedChannel {
        R"({"pane":"pane:x","blocks":[)"
        R"({"id":"block:x/2","pane":"pane:x","state":"finished","command":"make","exitCode":2,"durationMs":1200},)"
        R"({"id":"block:x/1","pane":"pane:x","state":"running"}]})"s
    };

    CHECK(contour::cli::runControlVerb(
              verbNamed("block.list"), parseFlags({ "block", "list" }), channel, run.io())
          == 0);
    CHECK(run.out.str()
          == "block:x/2     2     1.2s  make\n"
             "block:x/1     -        -  -\n");
}

TEST_CASE("block read prints prompt then output", "[contour][cli][control]")
{
    auto run = Run {};
    auto channel = ScriptedChannel {
        R"({"block":{"id":"block:x/1","pane":"pane:x","state":"finished"},"prompt":"$ make\n","output":"ok\n"})"s
    };
    CHECK(contour::cli::runControlVerb(
              verbNamed("block.read"), parseFlags({ "block", "read" }), channel, run.io())
          == 0);
    CHECK(run.out.str() == "$ make\nok\n");
}

TEST_CASE("events without --follow stops after the first event", "[contour][cli][control]")
{
    auto run = Run {};
    auto channel = ScriptedChannel { "{}"s };
    channel.events = { R"({"type":"pane.bell","window":"","tab":"","pane":"pane:1234abcd","payload":{}})",
                       R"({"type":"pane.bell","window":"","tab":"","pane":"pane:1234abcd","payload":{}})" };

    CHECK(contour::cli::runControlVerb(verbNamed("events"), parseFlags({ "events" }), channel, run.io())
          == 0);
    CHECK(channel.delivered == 1);

    auto following = ScriptedChannel { "{}"s };
    following.events = channel.events;
    CHECK(contour::cli::runControlVerb(
              verbNamed("events"), parseFlags({ "events", "--follow" }), following, run.io())
          == 0);
    CHECK(following.delivered == 2);
}

TEST_CASE("human event lines", "[contour][cli][control]")
{
    auto run = Run {};
    auto channel = ScriptedChannel { "{}"s };
    channel.events = {
        R"({"type":"block.finished","window":"","tab":"","pane":"pane:018f3c4e-9a1b-7c2d-8e3f-01231234abcd",)"
        R"("payload":{"block":{"id":"block:x/1","pane":"pane:x","state":"finished","command":"make","exitCode":3}}})",
        R"({"type":"pane.titleChanged","window":"","tab":"","pane":"","payload":{"title":"vim"}})",
        R"({"type":"future.thing","window":"","tab":"","pane":"","payload":{}})",
    };

    CHECK(contour::cli::runControlVerb(
              verbNamed("events"), parseFlags({ "events", "--follow" }), channel, run.io())
          == 0);
    CHECK(run.out.str()
          == "12:00:00 block.finished \u20261234abcd exit 3 make\n"
             "12:00:00 pane.titleChanged - vim\n"
             "12:00:00 "
             "{\"type\":\"future.thing\",\"window\":\"\",\"tab\":\"\",\"pane\":\"\",\"payload\":{}}\n");
}

TEST_CASE("events --json prints each event as received; Ctrl+C is 130", "[contour][cli][control]")
{
    auto run = Run {};
    auto channel = ScriptedChannel { "{}"s };
    channel.events = { R"({"type":"pane.bell","window":"","tab":"","pane":"","payload":{}})" };

    CHECK(contour::cli::runControlVerb(
              verbNamed("events"), parseFlags({ "events", "--follow", "--json" }), channel, run.io())
          == 0);
    CHECK(run.out.str() == channel.events.front() + "\n");

    run.stop.request_stop();
    CHECK(contour::cli::runControlVerb(
              verbNamed("events"), parseFlags({ "events", "--follow" }), channel, run.io())
          == 130);
}
