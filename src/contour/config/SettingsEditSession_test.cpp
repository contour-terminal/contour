// SPDX-License-Identifier: Apache-2.0
#include <contour/config/GuiConfigStore.hpp>
#include <contour/config/SettingsEditSession.hpp>
#include <contour/test/ManualSaveScheduler.hpp>

#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <vthost/testing/FaultyFileSystem.hpp>
#include <vthost/testing/RecordingDurableFiles.hpp>

using contour::config::FileGuiConfigStore;
using contour::config::guiSettingsFile;
using contour::config::PendingChange;
using contour::config::profileFile;
using contour::config::SettingsEditability;
using contour::config::SettingsEditSession;
using contour::config::SettingsError;
using contour::config::SettingsErrorKind;
using contour::config::SettingsSaveDebounce;
using contour::config::StagedFile;
using contour::test::ManualSaveScheduler;

namespace
{
/// Where contour.yml and its side files live in these tests.
[[nodiscard]] std::filesystem::path configDir()
{
    return std::filesystem::path { "/config" };
}

/// The side file of profile `main`, relative to configDir().
[[nodiscard]] std::filesystem::path mainProfile()
{
    return std::filesystem::path { "profiles" } / "main.yml";
}

/// A session over @p fileSystem with a hand-fired scheduler, counting its reloads and collecting what it
/// reports.
struct SessionFixture
{
    FileGuiConfigStore store;
    ManualSaveScheduler scheduler;
    int reloads = 0;
    std::vector<SettingsError> reported;
    SettingsEditSession session;

    explicit SessionFixture(core::platform::FileSystem const& fileSystem,
                            SettingsEditability editability = SettingsEditability::Editable):
        store { fileSystem, configDir() },
        session { store,
                  scheduler,
                  editability,
                  [this]() -> std::expected<void, std::string> {
                      ++reloads;
                      return {};
                  },
                  [this](SettingsError const& error) { reported.push_back(error); } }
    {
    }
};

/// The bytes of @p relative under configDir(); nullopt when there is no such file.
[[nodiscard]] std::optional<std::string> contentOf(core::platform::FileSystem const& fileSystem,
                                                   std::filesystem::path const& relative)
{
    auto const read = fileSystem.readFile(configDir() / relative);
    return read ? std::optional { *read } : std::nullopt;
}

/// Writes @p content to @p relative under configDir(), as an editor would.
void handEdit(core::platform::FileSystem const& fileSystem,
              std::filesystem::path const& relative,
              std::string_view content)
{
    REQUIRE(fileSystem.writeFile(configDir() / relative, content).has_value());
}

} // namespace

TEST_CASE("SettingsEditSession.debounceCoalesces", "[settingssession]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    auto fx = SessionFixture { fileSystem };

    for (auto const* bytes: { "a", "b", "c" })
        REQUIRE(fx.session.stage(profileFile("main"), std::string { bytes }).has_value());

    CHECK(contentOf(fileSystem, mainProfile()) == std::nullopt);
    CHECK(fx.scheduler.pendingDelay() == SettingsSaveDebounce);
    CHECK(SettingsSaveDebounce == std::chrono::milliseconds { 300 });
    CHECK(fx.scheduler.scheduleCount() == 3);
    CHECK(fx.session.hasPending());
    CHECK(fx.session.pendingFor(profileFile("main")) == PendingChange::Write);
    CHECK(fx.session.pendingFor(guiSettingsFile()) == PendingChange::None);

    fx.scheduler.fire();

    CHECK(contentOf(fileSystem, mainProfile()) == "c");
    CHECK(fx.reloads == 1);
    CHECK_FALSE(fx.session.hasPending());
    CHECK(fx.session.hasChanges());
    CHECK(fx.reported.empty());
}

