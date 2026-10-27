// SPDX-License-Identifier: Apache-2.0
#include <contour/config/GuiConfigStore.hpp>
#include <contour/config/SettingsEditSession.hpp>
#include <contour/test/ManualSaveScheduler.hpp>

#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <vthost/testing/FaultyFileSystem.hpp>

using contour::config::FileGuiConfigStore;
using contour::config::guiSettingsFile;
using contour::config::profileFile;
using contour::config::SettingsEditability;
using contour::config::SettingsEditSession;
using contour::config::SettingsError;
using contour::config::SettingsErrorKind;
using contour::config::SettingsSaveDebounce;
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
    CHECK(fx.session.hasPendingFor(profileFile("main")));
    CHECK_FALSE(fx.session.hasPendingFor(guiSettingsFile()));

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
