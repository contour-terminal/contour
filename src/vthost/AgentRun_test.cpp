// SPDX-License-Identifier: Apache-2.0
//
// An agent run end to end: the orchestrator's real module on the real daemon world, a window that granted it
// and opened its dashboard, and what the user types there -- a dispatch into a worktree, the agent's tab
// beside the dashboard, its status, its end, and the removal checks -- across a live upgrade, a keeper
// recovery, a cold restart and a guest fault. The agent is a fake (fakeAgentScript()), never `claude`: it
// reports the directory it physically started in and the environment it got, and the cases check both.

#include <vtpty/DirectoryHandle.hpp>
#include <vtpty/Process.hpp>

#include <core/Environment.hpp>
#include <core/Utils.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>
#include <vtextension/testing/FixtureProcessRunner.hpp>
#include <vtextension/testing/GitSandbox.hpp>
#include <vtextension/testing/GuestSlowdown.hpp>
#include <vthost/ExtensionHosting.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/WorkspaceCapture.hpp>
#include <vthost/handover/HandoverCodec.hpp>
#include <vthost/testing/OrchestratorDashboard.hpp>

using namespace vthost;
using namespace std::chrono_literals;
using vthost::testing::ClientKind;
using vthost::testing::LiveHost;
using vthost::testing::OrchestratorName;
using vthost::testing::OrchestratorWorld;
using vtworkspace::SessionId;

namespace
{

/// A finished run's outcome, as the orchestrator stores it.
struct StoredFinished
{
    std::string ended;
    std::string fate;
    std::vector<std::string> entries;
};

/// A preparation failure, as the orchestrator stores it.
struct StoredFailure
{
    std::string step;
    std::string why;
};

/// What the cases read of a stored run.
struct StoredRun
{
    std::string id;
    std::string state;
    std::string session;
    std::string worktree;
    std::optional<std::string> pane;
    std::optional<std::string> lastStatus;
    std::optional<StoredFinished> finished;
    std::optional<StoredFailure> failure;
};

/// How long a case waits for what it counts on.
constexpr auto Patience = std::chrono::seconds { 20 } * vtextension::testing::GuestSlowdown;

constexpr auto Enter = std::string_view { "\r" };
constexpr auto Tab = std::string_view { "\t" };
constexpr auto Home = std::string_view { "\033[H" };
constexpr auto Down = std::string_view { "\033[B" };

/// The prompt of the task for pull requests, on the first fixture pull request.
constexpr auto FixCiPrompt =
    std::string_view { "/contour-workflows:fix-ci https://github.com/contour-terminal/contour/pull/2375" };

/// @return The member @p key of @p node, if @p node is an object that has one.
[[nodiscard]] glz::generic const* memberOf(glz::generic const& node, std::string_view key)
{
    auto const* object = node.get_if<glz::generic::object_t>();
    if (object == nullptr)
        return nullptr;
    auto const member = object->find(key);
    return member != object->end() ? &member->second : nullptr;
}

/// @return The string member @p key of @p node, if it has one.
[[nodiscard]] std::optional<std::string> textOf(glz::generic const& node, std::string_view key)
{
    auto const* member = memberOf(node, key);
    auto const* text = member != nullptr ? member->get_if<std::string>() : nullptr;
    return text != nullptr ? std::optional { *text } : std::nullopt;
}

/// @return @p json read as a stored run; std::nullopt when it does not read as one.
[[nodiscard]] std::optional<StoredRun> parseRun(std::string_view json)
{
    auto document = glz::generic {};
    if (json.empty() || glz::read_json(document, json))
        return std::nullopt;
    auto run = StoredRun { .id = textOf(document, "id").value_or(""),
                           .state = textOf(document, "state").value_or(""),
                           .session = textOf(document, "session").value_or(""),
                           .worktree = textOf(document, "worktree").value_or(""),
                           .pane = textOf(document, "pane"),
                           .lastStatus = textOf(document, "lastStatus"),
                           .finished = std::nullopt,
                           .failure = std::nullopt };
    if (auto const* finished = memberOf(document, "finished"))
    {
        auto entries = std::vector<std::string> {};
        if (auto const* list = memberOf(*finished, "entries"); list != nullptr)
            if (auto const* array = list->get_if<glz::generic::array_t>(); array != nullptr)
                for (auto const& entry: *array)
                    if (auto const* text = entry.get_if<std::string>(); text != nullptr)
                        entries.push_back(*text);
        run.finished = StoredFinished { .ended = textOf(*finished, "ended").value_or(""),
                                        .fate = textOf(*finished, "fate").value_or(""),
                                        .entries = std::move(entries) };
    }
    if (auto const* failure = memberOf(document, "failure"))
        run.failure = StoredFailure { .step = textOf(*failure, "step").value_or(""),
                                      .why = textOf(*failure, "why").value_or("") };
    return run;
}

/// @return The run @p world's orchestrator stored whose id begins with @p prefix (`pr-2375-`), if one.
[[nodiscard]] std::optional<StoredRun> storedRun(OrchestratorWorld const& world, std::string_view prefix)
{
    for (auto const& id: world.storedRuns())
        if (id.starts_with(prefix))
            return parseRun(world.storedRun(id));
    return std::nullopt;
}

/// @return Whether @p world holds a run beginning with @p prefix in @p state.
[[nodiscard]] bool runIs(OrchestratorWorld const& world, std::string_view prefix, std::string_view state)
{
    auto const run = storedRun(world, prefix);
    return run && run->state == state;
}

/// Turns @p live's loop until @p condition holds, answering every command @p world's orchestrator runs.
/// @return Whether it held within Patience.
[[nodiscard]] bool turnUntil(LiveHost& live, OrchestratorWorld& world, std::function<bool()> const& condition)
{
    auto const deadline = std::chrono::steady_clock::now() + Patience;
    while (true)
    {
        std::ignore = world.processRunner.answerWaiting();
        if (condition())
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::ignore = live.loop.runOnce(10ms);
    }
}

/// @return The session of the agent's pane @p pane names in @p host, if it is there.
[[nodiscard]] std::optional<SessionId> sessionOfPane(SessionHost const& host, std::string_view pane)
{
    auto const uuid = vtcontrol::parseUuid(pane);
    if (!uuid)
        return std::nullopt;
    for (auto const id: host.sessionIds())
        if (host.objects().uuidOf(id) == uuid)
            return id;
    return std::nullopt;
}

/// @return The argv of every command @p world's orchestrator started that is not one of a poll's.
[[nodiscard]] std::vector<std::vector<std::string>> runCommands(OrchestratorWorld const& world)
{
    auto const isPolled = [](std::vector<std::string> const& argv) {
        return std::ranges::any_of(vtextension::testing::OrchestratorFixtureAnswers, [&](auto const& answer) {
            return argv.size() >= 3 && argv[0] == answer.program && argv[1] == answer.verb
                   && (answer.verb != "worktree" || argv[2] == "list")
                   && (answer.verb != "pr" || argv[2] == "list");
        });
    };
    return std::ranges::to<std::vector<std::vector<std::string>>>(
        world.processRunner.started() | std::views::filter(std::not_fn(isPolled)));
}

/// @return What @p world's orchestrator stored of its runs and the commands it ran for them, for a failure's
///         message.
[[nodiscard]] std::string runsOf(OrchestratorWorld const& world)
{
    auto text = std::string {};
    for (auto const& id: world.storedRuns())
        text += std::format("run {}: {}\n", id, world.storedRun(id));
    for (auto const& argv: runCommands(world))
    {
        text += "ran:";
        for (auto const& word: argv)
            text += std::format(" {}", word);
        text += '\n';
    }
    return text;
}

/// @return Whether @p argv begins with @p words.
[[nodiscard]] bool beginsWith(std::vector<std::string> const& argv,
                              std::vector<std::string_view> const& words)
{
    // std::ranges::starts_with, which libstdc++ 14 lacks.
    return std::ranges::mismatch(argv, words).in2 == words.end();
}

/// @return Whether @p commands holds a command beginning with each of @p prefixes, in their order.
[[nodiscard]] bool ranInOrder(std::vector<std::vector<std::string>> const& commands,
                              std::vector<std::vector<std::string_view>> const& prefixes)
{
    auto next = commands.begin();
    for (auto const& prefix: prefixes)
    {
        next = std::ranges::find_if(
            next, commands.end(), [&](auto const& argv) { return beginsWith(argv, prefix); });
        if (next == commands.end())
            return false;
        ++next;
    }
    return true;
}

/// @return A task that awaits @p task, then marks @p done.
core::async::Task<void> tracked(core::async::Task<void> task, bool* done)
{
    co_await std::move(task);
    *done = true;
}

/// A window attached to a daemon that serves the orchestrator, its connection run on the daemon's loop.
class Window
{
  public:
    /// Connects, and attaches.
    /// @param live The daemon.
    /// @param world Its daemon world.
    Window(LiveHost& live, OrchestratorWorld& world):
        _live { live }, _world { world }, _window { world.connect(live.loop, live.host, ClientKind::Window) }
    {
        _live.loop.spawn(tracked(_window->server->run(), &_serverDone));
        _live.loop.spawn(tracked(_window->client->run(), &_clientDone));
        REQUIRE(until([this] { return _window->client->selectedWorkspace() != 0; }));
    }

