// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/state/TerminalStateCodec.hpp>

#include <vtpty/test/HandleTesting.hpp>

#include <core/async/testing/ManualExecutor.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#ifndef _WIN32
    #include <vtpty/Process.hpp>
    #include <vtpty/UnixPty.hpp>

    #include <sys/socket.h>
    #include <sys/wait.h>

    #include <cerrno>
    #include <csignal>

    #include <fcntl.h>
    #include <spawn.h>
    #include <unistd.h>

    #ifdef __APPLE__
        #include <util.h>
    #elifdef __FreeBSD__
        #include <libutil.h>
    #else
        #include <pty.h>
    #endif
#else
    #include <Windows.h>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vthost/SessionHost.hpp>
#include <vthost/WorkspaceGit.hpp>
#include <vthost/handover/HandoverRestore.hpp>
#include <vthost/testing/FakeProfileCatalog.hpp>
#include <vthost/testing/HandoverTesting.hpp>
#include <vthost/testing/TestHelpers.hpp>

using namespace vthost;
using namespace vthost::handover;
using vtpty::testing::isOpen;
using vtworkspace::SessionId;

#ifndef _WIN32
TEST_CASE("the descriptors a handover keeps are the standard streams and every one the image names",
          "[vthost][handover]")
{
    auto image = HandoverImage {};
    image.sessions.resize(2);
    image.sessions[0].pty.masterFd = 9;
    image.sessions[0].pty.stdoutFastPipe = std::pair { 12, -1 };
    image.sessions[1].pty.masterFd = 7;
    image.listeners = { { .name = "native", .handle = vtpty::CrossingHandle { 5 } },
                        { .name = "control", .handle = vtpty::CrossingHandle { 7 } } };
    image.lease = vtpty::CrossingHandle { 3 };
    CHECK(descriptorsToKeep(image, {}) == std::vector { 0, 1, 2, 3, 5, 7, 9, 12 });
    CHECK(descriptorsToKeep(HandoverImage {}, {}) == std::vector { 0, 1, 2 });
}

TEST_CASE("a handover also keeps the PTY keeper's channel, which no image names", "[vthost][handover]")
{
    auto image = HandoverImage {};
    image.lease = vtpty::CrossingHandle { 5 };
    auto const channel = std::array { 3 };
    CHECK(descriptorsToKeep(image, channel) == std::vector { 0, 1, 2, 3, 5 });
    // Named twice, kept once.
    auto const twice = std::array { 5, 3 };
    CHECK(descriptorsToKeep(image, twice) == std::vector { 0, 1, 2, 3, 5 });
}

namespace
{

using vthost::testing::cellOf;
using vthost::testing::LiveHost;
using vthost::testing::occurrences;
using vthost::testing::releaseForAdoption;
using vthost::testing::shellProcess;

/// A Sixel image one cell wide and tall.
constexpr auto SixelImage = std::string_view { "\033Pq\"1;1;4;6#1;2;100;0;0#1~~~~\033\\" };

/// @return Whether @p session's main page shows a cell of an image.
[[nodiscard]] bool showsImage(LiveHost& live, SessionId session)
{
    auto& terminal = live.host.hostedSession(session)->terminal();
    auto const _ = std::lock_guard { terminal };
    auto const& screen = terminal.primaryScreen();
    auto const page = screen.pageSize();
    for (auto const line: std::views::iota(0, unbox(page.lines)))
        for (auto const column: std::views::iota(0, unbox(page.columns)))
            if (screen.at(vtbackend::LineOffset(line), vtbackend::ColumnOffset(column)).imageFragment())
                return true;
    return false;
}

/// @return Every session's child, as the image names it.
[[nodiscard]] std::map<uint64_t, int64_t> childrenOf(HandoverImage const& image)
{
    auto children = std::map<uint64_t, int64_t> {};
    for (auto const& session: image.sessions)
        children.emplace(session.id.value, session.child.pid);
    return children;
}

/// Makes the device of every session a test host spawns: the echoing shell.
[[nodiscard]] std::unique_ptr<vtpty::Pty> catDevice(vtpty::PageSize size)
{
    return shellProcess(size);
}

/// What the old image does up to the exec, in this process: quiesces and captures @p old, then lets go of
/// every PTY and child, and goes.
/// @param old The host handing over; destroyed.
/// @param beforeCapture Runs once every session is quiesced, before the capture.
/// @return The image.
[[nodiscard]] HandoverImage handOver(std::unique_ptr<LiveHost> old,
                                     std::function<void(LiveHost&)> const& beforeCapture = {})
{
    old->settle();
    if (beforeCapture)
        beforeCapture(*old);
    auto image = old->capture();
    REQUIRE(image.has_value());
    releaseForAdoption(old->host);
    old.reset();
    return *std::move(image);
}

/// @return A host started from @p image.
/// @param config How it is set up besides.
/// @param settings The host's own session settings.
[[nodiscard]] std::unique_ptr<LiveHost> takeOver(HandoverImage image,
                                                 SessionHostConfig config = {},
                                                 vtbackend::Settings settings = {})
{
    config.handover = std::move(image);
    return std::make_unique<LiveHost>(catDevice, std::move(config), std::move(settings));
}

/// @return A host with one echoing session, ready.
[[nodiscard]] std::unique_ptr<LiveHost> oneSession()
{
    auto live = std::make_unique<LiveHost>(catDevice);
    REQUIRE(live->host.createTab() != nullptr);
    auto const id = live->host.sessionIds().front();
    REQUIRE(live->eventually([&] { return live->pageText(id).contains("ready"); }));
    return live;
}

} // namespace

