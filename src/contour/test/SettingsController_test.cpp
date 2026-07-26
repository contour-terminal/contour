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

#include <QtCore/QStringList>
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

/// Finds the field row whose "key" is @p key in @p rows, or an empty map if absent.
[[nodiscard]] QVariantMap rowWithKey(QVariantList const& rows, QString const& key)
{
    for (auto const& row: rows)
        if (row.toMap().value("key").toString() == key)
            return row.toMap();
    return {};
}

/// Finds the profile row named @p name in @p rows, or an empty map if absent.
[[nodiscard]] QVariantMap rowNamed(QVariantList const& rows, QString const& name)
{
    for (auto const& row: rows)
        if (row.toMap().value("name").toString() == name)
            return row.toMap();
    return {};
}

constexpr auto BasicConfig = std::string_view { R"(
default_profile: main
profiles:
    main:
        show_title_bar: true
)" };

} // namespace

TEST_CASE("SettingsController: lists profiles with provenance", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    auto const main = rowNamed(fx.controller->profiles(), "main");
    REQUIRE(!main.isEmpty());
    CHECK(main.value("origin").toString() == "main");
    CHECK(main.value("editable").toBool() == false); // contour.yml profile: read-only in the GUI
    CHECK(main.value("isDefault").toBool() == true);
}

TEST_CASE("SettingsController: a contour.yml profile is read-only; Save is refused", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->editProfile("main");
    CHECK(fx.controller->editingReadOnly() == true);
    CHECK(fx.controller->saveProfile() == false); // refused: would shadow the hand-maintained file
}

TEST_CASE("SettingsController: new profile -> Save As creates an editable side-file profile", "[settings]")
{
    auto fx = Fixture(BasicConfig);
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
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");
    CHECK(fx.controller->saveProfileAs("main") == false); // 'main' is defined in contour.yml
}

TEST_CASE("SettingsController: Save As refuses an existing GUI profile name and does not clobber it",
          "[settings]")
{
    // 'work' is a GUI (side-file) profile, not a contour.yml one: Save As under that name must still be
    // refused, otherwise the existing work.yml is silently overwritten with the current draft.
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");
    REQUIRE(fx.controller->saveProfileAs("work")); // creates work.yml (show_title_bar inherited: true)

    fx.controller->newProfile("main");
    fx.controller->setProfileField("show_title_bar", false);
    CHECK(fx.controller->saveProfileAs("work") == false); // refused: 'work' already exists

    auto const* work = fx.cfg.findProfile("work");
    REQUIRE(work != nullptr);
    CHECK(work->showTitleBar.value() == true); // the original side file was NOT clobbered
}

TEST_CASE("SettingsController: edit then Save updates a side-file profile in place", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");
    REQUIRE(fx.controller->saveProfileAs("work"));

    fx.controller->setProfileField("dim_unfocused", 0.25);
    REQUIRE(fx.controller->saveProfile());
    CHECK(fx.cfg.findProfile("work")->dimUnfocused.value() == Catch::Approx(0.25));
}

TEST_CASE("SettingsController: delete removes a side-file profile", "[settings]")
{
    auto fx = Fixture(BasicConfig);
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
    auto fx = Fixture(BasicConfig);
    CHECK(fx.controller->deleteProfile("main") == false);
}

TEST_CASE("SettingsController: setDefaultProfile persists to settings.yml and overrides", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");
    REQUIRE(fx.controller->saveProfileAs("work"));

    REQUIRE(fx.controller->setDefaultProfile("work"));
    CHECK(fx.cfg.defaultProfileName.value() == "work");
    CHECK(fx.controller->defaultProfile() == "work");
    CHECK(std::filesystem::exists(std::filesystem::path(fx.dir.path().toStdString()) / "settings.yml"));
}

TEST_CASE("SettingsController: rename moves a side-file profile and follows the default + draft",
          "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");
    REQUIRE(fx.controller->saveProfileAs("work"));
    REQUIRE(fx.controller->setDefaultProfile("work"));
    fx.controller->editProfile("work"); // the profile we are about to rename is the open draft

    auto const dir = std::filesystem::path(fx.dir.path().toStdString());
    REQUIRE(fx.controller->renameProfile("work", "office"));

    // The side file moved, and the reloaded config carries the new name only.
    CHECK(std::filesystem::exists(dir / "profiles" / "office.yml"));
    CHECK_FALSE(std::filesystem::exists(dir / "profiles" / "work.yml"));
    CHECK(fx.cfg.findProfile("office") != nullptr);
    CHECK(fx.cfg.findProfile("work") == nullptr);

    // The default pointer and the open draft both followed the rename.
    CHECK(fx.controller->defaultProfile() == "office");
    CHECK(fx.cfg.defaultProfileName.value() == "office");
    CHECK(fx.controller->editingProfile() == "office");
}