    Window(Window const&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window const&) = delete;
    Window& operator=(Window&&) = delete;

    ~Window()
    {
        if (_serverDone && _clientDone)
            return;
        _window->client->detach();
        std::ignore = until([this] { return _serverDone && _clientDone; });
    }

    /// Turns the loop until @p condition holds, answering the orchestrator's commands.
    /// @return Whether it held within Patience.
    [[nodiscard]] bool until(std::function<bool()> const& condition)
    {
        return turnUntil(_live, _world, condition);
    }

    /// @return How many times the window was told to reveal @p session.
    [[nodiscard]] std::size_t revealsOf(SessionId session) const
    {
        return vthost::testing::revealsOf(_window->log, session);
    }

    /// Detaches, and turns the loop until the connection ended.
    void leave()
    {
        _window->client->detach();
        REQUIRE(until([this] { return _serverDone && _clientDone; }));
    }

    [[nodiscard]] vthost::testing::Connection& connection() noexcept { return *_window; }

  private:
    LiveHost& _live;
    OrchestratorWorld& _world;
    std::unique_ptr<vthost::testing::Connection> _window;
    bool _serverDone = false;
    bool _clientDone = false;
};

/// A window that granted the orchestrator and opened its dashboard, and types into it while the daemon runs
/// on -- every command the orchestrator runs answered meanwhile.
class Desk
{
  public:
    /// Connects, grants the orchestrator if asked, and opens its dashboard.
    /// @param live The daemon.
    /// @param world Its daemon world.
    Desk(LiveHost& live, OrchestratorWorld& world): _live { live }, _window { live, world }
    {
        auto& connection = _window.connection();
        auto const offered = [&] {
            return vthost::testing::offersRunning(connection.log, OrchestratorName, "open");
        };
        REQUIRE(until([&] { return offered() || !connection.log.requests.empty(); }));
        if (!offered())
        {
            auto const& request = connection.log.requests.front();
            connection.client->answerDaemonExtension(request.name, request.grantDigest, GrantState::Granted);
            REQUIRE(until(offered));
        }
        workspace = connection.client->selectedWorkspace();
        connection.client->invokeExtensionAction(std::string(OrchestratorName), "open");
        REQUIRE(until([&] {
            auto const session = vthost::testing::servedIn(_live.host, workspace);
            if (session && vthost::testing::showsFixtureLists(_live.pageText(*session)))
                dashboard = *session;
            return dashboard.value != 0;
        }));
    }

    /// @see Window::until()
    [[nodiscard]] bool until(std::function<bool()> const& condition) { return _window.until(condition); }

    /// Types @p keys into the dashboard.
    void type(std::string_view keys)
    {
        _window.connection().client->sendInput(dashboard.value, keys, proto::InputKind::Keyboard);
    }

    /// @return The dashboard's screen, as text.
    [[nodiscard]] std::string page() { return _live.pageText(dashboard); }

    /// @see Window::revealsOf()
    [[nodiscard]] std::size_t revealsOf(SessionId session) const { return _window.revealsOf(session); }

    /// @see Window::leave()
    void leave() { _window.leave(); }

    SessionId dashboard {}; ///< The dashboard's session.
    uint64_t workspace = 0; ///< The window's workspace, which holds the dashboard.