TEST_CASE("descriptors not kept are closed, and exactly those", "[vthost][handover]")
{
    // A few open descriptors, every other one kept. Run in a child, which the sweep leaves with nothing else.
    auto pipes = std::array<int, 6> {};
    REQUIRE(::pipe(pipes.data()) == 0);
    REQUIRE(::pipe(pipes.data() + 2) == 0);
    REQUIRE(::pipe(pipes.data() + 4) == 0);
    auto kept = std::vector { 0, 1, 2, pipes[0], pipes[2], pipes[4] };
    std::ranges::sort(kept);
    auto const highest = std::ranges::max(pipes) + 64;

    auto const child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        closeDescriptorsExcept(kept);
        auto const exact = std::ranges::all_of(std::views::iota(0, highest), [&](int fd) {
            return isOpen(fd) == std::ranges::binary_search(kept, fd);
        });
        ::_exit(exact ? 0 : 1);
    }
    auto status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    for (auto const fd: pipes)
        ::close(fd);
}

TEST_CASE("a handover image is read back from its memory, and its descriptor closed", "[vthost][handover]")
{
    auto const memory = makeNativeHandoverMemory();
    auto image = HandoverImage {};
    image.daemonInstance = 0xABCD;
    image.fromRelease = "0.7.1";
    image.lease = vtpty::CrossingHandle { 42 };

    auto const written = memory->write(encodeHandover(image));
    REQUIRE(written.has_value());
    auto const read = readHandover(*memory, *written);
    REQUIRE(read.has_value());
    CHECK(read->daemonInstance == 0xABCD);
    CHECK(read->fromRelease == "0.7.1");
    CHECK(read->lease == vtpty::CrossingHandle { 42 });
    CHECK_FALSE(isOpen(written->native()));

    // Bytes that are no image: refused, the descriptor closed all the same.
    auto const garbage = memory->write(std::vector<uint8_t> { 1, 2, 3, 4, 5 });
    REQUIRE(garbage.has_value());
    CHECK_FALSE(readHandover(*memory, *garbage).has_value());
    CHECK_FALSE(isOpen(garbage->native()));
}

TEST_CASE("an image that cannot be read closes the listening sockets and nothing else", "[vthost][handover]")
{
    auto pipe = std::array<int, 2> {};
    REQUIRE(::pipe(pipe.data()) == 0);
    auto const child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        // A listener as the old image left it, and a descriptor that is none: only the first goes.
        auto const listener = ::socket(AF_INET, SOCK_STREAM, 0);
        auto const bound = listener >= 0 && ::listen(listener, 1) == 0;
        auto const closed = closeListeningSockets();
        ::_exit(bound && closed == 1 && !isOpen(listener) && isOpen(pipe[0]) && isOpen(pipe[1]) ? 0 : 1);
    }
    auto status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    ::close(pipe[0]);
    ::close(pipe[1]);
}

