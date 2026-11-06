// SPDX-License-Identifier: Apache-2.0

// clang-format off
#ifdef _WIN32
    #include <winsock2.h>
    #include <afunix.h>
    #include <windows.h>
#endif
// clang-format on

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <vthost/handover/HandoverImage.hpp>
#include <vthost/handover/HandoverRestore.hpp>
#include <vthost/handover/SuccessorLaunch.hpp>
#include <vthost/testing/FakeSuccessorLauncher.hpp> // empty but on Windows; reached everywhere, as linted

#ifdef _WIN32
    #include <vtpty/ConPty.hpp>
    #include <vtpty/Process.hpp>
    #include <vtpty/WindowsHandle.hpp>

    #include <chrono>
    #include <memory>
    #include <system_error>
    #include <thread>
    #include <tuple>
    #include <utility>

    #include <vthost/SessionHost.hpp>
    #include <vthost/client/ClientSupervisor.hpp>
    #include <vthost/handover/HandoverCodec.hpp>
    #include <vthost/handover/HandoverMemory.hpp>
    #include <vthost/handover/LiveUpgrade.hpp>
    #include <vthost/testing/HandoverTesting.hpp>
    #include <vthost/testing/LiveUpgradeTesting.hpp>
    #include <vthost/testing/TestHelpers.hpp>
#endif

using namespace vthost::handover;

