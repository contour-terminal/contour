// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the SettingsController — the editable bridge behind the GUI settings page. Each test
// drives the real create/edit/save/save-as/delete workflow end to end: a real FileGuiConfigStore
// writing side files into a temp config directory, and an apply callback that reloads the config from
// disk exactly as the production apply does. So a test exercises the whole stack (controller → store →
// loader), not a mock of it.

#include <contour/Config.h>
#include <contour/GuiConfigStore.h>
#include <contour/SettingsController.h>

#include <vtbackend/Color.h>

#include <QtCore/QTemporaryDir>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>

using namespace contour;

namespace
{

/// Writes @p yaml as the contour.yml inside @p dir and returns its path.
[[nodiscard]] std::filesystem::path writeConfig(QTemporaryDir& dir, std::string_view yaml)
{
    auto const path = std::filesystem::path(dir.path().toStdString()) / "contour.yml";
    auto out = std::ofstream(path);
    out << yaml;
    return path;
}

/// Owns a controller wired exactly like production: a live Config, a file-backed side-file store rooted
/// at the config directory, and an apply callback that reloads the Config from disk. The whole
/// create/edit/save/delete workflow runs against real files under a temp directory.
struct Fixture
{
    QTemporaryDir dir;
    config::Config cfg;
    std::filesystem::path configPath;
    std::shared_ptr<FileGuiConfigStore> store;
    std::unique_ptr<SettingsController> controller;

    explicit Fixture(std::string_view yaml)
    {
        configPath = writeConfig(dir, yaml);
        config::loadConfigFromFile(cfg, configPath);
        store = std::make_shared<FileGuiConfigStore>(std::filesystem::path(dir.path().toStdString()));
        controller = std::make_unique<SettingsController>([this]() -> config::Config const& { return cfg; },
                                                          store,
                                                          [this]() {
                                                              cfg = config::Config {};
                                                              config::loadConfigFromFile(cfg, configPath);
                                                          });
    }
};

/// Finds the profile row named @p name in @p rows, or an empty map if absent.
[[nodiscard]] QVariantMap rowNamed(QVariantList const& rows, QString const& name)
{
    for (auto const& row: rows)
        if (row.toMap().value("name").toString() == name)
            return row.toMap();
    return {};
}

constexpr auto kBasicConfig = std::string_view { R"(
default_profile: main
profiles:
    main:
        show_title_bar: true
)" };

} // namespace

TEST_CASE("SettingsController: lists profiles with provenance", "[settings]")
{
    auto fx = Fixture(kBasicConfig);
    auto const main = rowNamed(fx.controller->profiles(), "main");
    REQUIRE(!main.isEmpty());
    CHECK(main.value("origin").toString() == "main");
    CHECK(main.value("editable").toBool() == false); // contour.yml profile: read-only in the GUI
    CHECK(main.value("isDefault").toBool() == true);
}

TEST_CASE("SettingsController: a contour.yml profile is read-only; Save is refused", "[settings]")
{
    auto fx = Fixture(kBasicConfig);
    fx.controller->editProfile("main");
    CHECK(fx.controller->editingReadOnly() == true);
    CHECK(fx.controller->saveProfile() == false); // refused: would shadow the hand-maintained file
}

TEST_CASE("SettingsController: new profile -> Save As creates an editable side-file profile", "[settings]")
{
    auto fx = Fixture(kBasicConfig);
    fx.controller->newProfile("main");
    fx.controller->setProfileField("show_title_bar", false);
    fx.controller->setProfileField("dim_unfocused", 0.5);
    REQUIRE(fx.controller->saveProfileAs("work"));

    // The side file exists and the reloaded config now carries the new profile.
    CHECK(std::filesystem::exists(std::filesystem::path(fx.dir.path().toStdString()) / "profiles"
                                  / "work.yml"));
    auto const* work = fx.cfg.findProfile("work");
    REQUIRE(work != nullptr);
    CHECK(work->showTitleBar.value() == false);
    CHECK(work->dimUnfocused.value() == Catch::Approx(0.5));

    // It is now the editing target, editable (a side file), and listed as such.
    CHECK(fx.controller->editingProfile() == "work");
    CHECK(fx.controller->editingReadOnly() == false);
    CHECK(rowNamed(fx.controller->profiles(), "work").value("editable").toBool() == true);
}

TEST_CASE("SettingsController: Save As refuses to shadow a contour.yml profile", "[settings]")
{
    auto fx = Fixture(kBasicConfig);
    fx.controller->newProfile("main");
    CHECK(fx.controller->saveProfileAs("main") == false); // 'main' is defined in contour.yml
}

TEST_CASE("SettingsController: edit then Save updates a side-file profile in place", "[settings]")
{
    auto fx = Fixture(kBasicConfig);
    fx.controller->newProfile("main");
    REQUIRE(fx.controller->saveProfileAs("work"));

    fx.controller->setProfileField("dim_unfocused", 0.25);
    REQUIRE(fx.controller->saveProfile());
    CHECK(fx.cfg.findProfile("work")->dimUnfocused.value() == Catch::Approx(0.25));
}

TEST_CASE("SettingsController: delete removes a side-file profile", "[settings]")
{
    auto fx = Fixture(kBasicConfig);
    fx.controller->newProfile("main");
    REQUIRE(fx.controller->saveProfileAs("work"));
    REQUIRE(fx.cfg.findProfile("work") != nullptr);

    REQUIRE(fx.controller->deleteProfile("work"));
    CHECK(fx.cfg.findProfile("work") == nullptr);
    CHECK(
        std::filesystem::exists(std::filesystem::path(fx.dir.path().toStdString()) / "profiles" / "work.yml")
        == false);
}