TEST_CASE("a handover keeps every program, its input, its output and its screen",
          "[vthost][handover][threading]")
{
    // Two sessions side by side, and one in a tab of its own showing an image.
    auto old = std::make_unique<LiveHost>(catDevice);
    old->spawnTwo();
    REQUIRE(old->host.createTab() != nullptr);
    auto const ids = old->host.sessionIds();
    REQUIRE(ids.size() == 3);
    REQUIRE(old->eventually([&] {
        return std::ranges::all_of(ids, [&](SessionId id) { return old->pageText(id).contains("ready"); });
    }));
    {
        // What a client reports as it attaches: without it there are no cells to lay an image out on.
        auto& terminal = old->host.hostedSession(ids[2])->terminal();
        auto const _ = std::lock_guard { terminal };
        terminal.setCellPixelSize(vtbackend::ImageSize { vtbackend::Width(10), vtbackend::Height(20) });
    }
    old->host.writeInput(ids[2], SixelImage);
    REQUIRE(old->eventually([&] { return showsImage(*old, ids[2]); }));
    auto const uuids = std::ranges::to<std::vector>(
        ids | std::views::transform([&](SessionId id) { return old->host.objects().uuidOf(id); }));
    auto const modelIds = old->host.model().idState();
    auto const workspace = old->host.workspaceId();

    // Typed while quiesced: carried across, not lost.
    auto const image =
        handOver(std::move(old), [&](LiveHost& live) { live.host.writeInput(ids[0], "carried"); });
    auto const children = childrenOf(image);
    auto const live = takeOver(image);
    auto& host = live->host;

    CHECK(host.handoverOutcome().has_value());
    REQUIRE(host.sessionIds() == ids);
    CHECK(host.workspaceId() == workspace);
    CHECK(host.model().idState().nextPane == modelIds.nextPane);
    CHECK(host.model().idState().nextTab == modelIds.nextTab);
    CHECK(host.model().idState().nextWorkspace == modelIds.nextWorkspace);
    for (auto const& [id, uuid]: std::views::zip(ids, uuids))
    {
        INFO("session " << id.value);
        CHECK(host.objects().uuidOf(id) == uuid);
        CHECK(std::get<2>(host.model().findSessionLeaf(id)) != nullptr);
        // The very child, still running.
        auto const* const process = host.hostedSession(id)->process();
        REQUIRE(process != nullptr);
        CHECK(process->crossing().child->pid == children.at(id.value));
        CHECK(::kill(static_cast<pid_t>(children.at(id.value)), 0) == 0);
    }
    CHECK(host.takeUnrestoredScreens([](SessionId) { return true; }).empty());

    // The screens as they were: the text, and the image.
    CHECK(live->pageText(ids[1]).contains("ready"));
    CHECK(showsImage(*live, ids[2]));

    // What was carried arrives once; what is typed now reaches each child once, its output the screen.
    host.writeInput(ids[0], "typed");
    host.writeInput(ids[1], "second");
    REQUIRE(live->eventually([&] { return live->pageText(ids[0]).contains("typed"); }));
    REQUIRE(live->eventually([&] { return live->pageText(ids[1]).contains("second"); }));
    auto const first = live->pageText(ids[0]);
    CHECK(occurrences(first, "carried") == 1);
    CHECK(occurrences(first, "typed") == 1);
    CHECK(first.find("carried") < first.find("typed"));
    CHECK(occurrences(live->pageText(ids[1]), "second") == 1);

    // A session minted from now on is named as none the old image had.
    REQUIRE(host.createTab() != nullptr);
    CHECK(host.sessionIds().back().value > ids.back().value);
}

TEST_CASE("adopted session whose child already exited closes normally", "[vthost][handover][threading]")
{
    auto const image = handOver(oneSession());
    REQUIRE(image.sessions.size() == 1);
    auto const pid = static_cast<pid_t>(image.sessions.front().child.pid);

    // Gone between the capture and the adoption, and not reaped: what the new image finds.
    REQUIRE(::kill(pid, SIGKILL) == 0);
    auto exited = siginfo_t {};
    REQUIRE(::waitid(P_PID, static_cast<id_t>(pid), &exited, WEXITED | WNOWAIT) == 0);

    auto const live = takeOver(image);
    REQUIRE(live->host.sessionCount() == 1);
    CHECK(live->eventually([&] { return live->host.sessionCount() == 0; }));
    CHECK(std::get<2>(live->host.model().findSessionLeaf(image.sessions.front().id)) == nullptr);
}

TEST_CASE("a corrupt terminal state keeps the pty and announces it", "[vthost][handover][threading]")
{
    auto image = handOver(oneSession());
    REQUIRE(image.sessions.size() == 1);
    auto& session = image.sessions.front();
    auto const id = session.id;
    auto const pid = session.child.pid;
    auto const masterFd = session.pty.masterFd;

    // A state that cannot be read, or one that reads but is no state of a terminal.
    auto const tampered = GENERATE(as<std::string_view> {}, "unreadable", "refused");
    if (tampered == "unreadable")
        session.terminalState.resize(session.terminalState.size() / 2);
    else
    {
        auto state = vtbackend::decodeTerminalState(session.terminalState);
        REQUIRE(state.has_value());
        state->input.consumedBytes = -1;
        session.terminalState = vtbackend::encodeTerminalState(*state);
    }

    auto const live = takeOver(std::move(image));
    auto& host = live->host;
    REQUIRE(host.sessionIds() == std::vector { id });
    CHECK(host.handoverOutcome().has_value());
    CHECK(isOpen(masterFd));
    CHECK(::kill(static_cast<pid_t>(pid), 0) == 0);
    CHECK_FALSE(host.hostedSession(id)->terminal().device().isClosed());

    // Told once, to a client that shows the session.
    CHECK(host.takeUnrestoredScreens([](SessionId) { return false; }).empty());
    CHECK(host.takeUnrestoredScreens([](SessionId) { return true; }) == std::vector { id });
    CHECK(host.takeUnrestoredScreens([](SessionId) { return true; }).empty());

    // A fresh screen on the program that runs on.
    CHECK_FALSE(live->pageText(id).contains("ready"));
    host.writeInput(id, "still here");
    CHECK(live->eventually([&] { return live->pageText(id).contains("still here"); }));
}