  private:
    LiveHost& _live;
    Window _window;
};

/// What a dispatch brought about.
struct Dispatched
{
    StoredRun run;                       ///< The run, Running.
    SessionId agent {};                  ///< Its agent's session.
    vthost::testing::AgentReport report; ///< What its agent reported of itself.
    std::string formPage;                ///< The dashboard's screen while the form was open.
};

/// @return The report of the agent @p world spawned for @p session, once it wrote it.
[[nodiscard]] std::optional<vthost::testing::AgentReport> reportOf(OrchestratorWorld const& world,
                                                                   std::string_view session)
{
    auto const spawned = world.spawnedAgents();
    auto const found = std::ranges::find_if(
        spawned, [&](auto const& exec) { return std::ranges::contains(exec.arguments, session); });
    if (found == spawned.end())
        return std::nullopt;
    return world.agentReport(static_cast<std::size_t>(std::distance(spawned.begin(), found)));
}

/// Checks what the agent of @p run found itself started with: in the run's worktree, physically, its
/// repository not chosen by the daemon's environment, git's push settings appended to the user's own.
void checkAgentStart(vthost::testing::AgentReport const& report, StoredRun const& run)
{
    INFO("agent's environment: " << report.environment.size() << " entries");
    CHECK(report.cwd == run.worktree);
    CHECK_FALSE(report.has("GIT_DIR"));
    CHECK_FALSE(report.has("GH_REPO"));
    CHECK_FALSE(report.has("GH_HOST"));
    CHECK(report.holds("GIT_SSH_COMMAND=ssh -i /keys/agents"));
    CHECK(report.holds("GIT_CONFIG_COUNT=3"));
    CHECK(report.holds("GIT_CONFIG_KEY_0=user.name"));
    CHECK(report.holds("GIT_CONFIG_VALUE_0=Agent User"));
    CHECK(report.holds("GIT_CONFIG_KEY_1=push.default"));
    CHECK(report.holds("GIT_CONFIG_VALUE_1=upstream"));
    CHECK(report.holds("GIT_CONFIG_KEY_2=push.autoSetupRemote"));
    CHECK(report.holds("GIT_CONFIG_VALUE_2=true"));
}

/// Selects the row @p downs below the first, and opens the dispatch form on it.
/// @return The dashboard's screen once the form shows.
[[nodiscard]] std::string openForm(Desk& desk, std::size_t downs)
{
    desk.type(Home);
    for ([[maybe_unused]] auto const _: std::views::iota(std::size_t { 0 }, downs))
        desk.type(Down);
    desk.type(Enter);
    auto page = std::string {};
    auto const shown = desk.until([&] {
        page = desk.page();
        return page.contains("enter dispatch");
    });
    INFO("dashboard: " << page);
    REQUIRE(shown);
    return page;
}

/// Waits until the run whose id begins with @p prefix runs in its agent's tab, the agent started.
/// @return The run and its agent's session.
[[nodiscard]] Dispatched awaitRunning(Desk& desk,
                                      LiveHost& live,
                                      OrchestratorWorld& world,
                                      std::string_view prefix)
{
    auto dispatched = Dispatched {};
    auto const running = desk.until([&] {
        auto run = storedRun(world, prefix);
        if (!run || run->state != "running" || !run->pane)
            return false;
        auto const agent = sessionOfPane(live.host, *run->pane);
        if (!agent || !live.pageText(*agent).contains("agent in"))
            return false;
        auto report = reportOf(world, run->session);
        if (!report)
            return false;
        dispatched.run = *std::move(run);
        dispatched.agent = *agent;
        dispatched.report = *std::move(report);
        return true;
    });
    INFO("dashboard: " << desk.page());
    INFO(runsOf(world));
    REQUIRE(running);
    checkAgentStart(dispatched.report, dispatched.run);
    return dispatched;
}

/// Selects the row @p downs below the first, dispatches on it with the task Enter offers, and waits until the
/// run whose id begins with @p prefix runs in its agent's tab.
/// @return The run and its agent's session.
[[nodiscard]] Dispatched dispatch(
    Desk& desk, LiveHost& live, OrchestratorWorld& world, std::size_t downs, std::string_view prefix)
{
    auto const form = openForm(desk, downs);
    desk.type(Enter);
    auto dispatched = awaitRunning(desk, live, world, prefix);
    dispatched.formPage = form;
    return dispatched;
}

/// The selectable rows above the runs': the fixture's five pull requests and five issues without one.
constexpr auto RowsAboveRuns = std::size_t { 10 };

/// Selects the @p index-th run's row -- AGENTS' first, then FINISHED's -- which shows @p run, and waits until
/// its details do.
void selectRun(Desk& desk, std::size_t index, std::string_view run)
{
    desk.type(Home);
    for ([[maybe_unused]] auto const _: std::views::iota(std::size_t { 0 }, RowsAboveRuns + index))
        desk.type(Down);
    auto const shown = desk.until([&] { return desk.page().contains(run); });
    INFO("dashboard: " << desk.page());
    REQUIRE(shown);
}

/// Has @p agent report @p state, as Claude Code does on its terminal (OSC 7501): the fake agent echoes it.
void report(LiveHost& live, SessionId agent, std::string_view state)
{
    live.host.writeInput(agent, std::format("\033]7501;state={}\033\\", state));
}

/// @return Whether the run whose id begins with @p prefix last reported @p status.
[[nodiscard]] bool reported(OrchestratorWorld const& world, std::string_view prefix, std::string_view status)
{
    auto const run = storedRun(world, prefix);
    return run && run->lastStatus == status;
}

/// @return Whether the run whose id begins with @p prefix finished as @p ended, its worktree's fate @p fate.
[[nodiscard]] bool finishedAs(OrchestratorWorld const& world,
                              std::string_view prefix,
                              std::string_view ended,
                              std::string_view fate)
{
    auto const run = storedRun(world, prefix);
    return run && run->state == "finished" && run->finished && run->finished->ended == ended
           && run->finished->fate == fate;
}

/// @return The tag of the agent's pane @p pane, as @p host's orchestrator lists it; empty if it is not
/// listed.
[[nodiscard]] std::string tagOf(SessionHost& host, std::string_view pane)
{
    auto const listed = host.ownedPanes().list(OrchestratorName);
    if (!listed)
        return {};
    auto const found = std::ranges::find(*listed, pane, &OwnedPaneInfo::pane);
    return found != listed->end() ? found->tag : std::string {};
}

/// @return The value @p exec's environment gives @p name, if it gives one.
[[nodiscard]] std::optional<std::string> envOf(vtpty::Process::ExecInfo const& exec, std::string const& name)
{
    auto const found = exec.env.find(name);
    return found != exec.env.end() ? std::optional { found->second } : std::nullopt;
}

/// @return The removal checks, in the order R11 runs them, on a clean worktree.
[[nodiscard]] std::vector<std::vector<std::string_view>> cleanRemovalCommands()
{
    return {
        { "git", "rev-parse", "--path-format=absolute", "--git-common-dir" },
        { "git", "ls-files", "-v", "--stage" },
        { "git", "for-each-ref", "--count=1", "--contains", "HEAD" },
        { "git", "-c", "status.displayCommentPrefix=false", "-c", "color.status=false", "status", "--long" },
        { "git", "status", "--porcelain" },
        { "git", "worktree", "remove" },
    };
}

/// A daemon serving the orchestrator, and the shells the user works in beside it.
struct AgentDaemon
{
    OrchestratorWorld world { VTHOST_TEST_ORCHESTRATOR_WASM, VTHOST_TEST_ORCHESTRATOR_TESTDATA };
    std::unique_ptr<LiveHost> live = std::make_unique<LiveHost>(world.ptys(), world.hostConfig());
    std::vector<SessionId> shells = vthost::testing::spawnShells(*live);
};

/// Checks that @p next carries the agent of @p before on: its pane's UUID and tag, its program answering; the
/// dashboard gone, and its run still Running with nothing removed -- also once a window opened the dashboard
/// again.
void checkAgentCarried(LiveHost& next, OrchestratorWorld& world, Dispatched const& before)
{
    auto const agent = sessionOfPane(next.host, *before.run.pane);
    REQUIRE(agent.has_value());
    CHECK(*agent == before.agent);
    REQUIRE(next.eventually([&] { return tagOf(next.host, *before.run.pane) == before.run.id; }));
    CHECK(std::ranges::none_of(next.host.sessionIds(),
                               [&](SessionId id) { return next.host.servedBy(id) != nullptr; }));
    next.host.writeInput(*agent, "still-here");
    REQUIRE(next.eventually([&] { return next.pageText(*agent).contains("still-here"); }));

    // The new guest listed the pane and kept the run: a second later it still runs, nothing removed.
    auto const reopened = vthost::testing::openDashboard(next, world);
    CHECK(reopened.questions == 0);
    REQUIRE(next.eventually([&] { return next.pageText(*reopened.session).contains("agents 1/4"); }));
    auto const settled = std::chrono::steady_clock::now() + 1s;
    std::ignore = turnUntil(next, world, [&] { return std::chrono::steady_clock::now() >= settled; });
    auto const run = storedRun(world, before.run.id);
    REQUIRE(run.has_value());
    CHECK(run->state == "running");
    CHECK(run->pane == before.run.pane);
    CHECK(world.hasWorktree(before.run.worktree));
    CHECK(std::ranges::none_of(runCommands(world), [](auto const& argv) {
        return beginsWith(argv, { "git", "worktree", "remove" });
    }));
}

} // namespace