TEST_CASE("SettingsEditSession.revertRestoresOnlyTheFilesItTouched", "[settingssession]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    handEdit(fileSystem, "settings.yml", "s0");
    handEdit(fileSystem, mainProfile(), "p0");
    handEdit(fileSystem, "contour.yml", "c0");
    auto fx = SessionFixture { fileSystem };

    REQUIRE(fx.session.stage(profileFile("main"), "p1").has_value());
    REQUIRE(fx.session.flush().has_value());
    REQUIRE(fx.session.stage(profileFile("New Profile"), "n1").has_value());
    REQUIRE(fx.session.flush().has_value());
    handEdit(fileSystem, "contour.yml", "c1");
    REQUIRE(contentOf(fileSystem, std::filesystem::path { "profiles" } / "New Profile.yml") == "n1");

    REQUIRE(fx.session.revert().has_value());

    CHECK(contentOf(fileSystem, mainProfile()) == "p0");
    CHECK(contentOf(fileSystem, std::filesystem::path { "profiles" } / "New Profile.yml") == std::nullopt);
    CHECK(contentOf(fileSystem, "contour.yml") == "c1");
    CHECK(contentOf(fileSystem, "settings.yml") == "s0");
    CHECK_FALSE(fx.session.hasChanges());
    CHECK(fx.reloads == 3);
    CHECK(fx.reported.empty());
}

TEST_CASE("SettingsEditSession.aFileChangedOnDiskIsNotOverwritten", "[settingssession]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    handEdit(fileSystem, mainProfile(), "p0");
    auto fx = SessionFixture { fileSystem };
    REQUIRE(fx.session.stage(profileFile("main"), "p1").has_value());
    REQUIRE(fx.session.flush().has_value());
    auto const reloadsBefore = fx.reloads;

    handEdit(fileSystem, mainProfile(), "vim");
    REQUIRE(fx.session.stage(profileFile("main"), "p2").has_value());
    fx.scheduler.fire();

    CHECK(contentOf(fileSystem, mainProfile()) == "vim");
    REQUIRE(fx.reported.size() == 1);
    CHECK(fx.reported.front().kind == SettingsErrorKind::ChangedOnDisk);
    CHECK(fx.reported.front().message.contains("profiles/main.yml"));
    CHECK(fx.reloads == reloadsBefore + 1);
    CHECK_FALSE(fx.session.hasChanges()); // the disk's version is the new base

    REQUIRE(fx.session.revert().has_value());
    CHECK(contentOf(fileSystem, mainProfile()) == "vim");
}

TEST_CASE("SettingsEditSession.twoSessionsOverOneFileNeverClobber", "[settingssession]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    handEdit(fileSystem, mainProfile(), "p0");
    auto a = SessionFixture { fileSystem };
    auto b = SessionFixture { fileSystem };

    // Both stage, and so snapshot p0, before either flushes.
    REQUIRE(a.session.stage(profileFile("main"), "a").has_value());
    REQUIRE(b.session.stage(profileFile("main"), "b").has_value());
    REQUIRE(a.session.flush().has_value());

    auto const flushed = b.session.flush();

    REQUIRE_FALSE(flushed.has_value());
    CHECK(flushed.error().kind == SettingsErrorKind::ChangedOnDisk);
    CHECK(contentOf(fileSystem, mainProfile()) == "a");
}

TEST_CASE("SettingsEditSession.aFailedWriteDropsThePendingBytes", "[settingssession]")
{
    auto const target = configDir() / mainProfile();
    auto const fileSystem =
        vthost::testing::FaultyFileSystem { [&](auto op, std::filesystem::path const& path) {
            return op == vthost::testing::FaultOp::Rename && path == target;
        } };
    handEdit(fileSystem, mainProfile(), "p0");
    auto fx = SessionFixture { fileSystem };
    REQUIRE(fx.session.stage(profileFile("main"), "p1").has_value());

    auto const flushed = fx.session.flush();

    REQUIRE_FALSE(flushed.has_value());
    CHECK(flushed.error().kind == SettingsErrorKind::WriteFailed);
    CHECK(flushed.error().message.contains("Permission denied"));
    CHECK(flushed.error().message.starts_with("Couldn't save profiles/main.yml: "));
    CHECK_FALSE(fx.session.hasPending());
    CHECK_FALSE(fx.session.hasChanges());
    CHECK(contentOf(fileSystem, mainProfile()) == "p0");
    CHECK(fx.reloads == 0);
}

TEST_CASE("SettingsEditSession.lockedStagesNothing", "[settingssession]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    auto fx = SessionFixture { fileSystem, SettingsEditability::Locked };

    auto const staged = fx.session.stage(guiSettingsFile(), "x");

    REQUIRE_FALSE(staged.has_value());
    CHECK(staged.error().kind == SettingsErrorKind::Locked);
    CHECK(staged.error().message == "Settings are locked by gui_config_locked in contour.yml.");
    CHECK(fx.scheduler.pendingDelay() == std::nullopt);
    CHECK_FALSE(fx.session.hasChanges());
}