TEST_CASE("the handles a successor is handed round-trip through their argument", "[vthost][handover]")
{
    auto const handles = SuccessorHandles { .image = vtpty::CrossingHandle { 0x1A4 },
                                            .ready = vtpty::CrossingHandle { 0x2B8 },
                                            .commit = vtpty::CrossingHandle { 0x2BC },
                                            .predecessor = vtpty::CrossingHandle { 0x7FFF'FFF0 } };
    auto const argument = handoverHandlesArgument(handles);
    REQUIRE(argument.starts_with(HandoverHandlesPrefix));
    CHECK(parseHandoverHandles(std::string_view { argument }.substr(HandoverHandlesPrefix.size()))
          == handles);
}

TEST_CASE("an argument that does not name four handles names none", "[vthost][handover]")
{
    auto const value = GENERATE(
        as<std::string_view> {}, "", "1,2,3", "1,2,3,4,5", "1,2,x,4", "1,0,3,4", "-1,2,3,4", "1,2,3,4,");
    CAPTURE(value);
    CHECK_FALSE(parseHandoverHandles(value).has_value());
}

// A pseudoconsole's handles are named on Windows alone (vtpty::InheritedPty::handles()).
#ifdef _WIN32

TEST_CASE("an image names each of its handles once, for the successor to inherit", "[vthost][handover]")
{
    using vtpty::CrossingHandle;
    auto image = HandoverImage {};
    auto session = HandoverSession {};
    session.pty.conpty = vtpty::InheritedConPty { .outputReader = CrossingHandle { 40 },
                                                  .inputWriter = CrossingHandle { 44 },
                                                  .signal = CrossingHandle { 48 },
                                                  .reference = CrossingHandle { 52 },
                                                  .consoleHost = CrossingHandle { 56 } };
    session.child.processHandle = CrossingHandle { 60 };
    image.sessions.push_back(session);
    // A second session that shares a handle with the first, as no real one does: it is named once.
    session.pty.conpty->outputReader = CrossingHandle { 44 };
    session.child.processHandle = CrossingHandle {};
    image.sessions.push_back(session);
    image.listeners.push_back(InheritedListener { .name = "control", .handle = CrossingHandle { 100 } });
    image.listeners.push_back(InheritedListener { .name = "native", .handle = CrossingHandle {} });
    image.lease = CrossingHandle { 8 };

    CHECK(handlesNamedBy(image)
          == std::vector<CrossingHandle> { CrossingHandle { 8 },
                                           CrossingHandle { 40 },
                                           CrossingHandle { 44 },
                                           CrossingHandle { 48 },
                                           CrossingHandle { 52 },
                                           CrossingHandle { 56 },
                                           CrossingHandle { 60 },
                                           CrossingHandle { 100 } });
}

TEST_CASE("no handle is named twice, and none is never named", "[vthost][handover]")
{
    using vtpty::CrossingHandle;
    auto image = HandoverImage {};
    auto session = HandoverSession {};
    session.pty.conpty = vtpty::InheritedConPty { .outputReader = CrossingHandle { 40 },
                                                  .inputWriter = CrossingHandle { 44 },
                                                  .signal = CrossingHandle {},
                                                  .reference = CrossingHandle { 52 },
                                                  .consoleHost = CrossingHandle { 56 } };
    image.sessions.push_back(session);
    image.listeners.push_back(InheritedListener { .name = "native", .handle = CrossingHandle { 12 } });
    image.lease = CrossingHandle { 12 };

    auto const named = handlesNamedBy(image);
    CHECK(std::ranges::count(named, CrossingHandle { 12 }) == 1);
    CHECK(std::ranges::none_of(named, &CrossingHandle::isNone));
    CHECK(named
          == std::vector<CrossingHandle> { CrossingHandle { 12 },
                                           CrossingHandle { 40 },
                                           CrossingHandle { 44 },
                                           CrossingHandle { 52 },
                                           CrossingHandle { 56 } });
}

using namespace std::chrono_literals;
using vthost::Admission;
using vthost::testing::FakeSuccessorLauncher;
using vthost::testing::LiveHost;
using vthost::testing::occurrences;
using vthost::testing::SuccessorBehaviour;
using vthost::testing::SuccessorTookOver;
using vthost::testing::UpgradeBench;
using vtworkspace::SessionId;

namespace
{

/// @return The echoing shell on a pseudoconsole of its own.
[[nodiscard]] std::unique_ptr<vtpty::Pty> shellDevice(vtpty::PageSize size)
{
    return vthost::testing::shellProcess(size);
}

/// A daemon about to be upgraded on Windows: the shared bench, its handoff a successor a fake launcher plays.
struct SuccessorUpgrade: UpgradeBench
{
    /// @param behaviour What the successor does once it is launched.
    /// @param readyTimeout How long the successor has to say it is ready.
    explicit SuccessorUpgrade(SuccessorBehaviour behaviour, std::chrono::milliseconds readyTimeout = 10s):
        UpgradeBench { shellDevice },
        launcher { behaviour },
        handoff { launcher, *memory, SuccessorHandoffConfig { .readyTimeout = readyTimeout } }
    {
        vthost::testing::requireBundledConsoleHost();
    }

    /// @return The orchestrator, over this daemon.
    [[nodiscard]] LiveUpgrade orchestrator()
    {
        return UpgradeBench::orchestrator(UpgradeBench::config(), handoff);
    }

    /// @return Upgrades this daemon to the staged target.
    [[nodiscard]] std::expected<void, UpgradeError> upgrade(LiveUpgrade& upgrade) const
    {
        return run(upgrade, UpgradeTarget { .imageId = imageId });
    }

    /// Two sessions, each showing it is ready.
    [[nodiscard]] std::vector<SessionId> spawnTwo()
    {
        live->spawnTwo();
        REQUIRE(live->eventually([&] { return allShow("ready"); }));
        return live->host.sessionIds();
    }

    std::unique_ptr<HandoverMemory> memory = makeNativeHandoverMemory();
    FakeSuccessorLauncher launcher;
    SuccessorHandoff handoff;
};

/// Checks that the daemon serves on after a handoff that did not happen: every listener accepting, every
/// session answering, and @p carried -- typed into the first while the upgrade had everything stopped, if
/// anything -- delivered once, ahead of what is typed afterwards.
void checkResumed(SuccessorUpgrade& u, std::vector<SessionId> const& ids, std::string_view carried = {})
{
    CHECK(u.acceptor->admission() == Admission::Accepting);
    u.live->host.writeInput(ids[0], "-after");
    u.live->host.writeInput(ids[1], "second");
    auto const expected = std::string { carried } + "-after";
    auto const shown = u.live->eventually([&] { return u.live->pageText(ids[0]).contains(expected); });
    INFO("first session's screen: " << u.live->pageText(ids[0]));
    REQUIRE(shown);
    REQUIRE(u.live->eventually([&] { return u.live->pageText(ids[1]).contains("second"); }));
    if (!carried.empty())
        CHECK(occurrences(u.live->pageText(ids[0]), std::string { carried }) == 1);
}

} // namespace

// The commit point, as the old daemon holds it: the successor inherits exactly the handles the image names
// and the handshake's, takes every session over, says it is ready -- and only then does the old daemon commit
// and end, every program running on under the very same handles.
TEST_CASE("a successor that is ready takes every session over, and the old daemon commits and ends",
          "[vthost][handover][upgrade][threading]")
{
    auto u = SuccessorUpgrade { SuccessorBehaviour::TakesOver };
    auto const ids = u.spawnTwo();
    auto const listenerSocket = u.acceptor->listeningHandle();
    REQUIRE_FALSE(listenerSocket.isNone());

    auto next = std::unique_ptr<LiveHost> {};
    auto image = std::optional<HandoverImage> {};
    u.launcher.onLaunch = [&](SuccessorHandles const& handles) {
        auto const bytes = u.memory->read(handles.image);
        REQUIRE(bytes.has_value());
        auto decoded = decodeHandover(*bytes);
        REQUIRE(decoded.has_value());
        auto expected = handlesNamedBy(*decoded);
        expected.insert(expected.end(),
                        { handles.image, handles.ready, handles.commit, handles.predecessor });
        std::ranges::sort(expected);
        auto inherited = u.launcher.launches.back().inherited;
        std::ranges::sort(inherited);
        CHECK(inherited == expected);
        CHECK(u.acceptor->admission() == Admission::Holding);
        image = *decoded;
        // The successor takes the pseudoconsoles and programs over under the handles it inherited, which in
        // this process are the old daemon's very handles.
        vthost::testing::releaseForAdoption(u.live->host);
        auto config = LiveHost::sequentialIds();
        config.handover = *std::move(decoded);
        next = std::make_unique<LiveHost>(shellDevice, std::move(config));
    };

    auto upgrade = u.orchestrator();
    REQUIRE_THROWS_AS(std::ignore = u.upgrade(upgrade), SuccessorTookOver);
    CHECK(u.launcher.committed);
    CHECK(u.launcher.terminated == 0);
    REQUIRE(u.launcher.launches.size() == 1);
    auto const& launch = u.launcher.launches.front();
    CHECK(launch.executable == u.target);
    CHECK(launch.argv.front() == u.target.string());
    CHECK(std::ranges::count_if(
              launch.argv,
              [](std::string const& argument) { return argument.starts_with(HandoverHandlesPrefix); })
          == 1);
    CHECK(std::ranges::none_of(
        launch.argv, [](std::string const& argument) { return argument.starts_with(HandoverFdPrefix); }));

    REQUIRE(image.has_value());
    CHECK(image->daemonInstance == 0xC0FFEE);
    REQUIRE(image->listeners.size() == 1);
    CHECK(image->listeners.front().handle == listenerSocket);
    REQUIRE(image->sessions.size() == 2);
    for (auto const& session: image->sessions)
    {
        REQUIRE(session.pty.conpty.has_value());
        CHECK(std::ranges::none_of(session.pty.conpty->handles(), &vtpty::CrossingHandle::isNone));
        CHECK_FALSE(session.child.processHandle.isNone());
    }

    // The old daemon's host lets go of nothing more as it goes.
    u.acceptor.reset();
    u.live.reset();

    REQUIRE(next != nullptr);
    CHECK(next->host.handoverOutcome().has_value());
    REQUIRE(next->host.sessionIds() == ids);
    for (auto const id: ids)
        next->host.writeInput(id, "after-upgrade");
    REQUIRE(next->eventually([&] {
        return std::ranges::all_of(
            ids, [&](SessionId id) { return next->pageText(id).contains("after-upgrade"); });
    }));
}

TEST_CASE("a successor that ends before it is ready leaves the old daemon serving, no input lost",
          "[vthost][handover][upgrade][threading]")
{
    auto u = SuccessorUpgrade { SuccessorBehaviour::EndsBeforeReady };
    auto const ids = u.spawnTwo();
    // Typed while the daemon was stopped: carried, and delivered once it resumes.
    u.launcher.onLaunch = [&](SuccessorHandles const&) {
        u.live->host.writeInput(ids[0], "carried");
    };

    auto upgrade = u.orchestrator();
    auto const result = u.upgrade(upgrade);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error()
          == UpgradeError { ExecFailure { std::make_error_code(std::errc::no_such_process) } });
    CHECK(upgrade.state() == UpgradeState::Idle);
    CHECK(u.launcher.committed == false);
    CHECK(u.launcher.terminated == 0); // nothing left to end
    CHECK(u.leases.heldCount() == 0);
    checkResumed(u, ids, "carried");
}

TEST_CASE("a successor not ready in time is ended, and the old daemon serves on",
          "[vthost][handover][upgrade][threading]")
{
    auto u = SuccessorUpgrade { SuccessorBehaviour::NeverReady, 200ms };
    auto const ids = u.spawnTwo();
    u.launcher.onLaunch = [&](SuccessorHandles const&) {
        u.live->host.writeInput(ids[0], "carried");
    };

    auto upgrade = u.orchestrator();
    auto const result = u.upgrade(upgrade);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == UpgradeError { ExecFailure { std::make_error_code(std::errc::timed_out) } });
    CHECK(u.launcher.terminated == 1);
    checkResumed(u, ids, "carried");
}