TEST_CASE("a restored session runs on the settings its profile makes up in the new configuration",
          "[vthost][handover][threading]")
{
    auto const image = handOver(oneSession());
    auto const id = image.sessions.front().id;

    // A setting no part of the image carries: only this image's configuration can have set it.
    auto profile = vthost::testing::fakeProfile("/bin/sh", vtbackend::LineCount(4321));
    profile.settings.inFlightTailLimit = 12345;
    REQUIRE(profile.settings.inFlightTailLimit != vtbackend::Settings {}.inFlightTailLimit);
    auto const live =
        takeOver(image,
                 SessionHostConfig {
                     .profiles = std::make_shared<vthost::testing::FakeProfileCatalog>(
                         std::map<std::string, ResolvedProfile> { { "default", profile } }, "default") });

    auto& terminal = live->host.hostedSession(id)->terminal();
    auto const _ = std::lock_guard { terminal };
    CHECK(terminal.settings().inFlightTailLimit == 12345);
    // The construction settings are the captured terminal's own, which its state restores into.
    CHECK(terminal.primaryScreen().renderMainPageText().contains("ready"));
}

TEST_CASE("a restored session takes the scrollback limit of the configuration the new image loaded",
          "[vthost][handover][threading]")
{
    // Built with a limit the new configuration no longer names; the terminal state carries it across.
    auto oldSettings = vtbackend::Settings {};
    oldSettings.historyLimits = vtbackend::HistoryLimits::plain(vtbackend::LineCount(1000));
    auto old = std::make_unique<LiveHost>(catDevice, LiveHost::sequentialIds(), oldSettings);
    REQUIRE(old->host.createTab() != nullptr);
    auto const id = old->host.sessionIds().front();
    REQUIRE(old->eventually([&] { return old->pageText(id).contains("ready"); }));
    auto const image = handOver(std::move(old));

    auto const configured = vtbackend::HistoryLimits::plain(vtbackend::LineCount(4321));
    auto const live = takeOver(
        image,
        SessionHostConfig {
            .profiles = std::make_shared<vthost::testing::FakeProfileCatalog>(
                std::map<std::string, ResolvedProfile> {
                    { "default", vthost::testing::fakeProfile("/bin/sh", vtbackend::LineCount(4321)) } },
                "default") });
    auto& terminal = live->host.hostedSession(id)->terminal();
    auto const _ = std::lock_guard { terminal };
    CHECK(terminal.settings().historyLimits == configured);
    // The screen is the captured one all the same.
    CHECK(terminal.primaryScreen().renderMainPageText().contains("ready"));
}

TEST_CASE("a reordered daemon keeps its workspace order and its default workspace across an upgrade",
          "[vthost][handover]")
{
    auto old = std::make_unique<LiveHost>(catDevice);
    REQUIRE(old->host.createTab() != nullptr);
    auto const defaultWorkspace = old->host.workspaceId();
    auto* const other = old->host.createWorkspace();
    REQUIRE(other != nullptr);
    // The host's own workspace is no longer the first.
    old->host.model().moveWorkspace(defaultWorkspace, 1);
    auto const order = [](SessionHost& host) {
        return std::ranges::to<std::vector>(
            std::views::iota(0, host.model().workspaceCount())
            | std::views::transform([&](int i) { return host.model().workspaceAt(i)->id().value; }));
    };
    auto const expected = order(old->host);
    REQUIRE(expected == std::vector { other->id().value, defaultWorkspace.value });

    auto const image = handOver(std::move(old));
    auto const live = takeOver(image);

    CHECK(order(live->host) == expected);
    CHECK(live->host.workspaceId() == defaultWorkspace);
}

TEST_CASE(
    "a workspace an older release kept empty after its last session ended is dropped as it is taken over",
    "[vthost][handover]")
{
    auto old = std::make_unique<LiveHost>(catDevice);
    REQUIRE(old->host.createTab() != nullptr);
    auto const defaultWorkspace = old->host.workspaceId();
    // What an older release left behind: a tabless workspace beside the one that holds a tab.
    auto const ended = old->host.createEmptyWorkspace().id();
    REQUIRE(old->host.model().workspaceCount() == 2);

    auto const image = handOver(std::move(old));
    auto const live = takeOver(image);

    CHECK(live->host.model().workspaceCount() == 1);
    CHECK(live->host.model().workspace(ended) == nullptr);
    CHECK_FALSE(live->host.objects().uuidOf(ended).has_value());
    CHECK(live->host.workspaceId() == defaultWorkspace);
}

TEST_CASE("a daemon whose every workspace is empty keeps its own one across an upgrade", "[vthost][handover]")
{
    auto old = std::make_unique<LiveHost>(catDevice);
    auto const defaultWorkspace = old->host.workspaceId();
    std::ignore = old->host.createEmptyWorkspace();
    // The host's own workspace is not the first.
    old->host.model().moveWorkspace(defaultWorkspace, 1);

    auto const image = handOver(std::move(old));
    auto const live = takeOver(image);

    REQUIRE(live->host.model().workspaceCount() == 1);
    CHECK(live->host.workspaceId() == defaultWorkspace);
    CHECK(live->host.model().workspace(defaultWorkspace) != nullptr);
}