TEST_CASE("SettingsEditSession.revertWithNothingTouchedIsANoOp", "[settingssession]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    auto fx = SessionFixture { fileSystem };

    CHECK(fx.session.revert().has_value());
    CHECK(fx.reloads == 0);
}

TEST_CASE("SettingsEditSession: a flush with nothing staged does not reload", "[settingssession]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    auto fx = SessionFixture { fileSystem };

    CHECK(fx.session.flush().has_value());
    CHECK(fx.reloads == 0);
}

TEST_CASE("SettingsEditSession: staging a removal deletes the file, and Revert brings it back",
          "[settingssession]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    handEdit(fileSystem, mainProfile(), "p0");
    auto fx = SessionFixture { fileSystem };

    REQUIRE(fx.session.stage(profileFile("main"), std::nullopt).has_value());
    REQUIRE(fx.session.flush().has_value());
    CHECK(contentOf(fileSystem, mainProfile()) == std::nullopt);
    CHECK(fx.session.hasChanges());

    REQUIRE(fx.session.revert().has_value());
    CHECK(contentOf(fileSystem, mainProfile()) == "p0");
}

TEST_CASE("SettingsEditSession: a direct flush returns the first error and reports the rest",
          "[settingssession]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    handEdit(fileSystem, mainProfile(), "p0");
    handEdit(fileSystem, "settings.yml", "s0");
    auto fx = SessionFixture { fileSystem };
    REQUIRE(fx.session.stage(profileFile("main"), "p1").has_value());
    REQUIRE(fx.session.stage(guiSettingsFile(), "s1").has_value());
    handEdit(fileSystem, mainProfile(), "vim");
    handEdit(fileSystem, "settings.yml", "nano");

    auto const flushed = fx.session.flush();

    REQUIRE_FALSE(flushed.has_value());
    CHECK(flushed.error().kind == SettingsErrorKind::ChangedOnDisk);
    REQUIRE(fx.reported.size() == 1);
    CHECK(fx.reported.front().kind == SettingsErrorKind::ChangedOnDisk);
    CHECK(fx.reloads == 1);
}

TEST_CASE("SettingsEditSession: flushing one file's change leaves the other pending changes alone",
          "[settingssession]")
{
    auto const target = configDir() / mainProfile();
    auto const fileSystem =
        vthost::testing::FaultyFileSystem { [&](auto op, std::filesystem::path const& path) {
            return op == vthost::testing::FaultOp::Rename && path == target;
        } };
    handEdit(fileSystem, mainProfile(), "p0");
    handEdit(fileSystem, "settings.yml", "s0");
    auto fx = SessionFixture { fileSystem };
    REQUIRE(fx.session.stage(profileFile("main"), "p1").has_value());
    REQUIRE(fx.session.stage(guiSettingsFile(), "s1").has_value());

    auto const flushed = fx.session.flushChangeOf(guiSettingsFile());

    // The unwritable profile is a different change: it neither fails this flush nor is written by it.
    REQUIRE(flushed.has_value());
    CHECK(contentOf(fileSystem, "settings.yml") == "s1");
    CHECK(fx.session.pendingFor(guiSettingsFile()) == PendingChange::None);
    CHECK(fx.session.pendingFor(profileFile("main")) == PendingChange::Write);
    CHECK(fx.reloads == 1);
    CHECK(fx.reported.empty());
}

TEST_CASE("SettingsEditSession: flushing one file's change writes the files staged with it",
          "[settingssession]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    handEdit(fileSystem, mainProfile(), "p0");
    handEdit(fileSystem, "settings.yml", "s0");
    auto fx = SessionFixture { fileSystem };
    auto const together = std::array {
        StagedFile { .file = guiSettingsFile(), .bytes = "s1" },
        StagedFile { .file = profileFile("main"), .bytes = "p1" },
    };
    REQUIRE(fx.session.stageTogether(together).has_value());

    REQUIRE(fx.session.flushChangeOf(guiSettingsFile()).has_value());

    CHECK(contentOf(fileSystem, "settings.yml") == "s1");
    CHECK(contentOf(fileSystem, mainProfile()) == "p1");
    CHECK_FALSE(fx.session.hasPending());
    CHECK(fx.reloads == 1);
}