TEST_CASE("dispatching a PR opens its agent tab beside the dashboard", "[daemon][e2e][agents]")
{
    auto d = AgentDaemon {};
    auto desk = Desk { *d.live, d.world };

    auto const dispatched = dispatch(desk, *d.live, d.world, 0, "pr-2375-");
    auto const& run = dispatched.run;
    INFO(runsOf(d.world));

    // Fetched, added detached, checked out as the run's branch -- in that order -- and the agent opened.
    CHECK(ranInOrder(runCommands(d.world),
                     { { "git", "fetch", "--quiet", "origin" },
                       { "git", "worktree", "add", "--detach", run.worktree, "origin/master" },
                       { "gh", "pr", "checkout", "2375", "--branch", std::format("agent/{}", run.id) } }));
    CHECK(run.worktree == std::format("{}/contour/{}", d.world.worktreeRoot(), run.id));
    CHECK(tagOf(d.live->host, *run.pane) == run.id);

    // Its tab opened beside the dashboard, which stayed the active tab.
    auto const [dashboardWorkspace, dashboardTab, dashboardLeaf] =
        d.live->host.model().findSessionLeaf(desk.dashboard);
    auto const [agentWorkspace, agentTab, agentLeaf] = d.live->host.model().findSessionLeaf(dispatched.agent);
    REQUIRE(dashboardWorkspace != nullptr);
    CHECK(agentWorkspace == dashboardWorkspace);
    CHECK(agentTab != dashboardTab);
    CHECK(dashboardWorkspace->activeTab() == dashboardTab);

    // It runs what the form showed, in the run's worktree, entered by its handle, with the repository's
    // choosers withheld and git's push settings appended to the user's own.
    auto const spawned = d.world.spawnedAgents();
    REQUIRE(spawned.size() == 1);
    auto const& exec = spawned.front();
    CHECK(exec.program == "claude");
    // The system prompt's addition asks for the status board the dashboard shows.
    REQUIRE(exec.arguments.size() == 7);
    CHECK(std::vector(exec.arguments.begin(), exec.arguments.begin() + 5)
          == std::vector<std::string> {
              "--model", "sonnet", "--session-id", run.session, "--append-system-prompt" });
    CHECK(exec.arguments[5].contains("STATUS.md"));
    CHECK(exec.arguments[6] == FixCiPrompt);
    CHECK(dispatched.formPage.contains("claude --model sonnet"));
    CHECK(exec.workingDirectory == run.worktree);
    REQUIRE(exec.directory != nullptr);
    CHECK(exec.directory->resolvedPath() == run.worktree);
    CHECK(envOf(exec, "GIT_CONFIG_COUNT") == "3");
    CHECK(envOf(exec, "GIT_CONFIG_KEY_1") == "push.default");
    CHECK(envOf(exec, "GIT_CONFIG_VALUE_1") == "upstream");
    CHECK(envOf(exec, "GIT_CONFIG_KEY_2") == "push.autoSetupRemote");
    CHECK(envOf(exec, "GIT_CONFIG_VALUE_2") == "true");
    CHECK(std::ranges::contains(exec.removedEnvironment, "GIT_DIR"));
    CHECK(std::ranges::contains(exec.removedEnvironment, "GH_REPO"));
    CHECK_FALSE(std::ranges::contains(exec.removedEnvironment, "GIT_SSH_COMMAND"));
    CHECK_FALSE(std::ranges::contains(exec.removedEnvironment, "GIT_CONFIG_KEY_0"));

    // Enter on the item now reveals its agent, to the window the dashboard was opened from -- not to another
    // window attached meanwhile.
    auto other = Window { *d.live, d.world };
    desk.type(Enter);
    REQUIRE(desk.until([&] { return desk.revealsOf(dispatched.agent) == 1; }));
    auto const settled = std::chrono::steady_clock::now() + 500ms;
    std::ignore = desk.until([&] { return std::chrono::steady_clock::now() >= settled; });
    CHECK(desk.revealsOf(dispatched.agent) == 1);
    CHECK(other.revealsOf(dispatched.agent) == 0);
    CHECK(d.world.spawnedAgents().size() == 1);
}