TEST_CASE("a refused workspace tree still adopts every session, each in a tab of its own",
          "[vthost][handover][threading]")
{
    auto old = std::make_unique<LiveHost>(catDevice);
    old->spawnTwo();
    auto const ids = old->host.sessionIds();
    REQUIRE(old->eventually([&] {
        return std::ranges::all_of(ids, [&](SessionId id) { return old->pageText(id).contains("ready"); });
    }));
    auto image = handOver(std::move(old));
    // A tab whose active pane is no leaf of it: the model refuses the whole tree.
    image.tree.workspaces.front().tabs.front().view.activePane = vtworkspace::PaneId { 9999 };

    auto const live = takeOver(std::move(image));
    auto& host = live->host;
    REQUIRE_FALSE(host.handoverOutcome().has_value());
    CHECK(host.handoverOutcome().error() == HandoverError::Malformed);
    REQUIRE(host.sessionIds() == ids);
    for (auto const id: ids)
    {
        auto const [workspace, tab, leaf] = host.model().findSessionLeaf(id);
        REQUIRE(leaf != nullptr);
        CHECK(tab->paneCount() == 1);
    }
    host.writeInput(ids[1], "alive");
    CHECK(live->eventually([&] { return live->pageText(ids[1]).contains("alive"); }));
}
#endif

#ifndef _WIN32
TEST_CASE("a kitty animation playing as it is handed over keeps the daemon's frame timer armed",
          "[vthost][handover][threading]")
{
    auto old = oneSession();
    auto const id = old->host.sessionIds().front();
    auto const image = handOver(std::move(old), [&](LiveHost& live) {
        auto& terminal = live.host.hostedSession(id)->terminal();
        {
            auto const _ = std::lock_guard { terminal };
            terminal.setCellPixelSize(vtbackend::ImageSize { vtbackend::Width(10), vtbackend::Height(20) });
        }
        // Two one-pixel frames a minute each, looping: due long after the takeover.
        terminal.writeToScreen("\033_Ga=T,f=24,i=7,s=1,v=1,q=2;/wAA\033\\"
                               "\033_Ga=a,i=7,r=1,z=60000,q=2\033\\"
                               "\033_Ga=f,i=7,X=1,s=1,v=1,z=60000,q=2;AP8A\033\\"
                               "\033_Ga=a,i=7,s=3,q=2\033\\");
        // What the parse posted for the loop is handled before the capture, as settle() leaves it.
        REQUIRE(live.eventually([&] { return live.host.pumpEventsInFlight() == 0; }));
        REQUIRE(terminal.kittyAnimationDue().has_value());
    });

    auto const live = takeOver(image);

    // Armed by the restore alone: nothing the program sends from now re-arms it.
    CHECK(live->eventually([&] { return live->host.ticksKittyAnimations(id); }));
}

namespace
{
/// Answers every directory with a branch named after it, and records what it was asked.
struct DirectoryNamingProbe final: IGitStatusProbe
{
    [[nodiscard]] std::optional<GitInfo> probe(std::filesystem::path const& cwd) const override
    {
        asked.push_back(cwd);
        return GitInfo { .branch = cwd.filename().string() };
    }

    mutable std::vector<std::filesystem::path> asked;
};

/// Hands one session over, and takes it over with git services on a fake probe.
/// @param beforeCapture Runs on the old host once its session is quiesced, before the capture.
/// @return The branch the successor looked up for the session's workspace, once it has: nothing, if it did
///         not within the wait. Asserts that it polls from then on.
[[nodiscard]] std::optional<std::string> branchLookedUpAfterTakeover(
    std::function<void(LiveHost&)> const& beforeCapture = {})
{
    // What git a workspace is on is looked up, not carried: the successor asks again for each one it takes
    // over, at once, rather than waiting for a window, a new directory or a finished command that may never
    // come -- and keeps asking on the poll, as a host whose workspaces were made in it does.
    auto old = oneSession();
    auto const workspace = old->host.workspaceId();
    auto const image = handOver(std::move(old), beforeCapture);

    auto probe = DirectoryNamingProbe {};
    auto executor = core::async::testing::ManualExecutor {};
    auto const live = takeOver(
        image,
        SessionHostConfig { .workspaceGit = WorkspaceGitServices { .probe = probe, .executor = executor } });

    if (!live->eventually([&] {
            executor.drain();
            return live->host.gitInfoOf(workspace).has_value();
        }))
        return std::nullopt;
    CHECK(live->host.pollsWorkspaceGit());
    return live->host.gitInfoOf(workspace)->branch;
}
} // namespace