TEST_CASE("SettingsEditSession: a file observed when its draft was seeded is not written over",
          "[settingssession]")
{
    // I1: the draft was seeded from p0; an editor wrote "vim" before the first commit staged anything.
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    handEdit(fileSystem, mainProfile(), "p0");
    auto fx = SessionFixture { fileSystem };
    fx.session.observe(profileFile("main"), "p0");
    CHECK_FALSE(fx.session.hasChanges());

    handEdit(fileSystem, mainProfile(), "vim");
    REQUIRE(fx.session.stage(profileFile("main"), "p1").has_value());
    auto const flushed = fx.session.flush();

    REQUIRE_FALSE(flushed.has_value());
    CHECK(flushed.error().kind == SettingsErrorKind::ChangedOnDisk);
    CHECK(contentOf(fileSystem, mainProfile()) == "vim");
}

TEST_CASE("SettingsEditSession: a hand edit made before the draft opened is not written over",
          "[settingssession]")
{
    // C1: the configuration loaded p0; an editor wrote "vim"; only then did a draft open, seeded from p0.
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    handEdit(fileSystem, mainProfile(), "vim");
    auto fx = SessionFixture { fileSystem };
    fx.session.observe(profileFile("main"), "p0");

    REQUIRE(fx.session.stage(profileFile("main"), "p1").has_value());
    auto const flushed = fx.session.flush();

    REQUIRE_FALSE(flushed.has_value());
    CHECK(flushed.error().kind == SettingsErrorKind::ChangedOnDisk);
    CHECK(contentOf(fileSystem, mainProfile()) == "vim");
}

TEST_CASE("SettingsEditSession: a draft re-seeded from a reload of a hand edit adopts it",
          "[settingssession]")
{
    // I3: the reload read the hand edit and the draft shows it; nothing is pending, so it is the new base.
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    handEdit(fileSystem, mainProfile(), "p0");
    auto fx = SessionFixture { fileSystem };
    REQUIRE(fx.session.stage(profileFile("main"), "p1").has_value());
    REQUIRE(fx.session.flush().has_value());
    handEdit(fileSystem, mainProfile(), "vim");

    fx.session.observe(profileFile("main"), "vim");
    REQUIRE(fx.session.stage(profileFile("main"), "vim2").has_value());

    CHECK(fx.session.flush().has_value());
    CHECK(contentOf(fileSystem, mainProfile()) == "vim2");
    // Revert goes back to the version the session adopted, not over it.
    CHECK(fx.session.revert().has_value());
    CHECK(contentOf(fileSystem, mainProfile()) == "vim");
}

TEST_CASE("SettingsEditSession: a re-seed while a change is pending keeps what the session saw",
          "[settingssession]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    handEdit(fileSystem, mainProfile(), "p0");
    auto fx = SessionFixture { fileSystem };
    REQUIRE(fx.session.stage(profileFile("main"), "p1").has_value());
    handEdit(fileSystem, mainProfile(), "vim");

    fx.session.observe(profileFile("main"), "vim");

    auto const flushed = fx.session.flush();
    REQUIRE_FALSE(flushed.has_value());
    CHECK(flushed.error().kind == SettingsErrorKind::ChangedOnDisk);
    CHECK(contentOf(fileSystem, mainProfile()) == "vim");
}

TEST_CASE("SettingsEditSession: observing a file already touched keeps what the session saw",
          "[settingssession]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    handEdit(fileSystem, mainProfile(), "p0");
    auto fx = SessionFixture { fileSystem };
    REQUIRE(fx.session.stage(profileFile("main"), "p1").has_value());
    REQUIRE(fx.session.flush().has_value());
    handEdit(fileSystem, mainProfile(), "vim");

    // A re-seed from what was loaded before the hand edit must not launder it.
    fx.session.observe(profileFile("main"), "p1");
    REQUIRE(fx.session.stage(profileFile("main"), "p2").has_value());

    auto const flushed = fx.session.flush();
    REQUIRE_FALSE(flushed.has_value());
    CHECK(flushed.error().kind == SettingsErrorKind::ChangedOnDisk);
    CHECK(contentOf(fileSystem, mainProfile()) == "vim");
}