TEST_CASE("a successor that cannot be started leaves the old daemon serving",
          "[vthost][handover][upgrade][threading]")
{
    auto u = SuccessorUpgrade { SuccessorBehaviour::FailsToStart };
    auto const ids = u.spawnTwo();

    auto upgrade = u.orchestrator();
    auto const result = u.upgrade(upgrade);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error()
          == UpgradeError { ExecFailure { std::make_error_code(std::errc::no_such_file_or_directory) } });
    CHECK(u.launcher.terminated == 0);
    REQUIRE(u.launcher.launches.size() == 1);
    checkResumed(u, ids);
}

// The old daemon's loop answers nothing while it waits for its successor, and a client that stops holding on
// for the upgrade starts a daemon of its own: the wait, and the end of a successor that missed it, are over
// well before the clients give up.
TEST_CASE("a successor is given up on before the clients stop waiting for the upgrade", "[vthost][handover]")
{
    STATIC_CHECK(SuccessorHandoffConfig {}.readyTimeout + SuccessorTerminateWait
                 < vthost::client::UpgradeHold);
}

namespace
{

/// @return A handle of this process's own to what @p handle names, as a successor inherits one: closed by
///         whatever it is handed to.
[[nodiscard]] vtpty::CrossingHandle duplicateOf(HANDLE handle)
{
    auto duplicate = HANDLE {};
    REQUIRE(DuplicateHandle(
                GetCurrentProcess(), handle, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)
            != FALSE);
    return vtpty::CrossingHandle::of(duplicate);
}

/// The predecessor's half of the commit point, in this process: the events it shares with its successor, and
/// an event that stands in for its process, signalled once it has ended.
struct Predecessor
{
    vtpty::UniqueHandle ready { CreateEventW(nullptr, TRUE, FALSE, nullptr) };
    vtpty::UniqueHandle commit { CreateEventW(nullptr, TRUE, FALSE, nullptr) };
    vtpty::UniqueHandle process { CreateEventW(nullptr, TRUE, FALSE, nullptr) };
    std::unique_ptr<HandoverMemory> memory = makeNativeHandoverMemory();