TEST_CASE("a restored daemon looks every workspace's git state up again and keeps polling it",
          "[vthost][handover][threading][git]")
{
    // The directory the shell last reported crosses in the terminal's state, which every platform restores --
    // unlike a process's own directory, which only some can look up (below). What sets the lookup off is the
    // restore, which announces the directory whether one was reported or not, not the report itself: that
    // reached the old host, which has no git services.
    auto const branch = branchLookedUpAfterTakeover([](LiveHost& live) {
        auto& terminal = live.host.hostedSession(live.host.sessionIds().front())->terminal();
        terminal.writeToScreen("\033]7;file:///restored/reported-directory\033\\");
        // What the parse posted is handled before the capture, as settle() leaves it.
        REQUIRE(live.eventually([&] { return live.host.pumpEventsInFlight() == 0; }));
    });
    REQUIRE(branch.has_value());
    CHECK(*branch == "reported-directory");
}

    #if defined(__linux__) || defined(__APPLE__)
TEST_CASE("a restored daemon looks the git state of a directory no shell reported up again",
          "[vthost][handover][threading][git]")
{
    // No OSC 7: the adopted process's own directory is the only one there is, and nothing about it changes
    // across the upgrade. Only these platforms can look a process's directory up
    // (Process::workingDirectory()).
    auto const branch = branchLookedUpAfterTakeover();
    REQUIRE(branch.has_value());
    CHECK(*branch == std::filesystem::current_path().filename().string());
}
    #endif

namespace
{
/// A client that listens to nothing: the one that states a cell size.
struct SilentClient final: vthost::SessionStreamEvents
{
};

/// @return Whether @p session's terminal in @p live lays out on @p cell.
[[nodiscard]] bool laysOutOn(LiveHost& live, SessionId session, vtpty::ImageSize cell)
{
    auto& terminal = live.host.hostedSession(session)->terminal();
    auto const _ = std::lock_guard { terminal };
    return terminal.cellPixelSize() == cell;
}

} // namespace

TEST_CASE("a learned cell size crosses, and the successor's sessions lay out on it",
          "[vthost][handover][threading][cell]")
{
    auto old = oneSession();
    auto const id = old->host.sessionIds().front();
    auto client = SilentClient {};
    old->host.subscribeStream(&client);
    std::ignore = old->host.applyClientSize(&client, old->host.pageSize(), cellOf(9, 18));
    old->host.unsubscribeStream(&client);
    REQUIRE(old->host.learnedCellPixelSize() == cellOf(9, 18));

    auto image = handOver(std::move(old));
    CHECK(image.cellPixelSize == cellOf(9, 18));
    auto const live = takeOver(std::move(image));
    CHECK(live->host.learnedCellPixelSize() == cellOf(9, 18));
    CHECK(laysOutOn(*live, id, cellOf(9, 18)));

    // A program drawing an image before any client attached again.
    live->host.writeInput(id, SixelImage);
    CHECK(live->eventually([&] { return showsImage(*live, id); }));
}

TEST_CASE("a host that learned nothing hands over none, and the successor uses its own default",
          "[vthost][handover][threading][cell]")
{
    auto old = oneSession();
    auto const id = old->host.sessionIds().front();
    auto image = handOver(std::move(old));
    CHECK_FALSE(image.cellPixelSize.has_value());

    auto const live = takeOver(std::move(image));
    CHECK_FALSE(live->host.learnedCellPixelSize().has_value());
    CHECK(laysOutOn(*live, id, cellOf(10, 20)));
}

TEST_CASE("a corrupt cell size in the image is ignored", "[vthost][handover][threading][cell]")
{
    auto image = handOver(oneSession());
    auto const id = image.sessions.front().id;
    image.cellPixelSize = cellOf(0, 18);

    auto const live = takeOver(std::move(image));
    CHECK_FALSE(live->host.learnedCellPixelSize().has_value());
    CHECK(laysOutOn(*live, id, cellOf(10, 20)));
}

TEST_CASE("a client's session is rebuilt over the new image's host settings, as its connection built it",
          "[vthost][handover][threading]")
{
    // A host setting no part of the wire subset, nor of the terminal's state, carries.
    auto hostSettings = vtbackend::Settings {};
    hostSettings.maxImageSize = vtbackend::ImageSize { vtbackend::Width(1234), vtbackend::Height(987) };
    REQUIRE(hostSettings.maxImageSize != vtbackend::Settings {}.maxImageSize);

    auto old = std::make_unique<LiveHost>(catDevice, LiveHost::sequentialIds(), hostSettings);
    auto stated = toWireSessionSettings(hostSettings);
    stated.terminalId = std::to_underlying(vtbackend::VTType::VT420);
    // As NativeSession::adoptClientSessionSettings() builds what a client stated.
    REQUIRE(old->host.createTab(
                SessionSpawnRequest { .settings = fromWireSessionSettings(stated, old->host.settings()) })
            != nullptr);
    auto const id = old->host.sessionIds().front();
    REQUIRE(old->host.hostedSession(id)->settingsOrigin() == SettingsOrigin::Client);
    REQUIRE(old->eventually([&] { return old->pageText(id).contains("ready"); }));

    auto const live = takeOver(handOver(std::move(old)), {}, hostSettings);
    auto& terminal = live->host.hostedSession(id)->terminal();
    auto const _ = std::lock_guard { terminal };
    CHECK(terminal.settings().maxImageSize == hostSettings.maxImageSize);
    CHECK(terminal.settings().terminalId == vtbackend::VTType::VT420);
}