TEST_CASE("SettingsEditSession: Revert adopts a file changed on disk", "[settingssession]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    handEdit(fileSystem, mainProfile(), "p0");
    auto fx = SessionFixture { fileSystem };
    REQUIRE(fx.session.stage(profileFile("main"), "p1").has_value());
    REQUIRE(fx.session.flush().has_value());
    handEdit(fileSystem, mainProfile(), "vim");

    auto const reverted = fx.session.revert();

    REQUIRE_FALSE(reverted.has_value());
    CHECK(reverted.error().kind == SettingsErrorKind::ChangedOnDisk);
    CHECK(contentOf(fileSystem, mainProfile()) == "vim");
    CHECK_FALSE(fx.session.hasChanges());
}

TEST_CASE(
    "SettingsEditSession: bytes that reached the disk are what the session last saw, even when the sync "
    "failed",
    "[settingssession]")
{
    // m2: the replace took, the directory sync did not. The write is reported, and the next flush does not
    // take Settings' own bytes for a change made outside it.
    auto fileSystem = vthost::testing::RecordingDurableFiles {};
    handEdit(fileSystem, mainProfile(), "p0");
    fileSystem.failing = vthost::testing::DurableOp::SyncDirectory;
    auto fx = SessionFixture { fileSystem };

    REQUIRE(fx.session.stage(profileFile("main"), "p1").has_value());
    auto const first = fx.session.flush();
    REQUIRE_FALSE(first.has_value());
    CHECK(first.error().kind == SettingsErrorKind::WriteFailed);
    REQUIRE(contentOf(fileSystem, mainProfile()) == "p1");

    fileSystem.failing.reset();
    REQUIRE(fx.session.stage(profileFile("main"), "p2").has_value());
    CHECK(fx.session.flush().has_value());
    CHECK(contentOf(fileSystem, mainProfile()) == "p2");
}

TEST_CASE("SettingsEditSession: files staged together are written together or not at all",
          "[settingssession]")
{
    // m7: a rename writes the new file and removes the old one, as one change.
    auto const newProfile = std::filesystem::path { "profiles" } / "office.yml";
    auto const rename = std::array {
        StagedFile { .file = profileFile("office"), .bytes = std::string { "p0" } },
        StagedFile { .file = profileFile("main"), .bytes = std::nullopt },
    };

    SECTION("both land")
    {
        auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
        handEdit(fileSystem, mainProfile(), "p0");
        auto fx = SessionFixture { fileSystem };
        REQUIRE(fx.session.stageTogether(rename).has_value());
        REQUIRE(fx.session.flush().has_value());
        CHECK(contentOf(fileSystem, newProfile) == "p0");
        CHECK(contentOf(fileSystem, mainProfile()) == std::nullopt);
        CHECK(fx.reloads == 1);
    }

    SECTION("one changed on disk: none is written")
    {
        auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
        handEdit(fileSystem, mainProfile(), "p0");
        auto fx = SessionFixture { fileSystem };
        REQUIRE(fx.session.stageTogether(rename).has_value());
        handEdit(fileSystem, mainProfile(), "vim");

        auto const flushed = fx.session.flush();
        REQUIRE_FALSE(flushed.has_value());
        CHECK(flushed.error().kind == SettingsErrorKind::ChangedOnDisk);
        CHECK(contentOf(fileSystem, mainProfile()) == "vim");
        CHECK(contentOf(fileSystem, newProfile) == std::nullopt); // no duplicate beside the hand edit
    }

    SECTION("the first write fails: the rest is not done")
    {
        auto const target = configDir() / newProfile;
        auto const fileSystem = vthost::testing::FaultyFileSystem {
            [&](auto op, std::filesystem::path const& path) {
                return op == vthost::testing::FaultOp::Rename && path == target;
            },
        };
        handEdit(fileSystem, mainProfile(), "p0");
        auto fx = SessionFixture { fileSystem };
        REQUIRE(fx.session.stageTogether(rename).has_value());

        auto const flushed = fx.session.flush();
        REQUIRE_FALSE(flushed.has_value());
        CHECK(flushed.error().kind == SettingsErrorKind::WriteFailed);
        CHECK(contentOf(fileSystem, mainProfile()) == "p0"); // the profile is not lost
        CHECK(contentOf(fileSystem, newProfile) == std::nullopt);
        CHECK_FALSE(fx.session.hasChanges());
    }
}