TEST_CASE("dispatching an issue uses a new branch", "[daemon][e2e][agents]")
{
    auto d = AgentDaemon {};
    auto desk = Desk { *d.live, d.world };

    // The first issue is below the five pull requests.
    auto const dispatched = dispatch(desk, *d.live, d.world, 5, "issue-2356-");
    auto const& run = dispatched.run;
    INFO(runsOf(d.world));
    CHECK(ranInOrder(runCommands(d.world),
                     { { "git", "fetch", "--quiet", "origin" },
                       { "git",
                         "worktree",
                         "add",
                         "--no-track",
                         "-b",
                         std::format("agent/{}", run.id),
                         run.worktree,
                         "origin/master" } }));
    CHECK(std::ranges::none_of(
        runCommands(d.world), [](auto const& argv) { return beginsWith(argv, { "gh", "pr", "checkout" }); }));
    auto const spawned = d.world.spawnedAgents();
    REQUIRE(spawned.size() == 1);
    auto const& arguments = spawned.front().arguments;
    REQUIRE(arguments.size() == 7);
    CHECK(std::vector(arguments.begin(), arguments.begin() + 5)
          == std::vector<std::string> {
              "--model", "opus", "--session-id", run.session, "--append-system-prompt" });
    CHECK(arguments[6]
          == "/contour-workflows:work-issue https://github.com/contour-terminal/contour/issues/2356");
}

TEST_CASE("the agent's tab closing removes the worktree", "[daemon][e2e][agents]")
{
    auto d = AgentDaemon {};
    auto desk = Desk { *d.live, d.world };
    auto const dispatched = dispatch(desk, *d.live, d.world, 0, "pr-2375-");
    report(*d.live, dispatched.agent, "working");
    REQUIRE(desk.until([&] { return reported(d.world, "pr-2375-", "working"); }));
    auto const before = runCommands(d.world).size();

    d.live->host.closeSession(dispatched.agent);
    REQUIRE(desk.until([&] { return finishedAs(d.world, "pr-2375-", "completed", "removed"); }));
    INFO(runsOf(d.world));
    auto const commands = runCommands(d.world);
    auto const cleanup = std::vector(commands.begin() + static_cast<std::ptrdiff_t>(before), commands.end());
    CHECK(cleanup.size() == cleanRemovalCommands().size());
    CHECK(ranInOrder(cleanup, cleanRemovalCommands()));
    CHECK(cleanup.back().back() == dispatched.run.worktree);
    CHECK_FALSE(d.world.hasWorktree(dispatched.run.worktree));
}

TEST_CASE("a kept worktree is checked again on the next poll", "[daemon][e2e][agents]")
{
    auto d = AgentDaemon {};
    auto desk = Desk { *d.live, d.world };
    auto const dispatched = dispatch(desk, *d.live, d.world, 0, "pr-2375-");
    report(*d.live, dispatched.agent, "working");
    REQUIRE(desk.until([&] { return reported(d.world, "pr-2375-", "working"); }));

    // Once, git says a file was changed: the worktree is kept, and the change named.
    d.world.processRunner.script({ .prefix = { "git", "status", "--porcelain" },
                                   .standardOutput = " M a.txt\n",
                                   .exitCode = 0,
                                   .effect = {},
                                   .times = 1 });
    d.live->host.closeSession(dispatched.agent);
    REQUIRE(desk.until([&] { return finishedAs(d.world, "pr-2375-", "completed", "kept"); }));
    auto const kept = storedRun(d.world, "pr-2375-");
    REQUIRE(kept.has_value());
    CHECK(kept->finished->entries == std::vector<std::string> { "M a.txt" });
    CHECK(d.world.hasWorktree(dispatched.run.worktree));

    // The next poll checks it again; committed and pushed meanwhile, it is removed.
    desk.type("R");
    REQUIRE(desk.until([&] { return finishedAs(d.world, "pr-2375-", "completed", "removed"); }));
    CHECK_FALSE(d.world.hasWorktree(dispatched.run.worktree));
}

TEST_CASE("e on a Done run ends it", "[daemon][e2e][agents]")
{
    auto d = AgentDaemon {};
    auto desk = Desk { *d.live, d.world };
    auto const dispatched = dispatch(desk, *d.live, d.world, 0, "pr-2375-");
    report(*d.live, dispatched.agent, "done");
    REQUIRE(desk.until([&] { return runIs(d.world, "pr-2375-", "done"); }));

    // The first `e` on its row asks; nothing ends yet.
    selectRun(desk, 0, dispatched.run.id);
    desk.type("e");
    auto const asked = desk.until([&] { return desk.page().contains("e to confirm"); });
    INFO("dashboard: " << desk.page());
    REQUIRE(asked);
    CHECK(runIs(d.world, "pr-2375-", "done"));
    CHECK(sessionOfPane(d.live->host, *dispatched.run.pane).has_value());

    // The second ends it: the tab is closed, and the worktree removed.
    desk.type("e");
    REQUIRE(desk.until([&] { return finishedAs(d.world, "pr-2375-", "ended-by-you", "removed"); }));
    CHECK_FALSE(sessionOfPane(d.live->host, *dispatched.run.pane).has_value());
    CHECK_FALSE(d.world.hasWorktree(dispatched.run.worktree));
}

TEST_CASE("a live upgrade mid-run keeps the agent pane", "[daemon][e2e][agents][upgrade]")
{
    auto world = OrchestratorWorld { VTHOST_TEST_ORCHESTRATOR_WASM, VTHOST_TEST_ORCHESTRATOR_TESTDATA };
    auto u = vthost::testing::ServedUpgrade { world.ptys(), world.hostConfig() };
    std::ignore = vthost::testing::spawnShells(*u.live);
    auto dispatched = Dispatched {};
    auto dashboard = SessionId {};
    {
        auto desk = Desk { *u.live, world };
        dispatched = dispatch(desk, *u.live, world, 0, "pr-2375-");
        report(*u.live, dispatched.agent, "working");
        REQUIRE(desk.until([&] { return reported(world, "pr-2375-", "working"); }));
        dashboard = desk.dashboard;
        desk.leave();
    }

    auto image = vthost::testing::upgradeWithDashboard(u, dashboard);
    CHECK(std::ranges::contains(image.sessions, dispatched.agent, &handover::HandoverSession::id));
    u.acceptor.reset();
    u.live.reset();

    auto const next = vthost::testing::takeOver(std::move(image), world);
    checkAgentCarried(*next, world, dispatched);
}

