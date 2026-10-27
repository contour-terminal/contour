// SPDX-License-Identifier: Apache-2.0
#include <contour/config/GuiStateStore.hpp>

#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using contour::config::FileGuiStateStore;
using contour::config::GuiState;
using contour::config::GuiStateError;
using contour::config::GuiStateFileName;
using contour::config::NewDefaultsOffer;
using contour::config::SettingsWindowState;
using contour::config::ShellKind;
using contour::config::WindowShowState;

namespace
{
/// The configuration directory of these tests.
[[nodiscard]] std::filesystem::path configDir()
{
    return std::filesystem::path { "/config" };
}

/// The GUI state file of these tests.
[[nodiscard]] std::filesystem::path stateFile()
{
    return configDir() / GuiStateFileName;
}

/// The backup of the GUI state file.
[[nodiscard]] std::filesystem::path backupFile()
{
    auto backup = stateFile();
    backup += contour::config::GuiStateBackupSuffix;
    return backup;
}

/// Writes @p content as the GUI state file on @p fileSystem.
void writeStateFile(core::platform::FileSystem const& fileSystem, std::string_view content)
{
    REQUIRE(fileSystem.writeFile(stateFile(), content).has_value());
}
} // namespace

TEST_CASE("GuiStateStore: a missing file is the empty state", "[config][guistate]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    auto const store = FileGuiStateStore { fileSystem, configDir() };
    auto const loaded = store.load();
    REQUIRE(loaded.has_value());
    CHECK(*loaded == GuiState {});
}

TEST_CASE("GuiStateStore: what is saved loads back", "[config][guistate]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    auto store = FileGuiStateStore { fileSystem, configDir() };
    auto const state = GuiState { .dismissedOffers = { NewDefaultsOffer::ExistingConfig,
                                                       NewDefaultsOffer::SettingsOverride },
                                  .settingsWindow = std::nullopt };
    REQUIRE(store.save(state).has_value());
    CHECK(fileSystem.exists(stateFile()));
    auto const loaded = FileGuiStateStore { fileSystem, configDir() }.load();
    REQUIRE(loaded.has_value());
    CHECK(*loaded == state);
}

TEST_CASE("GuiStateStore: offers that are not a list are malformed", "[config][guistate]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    writeStateFile(fileSystem, "dismissed_new_defaults: 7\n");
    auto const loaded = FileGuiStateStore { fileSystem, configDir() }.load();
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error() == GuiStateError::Malformed);
}

TEST_CASE("GuiStateStore: an unknown token is skipped, not an error", "[config][guistate]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    writeStateFile(fileSystem, "dismissed_new_defaults: [from_the_future, settings_override]\n");
    auto const loaded = FileGuiStateStore { fileSystem, configDir() }.load();
    REQUIRE(loaded.has_value());
    CHECK(*loaded
          == GuiState { .dismissedOffers = { NewDefaultsOffer::SettingsOverride },
                        .settingsWindow = std::nullopt });
}

TEST_CASE("GuiStateStore: saving over a malformed file backs it up first", "[config][guistate]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    auto const malformed = std::string_view { "dismissed_new_defaults: 7\n" };
    writeStateFile(fileSystem, malformed);
    auto store = FileGuiStateStore { fileSystem, configDir() };
    auto const state =
        GuiState { .dismissedOffers = { NewDefaultsOffer::ExistingConfig }, .settingsWindow = std::nullopt };
    REQUIRE(store.save(state).has_value());
    CHECK(fileSystem.readFile(backupFile())
          == std::expected<std::string, std::string> { std::string { malformed } });
    auto const loaded = store.load();
    REQUIRE(loaded.has_value());
    CHECK(*loaded == state);
}

TEST_CASE("GuiStateStore: a save keeps the tokens and keys this release does not know", "[config][guistate]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    writeStateFile(fileSystem, "dismissed_new_defaults: [from_the_future]\nwindow_hint: wide\n");
    auto store = FileGuiStateStore { fileSystem, configDir() };
    REQUIRE(store
                .save(GuiState { .dismissedOffers = { NewDefaultsOffer::ExistingConfig },
                                 .settingsWindow = std::nullopt })
                .has_value());
    auto const written = fileSystem.readFile(stateFile()).value_or(std::string {});
    INFO(written);
    CHECK(written.contains("[existing_config, from_the_future]"));
    CHECK(written.contains("window_hint: wide"));
    CHECK_FALSE(fileSystem.exists(backupFile()));
}

TEST_CASE("GuiStateStore: the Settings window's frame and page round-trip", "[config][guistate]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    auto store = FileGuiStateStore { fileSystem, configDir() };
    auto const state = GuiState {
        .dismissedOffers = { NewDefaultsOffer::ExistingConfig },
        .settingsWindow = SettingsWindowState { .x = 300,
                                                .y = -20,
                                                .width = 900,
                                                .height = 700,
                                                .screen = "DELL U2720Q",
                                                .show = WindowShowState::Maximized,
                                                .page = "section:appearance" },
    };
    REQUIRE(store.save(state).has_value());
    auto const written = fileSystem.readFile(stateFile()).value_or(std::string {});
    INFO(written);
    CHECK(written.contains("settings_window:"));
    CHECK(written.contains("maximized"));

    auto const loaded = FileGuiStateStore { fileSystem, configDir() }.load();
    REQUIRE(loaded.has_value());
    CHECK(*loaded == state);

    // A save without it drops it: the state is what was saved.
    REQUIRE(store.save(GuiState { .dismissedOffers = {}, .settingsWindow = std::nullopt }).has_value());
    CHECK_FALSE(store.load()->settingsWindow.has_value());
}