TEST_CASE("SettingsController: rename refuses contour.yml profiles and name collisions", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");
    REQUIRE(fx.controller->saveProfileAs("work"));

    CHECK(fx.controller->renameProfile("main", "whatever") == false); // 'main' is from contour.yml
    CHECK(fx.controller->renameProfile("work", "main") == false);     // collides with contour.yml 'main'
    CHECK(std::filesystem::exists(std::filesystem::path(fx.dir.path().toStdString()) / "profiles"
                                  / "work.yml"));
}

TEST_CASE("SettingsController: rename moves a side-file color scheme with its draft", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->newColorScheme("");
    fx.controller->setSchemeColor("background", "#101010");
    REQUIRE(fx.controller->saveColorScheme("midnight"));

    auto const dir = std::filesystem::path(fx.dir.path().toStdString());
    REQUIRE(std::filesystem::exists(dir / "colorschemes" / "midnight.yml"));

    fx.controller->editColorScheme("midnight");
    REQUIRE(fx.controller->renameColorScheme("midnight", "nightfall"));
    CHECK(std::filesystem::exists(dir / "colorschemes" / "nightfall.yml"));
    CHECK_FALSE(std::filesystem::exists(dir / "colorschemes" / "midnight.yml"));
    CHECK(fx.controller->editingScheme() == "nightfall");
}

// A contour.yml with an inline color scheme; the settings page must treat that name as read-only, since
// an inline scheme shadows a same-named side file at load time.
constexpr auto InlineSchemeConfig = std::string_view { R"(
default_profile: main
color_schemes:
    solarized:
        default:
            background: '#002b36'
profiles:
    main:
        show_title_bar: true
)" };

TEST_CASE("SettingsController: saving a color scheme named like a contour.yml scheme is refused",
          "[settings]")
{
    // An inline scheme shadows a side file at load, so a GUI save under that name would silently have no
    // effect. Refuse it (and write nothing), mirroring Save As's contour.yml guard for profiles.
    auto fx = Fixture(InlineSchemeConfig);
    fx.controller->newColorScheme("");
    fx.controller->setSchemeColor("background", "#111111");
    CHECK(fx.controller->saveColorScheme("solarized") == false);
    CHECK_FALSE(std::filesystem::exists(std::filesystem::path(fx.dir.path().toStdString()) / "colorschemes"
                                        / "solarized.yml"));
}

TEST_CASE("SettingsController: renaming a color scheme onto a contour.yml scheme name is refused",
          "[settings]")
{
    auto fx = Fixture(InlineSchemeConfig);
    fx.controller->newColorScheme("");
    fx.controller->setSchemeColor("background", "#101010");
    REQUIRE(fx.controller->saveColorScheme("midnight")); // a real GUI side-file scheme

    // Renaming it onto the inline 'solarized' would write a dead colorschemes/solarized.yml the loader
    // never resolves (the inline node wins); refuse it, and leave the source scheme untouched.
    fx.controller->editColorScheme("midnight");
    CHECK(fx.controller->renameColorScheme("midnight", "solarized") == false);
    auto const dir = std::filesystem::path(fx.dir.path().toStdString());
    CHECK_FALSE(std::filesystem::exists(dir / "colorschemes" / "solarized.yml"));
    CHECK(std::filesystem::exists(dir / "colorschemes" / "midnight.yml"));
}

TEST_CASE("SettingsController: a non-side-file color scheme cannot be deleted", "[settings]")
{
    // deleteColorScheme must refuse a builtin/inline scheme (no side file): removeFile treats an absent
    // file as success, so without the guard the controller would report a false 'deleted'.
    auto fx = Fixture(InlineSchemeConfig);
    CHECK(fx.controller->deleteColorScheme("default") == false);   // builtin
    CHECK(fx.controller->deleteColorScheme("solarized") == false); // contour.yml inline
}

TEST_CASE("SettingsController: integer and bool profile fields round-trip", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");
    fx.controller->setProfileField("slow_scrolling_time", 250);
    fx.controller->setProfileField("maximized", true);
    REQUIRE(fx.controller->saveProfileAs("work"));

    auto const* work = fx.cfg.findProfile("work");
    REQUIRE(work != nullptr);
    CHECK(work->smoothLineScrolling.value() == std::chrono::milliseconds(250));
    CHECK(work->maximized.value() == true);
}