TEST_CASE("a keeper recovery mid-run keeps the agent pane", "[daemon][e2e][agents][keeper]")
{
    auto d = AgentDaemon {};
    auto dispatched = Dispatched {};
    auto dashboard = SessionId {};
    {
        auto desk = Desk { *d.live, d.world };
        dispatched = dispatch(desk, *d.live, d.world, 0, "pr-2375-");
        report(*d.live, dispatched.agent, "working");
        REQUIRE(desk.until([&] { return reported(d.world, "pr-2375-", "working"); }));
        dashboard = desk.dashboard;
        desk.leave();
    }
    // The registry was written well before the daemon died.
    d.live->host.flushOwnedPanes();

    auto kept = d.shells;
    kept.push_back(dispatched.agent);
    auto image = vthost::testing::recoverFromKeeper(d.live, kept, dashboard);
    auto const next = vthost::testing::takeOver(std::move(image), d.world);
    checkAgentCarried(*next, d.world, dispatched);
}

TEST_CASE("a cold restart ends the run through the removal checks", "[daemon][e2e][agents][restore]")
{
    auto d = AgentDaemon {};
    auto dispatched = Dispatched {};
    {
        auto desk = Desk { *d.live, d.world };
        dispatched = dispatch(desk, *d.live, d.world, 0, "pr-2375-");
        report(*d.live, dispatched.agent, "working");
        REQUIRE(desk.until([&] { return reported(d.world, "pr-2375-", "working"); }));
        desk.leave();
    }
    d.live->host.flushOwnedPanes();
    auto snapshot = captureWorkspace(d.live->host, CaptureOptions {}, std::chrono::sys_seconds {});
    d.live.reset();

    // The snapshot left the agent out: the restored daemon has no pane for the run, which ends -- completed,
    // as its agent had reported -- and whose worktree goes through the removal checks.
    auto config = d.world.hostConfig();
    config.restore = WorkspaceRestore { .snapshot = std::move(snapshot) };
    auto next = LiveHost { d.world.ptys(), std::move(config) };
    REQUIRE(
        turnUntil(next, d.world, [&] { return finishedAs(d.world, "pr-2375-", "completed", "removed"); }));
    CHECK_FALSE(sessionOfPane(next.host, *dispatched.run.pane).has_value());
    CHECK(next.host.sessionCount() == d.shells.size());
    CHECK_FALSE(d.world.hasWorktree(dispatched.run.worktree));
}

TEST_CASE("a guest fault mid-dispatch adopts the pane by tag", "[daemon][e2e][agents]")
{
    auto d = AgentDaemon {};
    auto desk = Desk { *d.live, d.world };
    std::ignore = openForm(desk, 0);

    // The host opens the pane, but the guest hears of it only after its deadline: it faults before it could
    // store that its run runs. The guest that replaces it finds the pane by its tag.
    d.world.stallNextPaneOpen();
    desk.type(Enter);
    auto const dispatched = awaitRunning(desk, *d.live, d.world, "pr-2375-");
    auto const hosting = d.world.world();
    REQUIRE(hosting != nullptr);
    auto const status = hosting->status(OrchestratorName);
    REQUIRE(status.has_value());
    CHECK_FALSE(status->lastFault.empty());
    CHECK(d.world.spawnedAgents().size() == 1);
    CHECK(tagOf(d.live->host, *dispatched.run.pane) == dispatched.run.id);
}

TEST_CASE("the same item twice is refused", "[daemon][e2e][agents]")
{
    auto d = AgentDaemon {};
    auto desk = Desk { *d.live, d.world };
    std::ignore = dispatch(desk, *d.live, d.world, 0, "pr-2375-");
    auto const fetches = d.world.processRunner.startsOf("git", "fetch");

    // `d` opens the form on the item all the same; it says why, and Enter dispatches nothing.
    desk.type(Home);
    desk.type("d");
    REQUIRE(desk.until([&] { return desk.page().contains("already"); }));
    desk.type(Enter);
    REQUIRE(desk.until([&] { return d.world.isRunning() && d.world.pollDone(); }));
    CHECK(d.world.processRunner.startsOf("git", "fetch") == fetches);
    CHECK(d.world.storedRuns().size() == 1);
    CHECK(d.world.spawnedAgents().size() == 1);
}

namespace
{

/// The board an agent wrote, as the cases have it write one.
constexpr auto AgentBoard =
    std::string_view { "# Status\n\n- reproduced the failure\n- fixing the reflow\n" };

/// @return The contract's first page of the recorded transcript, as `contour agent transcript --json` prints
///         it: what the orchestrator reads.
[[nodiscard]] std::string contractPage()
{
    auto const contract =
        std::filesystem::path { VTHOST_TEST_ORCHESTRATOR_TESTDATA }.parent_path().parent_path()
        / "contract/agent-transcript/format-1/basic-page-1.json";
    return vthost::testing::wholeFile(contract);
}

/// Writes @p text as the `STATUS.md` of the worktree @p worktree, written @p age before @p world's wall clock
/// says it is now.
void writeBoard(OrchestratorWorld const& world,
                std::string const& worktree,
                std::string_view text,
                std::chrono::system_clock::duration age)
{
    auto const path = std::filesystem::path { worktree } / "STATUS.md";
    {
        auto file = std::ofstream { path, std::ios::binary };
        file << text;
    }
    // The wall-clock instant as a file time, by its distance from now: std::chrono::clock_cast, which would
    // say it directly, is missing from libc++ 19 (AppleClang 17's).
    auto const distance = (world.wallClock.now() - age) - std::chrono::system_clock::now();
    std::filesystem::last_write_time(
        path,
        std::filesystem::file_time_type::clock::now()
            + std::chrono::duration_cast<std::filesystem::file_time_type::duration>(distance));
}

/// Dispatches on the first pull request, then closes its agent's tab: the run finishes, its worktree removed.
/// @return The run as it was while it ran.
[[nodiscard]] StoredRun finishedRun(Desk& desk, AgentDaemon& d)
{
    auto const dispatched = dispatch(desk, *d.live, d.world, 0, "pr-2375-");
    report(*d.live, dispatched.agent, "working");
    REQUIRE(desk.until([&] { return reported(d.world, "pr-2375-", "working"); }));
    d.live->host.closeSession(dispatched.agent);
    REQUIRE(desk.until([&] { return finishedAs(d.world, "pr-2375-", "completed", "removed"); }));
    REQUIRE_FALSE(d.world.hasWorktree(dispatched.run.worktree));
    return dispatched.run;
}

/// @return The argv of every `contour agent transcript` @p world's orchestrator ran, with where it ran.
[[nodiscard]] std::vector<std::pair<std::vector<std::string>, std::string>> transcriptReads(
    OrchestratorWorld const& world)
{
    auto reads = std::vector<std::pair<std::vector<std::string>, std::string>> {};
    auto const started = world.processRunner.started();
    auto const startedIn = world.processRunner.startedIn();
    for (auto const index: std::views::iota(std::size_t { 0 }, started.size()))
        if (beginsWith(started[index], { "contour", "agent", "transcript" }))
            reads.emplace_back(started[index], startedIn[index]);
    return reads;
}

} // namespace