TEST_CASE("SettingsController: a contour.yml profile cannot be deleted", "[settings]")
{
    auto fx = Fixture(kBasicConfig);
    CHECK(fx.controller->deleteProfile("main") == false);
}

TEST_CASE("SettingsController: setDefaultProfile persists to settings.yml and overrides", "[settings]")
{
    auto fx = Fixture(kBasicConfig);
    fx.controller->newProfile("main");
    REQUIRE(fx.controller->saveProfileAs("work"));

    REQUIRE(fx.controller->setDefaultProfile("work"));
    CHECK(fx.cfg.defaultProfileName.value() == "work");
    CHECK(fx.controller->defaultProfile() == "work");
    CHECK(std::filesystem::exists(std::filesystem::path(fx.dir.path().toStdString()) / "settings.yml"));
}

TEST_CASE("SettingsController: enum and integer profile fields round-trip", "[settings]")
{
    auto fx = Fixture(kBasicConfig);
    fx.controller->newProfile("main");
    fx.controller->setProfileField("tab_bar_position", "Bottom");
    fx.controller->setProfileField("tab_bar_visibility", "Never");
    fx.controller->setProfileField("slow_scrolling_time", 250);
    fx.controller->setProfileField("maximized", true);
    REQUIRE(fx.controller->saveProfileAs("work"));

    auto const* work = fx.cfg.findProfile("work");
    REQUIRE(work != nullptr);
    CHECK(work->tabBarPosition.value() == config::TabBarPosition::Bottom);
    CHECK(work->tabBarVisibility.value() == config::TabBarVisibility::Never);
    CHECK(work->smoothLineScrolling.value() == std::chrono::milliseconds(250));
    CHECK(work->maximized.value() == true);
}

TEST_CASE("SettingsController: color-scheme selection supports a dark/light pair", "[settings]")
{
    auto fx = Fixture(kBasicConfig);
    fx.controller->newProfile("main");

    fx.controller->setColorSchemeMode("dual");
    CHECK(fx.controller->colorSchemeMode() == "dual");
    fx.controller->setColorSchemeLight("default");
    fx.controller->setColorSchemeDark("default");
    REQUIRE(fx.controller->saveProfileAs("dually"));

    auto const* profile = fx.cfg.findProfile("dually");
    REQUIRE(profile != nullptr);
    CHECK(std::holds_alternative<config::DualColorConfig>(profile->colors.value()));
}

TEST_CASE("SettingsController: create, edit and reload a color scheme", "[settings]")
{
    auto fx = Fixture(kBasicConfig);
    fx.controller->newColorScheme("");
    fx.controller->setSchemeColor("background", "#123456");
    REQUIRE(fx.controller->saveColorScheme("mono"));

    // The side file exists; re-opening it loads the color we set back.
    CHECK(std::filesystem::exists(std::filesystem::path(fx.dir.path().toStdString()) / "colorschemes"
                                  / "mono.yml"));
    fx.controller->editColorScheme("mono");
    auto background = QString {};
    for (auto const& row: fx.controller->schemeColors())
        if (row.toMap().value("key").toString() == "background")
            background = row.toMap().value("color").toString();
    CHECK(background == "#123456");

    REQUIRE(fx.controller->deleteColorScheme("mono"));
    CHECK(std::filesystem::exists(std::filesystem::path(fx.dir.path().toStdString()) / "colorschemes"
                                  / "mono.yml")
          == false);
}

TEST_CASE("SettingsController: global overrides write settings.yml, apply, and reset", "[settings]")
{
    auto fx = Fixture("default_profile: main\nreflow_on_resize: true\n");
    auto const configDir = std::filesystem::path(fx.dir.path().toStdString());

    REQUIRE(fx.controller->setGlobalField("reflow_on_resize", false));
    CHECK(fx.cfg.reflowOnResize.value() == false);
    REQUIRE(fx.controller->setGlobalField("read_buffer_size", 32768));
    CHECK(fx.cfg.ptyReadBufferSize.value() == 32768);
    REQUIRE(fx.controller->setGlobalField("word_delimiters", "abc"));
    CHECK(fx.cfg.wordDelimiters.value() == "abc");
    CHECK(std::filesystem::exists(configDir / "settings.yml"));

    // The overridden field is flagged as such in the model.
    auto overridden = false;
    for (auto const& row: fx.controller->globalFields())
        if (row.toMap().value("key").toString() == "reflow_on_resize")
            overridden = row.toMap().value("overridden").toBool();
    CHECK(overridden);

    // Reset falls back to the contour.yml value.
    REQUIRE(fx.controller->resetGlobalField("reflow_on_resize"));
    CHECK(fx.cfg.reflowOnResize.value() == true);
}

TEST_CASE("SettingsController: exposes the configured keybindings read-only", "[settings]")
{
    auto fx = Fixture(kBasicConfig);
    auto const bindings = fx.controller->keybindings();
    REQUIRE(!bindings.isEmpty()); // the default config ships input mappings
    auto const first = bindings.first().toMap();
    CHECK(!first.value("trigger").toString().isEmpty());
    CHECK(!first.value("action").toString().isEmpty());
}

TEST_CASE("SettingsController: gui_config_locked makes the page read-only", "[settings]")
{
    auto fx = Fixture("default_profile: main\ngui_config_locked: true\n");
    CHECK(fx.controller->locked() == true);

    fx.controller->newProfile("main");
    CHECK(fx.controller->saveProfileAs("work") == false);
    CHECK(fx.controller->setDefaultProfile("main") == false);
}