    /// @return What a successor is handed for @p image, which receiveHandover() takes and closes.
    [[nodiscard]] SuccessorHandles handlesFor(HandoverImage const& image) const
    {
        auto const mapping = memory->write(encodeHandover(image));
        REQUIRE(mapping.has_value());
        return SuccessorHandles { .image = *mapping,
                                  .ready = duplicateOf(ready.get()),
                                  .commit = duplicateOf(commit.get()),
                                  .predecessor = duplicateOf(process.get()) };
    }

    /// @return Whether the successor said it is ready.
    [[nodiscard]] bool sawReady() const { return WaitForSingleObject(ready.get(), 0) == WAIT_OBJECT_0; }

    /// Waits for the successor to say it is ready, then signals each of @p events, in order.
    [[nodiscard]] std::thread answer(std::vector<HANDLE> events) const
    {
        return std::thread { [this, events = std::move(events)] {
            if (WaitForSingleObject(ready.get(), 10'000) != WAIT_OBJECT_0)
                return;
            for (auto* const event: events)
                SetEvent(event);
        } };
    }
};

/// How long a test's successor waits for a commit that is not coming.
constexpr auto ShortCommitWait = SuccessorTakeoverConfig { .commitWait = 100ms, .predecessorEndWait = 100ms };

} // namespace

TEST_CASE("a successor its predecessor commits to takes the image over", "[vthost][handover][threading]")
{
    auto const predecessor = Predecessor {};
    auto image = HandoverImage {};
    image.fromRelease = "0.9.0";
    auto answering = predecessor.answer({ predecessor.commit.get(), predecessor.process.get() });
    auto const received = receiveHandover(predecessor.handlesFor(image), *predecessor.memory);
    answering.join();
    INFO(received.error_or(""));
    REQUIRE(received.has_value());
    CHECK(received->fromRelease == "0.9.0");
}

// It saw this one ready and ended before it could commit: nothing of it runs on to resume, and what it left
// is taken over rather than lost.
TEST_CASE("a successor whose predecessor ends as it is ready takes over what it left",
          "[vthost][handover][threading]")
{
    auto const predecessor = Predecessor {};
    auto answering = predecessor.answer({ predecessor.process.get() });
    auto const received = receiveHandover(predecessor.handlesFor(HandoverImage {}), *predecessor.memory);
    answering.join();
    CHECK(received.has_value());
}

// The predecessor gave up on it -- it was not ready in time, and could not be ended -- and serves on: it must
// not take over later, from a predecessor that ends for another reason.
TEST_CASE("a successor its predecessor neither commits to nor ends for takes nothing over",
          "[vthost][handover]")
{
    auto const predecessor = Predecessor {};
    auto const received =
        receiveHandover(predecessor.handlesFor(HandoverImage {}), *predecessor.memory, ShortCommitWait);
    CHECK(predecessor.sawReady());
    REQUIRE_FALSE(received.has_value());
    CHECK(received.error().contains("did not commit"));
}

// Nothing is said before the image is read and every session in it found adoptable: a predecessor that never
// sees it ready serves on, and ends it.
TEST_CASE("a successor says nothing of an image it cannot take over", "[vthost][handover]")
{
    auto const predecessor = Predecessor {};
    auto handles = SuccessorHandles {};
    auto why = std::string {};
    SECTION("one it cannot read")
    {
        handles = predecessor.handlesFor(HandoverImage {});
        CloseHandle(handles.image.native());
        // An event where the mapping should be.
        handles.image = duplicateOf(predecessor.commit.get());
        why = "cannot read the image";
    }
    SECTION("one with a session whose pseudoconsole did not cross")
    {
        auto image = HandoverImage {};
        auto session = HandoverSession {};
        session.label = "lost";
        image.sessions.push_back(std::move(session));
        handles = predecessor.handlesFor(image);
        why = "cannot adopt the session \"lost\"";
    }
    SECTION("one with a session whose handles are not this process's")
    {
        auto image = HandoverImage {};
        auto session = HandoverSession {};
        session.label = "foreign";
        session.pty.conpty = vtpty::InheritedConPty { .outputReader = vtpty::CrossingHandle { 4 },
                                                      .inputWriter = vtpty::CrossingHandle { 8 },
                                                      .signal = vtpty::CrossingHandle { 12 },
                                                      .reference = vtpty::CrossingHandle { 16 },
                                                      .consoleHost = vtpty::CrossingHandle { 20 } };
        image.sessions.push_back(std::move(session));
        handles = predecessor.handlesFor(image);
        why = "cannot adopt the session \"foreign\"";
    }

    auto const received = receiveHandover(handles, *predecessor.memory, ShortCommitWait);
    REQUIRE_FALSE(received.has_value());
    CHECK(received.error().contains(why));
    CHECK_FALSE(predecessor.sawReady());
}

TEST_CASE("a successor handed no event to say it is ready with takes nothing over", "[vthost][handover]")
{
    auto const predecessor = Predecessor {};
    auto handles = predecessor.handlesFor(HandoverImage {});
    CloseHandle(handles.ready.native());
    // A mapping where the event should be: it cannot be set.
    auto const notAnEvent = vtpty::UniqueHandle { CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 16, nullptr) };
    handles.ready = duplicateOf(notAnEvent.get());
    auto const received = receiveHandover(handles, *predecessor.memory, ShortCommitWait);
    REQUIRE_FALSE(received.has_value());
    CHECK(received.error().contains("names no event"));
}

#endif