TEST_CASE("a session's colour preference crosses, so a client stating it again keeps what a program set",
          "[vthost][handover][threading]")
{
    auto const catalog = std::make_shared<vthost::testing::FakeDualPaletteCatalog>();
    auto config = LiveHost::sequentialIds();
    config.profiles = catalog;
    auto old = std::make_unique<LiveHost>(catDevice, std::move(config));
    REQUIRE(old->host.createTab() != nullptr);
    auto const id = old->host.sessionIds().front();
    REQUIRE(old->eventually([&] { return old->pageText(id).contains("ready"); }));

    // A dark-mode client attached, then a program set the background of its own (OSC 11).
    constexpr auto ProgramBackground = vtbackend::RGBColor { 0x123456 };
    old->host.applyColorPreference(id, vtbackend::ColorPreference::Dark);
    old->host.writeInput(id, "\033]11;rgb:12/34/56\033\\");
    auto const background = [](LiveHost& live, SessionId session) {
        auto& terminal = live.host.hostedSession(session)->terminal();
        auto const _ = std::lock_guard { terminal };
        return terminal.colorPalette().defaultBackground;
    };
    REQUIRE(old->eventually([&] { return background(*old, id) == ProgramBackground; }));

    auto const image = handOver(std::move(old));
    CHECK(image.defaultColorPreference == vtbackend::ColorPreference::Dark);
    CHECK(image.sessions.front().colorPreference == vtbackend::ColorPreference::Dark);
    auto const live = takeOver(image, SessionHostConfig { .profiles = catalog });
    CHECK(live->host.defaultColorPreference() == vtbackend::ColorPreference::Dark);
    CHECK(live->host.colorPreferenceOf(id) == vtbackend::ColorPreference::Dark);

    // The same client reattaches, stating the same preference: no change, nothing re-seeded.
    live->host.applyColorPreference(id, vtbackend::ColorPreference::Dark);
    CHECK(background(*live, id) == ProgramBackground);
}

TEST_CASE("a session's interrupted command crosses", "[vthost][handover][threading]")
{
    auto old = std::make_unique<LiveHost>(catDevice);
    REQUIRE(old->host.createTab(SessionSpawnRequest { .interruptedCommand = "make -j8" }) != nullptr);
    auto const id = old->host.sessionIds().front();
    REQUIRE(old->eventually([&] { return old->pageText(id).contains("ready"); }));
    auto const interruption = old->host.interruptionOf(id);
    REQUIRE(interruption.has_value());

    auto const live = takeOver(handOver(std::move(old)));
    CHECK(live->host.interruptionOf(id) == interruption);
    CHECK(live->host.interruptedCommandOf(id) == interruption->commandLine);
}

TEST_CASE("a PTY that cannot be adopted is closed, its fast pipe too", "[vthost][handover]")
{
    // No terminal: a descriptor the image names as a PTY master that is none, beside an open fast pipe.
    auto master = std::array<int, 2> {};
    auto fastPipe = std::array<int, 2> {};
    REQUIRE(::pipe(master.data()) == 0);
    REQUIRE(::pipe(fastPipe.data()) == 0);
    auto image = HandoverImage {};
    auto session = HandoverSession {};
    session.id = SessionId { 1 };
    session.pty.masterFd = master[0];
    session.pty.stdoutFastPipe = std::pair { fastPipe[0], fastPipe[1] };
    session.pty.pageSize = vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) };
    image.sessions.push_back(std::move(session));

    auto const live = takeOver(std::move(image));
    CHECK(live->host.sessionCount() == 0);
    CHECK_FALSE(isOpen(master[0]));
    CHECK_FALSE(isOpen(fastPipe[0]));
    CHECK_FALSE(isOpen(fastPipe[1]));
    // What the image did not name stays open.
    CHECK(isOpen(master[1]));
    ::close(master[1]);
}

TEST_CASE("a session whose PTY cannot be adopted leaves neither a zombie nor its pane's UUID behind",
          "[vthost][handover][threading]")
{
    auto master = std::array<int, 2> {};
    REQUIRE(::pipe(master.data()) == 0);
    // Its child, which the hangup of its PTY ends: here it ends by itself.
    auto child = pid_t { -1 };
    auto program = std::string { "sh" };
    auto option = std::string { "-c" };
    auto script = std::string { "exit 0" };
    auto argv = std::array<char*, 4> { program.data(), option.data(), script.data(), nullptr };
    auto envp = std::array<char*, 1> { nullptr };
    REQUIRE(::posix_spawn(&child, "/bin/sh", nullptr, nullptr, argv.data(), envp.data()) == 0);

    auto image = HandoverImage {};
    auto session = HandoverSession {};
    session.id = SessionId { 1 };
    session.uuid.bytes.fill(0xA5);
    session.pty.masterFd = master[0];
    session.pty.pageSize = vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) };
    session.child.pid = child;
    image.objects.panes = { BoundObject { .id = 1, .uuid = session.uuid } };
    image.sessions.push_back(std::move(session));

    auto const live = takeOver(std::move(image));
    CHECK(live->host.sessionCount() == 0);
    CHECK_FALSE(live->host.objects().uuidOf(SessionId { 1 }).has_value());
    // Reaped by the host, which no longer has it as a child.
    CHECK(live->eventually([&] {
        auto exited = siginfo_t {};
        return ::waitid(P_PID, static_cast<id_t>(child), &exited, WEXITED | WNOHANG | WNOWAIT) < 0
               && errno == ECHILD;
    }));
    ::close(master[1]);
}