TEST_CASE("SettingsEditSession: a file restaged after a rename stays in the rename", "[settingssession]")
{
    // I1: the new file of a rename is edited again before the flush; its write still fails first.
    auto const newProfile = std::filesystem::path { "profiles" } / "office.yml";
    auto const target = configDir() / newProfile;
    auto const fileSystem = vthost::testing::FaultyFileSystem {
        [&](auto op, std::filesystem::path const& path) {
            return op == vthost::testing::FaultOp::Rename && path == target;
        },
    };
    handEdit(fileSystem, mainProfile(), "p0");
    auto fx = SessionFixture { fileSystem };
    auto const rename = std::array {
        StagedFile { .file = profileFile("office"), .bytes = std::string { "p0" } },
        StagedFile { .file = profileFile("main"), .bytes = std::nullopt },
    };
    REQUIRE(fx.session.stageTogether(rename).has_value());
    REQUIRE(fx.session.stage(profileFile("office"), "p1").has_value());

    auto const flushed = fx.session.flush();

    REQUIRE_FALSE(flushed.has_value());
    CHECK(flushed.error().kind == SettingsErrorKind::WriteFailed);
    CHECK(contentOf(fileSystem, mainProfile()) == "p0"); // the profile is not lost
    CHECK(contentOf(fileSystem, newProfile) == std::nullopt);
}

TEST_CASE("SettingsEditSession: a change touching another merges with it", "[settingssession]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    handEdit(fileSystem, mainProfile(), "p0");
    auto fx = SessionFixture { fileSystem };
    auto const first = std::array {
        StagedFile { .file = profileFile("office"), .bytes = std::string { "p0" } },
        StagedFile { .file = profileFile("main"), .bytes = std::nullopt },
    };
    auto const second = std::array {
        StagedFile { .file = profileFile("office"), .bytes = std::string { "p1" } },
        StagedFile { .file = guiSettingsFile(), .bytes = std::string { "default_profile: office\n" } },
    };
    REQUIRE(fx.session.stageTogether(first).has_value());
    REQUIRE(fx.session.stageTogether(second).has_value());
    handEdit(fileSystem, "settings.yml", "theme: dark\n"); // one of them changed on disk: none is written

    auto const flushed = fx.session.flush();
    REQUIRE_FALSE(flushed.has_value());
    CHECK(flushed.error().kind == SettingsErrorKind::ChangedOnDisk);
    CHECK(contentOf(fileSystem, mainProfile()) == "p0");
    CHECK(contentOf(fileSystem, std::filesystem::path { "profiles" } / "office.yml") == std::nullopt);
}

TEST_CASE("SettingsEditSession: a failed removal rolls back what the change wrote", "[settingssession]")
{
    // m4: the rename's new file is written, the old one's removal fails, so the new one goes again.
    auto const newProfile = std::filesystem::path { "profiles" } / "office.yml";
    auto const fileSystem = vthost::testing::FaultyFileSystem {
        [&](auto op, std::filesystem::path const& path) {
            return op == vthost::testing::FaultOp::Remove && path == configDir() / mainProfile();
        },
    };
    handEdit(fileSystem, mainProfile(), "p0");
    auto fx = SessionFixture { fileSystem };
    auto const rename = std::array {
        StagedFile { .file = profileFile("office"), .bytes = std::string { "p0" } },
        StagedFile { .file = profileFile("main"), .bytes = std::nullopt },
    };
    REQUIRE(fx.session.stageTogether(rename).has_value());

    auto const flushed = fx.session.flush();

    REQUIRE_FALSE(flushed.has_value());
    CHECK(flushed.error().kind == SettingsErrorKind::WriteFailed);
    CHECK(contentOf(fileSystem, mainProfile()) == "p0");
    CHECK(contentOf(fileSystem, newProfile) == std::nullopt);
}

TEST_CASE("SettingsEditSession: what is pending for a file", "[settingssession]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    auto fx = SessionFixture { fileSystem };
    CHECK(fx.session.pendingFor(profileFile("main")) == PendingChange::None);
    REQUIRE(fx.session.stage(profileFile("main"), "p1").has_value());
    CHECK(fx.session.pendingFor(profileFile("main")) == PendingChange::Write);
    REQUIRE(fx.session.stage(profileFile("main"), std::nullopt).has_value());
    CHECK(fx.session.pendingFor(profileFile("main")) == PendingChange::Removal);
}
