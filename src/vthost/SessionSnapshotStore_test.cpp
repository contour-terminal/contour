// SPDX-License-Identifier: Apache-2.0
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

#include <vthost/SessionSnapshotCodec.hpp>
#include <vthost/SessionSnapshotStore.hpp>
#include <vthost/testing/FaultyFileSystem.hpp>
#include <vthost/testing/RecordingDurableFiles.hpp>
#include <vthost/testing/RecordingFilePrivacy.hpp>

using namespace vthost;
using vthost::testing::DurableOp;
using vthost::testing::FaultOp;
using vthost::testing::FaultyFileSystem;
using vthost::testing::RecordedDurableCall;
using vthost::testing::RecordedRestrict;
using vthost::testing::RecordingDurableFiles;
using vthost::testing::RecordingFilePrivacy;
namespace fs = std::filesystem;

namespace
{

[[nodiscard]] fs::path stateDir()
{
    return fs::path { "/state/contour/sessions" };
}

[[nodiscard]] fs::path snapshotFile()
{
    return stateDir() / "default.snapshot";
}

[[nodiscard]] fs::path tempFile()
{
    return stateDir() / "default.snapshot.tmp";
}

[[nodiscard]] fs::path restoredFile()
{
    return stateDir() / "default.snapshot.restored";
}

[[nodiscard]] WorkspaceSnapshot sample(int seconds = 100)
{
    auto workspace = WorkspaceSnapshot {};
    workspace.takenAt = std::chrono::sys_seconds { std::chrono::seconds { seconds } };
    auto tab = TabSnapshot {};
    tab.title = "t";
    workspace.windows.push_back(WindowSnapshot { .tabs = { tab } });
    return workspace;
}

} // namespace

TEST_CASE("write makes the directory and the file private before any content lands",
          "[vthost][snapshot][store]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    auto privacy = RecordingFilePrivacy { files };
    auto durability = RecordingDurableFiles { files };
    auto const store = SessionSnapshotStore { files, privacy, durability, snapshotFile() };

    REQUIRE(store.write(sample()));
    CHECK(privacy.calls
          == std::vector<RecordedRestrict> { { stateDir(), PrivateEntry::Directory },
                                             { tempFile(), PrivateEntry::File } });
    CHECK(privacy.contentAtRestrict(tempFile()).empty()); // restricted while still empty
    CHECK_FALSE(files.exists(tempFile()));                // renamed away
    auto const loaded = store.load();
    CHECK(loaded.verdict == LoadVerdict::Restorable);
    CHECK(loaded.snapshot == sample());
}

