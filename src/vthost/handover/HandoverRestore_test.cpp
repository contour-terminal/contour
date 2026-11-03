// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/state/TerminalStateCodec.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#ifndef _WIN32
    #include <vtpty/Process.hpp>
    #include <vtpty/UnixPty.hpp>

    #include <sys/socket.h>
    #include <sys/wait.h>

    #include <csignal>

    #include <fcntl.h>
    #include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vthost/SessionHost.hpp>
#include <vthost/handover/HandoverRestore.hpp>
#include <vthost/testing/FakeProfileCatalog.hpp>
#include <vthost/testing/HandoverTesting.hpp>

using namespace vthost;
using namespace vthost::handover;
using vtworkspace::SessionId;

TEST_CASE("the descriptors a handover keeps are the standard streams and every one the image names",
          "[vthost][handover]")
{
    auto image = HandoverImage {};
    image.sessions.resize(2);
    image.sessions[0].pty.masterFd = 9;
    image.sessions[0].pty.stdoutFastPipe = std::pair { 12, -1 };
    image.sessions[1].pty.masterFd = 7;
    image.listeners = { { .name = "native", .fd = 5 }, { .name = "control", .fd = 7 } };
    image.leaseFd = 3;
    CHECK(descriptorsToKeep(image) == std::vector { 0, 1, 2, 3, 5, 7, 9, 12 });
    CHECK(descriptorsToKeep(HandoverImage {}) == std::vector { 0, 1, 2 });
}

#ifndef _WIN32
namespace
{

using vthost::testing::LiveHost;
using vthost::testing::releaseForAdoption;
using vthost::testing::shellProcess;

/// @return Whether descriptor @p fd is open.
[[nodiscard]] bool isOpen(int fd)
{
    return ::fcntl(fd, F_GETFD) != -1;
}

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

/// @return How often @p needle occurs in @p text.
[[nodiscard]] std::size_t occurrences(std::string_view text, std::string_view needle)
{
    auto count = std::size_t { 0 };
    for (auto at = text.find(needle); at != std::string_view::npos;
         at = text.find(needle, at + needle.size()))
        ++count;
    return count;
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
[[nodiscard]] std::unique_ptr<LiveHost> takeOver(HandoverImage image, SessionHostConfig config = {})
{
    config.handover = std::move(image);
    return std::make_unique<LiveHost>(catDevice, std::move(config));
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
    image.leaseFd = 42;

    auto const written = memory->write(encodeHandover(image));
    REQUIRE(written.has_value());
    auto const read = readHandover(*memory, *written);
    REQUIRE(read.has_value());
    CHECK(read->daemonInstance == 0xABCD);
    CHECK(read->fromRelease == "0.7.1");
    CHECK(read->leaseFd == 42);
    CHECK_FALSE(isOpen(*written));

    // Bytes that are no image: refused, the descriptor closed all the same.
    auto const garbage = memory->write(std::vector<uint8_t> { 1, 2, 3, 4, 5 });
    REQUIRE(garbage.has_value());
    CHECK_FALSE(readHandover(*memory, *garbage).has_value());
    CHECK_FALSE(isOpen(*garbage));
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
    auto const window = old->host.windowId();

    // Typed while quiesced: carried across, not lost.
    auto const image =
        handOver(std::move(old), [&](LiveHost& live) { live.host.writeInput(ids[0], "carried"); });
    auto const children = childrenOf(image);
    auto const live = takeOver(image);
    auto& host = live->host;

    CHECK(host.handoverOutcome().has_value());
    REQUIRE(host.sessionIds() == ids);
    CHECK(host.windowId() == window);
    CHECK(host.model().idState().nextPane == modelIds.nextPane);
    CHECK(host.model().idState().nextTab == modelIds.nextTab);
    CHECK(host.model().idState().nextWindow == modelIds.nextWindow);
    for (auto const& [id, uuid]: std::views::zip(ids, uuids))
    {
        INFO("session " << id.value);
        CHECK(host.objects().uuidOf(id) == uuid);
        CHECK(std::get<2>(host.model().findSessionLeaf(id)) != nullptr);
        // The very child, still running.
        auto const* const process = host.hostedSession(id)->process();
        REQUIRE(process != nullptr);
        CHECK(process->handoverChild()->pid == children.at(id.value));
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
    image.tree.windows.front().tabs.front().view.activePane = vtworkspace::PaneId { 9999 };

    auto const live = takeOver(std::move(image));
    auto& host = live->host;
    REQUIRE_FALSE(host.handoverOutcome().has_value());
    CHECK(host.handoverOutcome().error() == HandoverError::Malformed);
    REQUIRE(host.sessionIds() == ids);
    for (auto const id: ids)
    {
        auto const [window, tab, leaf] = host.model().findSessionLeaf(id);
        REQUIRE(leaf != nullptr);
        CHECK(tab->paneCount() == 1);
    }
    host.writeInput(ids[1], "alive");
    CHECK(live->eventually([&] { return live->pageText(ids[1]).contains("alive"); }));
}
#endif

#ifndef _WIN32
TEST_CASE("a restored terminal lays an image out at the cell size its PTY's pixels give",
          "[vthost][handover][threading]")
{
    auto image = handOver(oneSession());
    auto& session = image.sessions.front();
    auto const id = session.id;
    // As a client reported it: the cell size is no part of the terminal's state, the PTY's pixels are.
    session.pty.pixels = vtpty::ImageSize { vtpty::Width(10 * unbox(session.pty.pageSize.columns)),
                                            vtpty::Height(20 * unbox(session.pty.pageSize.lines)) };

    auto const live = takeOver(std::move(image));
    {
        auto& terminal = live->host.hostedSession(id)->terminal();
        auto const _ = std::lock_guard { terminal };
        CHECK(terminal.cellPixelSize().width == vtbackend::Width(10));
        CHECK(terminal.cellPixelSize().height.value > 0);
    }

    // A program drawing an image before any client attached again.
    live->host.writeInput(id, SixelImage);
    CHECK(live->eventually([&] { return showsImage(*live, id); }));
}
#endif
