// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>

#include <core/Environment.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include <vthost/ClientCensus.hpp>
#include <vthost/HistorySeed.hpp>
#include <vthost/IdleExitWatcher.hpp>
#include <vthost/LastSessionWatcher.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/SessionSnapshotStore.hpp>
#include <vthost/WorkspacePersistence.hpp>
#include <vthost/testing/FaultyFileSystem.hpp>
#include <vthost/testing/RecordingDurableFiles.hpp>
#include <vthost/testing/RecordingFilePrivacy.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

using namespace std::chrono_literals;
using namespace vthost;
using vthost::testing::FaultOp;
using vthost::testing::FaultyFileSystem;
using vthost::testing::RecordingDurableFiles;
using vthost::testing::RecordingFilePrivacy;
namespace fs = std::filesystem;

namespace
{

constexpr auto SnapshotFile = std::string_view { "/state/contour/sessions/default.snapshot" };
constexpr auto TempFile = std::string_view { "/state/contour/sessions/default.snapshot.tmp" };
constexpr auto RestoredFile = std::string_view { "/state/contour/sessions/default.snapshot.restored" };
constexpr auto Interval = 30s;

[[nodiscard]] std::size_t leafCount(PaneNode const& node)
{
    if (node.children.empty())
        return 1;
    return leafCount(node.children[0]) + leafCount(node.children[1]);
}

[[nodiscard]] std::size_t leafCount(std::optional<WorkspaceSnapshot> const& snapshot)
{
    auto count = std::size_t { 0 };
    if (snapshot)
        for (auto const& window: snapshot->windows)
            for (auto const& tab: window.tabs)
                count += leafCount(tab.root);
    return count;
}

[[nodiscard]] WorkspaceSnapshot onePane(std::chrono::sys_seconds takenAt = {})
{
    auto window = WindowSnapshot {};
    window.tabs.push_back(TabSnapshot {});
    return WorkspaceSnapshot { .takenAt = takenAt, .windows = { std::move(window) } };
}

/// A host on a manual clock, persisted to an in-memory file whose replacing renames are counted, and fail
/// while @ref failing is set.
struct PersistenceHarness
{
    core::platform::ManualClock clock {};
    std::unique_ptr<core::net::IoBackend> backend = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *backend, clock };
    core::platform::ManualWallClock wallClock { std::chrono::system_clock::time_point { 1'761'000'000s } };
    bool failing = false;
    int writes = 0; ///< Snapshots that reached the file.
    FaultyFileSystem files { [this](FaultOp op, fs::path const& path) {
        if (op != FaultOp::Rename || path != fs::path { SnapshotFile })
            return false;
        if (!failing)
            ++writes;
        return failing;
    } };
    RecordingFilePrivacy privacy { files };
    RecordingDurableFiles durability { files };
    SessionSnapshotStore store { files, privacy, durability, fs::path { SnapshotFile } };
    std::vector<std::string> lines; ///< What the writer reported.
    SnapshotWriter writer { store, WriterThreading::Inline, [this](std::string_view line) {
                               lines.emplace_back(line);
                           } };
    std::unique_ptr<SessionHost> host;
    std::unique_ptr<WorkspacePersistence> persistence;

    PersistenceHarness() { makeHost({}); }

    void makeHost(SessionHostConfig config)
    {
        persistence.reset();
        config.pumping = SessionPumping::Manual;
        host = std::make_unique<SessionHost>(
            loop,
            [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                return std::make_unique<vtpty::MockPty>(size);
            },
            vtbackend::Settings {},
            core::defaultEnvironment(),
            std::move(config));
    }

    /// @return The sessions of a tab split in two.
    [[nodiscard]] std::pair<vtworkspace::SessionId, vtworkspace::SessionId> twoPanes() const
    {
        auto* const tab = host->createTab();
        REQUIRE(tab != nullptr);
        auto const first = tab->rootPane()->session();
        host->splitActivePane(tab->id(), vtworkspace::SplitState::Vertical);
        REQUIRE(tab->paneCount() == 2);
        return { first, tab->activePane()->session() };
    }

    void persist()
    {
        persistence = std::make_unique<WorkspacePersistence>(
            loop, *host, writer, WorkspacePersistenceConfig { .interval = Interval }, wallClock);
    }

    void pass(std::chrono::milliseconds duration)
    {
        clock.advance(duration);
        std::ignore = loop.runUntilIdle();
    }

    [[nodiscard]] std::size_t leavesOnFile() const { return leafCount(store.load().snapshot); }
};

} // namespace

TEST_CASE("closing a pane on request removes it from the file at once", "[vthost][snapshot][persistence]")
{
    auto h = PersistenceHarness {};
    auto const [a, b] = h.twoPanes();
    h.persist();

    h.host->closeSession(b);
    std::ignore = h.loop.runOnce(0ms);
    CHECK(h.leavesOnFile() == 1);
}

TEST_CASE("a shutdown right after the shells were killed keeps them", "[vthost][snapshot][persistence]")
{
    auto h = PersistenceHarness {};
    auto const [a, b] = h.twoPanes();
    h.persist();
    h.pass(Interval);
    REQUIRE(h.leavesOnFile() == 2);

    h.host->endSession(a, SessionEnd::Killed);
    h.host->endSession(b, SessionEnd::Killed);
    h.pass(1s);
    h.persistence->finalize(ShutdownWorkspace::Keep);
    CHECK(h.leavesOnFile() == 2);
}

TEST_CASE("ExitWhenEmpty: both panes killed, LastSessionClosed finalize keeps the file",
          "[vthost][snapshot][persistence]")
{
    auto h = PersistenceHarness {};
    auto const [a, b] = h.twoPanes();
    h.persist();
    h.pass(Interval);
    REQUIRE(h.leavesOnFile() == 2);
    auto finalized = 0;
    auto const watcher = LastSessionWatcher { *h.host, h.loop, [&] {
                                                 ++finalized;
                                                 h.persistence->finalize(ShutdownWorkspace::Keep);
                                             } };

    // In one turn, as a system shutdown ends both shells: the posted shutdown runs before any timer.
    h.host->endSession(a, SessionEnd::Killed);
    h.host->endSession(b, SessionEnd::Killed);
    std::ignore = h.loop.runOnce(0ms);
    CHECK(finalized == 1);
    CHECK(h.leavesOnFile() == 2);
}

TEST_CASE("a kill older than the grace is written out", "[vthost][snapshot][persistence]")
{
    auto h = PersistenceHarness {};
    auto const [a, b] = h.twoPanes();
    h.persist();
    h.pass(Interval);
    REQUIRE(h.leavesOnFile() == 2);

    h.host->endSession(a, SessionEnd::Killed);
    h.pass(4s);
    CHECK(h.leavesOnFile() == 2);
    h.pass(2s);
    CHECK(h.leavesOnFile() == 1);
}

TEST_CASE("finalize Discard leaves an empty snapshot", "[vthost][snapshot][persistence]")
{
    auto h = PersistenceHarness {};
    std::ignore = h.twoPanes();
    h.persist();
    h.pass(Interval);
    h.persistence->finalize(ShutdownWorkspace::Discard);
    CHECK(h.store.load().verdict == LoadVerdict::Empty);
}

TEST_CASE("nothing is written while nothing changed", "[vthost][snapshot][persistence]")
{
    auto h = PersistenceHarness {};
    REQUIRE(h.host->createTab() != nullptr);
    h.persist();
    h.pass(Interval);
    CHECK(h.writes == 1);
    h.pass(Interval);
    h.pass(Interval);
    h.pass(Interval);
    CHECK(h.writes == 1);
}

TEST_CASE("events after finalize write nothing", "[vthost][snapshot][persistence]")
{
    auto h = PersistenceHarness {};
    auto const [a, b] = h.twoPanes();
    h.persist();
    h.persistence->finalize(ShutdownWorkspace::Keep);
    REQUIRE(h.writes == 1);
    auto const written = h.files.readFile(SnapshotFile);

    h.host->closeSession(a);
    h.pass(Interval);
    h.persistence->finalize(ShutdownWorkspace::Discard);
    CHECK(h.writes == 1);
    CHECK(h.files.readFile(SnapshotFile) == written);
}

TEST_CASE("the persistence observer does not keep an idle daemon alive", "[vthost][snapshot][persistence]")
{
    auto h = PersistenceHarness {};
    h.persist();
    auto census = ClientCensus {};
    auto const watcher = IdleExitWatcher { *h.host, census, h.loop, 10s, [] {} };
    CHECK(census.count() == 0);
    CHECK(watcher.isArmed());
}

TEST_CASE("the first write after a realized restore drops the kept restore file",
          "[vthost][snapshot][persistence]")
{
    auto h = PersistenceHarness {};
    REQUIRE(h.store.write(onePane()));
    auto report = std::vector<std::string> {};
    auto restore = prepareRestore(h.store, {}, [&](std::string_view line) { report.emplace_back(line); });
    REQUIRE(restore.has_value());
    CHECK(h.files.exists(RestoredFile));
    CHECK_FALSE(h.files.exists(SnapshotFile));
    auto const source = restore->source;
    h.makeHost(SessionHostConfig { .restore = std::move(restore) });
    REQUIRE(h.host->restoreOutcome() == RestoreOutcome::Realized);
    REQUIRE(h.store.endRestore(source, h.host->restoreOutcome()));
    h.persist();

    SECTION("a write that succeeds")
    {
        h.pass(Interval);
        CHECK(h.leavesOnFile() == 1);
        CHECK_FALSE(h.files.exists(RestoredFile));
    }
    SECTION("a write that fails")
    {
        h.failing = true;
        h.pass(Interval);
        CHECK(h.files.exists(RestoredFile));
        h.failing = false;
        h.pass(Interval);
        CHECK_FALSE(h.files.exists(RestoredFile));
    }
}

TEST_CASE("a failing disk is reported once until a write succeeds", "[vthost][snapshot][persistence]")
{
    auto h = PersistenceHarness {};
    REQUIRE(h.host->createTab() != nullptr);
    h.persist();

    h.failing = true;
    h.pass(Interval);
    h.pass(Interval);
    h.pass(Interval);
    CHECK(h.lines.size() == 1);
    CHECK_FALSE(h.files.exists(TempFile));

    h.failing = false;
    h.pass(Interval);
    REQUIRE(h.lines.size() == 2);
    CHECK(h.lines[1].contains("snapshot writes recovered"));
    CHECK(h.leavesOnFile() == 1);

    h.failing = true;
    REQUIRE(h.host->createTab() != nullptr);
    h.pass(1s);
    CHECK(h.lines.size() == 3);
}

TEST_CASE("a background writer writes the newest snapshot and flush waits for it",
          "[vthost][snapshot][persistence][threading]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    auto privacy = RecordingFilePrivacy { files };
    auto durability = RecordingDurableFiles { files };
    auto const store = SessionSnapshotStore { files, privacy, durability, fs::path { SnapshotFile } };
    auto const at = [](int seconds) {
        return std::chrono::sys_seconds { std::chrono::seconds { seconds } };
    };

    SECTION("submitted back to back")
    {
        auto writer = SnapshotWriter { store, WriterThreading::Background, [](std::string_view) {} };
        writer.submit(onePane(at(1)));
        writer.submit(onePane(at(2)));
        writer.submit(onePane(at(3)));
        writer.flush();
        CHECK(store.load().snapshot == onePane(at(3)));
    }
    SECTION("the destructor flushes")
    {
        {
            auto writer = SnapshotWriter { store, WriterThreading::Background, [](std::string_view) {} };
            writer.submit(onePane(at(7)));
        }
        CHECK(store.load().snapshot == onePane(at(7)));
    }
    SECTION("submitted from two threads")
    {
        auto writer = SnapshotWriter { store, WriterThreading::Background, [](std::string_view) {} };
        {
            auto other = std::jthread { [&] {
                for (auto const seconds: std::views::iota(1, 51))
                    writer.submit(onePane(at(seconds)));
            } };
            for (auto const seconds: std::views::iota(101, 151))
                writer.submit(onePane(at(seconds)));
        }
        writer.submit(onePane(at(999)));
        writer.flush();
        CHECK(store.load().snapshot == onePane(at(999)));
        CHECK(writer.health() == WriteHealth::Healthy);
    }
}

TEST_CASE("prepareRestore turns each verdict into a restore or nothing", "[vthost][snapshot][persistence]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    auto privacy = RecordingFilePrivacy { files };
    auto durability = RecordingDurableFiles { files };
    auto const store = SessionSnapshotStore { files, privacy, durability, fs::path { SnapshotFile } };
    auto lines = std::vector<std::string> {};
    auto const report = [&](std::string_view line) {
        lines.emplace_back(line);
    };
    auto const config = WorkspacePersistenceConfig { .rerunPrograms = { "htop" } };

    SECTION("absent")
    {
        CHECK_FALSE(prepareRestore(store, config, report));
        CHECK(lines.empty());
    }
    SECTION("empty")
    {
        REQUIRE(store.clear());
        CHECK_FALSE(prepareRestore(store, config, report));
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].contains("previous daemon left no workspace"));
    }
    SECTION("damaged")
    {
        REQUIRE(files.writeFile(SnapshotFile, "garbage"));
        CHECK_FALSE(prepareRestore(store, config, report));
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].contains(std::string { SnapshotFile } + ".corrupt"));
    }
    SECTION("a newer format")
    {
        REQUIRE(store.write(onePane()));
        auto bytes = files.readFile(SnapshotFile).value();
        bytes[8] = '\x02';
        REQUIRE(files.writeFile(SnapshotFile, bytes));
        CHECK_FALSE(prepareRestore(store, config, report));
        CHECK(lines.size() == 1);
        CHECK(files.exists(SnapshotFile));
    }
    SECTION("restorable")
    {
        auto const takenAt = std::chrono::sys_seconds { 1'761'000'000s };
        REQUIRE(store.write(onePane(takenAt)));
        auto const restore = prepareRestore(store, config, report);
        REQUIRE(restore.has_value());
        CHECK(restore->snapshot == onePane(takenAt));
        CHECK(restore->rerunPrograms == std::vector<std::string> { "htop" });
        CHECK(restore->marker == restoredMarker(localTimeOf(takenAt)));
        CHECK(restore->source == RestoreSource::Snapshot);
        CHECK(files.exists(RestoredFile));
    }
}