TEST_CASE("write syncs the content before the rename, and the rename after it", "[vthost][snapshot][store]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    auto privacy = RecordingFilePrivacy { files };
    auto durability = RecordingDurableFiles { files };
    auto const store = SessionSnapshotStore { files, privacy, durability, snapshotFile() };

    REQUIRE(store.write(sample()));
    auto const bytes = encodeSnapshot(sample());
    auto const content = std::string { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
    CHECK(durability.calls
          == std::vector<RecordedDurableCall> { { DurableOp::CreateExclusive, tempFile() },
                                                { DurableOp::SyncFile, tempFile(), content },
                                                { DurableOp::Replace, tempFile(), content },
                                                { DurableOp::SyncDirectory, stateDir() } });
}

TEST_CASE("write drops a temporary file a crash left, and never follows one planted there",
          "[vthost][snapshot][store]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    auto privacy = RecordingFilePrivacy { files };
    auto durability = RecordingDurableFiles { files };
    auto const store = SessionSnapshotStore { files, privacy, durability, snapshotFile() };
    REQUIRE(files.createDirectories(stateDir()));
    REQUIRE(files.writeFile(tempFile(), "left by a crash"));

    REQUIRE(store.write(sample()));
    CHECK(store.load().snapshot == sample());
    CHECK_FALSE(files.exists(tempFile()));
}

TEST_CASE("a failed sync keeps the previous snapshot", "[vthost][snapshot][store]")
{
    struct Row
    {
        DurableOp failing;
        StoreStep step;
        int left; ///< The snapshot on file afterwards.
    };
    for (auto const row: { Row { DurableOp::SyncFile, StoreStep::SyncTemporary, 1 },
                           Row { DurableOp::Replace, StoreStep::Replace, 1 },
                           // The rename landed; only whether it survives a power loss is in doubt.
                           Row { DurableOp::SyncDirectory, StoreStep::SyncDirectory, 2 } })
    {
        auto files = core::platform::testing::InMemoryFileSystem {};
        auto privacy = RecordingFilePrivacy { files };
        auto durability = RecordingDurableFiles { files };
        auto const store = SessionSnapshotStore { files, privacy, durability, snapshotFile() };
        REQUIRE(store.write(sample(1)));
        durability.failing = row.failing;
        auto const result = store.write(sample(2));
        REQUIRE_FALSE(result);
        CHECK(result.error().step == row.step);
        CHECK(store.load().snapshot == sample(row.left));
        CHECK_FALSE(files.exists(tempFile()));
    }
}

TEST_CASE("a failed write keeps the previous snapshot and leaves no temporary file",
          "[vthost][snapshot][store]")
{
    SECTION("the content cannot be written")
    {
        auto failing = false;
        auto files = FaultyFileSystem { [&](FaultOp op, fs::path const& path) {
            return failing && op == FaultOp::WriteFile && path == tempFile();
        } };
        auto privacy = RecordingFilePrivacy { files };
        auto durability = RecordingDurableFiles { files };
        auto const store = SessionSnapshotStore { files, privacy, durability, snapshotFile() };
        REQUIRE(store.write(sample(1)));
        failing = true;
        auto const result = store.write(sample(2));
        REQUIRE_FALSE(result);
        CHECK(result.error().step == StoreStep::WriteTemporary);
        CHECK(store.load().snapshot == sample(1));
        CHECK_FALSE(files.exists(tempFile()));
    }
    SECTION("the rename fails")
    {
        auto failing = false;
        auto files = FaultyFileSystem { [&](FaultOp op, fs::path const& path) {
            return failing && op == FaultOp::Rename && path == snapshotFile();
        } };
        auto privacy = RecordingFilePrivacy { files };
        auto durability = RecordingDurableFiles { files };
        auto const store = SessionSnapshotStore { files, privacy, durability, snapshotFile() };
        REQUIRE(store.write(sample(1)));
        failing = true;
        auto const result = store.write(sample(2));
        REQUIRE_FALSE(result);
        CHECK(result.error().step == StoreStep::Replace);
        CHECK(store.load().snapshot == sample(1));
        CHECK_FALSE(files.exists(tempFile()));
    }
    SECTION("the file cannot be restricted")
    {
        auto files = core::platform::testing::InMemoryFileSystem {};
        auto privacy = RecordingFilePrivacy { files };
        auto durability = RecordingDurableFiles { files };
        privacy.failure = "denied";
        auto const store = SessionSnapshotStore { files, privacy, durability, snapshotFile() };
        auto const result = store.write(sample());
        REQUIRE_FALSE(result);
        CHECK(result.error().step == StoreStep::Restrict);
        CHECK_FALSE(files.exists(snapshotFile()));
        CHECK_FALSE(files.exists(tempFile()));
    }
}

TEST_CASE("load reports each state of the file", "[vthost][snapshot][store]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    auto privacy = RecordingFilePrivacy { files };
    auto durability = RecordingDurableFiles { files };
    auto const store = SessionSnapshotStore { files, privacy, durability, snapshotFile() };

    SECTION("no file")
    {
        CHECK(store.load().verdict == LoadVerdict::Absent);
    }
    SECTION("a cleared snapshot")
    {
        REQUIRE(store.clear());
        auto const loaded = store.load();
        CHECK(loaded.verdict == LoadVerdict::Empty);
        CHECK_FALSE(loaded.snapshot);
    }
    SECTION("garbage is set aside")
    {
        REQUIRE(files.writeFile(snapshotFile(), "this is not a snapshot"));
        auto const loaded = store.load();
        CHECK(loaded.verdict == LoadVerdict::SetAside);
        CHECK(loaded.detail == describe(SnapshotError::BadMagic));
        CHECK(files.exists(snapshotFile().string() + ".corrupt"));
        CHECK_FALSE(files.exists(snapshotFile()));
    }
    SECTION("a second corrupt file replaces the first set-aside copy")
    {
        REQUIRE(files.writeFile(snapshotFile(), "first"));
        (void) store.load();
        REQUIRE(files.writeFile(snapshotFile(), "second"));
        (void) store.load();
        CHECK(files.readFile(snapshotFile().string() + ".corrupt").value() == "second");
    }
    SECTION("a newer format is left in place")
    {
        REQUIRE(store.write(sample()));
        auto bytes = files.readFile(snapshotFile()).value();
        bytes[8] = '\x02';
        REQUIRE(files.writeFile(snapshotFile(), bytes));
        auto const loaded = store.load();
        CHECK(loaded.verdict == LoadVerdict::Unsupported);
        CHECK(files.readFile(snapshotFile()).value() == bytes);
        CHECK_FALSE(files.exists(snapshotFile().string() + ".corrupt"));
    }
    SECTION("a file past the size limit is set aside unread")
    {
        REQUIRE(files.writeFile(snapshotFile(), std::string(MaxSnapshotFileBytes + 1, 'x')));
        CHECK(store.load().verdict == LoadVerdict::SetAside);
        CHECK_FALSE(files.exists(snapshotFile()));
    }
    SECTION("a workspace is restorable")
    {
        REQUIRE(store.write(sample()));
        auto const loaded = store.load();
        CHECK(loaded.verdict == LoadVerdict::Restorable);
        CHECK(loaded.source == RestoreSource::Snapshot);
        CHECK(loaded.droppedHistories == 0);
    }
}

TEST_CASE("a restore keeps its file until the restored workspace is written", "[vthost][snapshot][store]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    auto privacy = RecordingFilePrivacy { files };
    auto durability = RecordingDurableFiles { files };
    auto const store = SessionSnapshotStore { files, privacy, durability, snapshotFile() };
    REQUIRE(store.write(sample()));
    auto const loaded = store.load();
    REQUIRE(loaded.verdict == LoadVerdict::Restorable);

    REQUIRE(store.beginRestore(loaded.source));
    CHECK_FALSE(files.exists(snapshotFile()));
    CHECK(files.exists(restoredFile()));

    REQUIRE(store.endRestore(loaded.source, RestoreOutcome::Realized));
    REQUIRE(store.write(sample(200)));
    CHECK(files.exists(restoredFile()));

    REQUIRE(store.confirmRestored());
    CHECK_FALSE(files.exists(restoredFile()));
    CHECK(store.load().snapshot == sample(200));
}

TEST_CASE("a restore that realized nothing is retried once, then set aside", "[vthost][snapshot][store]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    auto privacy = RecordingFilePrivacy { files };
    auto durability = RecordingDurableFiles { files };
    auto const store = SessionSnapshotStore { files, privacy, durability, snapshotFile() };
    REQUIRE(store.write(sample()));
    REQUIRE(store.beginRestore(RestoreSource::Snapshot));
    REQUIRE(store.endRestore(RestoreSource::Snapshot, RestoreOutcome::NothingRealized));
    REQUIRE(store.write(sample(300))); // the startup layout

    auto const retry = store.load();
    CHECK(retry.source == RestoreSource::PendingRestore);
    CHECK(retry.snapshot == sample());

    REQUIRE(store.beginRestore(retry.source));
    REQUIRE(store.endRestore(retry.source, RestoreOutcome::NothingRealized));
    CHECK(files.exists(snapshotFile().string() + ".failed"));
    CHECK_FALSE(files.exists(restoredFile()));
    CHECK(store.load().snapshot == sample(300));
}

TEST_CASE("a damaged retry copy does not hide the current snapshot", "[vthost][snapshot][store]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    auto privacy = RecordingFilePrivacy { files };
    auto durability = RecordingDurableFiles { files };
    auto const store = SessionSnapshotStore { files, privacy, durability, snapshotFile() };
    REQUIRE(store.write(sample(5)));
    REQUIRE(files.writeFile(restoredFile(), "damaged"));
    auto const loaded = store.load();
    CHECK(loaded.verdict == LoadVerdict::Restorable);
    CHECK(loaded.source == RestoreSource::Snapshot);
    CHECK(files.exists(restoredFile().string() + ".corrupt"));
}

TEST_CASE("sessionSnapshotPath puts one file per label under sessions/", "[vthost][snapshot][store]")
{
    CHECK(sessionSnapshotPath("/s/contour", "default")
          == fs::path { "/s/contour/sessions/default.snapshot" });
    CHECK(sessionSnapshotPath("/s/contour", "work") == fs::path { "/s/contour/sessions/work.snapshot" });
    for (auto const label: { std::string_view { "../x" },
                             std::string_view { "a/b" },
                             std::string_view { "a\\b" },
                             std::string_view { "" },
                             std::string_view { ".." },
                             std::string_view { "C:x" },
                             std::string_view { "a:" },
                             std::string_view { "a\0b", 3 } })
        CHECK_FALSE(sessionSnapshotPath("/s/contour", label));
}

TEST_CASE("daemonSnapshotPath keys a daemon at a socket of its own by that socket",
          "[vthost][snapshot][store]")
{
    auto const own = fs::path { "/run/user/1000/contour/default" };
    SECTION("the label's own socket keeps the label's file")
    {
        CHECK(daemonSnapshotPath("/s/contour", "default", own, own)
              == fs::path { "/s/contour/sessions/default.snapshot" });
        CHECK(daemonSnapshotPath("/s/contour", "default", "/run/user/1000/contour/./default", own)
              == fs::path { "/s/contour/sessions/default.snapshot" });
    }
    SECTION("two daemons on one label at different sockets keep different files")
    {
        auto const first = daemonSnapshotPath("/s/contour", "default", "/tmp/x", own);
        auto const second = daemonSnapshotPath("/s/contour", "default", "/tmp/y", own);
        REQUIRE(first);
        REQUIRE(second);
        CHECK(*first != *second);
        CHECK(first->parent_path() == fs::path { "/s/contour/sessions" });
        CHECK(first->filename().string().starts_with("default@"));
        CHECK(first->filename().string().ends_with(".snapshot"));
        CHECK(*first != fs::path { "/s/contour/sessions/default.snapshot" });
        // Stable: the same socket names the same file at the next start, and for `daemon stop --socket`.
        CHECK(daemonSnapshotPath("/s/contour", "default", "/tmp/x", own) == first);
    }
    SECTION("a label no file can be named for")
    {
        CHECK_FALSE(daemonSnapshotPath("/s/contour", "a/b", "/tmp/x", own));
    }
}

TEST_CASE("discard empties the snapshot and drops a pending restore", "[vthost][snapshot][store]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    auto privacy = RecordingFilePrivacy { files };
    auto durability = RecordingDurableFiles { files };
    auto const store = SessionSnapshotStore { files, privacy, durability, snapshotFile() };
    REQUIRE(store.write(sample()));

    SECTION("a plain snapshot")
    {
        REQUIRE(store.discard());
        CHECK(store.load().verdict == LoadVerdict::Empty);
    }
    SECTION("a restore kept for confirmation, which load() would prefer")
    {
        REQUIRE(store.beginRestore(RestoreSource::Snapshot));
        REQUIRE(files.exists(restoredFile()));
        REQUIRE(store.discard());
        CHECK_FALSE(files.exists(restoredFile()));
        CHECK(store.load().verdict == LoadVerdict::Empty);
    }
    SECTION("no snapshot at all")
    {
        auto const fresh = SessionSnapshotStore { files, privacy, durability, stateDir() / "other.snapshot" };
        REQUIRE(fresh.discard());
        CHECK(fresh.load().verdict == LoadVerdict::Empty);
    }
}