TEST_CASE("b shows the board an agent wrote, with its age", "[daemon][e2e][agents]")
{
    auto d = AgentDaemon {};
    auto desk = Desk { *d.live, d.world };
    auto const dispatched = dispatch(desk, *d.live, d.world, 0, "pr-2375-");
    writeBoard(d.world, dispatched.run.worktree, AgentBoard, std::chrono::minutes { 3 });

    selectRun(desk, 0, dispatched.run.id);
    desk.type("b");
    auto page = std::string {};
    auto const shown = desk.until([&] {
        page = desk.page();
        return page.contains("fixing the reflow");
    });
    INFO("dashboard: " << page);
    REQUIRE(shown);
    CHECK(page.contains(std::format("STATUS.md · {}", dispatched.run.id)));
    CHECK(page.contains("updated 3 min ago"));
    CHECK(page.contains("t transcript"));
    CHECK(page.contains("d new run"));
}

TEST_CASE("a board that appears later is picked up while shown", "[daemon][e2e][agents]")
{
    auto d = AgentDaemon {};
    auto desk = Desk { *d.live, d.world };
    auto const dispatched = dispatch(desk, *d.live, d.world, 0, "pr-2375-");

    selectRun(desk, 0, dispatched.run.id);
    desk.type("b");
    REQUIRE(desk.until([&] { return desk.page().contains("No STATUS.md to show yet"); }));

    // Written while the board is shown: the next read, within its period, shows it.
    writeBoard(d.world, dispatched.run.worktree, AgentBoard, std::chrono::seconds { 0 });
    auto page = std::string {};
    auto const shown = desk.until([&] {
        page = desk.page();
        return page.contains("fixing the reflow");
    });
    INFO("dashboard: " << page);
    REQUIRE(shown);
    CHECK(page.contains("updated just now"));
}

TEST_CASE("t shows a finished run's transcript after its worktree is gone", "[daemon][e2e][agents]")
{
    auto d = AgentDaemon {};
    auto desk = Desk { *d.live, d.world };
    d.world.processRunner.script({ .prefix = { "contour", "agent", "transcript" },
                                   .standardOutput = contractPage(),
                                   .exitCode = 0,
                                   .effect = {},
                                   .times = std::nullopt });
    auto const run = finishedRun(desk, d);

    // The finished run is the first of the runs' rows.
    selectRun(desk, 0, run.id);
    desk.type("t");
    auto page = std::string {};
    auto const shown = desk.until([&] {
        page = desk.page();
        return page.contains("written by Claude Code 2.1.328");
    });
    INFO("dashboard: " << page);
    INFO(runsOf(d.world));
    REQUIRE(shown);
    CHECK(page.contains(std::format("Transcript · {} · page 1 of 1", run.id)));
    CHECK(page.contains("tool Bash"));

    // Read through the declared command alone, for the run's session and worktree, in the worktree root: the
    // run's own worktree is gone.
    auto const reads = transcriptReads(d.world);
    REQUIRE(reads.size() == 1);
    CHECK(reads.front().first
          == std::vector<std::string> { "contour",
                                        "agent",
                                        "transcript",
                                        "--session",
                                        run.session,
                                        "--cwd",
                                        run.worktree,
                                        "--page",
                                        "1",
                                        "--format",
                                        "1",
                                        "--json" });
    CHECK(reads.front().second == d.world.worktreeRoot());
}

TEST_CASE("d on a finished run dispatches the same item again", "[daemon][e2e][agents]")
{
    auto d = AgentDaemon {};
    auto desk = Desk { *d.live, d.world };
    // The world's clock stands still: the new run is dispatched in the same second as the finished one.
    auto const first = finishedRun(desk, d);

    selectRun(desk, 0, first.id);
    desk.type("d");
    auto page = std::string {};
    auto const opened = desk.until([&] {
        page = desk.page();
        return page.contains("enter dispatch");
    });
    INFO("dashboard: " << page);
    REQUIRE(opened);
    CHECK(page.contains("#2375"));
    CHECK(page.contains("fix CI"));
    CHECK(d.world.storedRuns().size() == 1);

    // Confirmed, it dispatches on the same item, as a run of its own.
    desk.type(Enter);
    auto second = std::string {};
    REQUIRE(desk.until([&] {
        for (auto const& id: d.world.storedRuns())
            if (id.starts_with("pr-2375-") && id != first.id)
                second = id;
        return !second.empty();
    }));
    CHECK(second == std::format("{}-2", first.id));
    auto const dispatched = awaitRunning(desk, *d.live, d.world, second);
    CHECK(dispatched.run.worktree != first.worktree);
    CHECK(d.world.spawnedAgents().size() == 2);
    CHECK(d.world.spawnedAgents().back().arguments.back() == FixCiPrompt);
}