TEST_CASE("a session served on its PTY alone crosses every later handover so, and refuses no upgrade",
          "[vthost][handover][threading]")
{
    // A PTY no child of the host runs on, as a handover leaves one whose child it could not adopt; the test
    // holds its other end, as the program on it would.
    auto slave = -1;
    auto old = std::make_unique<LiveHost>([&slave](vtpty::PageSize size) -> std::unique_ptr<vtpty::Pty> {
        auto master = -1;
        REQUIRE(::openpty(&master, &slave, nullptr, nullptr, nullptr) == 0);
        auto pty = vtpty::UnixPty::adopt(vtpty::InheritedPty {
            .masterFd = master, .stdoutFastPipe = std::nullopt, .pageSize = size, .pixels = std::nullopt });
        REQUIRE(pty.has_value());
        return *std::move(pty);
    });
    REQUIRE(old->host.createTab() != nullptr);
    auto const id = old->host.sessionIds().front();
    REQUIRE(old->host.hostedSession(id)->process() == nullptr);
    REQUIRE(slave >= 0);
    REQUIRE(::write(slave, "ready", 5) == 5);
    REQUIRE(old->eventually([&] { return old->pageText(id).contains("ready"); }));
    REQUIRE(checkCapturable(old->host).has_value());

    auto image = handOver(std::move(old));
    REQUIRE(image.sessions.size() == 1);
    CHECK(image.sessions.front().device == SessionDevice::PtyOnly);
    CHECK(image.sessions.front().child.pid == -1);

    // Served on its PTY alone, its screen as it was.
    auto live = takeOver(std::move(image));
    REQUIRE(live->host.sessionIds() == std::vector { id });
    CHECK(live->host.hostedSession(id)->process() == nullptr);
    CHECK(live->pageText(id).contains("ready"));

    // A later upgrade takes it as it is, and the next image serves it again.
    CHECK(checkCapturable(live->host).has_value());
    auto again = handOver(std::move(live));
    REQUIRE(again.sessions.size() == 1);
    CHECK(again.sessions.front().device == SessionDevice::PtyOnly);
    auto const next = takeOver(std::move(again));
    REQUIRE(next->host.sessionIds() == std::vector { id });
    REQUIRE(::write(slave, "-again", 6) == 6);
    CHECK(next->eventually([&] { return next->pageText(id).contains("ready-again"); }));
    ::close(slave);
}
#endif

#ifdef _WIN32
TEST_CASE("a pseudoconsole that cannot be adopted is closed, and its program's handle let go",
          "[vthost][handover]")
{
    // No pseudoconsole: two events the image names as one's pipes, the rest of its handles missing, beside a
    // handle its program would have crossed under.
    auto const event = [] {
        return vtpty::CrossingHandle::of(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    };
    auto program = HANDLE {};
    REQUIRE(
        DuplicateHandle(
            GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &program, SYNCHRONIZE, FALSE, 0)
        != FALSE);
    auto image = HandoverImage {};
    auto session = HandoverSession {};
    session.id = SessionId { 1 };
    session.pty.pageSize = vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) };
    session.pty.conpty = vtpty::InheritedConPty { .outputReader = event(),
                                                  .inputWriter = event(),
                                                  .signal = vtpty::CrossingHandle {},
                                                  .reference = vtpty::CrossingHandle {},
                                                  .consoleHost = vtpty::CrossingHandle {} };
    session.child.pid = static_cast<int64_t>(GetCurrentProcessId());
    session.child.processHandle = vtpty::CrossingHandle::of(program);
    auto const named = std::array { session.pty.conpty->outputReader,
                                    session.pty.conpty->inputWriter,
                                    session.child.processHandle };
    image.sessions.push_back(std::move(session));
    auto const unnamed = event();

    auto config = SessionHostConfig {};
    config.handover = std::move(image);
    auto const live = std::make_unique<vthost::testing::LiveHost>(
        [](vtpty::PageSize size) { return vthost::testing::shellProcess(size); }, std::move(config));
    CHECK(live->host.sessionCount() == 0);
    for (auto const handle: named)
    {
        INFO("handle " << handle.value);
        CHECK_FALSE(isOpen(handle));
    }
    // What the image did not name stays open.
    CHECK(isOpen(unnamed));
    CloseHandle(unnamed.native());
}
#endif