TEST_CASE("SettingsController: color-scheme selection supports a dark/light pair", "[settings]")
{
    auto fx = Fixture(BasicConfig);
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
    auto fx = Fixture(BasicConfig);
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
    auto fx = Fixture("default_profile: main\n"
                      "reflow_on_resize: true\n");
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

TEST_CASE("SettingsController: the global theme enum field round-trips with its options", "[settings]")
{
    auto fx = Fixture("default_profile: main\n");

    // The theme row advertises the enum type and the three allowed values, so the QML renders a
    // combo box rather than a text field (the global-fields path previously exposed no options).
    auto type = QString {};
    auto options = QStringList {};
    for (auto const& row: fx.controller->globalFields())
        if (row.toMap().value("key").toString() == "theme")
        {
            type = row.toMap().value("type").toString();
            options = row.toMap().value("options").toStringList();
        }
    CHECK(type == "enum");
    CHECK(options == QStringList { "system", "dark", "light" });

    // Selecting a value persists it as the corresponding enum on the live config.
    REQUIRE(fx.controller->setGlobalField("theme", "dark"));
    CHECK(fx.cfg.theme.value() == config::GuiTheme::Dark);

    REQUIRE(fx.controller->setGlobalField("theme", "system"));
    CHECK(fx.cfg.theme.value() == config::GuiTheme::System);
}

TEST_CASE("SettingsController: the tab bar fields are global and offer their table's tokens", "[settings]")
{
    auto fx = Fixture(BasicConfig);

    // The options come from contour/TabBarMode.h, so the page can only ever offer tokens the
    // configuration reader accepts -- asserted against the table itself rather than a second literal
    // list, which is the whole point of the table.
    auto expected = QStringList {};
    for (auto const& info: config::tabBarModes<config::TabBarVisibility>())
        expected.push_back(QString::fromUtf8(info.token.data(), static_cast<qsizetype>(info.token.size())));

    auto options = QStringList {};
    auto type = QString {};
    for (auto const& row: fx.controller->globalFields())
        if (row.toMap().value("key").toString() == "tab_bar_visibility")
        {
            type = row.toMap().value("type").toString();
            options = row.toMap().value("options").toStringList();
        }
    CHECK(type == "enum");
    CHECK(options == expected);

    REQUIRE(fx.controller->setGlobalField("tab_bar_visibility", "Multiple"));
    CHECK(fx.cfg.tabBarVisibility.value() == config::TabBarVisibility::Multiple);

    REQUIRE(fx.controller->setGlobalField("tab_bar_position", "Bottom"));
    CHECK(fx.cfg.tabBarPosition.value() == config::TabBarPosition::Bottom);

    // They are no longer profile fields at all.
    auto profileKeys = QStringList {};
    fx.controller->newProfile("main");
    for (auto const& row: fx.controller->profileFields())
        profileKeys.push_back(row.toMap().value("key").toString());
    CHECK(!profileKeys.contains("tab_bar_position"));
    CHECK(!profileKeys.contains("tab_bar_visibility"));
}

TEST_CASE("SettingsController: exposes the configured keybindings read-only", "[settings]")
{
    auto fx = Fixture(BasicConfig);
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

// {{{ Profile field descriptors

TEST_CASE("SettingsController: every enum field's value is one of its own options", "[settings]")
{
    // An enum row hands QML both a combo-box model (`options`) and the currently selected value. If the
    // value is spelled differently from the options -- which is what happens the moment those two are
    // derived separately -- the combo box silently shows nothing selected, and the first interaction
    // writes a value the user never chose. Checking every enum row at once catches that for all of them,
    // including ones added later.
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");

    auto enumRows = 0;
    for (auto const& raw: fx.controller->profileFields())
    {
        auto const row = raw.toMap();
        if (row.value("type").toString() != "enum")
            continue;
        ++enumRows;

        auto const key = row.value("key").toString();
        auto const options = row.value("options").toStringList();
        auto const value = row.value("value").toString();

        INFO("enum field: " << key.toStdString() << " = '" << value.toStdString() << "'");
        CHECK(!options.isEmpty());
        CHECK(options.contains(value));
    }
    CHECK(enumRows > 0); // the loop above is worthless if it never runs
}

TEST_CASE("SettingsController: enum fields round-trip through their options", "[settings]")
{
    // Setting a row to each of its own options must stick. This is the other half of the drift check
    // above: the getter and the setter have to agree on the spelling, not merely each be self-consistent.
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");

    for (auto const& raw: fx.controller->profileFields())
    {
        auto const row = raw.toMap();
        if (row.value("type").toString() != "enum")
            continue;

        auto const key = row.value("key").toString();
        for (auto const& option: row.value("options").toStringList())
        {
            fx.controller->setProfileField(key, option);
            auto const after = rowWithKey(fx.controller->profileFields(), key);
            INFO("enum field: " << key.toStdString() << " set to '" << option.toStdString() << "'");
            CHECK(after.value("value").toString() == option);
        }
    }
}

TEST_CASE("SettingsController: unlimited scrollback survives a round-trip", "[settings]")
{
    // -1 is how the config file spells unlimited history, so it is what the page must show and accept.
    // Reporting Infinite as 0 and reading 0 back as LineCount(0) turned unlimited scrollback into no
    // scrollback the first time anything touched the field.
    auto fx = Fixture(std::string_view { R"(
default_profile: main
profiles:
    main:
        history:
            limit: -1
)" });
    fx.controller->newProfile("main");

    auto const row = rowWithKey(fx.controller->profileFields(), "history_max_lines");
    REQUIRE(!row.isEmpty());
    CHECK(row.value("value").toInt() == -1);

    SECTION("and writing -1 back keeps it unlimited")
    {
        fx.controller->setProfileField("history_max_lines", -1);
        CHECK(rowWithKey(fx.controller->profileFields(), "history_max_lines").value("value").toInt() == -1);
    }

    SECTION("while a finite limit is still a finite limit")
    {
        fx.controller->setProfileField("history_max_lines", 4200);
        CHECK(rowWithKey(fx.controller->profileFields(), "history_max_lines").value("value").toInt() == 4200);
    }
}

TEST_CASE("SettingsController: profile fields are grouped for the settings page", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");

    auto groups = QStringList {};
    for (auto const& raw: fx.controller->profileFields())
    {
        auto const row = raw.toMap();
        auto const group = row.value("group").toString();
        INFO("field: " << row.value("key").toString().toStdString());
        CHECK(!group.isEmpty()); // every field belongs to a section
        CHECK(!row.value("groupGlyph").toString().isEmpty());
        if (!groups.contains(group))
            groups.push_back(group);
    }
    CHECK(groups.size() > 1); // and they are not all in one bucket

    // The three raw indicator template fields are gone: the visual editor owns them now, so the page no
    // longer has to hide them by matching on a key prefix.
    auto keys = QStringList {};
    for (auto const& raw: fx.controller->profileFields())
        keys.push_back(raw.toMap().value("key").toString());
    CHECK(!keys.contains("status_line_indicator_left"));
    CHECK(!keys.contains("status_line_indicator_middle"));
    CHECK(!keys.contains("status_line_indicator_right"));
}

// }}}
// {{{ Indicator status line bridge

TEST_CASE("SettingsController: the indicator bridge round-trips a segment unchanged", "[settings]")
{
    // The whole point of the bridge: opening the settings page parses each segment into items, and saving
    // serializes them back. A segment nobody edited must come back byte-identical, or merely visiting the
    // page rewrites the user's profile.
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");

    for (auto const segmentIndex: { 0, 1, 2 })
    {
        auto const original = fx.controller->indicatorSegment(segmentIndex);
        auto const items = fx.controller->parseIndicatorSegment(original);
        INFO("segment " << segmentIndex << ": " << original.toStdString());
        REQUIRE(!items.isEmpty());

        // Canonical form is a fixed point, so repeated opens and saves cannot drift the template.
        auto const once = fx.controller->serializeIndicatorSegment(items);
        CHECK(fx.controller->serializeIndicatorSegment(fx.controller->parseIndicatorSegment(once)) == once);

        // And every item kept its type rather than degrading into literal text.
        auto typesOf = [](QVariantList const& rows) {
            auto out = QStringList {};
            for (auto const& row: rows)
                out.push_back(row.toMap().value("type").toString());
            return out;
        };
        CHECK(typesOf(fx.controller->parseIndicatorSegment(once)) == typesOf(items));
    }
}

TEST_CASE("SettingsController: the indicator bridge preserves the default left segment's detail",
          "[settings]")
{
    // The shipped default's {Tabs:ActiveColor=#FFFF00,...} is the case that used to be destroyed: the
    // bridge rebuilt Tabs with its three extra fields defaulted, so opening the page dropped the colour.
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");

    auto const items = fx.controller->parseIndicatorSegment(fx.controller->indicatorSegment(0));
    auto tabs = QVariantMap {};
    for (auto const& raw: items)
        if (raw.toMap().value("type").toString() == "Tabs")
            tabs = raw.toMap();

    REQUIRE(!tabs.isEmpty());
    CHECK(tabs.value("hasActiveColor").toBool());
    CHECK(tabs.value("activeColor").toString() == "#FFFF00");

    // ... and it is still there after a save.
    auto const reparsed =
        fx.controller->parseIndicatorSegment(fx.controller->serializeIndicatorSegment(items));
    auto tabsAgain = QVariantMap {};
    for (auto const& raw: reparsed)
        if (raw.toMap().value("type").toString() == "Tabs")
            tabsAgain = raw.toMap();
    CHECK(tabsAgain.value("activeColor").toString() == "#FFFF00");
}

TEST_CASE("SettingsController: the indicator bridge exposes every flag the template supports", "[settings]")
{
    // Four of the thirteen used to be exposed, so a profile using Underline or Inverse lost it on save.
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");

    auto const flagCatalog = fx.controller->indicatorFlags();
    REQUIRE(flagCatalog.size() >= 13);

    auto const items = fx.controller->parseIndicatorSegment("{InputMode:Underline,Inverse,CrossedOut}");
    REQUIRE(items.size() == 1);
    auto const flags = items.first().toMap().value("flags").toMap();

    // Each catalog entry is a key of the per-item flag map, so QML can drive one from the other.
    for (auto const& raw: flagCatalog)
    {
        auto const key = raw.toMap().value("key").toString();
        INFO("flag: " << key.toStdString());
        CHECK(!raw.toMap().value("label").toString().isEmpty());
        CHECK(flags.contains(key));
    }

    CHECK(flags.value("Underline").toBool());
    CHECK(flags.value("Inverse").toBool());
    CHECK(flags.value("CrossedOut").toBool());
    CHECK(!flags.value("Bold").toBool());

    // And they survive the trip back out.
    auto const reparsed =
        fx.controller->parseIndicatorSegment(fx.controller->serializeIndicatorSegment(items));
    REQUIRE(reparsed.size() == 1);
    auto const after = reparsed.first().toMap().value("flags").toMap();
    CHECK(after.value("Underline").toBool());
    CHECK(after.value("Inverse").toBool());
    CHECK(after.value("CrossedOut").toBool());
}

TEST_CASE("SettingsController: the indicator placeholder catalog matches what parsing produces", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    auto const catalog = fx.controller->indicatorPlaceholders();
    REQUIRE(!catalog.isEmpty());

    for (auto const& raw: catalog)
    {
        auto const entry = raw.toMap();
        auto const type = entry.value("type").toString();
        INFO("placeholder: " << type.toStdString());
        CHECK(!entry.value("label").toString().isEmpty());
        CHECK(!entry.value("sample").toString().isEmpty());

        // Text and Command need their payload attribute to be recognized at all; the rest stand alone.
        auto const template_ = type == "Text"      ? QString("{Text:text=x}")
                               : type == "Command" ? QString("{Command:Program=true}")
                                                   : QString("{%1}").arg(type);
        auto const items = fx.controller->parseIndicatorSegment(template_);
        REQUIRE(items.size() == 1);
        CHECK(items.first().toMap().value("type").toString() == type);
    }
}

TEST_CASE("SettingsController: setIndicatorSegment refuses a read-only profile", "[settings]")
{
    // The chips in the editor are gated in QML too, but the controller is the boundary that has to hold:
    // a contour.yml profile is shown read-only and must not be editable through this path.
    auto fx = Fixture(BasicConfig);
    fx.controller->editProfile("main"); // a contour.yml profile: read-only in the GUI
    REQUIRE(fx.controller->editingReadOnly());

    auto const before = fx.controller->indicatorSegment(0);
    fx.controller->setIndicatorSegment(0, "{Clock}");
    CHECK(fx.controller->indicatorSegment(0) == before);
}

TEST_CASE("SettingsController: indicator segment indices are bounds-checked", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");

    CHECK(fx.controller->indicatorSegment(-1).isEmpty());
    CHECK(fx.controller->indicatorSegment(3).isEmpty());

    // An out-of-range write is dropped rather than landing on a neighbouring segment.
    auto const middle = fx.controller->indicatorSegment(1);
    fx.controller->setIndicatorSegment(7, "{Clock}");
    CHECK(fx.controller->indicatorSegment(1) == middle);
}

// }}}