TEST_CASE("an agent that cannot start in its worktree here says so", "[daemon][e2e][agents]")
{
    // A Flatpak whose programs run on the host, or Windows: no program can be started in a directory held
    // open. The run fails at its pane, by that name -- not as an agent that never reported.
    auto world = OrchestratorWorld { VTHOST_TEST_ORCHESTRATOR_WASM, VTHOST_TEST_ORCHESTRATOR_TESTDATA };
    auto config = world.hostConfig();
    config.heldDirectorySpawn = [](SandboxEscape) {
        return vtpty::HeldDirectorySpawn::Unsupported;
    };
    auto live = LiveHost { world.ptys(), std::move(config) };
    std::ignore = vthost::testing::spawnShells(live);
    auto desk = Desk { live, world };

    std::ignore = openForm(desk, 0);
    desk.type(Enter);
    auto const failed =
        desk.until([&] { return finishedAs(world, "pr-2375-", "preparation-failed", "removed"); });
    INFO(runsOf(world));
    INFO("dashboard: " << desk.page());
    REQUIRE(failed);
    auto const run = storedRun(world, "pr-2375-");
    REQUIRE(run.has_value());
    REQUIRE(run->failure.has_value());
    CHECK(run->failure->step == "open-pane");
    CHECK(run->failure->why == "not-supported-yet");
    CHECK(world.spawnedAgents().empty());

    // Its row in FINISHED says so, in plain words, and keeps the code for diagnosis. Tab shows the details on
    // the whole page: below the list they are cut before the failure line.
    selectRun(desk, 0, run->id);
    desk.type(Tab);
    auto const said = desk.until([&] {
        auto const page = desk.page();
        return page.contains("preparation failed") && page.contains("agents can't start here yet")
               && page.contains("not-supported-yet");
    });
    INFO("dashboard: " << desk.page());
    CHECK(said);
}

TEST_CASE("ending runs keeps a worktree with work and removes a clean one, against real git",
          "[daemon][e2e][agents][git]")
{
    auto sandbox = vtextension::testing::GitSandbox::create(core::defaultEnvironment());
    if (!sandbox && sandbox.error() == vtextension::testing::GitSandboxError::GitNotFound)
        SKIP("no git on PATH");
    REQUIRE(sandbox.has_value());
    auto const& git = **sandbox;
    INFO(git.gitVersion());
    auto const repository = git.repository();
    // The pull requests' base, fetched from a remote that is the repository itself.
    for (auto const& argv:
         { std::vector<std::string> { "git", "branch", "master", "main" },
           std::vector<std::string> { "git", "remote", "add", "origin", repository.string() } })
        REQUIRE(git.runGit(repository, argv).exitCode == 0);

    auto world = OrchestratorWorld { VTHOST_TEST_ORCHESTRATOR_WASM,
                                     VTHOST_TEST_ORCHESTRATOR_TESTDATA,
                                     vthost::testing::RealGit { .repository = repository,
                                                                .worktreeRoot = git.worktreeRoot(),
                                                                .runner = git.runner(),
                                                                .environment = git.environment() } };
    auto live = LiveHost { world.ptys(), world.hostConfig() };
    std::ignore = vthost::testing::spawnShells(live);
    auto desk = Desk { live, world };

    // Three pull requests, one after the other: each agent starts in its own worktree, entered by its
    // handle. The first leaves it clean; the second leaves a file nobody committed, the third a commit nobody
    // pushed. Each says it is done, and its run is ended with `e`, asked once.
    auto const prefixes = std::array { std::string_view { "pr-2375-" },
                                       std::string_view { "pr-2369-" },
                                       std::string_view { "pr-2346-" } };
    auto const work = std::array<std::function<void(std::filesystem::path const&)>, 3> {
        [](std::filesystem::path const&) {},
        [](std::filesystem::path const& worktree) {
            vtextension::testing::GitSandbox::writeFile(worktree / "notes.txt", "draft\n");
        },
        [&git](std::filesystem::path const& worktree) {
            REQUIRE(git.runGit(worktree, { "git", "commit", "-q", "--allow-empty", "-m", "work" }).exitCode
                    == 0);
        },
    };
    auto runs = std::vector<Dispatched> {};
    for (auto const [index, prefix]: core::views::enumerate(prefixes))
    {
        auto const& dispatched =
            runs.emplace_back(dispatch(desk, live, world, static_cast<std::size_t>(index), prefix));
        CHECK(live.pageText(dispatched.agent).contains(std::format("agent in {} ready", dispatched.run.id)));
        CHECK(std::filesystem::is_regular_file(std::filesystem::path(dispatched.run.worktree) / ".git"));
        work[static_cast<std::size_t>(index)](dispatched.run.worktree);

        report(live, dispatched.agent, "done");
        REQUIRE(desk.until([&] { return runIs(world, dispatched.run.id, "done"); }));
        selectRun(desk, 0, dispatched.run.id);
        desk.type("e");
        REQUIRE(desk.until([&] { return desk.page().contains("e to confirm"); }));
        desk.type("e");
        REQUIRE(desk.until([&] { return runIs(world, dispatched.run.id, "finished"); }));
    }
    auto const& untracked = runs[1].run.worktree;
    auto const& unpushed = runs[2].run.worktree;
    INFO(runsOf(world));

    auto const clean = storedRun(world, runs[0].run.id);
    REQUIRE(clean.has_value());
    CHECK(clean->finished->ended == "ended-by-you");
    CHECK(clean->finished->fate == "removed");
    CHECK_FALSE(std::filesystem::exists(runs[0].run.worktree));

    auto const withFile = storedRun(world, runs[1].run.id);
    REQUIRE(withFile.has_value());
    CHECK(withFile->finished->fate == "kept");
    CHECK(withFile->finished->entries == std::vector<std::string> { "?? notes.txt" });
    CHECK(std::filesystem::exists(std::filesystem::path(untracked) / "notes.txt"));

    auto const withCommit = storedRun(world, runs[2].run.id);
    REQUIRE(withCommit.has_value());
    CHECK(withCommit->finished->fate == "kept");
    CHECK(withCommit->finished->entries == std::vector<std::string> { "HEAD is not contained in any ref" });
    CHECK(git.runGit(unpushed, { "git", "log", "-1", "--format=%s" }).standardOutput == "work\n");

    // Git agrees: the clean worktree is gone from its list, the other two are still there.
    auto const listed = git.runGit(repository, { "git", "worktree", "list", "--porcelain" }).standardOutput;
    CHECK_FALSE(listed.contains(runs[0].run.worktree));
    CHECK(listed.contains(untracked));
    CHECK(listed.contains(unpushed));
    // Every git it forwarded ended, and none is remembered.
    CHECK(world.processRunner.forwardedRunning() == 0);
}