TEST_CASE("GuiStateStore: a file without the Settings window loads without it", "[config][guistate]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    writeStateFile(fileSystem, "dismissed_new_defaults: [existing_config]\n");
    auto const loaded = FileGuiStateStore { fileSystem, configDir() }.load();
    REQUIRE(loaded.has_value());
    CHECK(loaded->settingsWindow == std::nullopt);
}

TEST_CASE("GuiStateStore: an unknown show word loads as Normal", "[config][guistate]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    writeStateFile(fileSystem,
                   "settings_window:\n"
                   "    x: 10\n    y: 20\n    width: 800\n    height: 600\n"
                   "    screen: eDP-1\n    show: sideways\n    page: globals\n");
    auto const loaded = FileGuiStateStore { fileSystem, configDir() }.load();
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->settingsWindow.has_value());
    CHECK(loaded->settingsWindow->show == WindowShowState::Normal);
    CHECK(*loaded->settingsWindow
          == SettingsWindowState { .x = 10,
                                   .y = 20,
                                   .width = 800,
                                   .height = 600,
                                   .screen = "eDP-1",
                                   .show = WindowShowState::Normal,
                                   .page = "globals" });
}

TEST_CASE("GuiStateStore: a Settings window entry that is not a map is ignored", "[config][guistate]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    writeStateFile(fileSystem, "dismissed_new_defaults: [existing_config]\nsettings_window: 7\n");
    auto const loaded = FileGuiStateStore { fileSystem, configDir() }.load();
    REQUIRE(loaded.has_value());
    CHECK(loaded->settingsWindow == std::nullopt);
    CHECK(loaded->dismissedOffers == std::vector { NewDefaultsOffer::ExistingConfig });
}

TEST_CASE("GuiStateStore: dismissed shells round-trip", "[config][guistate]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    auto store = FileGuiStateStore { fileSystem, configDir() };
    auto const state = GuiState { .dismissedShellIntegrations = { ShellKind::Zsh, ShellKind::Fish } };
    REQUIRE(store.save(state).has_value());
    auto const written = fileSystem.readFile(stateFile()).value();
    INFO(written);
    CHECK(written.contains("dismissed_shell_integration: [zsh, fish]"));
    auto const loaded = FileGuiStateStore { fileSystem, configDir() }.load();
    REQUIRE(loaded.has_value());
    CHECK(*loaded == state);
}

TEST_CASE("GuiStateStore: an unknown shell token is skipped, a known one kept", "[config][guistate]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    writeStateFile(fileSystem, "dismissed_shell_integration: [zsh, csh, \"\", -zsh]\n");
    auto const loaded = FileGuiStateStore { fileSystem, configDir() }.load();
    REQUIRE(loaded.has_value());
    CHECK(*loaded == GuiState { .dismissedShellIntegrations = { ShellKind::Zsh } });
}

TEST_CASE("GuiStateStore: shells that are not a list are malformed", "[config][guistate]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    writeStateFile(fileSystem, "dismissed_shell_integration: zsh\n");
    auto const loaded = FileGuiStateStore { fileSystem, configDir() }.load();
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error() == GuiStateError::Malformed);
}

TEST_CASE("GuiStateStore: saving one list keeps the other and what this release does not know",
          "[config][guistate]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    writeStateFile(
        fileSystem,
        "dismissed_new_defaults: [existing_config]\ndismissed_shell_integration: [csh]\nwindow_hint: wide\n");
    auto store = FileGuiStateStore { fileSystem, configDir() };

    auto state = store.load().value();
    CHECK(state == GuiState { .dismissedOffers = { NewDefaultsOffer::ExistingConfig } });
    state.dismissedShellIntegrations.push_back(ShellKind::Bash);
    REQUIRE(store.save(state).has_value());

    auto const written = fileSystem.readFile(stateFile()).value();
    INFO(written);
    CHECK(written.contains("dismissed_new_defaults: [existing_config]"));
    CHECK(written.contains("dismissed_shell_integration: [bash, csh]"));
    CHECK(written.contains("window_hint: wide"));
    CHECK(FileGuiStateStore { fileSystem, configDir() }.load().value() == state);

    // Answering the new-defaults banner afterwards must not wipe "Not Now".
    state.dismissedOffers.push_back(NewDefaultsOffer::SettingsOverride);
    REQUIRE(store.save(state).has_value());
    CHECK(FileGuiStateStore { fileSystem, configDir() }.load().value().dismissedShellIntegrations
          == std::vector<ShellKind> { ShellKind::Bash });
}

TEST_CASE("GuiStateStore: update changes one list and keeps the other", "[config][guistate]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    auto store = FileGuiStateStore { fileSystem, configDir() };
    REQUIRE(store.update([](GuiState& state) { state.dismissedShellIntegrations.push_back(ShellKind::Fish); })
                .has_value());
    REQUIRE(store
                .update([](GuiState& state) {
                    state.dismissedOffers.push_back(NewDefaultsOffer::ExistingConfig);
                })
                .has_value());
    CHECK(store.load().value()
          == GuiState { .dismissedOffers = { NewDefaultsOffer::ExistingConfig },
                        .dismissedShellIntegrations = { ShellKind::Fish } });
}
