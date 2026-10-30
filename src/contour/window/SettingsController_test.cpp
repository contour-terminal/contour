// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the SettingsController — the editable bridge behind the GUI settings page. Each test
// drives the real create/edit/save/save-as/delete workflow end to end: a real FileGuiConfigStore
// writing side files into a temp config directory, and an apply callback that reloads the config from
// disk exactly as the production apply does. So a test exercises the whole stack (controller → store →
// loader), not a mock of it.

#include <contour/config/Config.hpp>
#include <contour/config/ConfigEnum.hpp>
#include <contour/config/GlobalOverrides.hpp>
#include <contour/config/GuiConfigStore.hpp>
#include <contour/config/HostSideFile.hpp>
#include <contour/display/GpuInventory.hpp>
#include <contour/test/ManualSaveScheduler.hpp>
#include <contour/test/RecordingShellRcEditor.hpp>
#include <contour/window/HotkeyRefusals.hpp>
#include <contour/window/NoticeQueue.hpp>
#include <contour/window/ProfileRules.hpp>
#include <contour/window/SettingsController.hpp>
#include <contour/window/SettingsControls.hpp>
#include <contour/window/SettingsFields.hpp>
#include <contour/window/SettingsSections.hpp>
#include <contour/window/ShellIntegrationOffer.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/core/CommandBlockOutcome.hpp>
#include <vtbackend/links/LinkPatterns.hpp>
#include <vtbackend/screen/GutterSettings.hpp>
#include <vtbackend/screen/StatusLineBuilder.hpp>
#include <vtbackend/shell/Folding.hpp>
#include <vtbackend/shell/ScrollbarMarks.hpp>
#include <vtbackend/shell/StickyHeader.hpp>

#include <vtpty/test/NonAsciiSample.hpp>

#include <core/platform/NativeFileSystem.hpp>

#include <yaml-cpp/yaml.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>
#include <QtGui/QColor>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <QtTest/QSignalSpy>
#include <vthost/testing/FaultyFileSystem.hpp>

using namespace contour;
using namespace contour::window;

namespace
{

/// Writes @p yaml as the contour.yml inside @p dir and returns its path.
[[nodiscard]] std::filesystem::path writeConfig(QTemporaryDir& dir, std::string_view yaml)
{
    auto const path = std::filesystem::path(dir.path().toStdU16String()) / "contour.yml";
    auto out = std::ofstream(path);
    out << yaml;
    return path;
}

/// A GPU inventory that lists whatever the test put in it.
class FakeGpuInventory final: public display::IGpuInventory
{
  public:
    std::vector<display::GpuCandidate> gpus;
    [[nodiscard]] std::vector<display::GpuCandidate> list() const override { return gpus; }
};

/// A font source that lists whatever the test put in it.
class FakeFontFamilySource final: public platform::IFontFamilySource
{
  public:
    std::vector<std::string> families;
    [[nodiscard]] std::vector<std::string> monospaceFamilies() const override { return families; }
};

/// A hybrid laptop: an integrated and a discrete GPU.
[[nodiscard]] std::shared_ptr<FakeGpuInventory> hybridGpus()
{
    auto inventory = std::make_shared<FakeGpuInventory>();
    inventory->gpus = {
        { .title = "Intel integrated GPU",
          .id = { 0x8086, 0xa788 },
          .kind = display::GpuKind::Integrated,
          .driver = "i915" },
        { .title = "NVIDIA GeForce RTX 4070 Max-Q / Mobile",
          .id = { 0x10de, 0x2820 },
          .kind = display::GpuKind::Discrete,
          .driver = "nvidia" },
    };
    return inventory;
}

/// Owns a controller wired exactly like production: a live Config, a file-backed side-file store rooted
/// at the config directory, and a reload that reads the Config from disk and refreshes the controller, as
/// the app's onConfigurationReloaded() does. The whole workflow runs against real files under a temp
/// directory -- the loader is not behind a FileSystem -- and the debounce is fired by hand.
/// The binary the Install rows' line runs.
[[nodiscard]] std::filesystem::path contourBinary()
{
    return "/opt/contour/bin/contour";
}

/// The home the startup files are shown relative to.
[[nodiscard]] std::filesystem::path home()
{
    return "/home/me";
}

struct Fixture
{
    core::platform::NativeFileSystem fileSystem; ///< What the store writes through: the fixture's own files.
    QTemporaryDir dir;
    config::Config cfg;
    std::filesystem::path configPath;
    std::shared_ptr<config::FileGuiConfigStore> store;
    std::shared_ptr<test::ManualSaveScheduler> scheduler = std::make_shared<test::ManualSaveScheduler>();
    int reloads = 0; ///< How many times Settings reloaded the configuration.
    std::shared_ptr<display::IGpuInventory const> gpuInventory;
    std::shared_ptr<platform::IFontFamilySource const> fontFamilies; ///< Null: the bundled family alone.
    vtbackend::ColorPreference colorPreference = vtbackend::ColorPreference::Dark;
    test::RecordingShellRcEditor shellEditor;         ///< The shells' startup files, scripted and recorded.
    BinaryReach binaryReach = BinaryReach::Reachable; ///< Whether the Install rows' binary outlives us.
    config::HostPlatform shellHost = config::HostPlatform::Other; ///< The host the Install rows run on.
    command::MenuBarPlacement menuBar = command::MenuBarPlacement::None; ///< Where the host's menu bar lives.
    std::unique_ptr<SettingsController> controller;
    QStringList errors; ///< What the controller reported through errorOccurred, in order.
    std::vector<actions::Action> ranActions;         ///< What Settings ran as the app's own commands.
    std::optional<std::filesystem::path> chosenFile; ///< What the file dialog answers; nullopt cancels.
    std::vector<std::string> fileDialogTitles;       ///< The title of each file dialog asked for.

    explicit Fixture(std::string_view yaml,
                     std::shared_ptr<display::IGpuInventory const> gpus = nullptr,
                     vtbackend::ColorPreference appearance = vtbackend::ColorPreference::Dark):
        gpuInventory { std::move(gpus) }, colorPreference { appearance }
    {
        configPath = writeConfig(dir, yaml);
        // Through the path overload, as production loads: it reads the file's defaults_edition first.
        cfg = config::loadConfigFromFile(configPath);
        store = std::make_shared<config::FileGuiConfigStore>(
            fileSystem, std::filesystem::path(dir.path().toStdU16String()));
        restartController();
    }

    /// Builds the controller anew over the configuration as it was last loaded, as the app builds it the
    /// first time Settings opens, which may be long after the load.
    void restartController()
    {
        controller.reset();
        controller = std::make_unique<SettingsController>(
            [this]() -> config::Config const& { return cfg; },
            SettingsControllerServices {
                .store = store,
                .reload = [this]() -> std::expected<void, std::string> {
                    ++reloads;
                    reload();
                    return {};
                },
                .gpuInventory = gpuInventory,
                .fontFamilies = fontFamilies,
                .scheduler = scheduler,
                .shell = { .editor = &shellEditor,
                           .contourBinary =
                               [this] {
                                   return LocatedBinary { .path = contourBinary(), .reach = binaryReach };
                               },
                           .host = shellHost,
                           .homeDirectory = home() },
                .menuBar = menuBar,
                .runAction = [this](actions::Action const& action) { ranActions.push_back(action); },
                .chooseFile =
                    [this](std::string const& title, std::filesystem::path const&) {
                        fileDialogTitles.push_back(title);
                        return chosenFile;
                    },
            },
            colorPreference);
        QObject::connect(controller.get(),
                         &SettingsController::errorOccurred,
                         [this](QString const& message) { errors.push_back(message); });
    }

    /// Writes @p content to @p relative under the configuration directory, as an editor would, without
    /// a reload: nothing watches the side files. The bytes land as given -- a text-mode stream would turn
    /// each newline into CRLF on Windows, which the store then reads back as a different file.
    void handEdit(std::filesystem::path const& relative, std::string_view content) const
    {
        REQUIRE(fileSystem.writeFile(configDir() / relative, content).has_value());
    }

    /// The configuration directory.
    [[nodiscard]] std::filesystem::path configDir() const { return configPath.parent_path(); }

    /// Reloads the configuration from disk and refreshes the controller, as a reload does.
    void reload()
    {
        cfg = config::loadConfigFromFile(configPath);
        controller->refresh();
    }

    /// Fires the debounce: what was committed is written, and the configuration reloaded.
    void fire() const { scheduler->fire(); }

    /// Writes side-file profile @p name as a copy of contour.yml's `main`, reloads, and opens it.
    void openSideProfile(std::string const& name)
    {
        auto const* main = cfg.findProfile("main");
        REQUIRE(main != nullptr);
        REQUIRE(store->write(config::profileFile(name), config::emitProfileYaml(*main)).has_value());
        reload();
        controller->editProfile(QString::fromStdString(name));
        REQUIRE(controller->editingReadOnly() == false);
    }

    /// Writes side-file host @p name with @p yaml, reloads, and opens it.
    void openSideHost(std::string const& name, std::string_view yaml)
    {
        REQUIRE(store->write(config::hostFile(name), yaml).has_value());
        reload();
        controller->editHost(QString::fromStdString(name));
        REQUIRE(controller->editingHost().toStdString() == name);
        REQUIRE_FALSE(controller->editingHostReadOnly());
    }

    /// Writes side-file color scheme @p name with the default palette, reloads, and opens it.
    void openSideScheme(std::string const& name)
    {
        REQUIRE(store
                    ->write(config::colorSchemeFile(name),
                            config::emitColorSchemeYaml(vtbackend::ColorPalette {}))
                    .has_value());
        reload();
        controller->editColorScheme(QString::fromStdString(name));
    }

    /// Commits global field @p key and writes it.
    void commitGlobal(QString const& key, QVariant const& value) const
    {
        REQUIRE(controller->setGlobalField(key, value).has_value());
        fire();
    }

    /// Resets global field @p key and writes it.
    void resetGlobal(QString const& key) const
    {
        REQUIRE(controller->resetGlobalField(key).has_value());
        fire();
    }

    /// The bytes of @p relative under the configuration directory; nullopt when there is no such file.
    [[nodiscard]] std::optional<std::string> fileContent(std::filesystem::path const& relative) const
    {
        auto const read = fileSystem.readFile(configDir() / relative);
        return read ? std::optional { *read } : std::nullopt;
    }

    /// The palette colorschemes/@p name.yml holds.
    [[nodiscard]] std::optional<vtbackend::ColorPalette> schemeFile(std::string const& name) const
    {
        return config::loadColorSchemeFile(configDir()
                                           / config::relativePathOf(config::colorSchemeFile(name)));
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

/// "#rrggbb" for @p color, as the scheme editor shows it.
[[nodiscard]] QString hexOf(vtbackend::RGBColor color)
{
    return QString::asprintf("#%02x%02x%02x", color.red, color.green, color.blue);
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

/// A value for global-field row @p row that differs from the one it shows now, in the type the row's
/// editor would send.
[[nodiscard]] QVariant differentValue(QVariantMap const& row)
{
    auto const type = row.value("type").toString();
    auto const current = row.value("value");
    if (type == "bool")
        return { !current.toBool() };
    // Down rather than up where it can go: a row's range is not on it, and a default at its maximum -- the
    // block column's two cells -- has room only below. A small one goes up, from the floor of a count.
    if (type == "int")
        return { current.toInt() > 1 ? current.toInt() - 1 : current.toInt() + 1 };
    if (type == "double")
        return { current.toDouble() + 1.0 };
    if (type == "enum")
    {
        for (auto const& option: row.value("options").toStringList())
            if (option != current.toString())
                return { option };
        return row.value("value"); // a single-option enum has no other value to send
    }
    // Appended to rather than replaced, so a value that must keep its shape keeps it: a timestamp format
    // has to begin with a conversion.
    return { current.toString() + QStringLiteral("x") };
}

/// The configuration tokens of every value of configuration enum @p Enum, as the page shows them.
template <typename Enum>
[[nodiscard]] QStringList tokensOf()
{
    auto tokens = QStringList {};
    for (auto const& info: config::configEnumValues<Enum>())
        tokens.push_back(QString::fromUtf8(info.token.data(), static_cast<qsizetype>(info.token.size())));
    return tokens;
}

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

TEST_CASE(
    "SettingsController: a contour.yml profile is read-only and Duplicate to Edit makes an editable copy",
    "[settings]")
{
    // Review Focus 3, and R-f: a profile defined in contour.yml takes no change; its copy, a side file, does.
    auto fx = Fixture(BasicConfig);
    fx.controller->editProfile("main");
    CHECK(fx.controller->editingReadOnly() == true);
    auto const refused = fx.controller->setProfileField("show_title_bar", false);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == config::SettingsErrorKind::ReadOnly);
    CHECK(refused.error().message.contains("contour.yml"));
    CHECK(fx.scheduler->pendingDelay() == std::nullopt);

    auto const copy = fx.controller->duplicateProfile("main");
    REQUIRE(copy.has_value());
    CHECK(*copy == "main copy");
    CHECK(fx.controller->editingProfile() == "main copy");
    CHECK(fx.controller->editingReadOnly() == false);
    fx.fire();
    CHECK(fx.cfg.profileOrigins.at("main copy") == config::SettingsOrigin::SideFile);
    CHECK(config::emitProfileYaml(*fx.cfg.findProfile("main copy"))
          == config::emitProfileYaml(*fx.cfg.findProfile("main")));
    CHECK(rowNamed(fx.controller->profiles(), "main copy").value("editable").toBool());

    auto const second = fx.controller->duplicateProfile("main");
    REQUIRE(second.has_value());
    CHECK(*second == "main copy 2");
}

TEST_CASE("SettingsController: a profile field applies after 300 ms, not before", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    auto const before = fx.fileContent(config::relativePathOf(config::profileFile("work")));

    REQUIRE(fx.controller->setProfileField("font_size", 14.0).has_value());

    CHECK(fx.fileContent(config::relativePathOf(config::profileFile("work"))) == before);
    CHECK(fx.controller->hasChanges());
    CHECK(fx.scheduler->pendingDelay() == config::SettingsSaveDebounce);

    fx.fire();

    CHECK(fx.fileContent(config::relativePathOf(config::profileFile("work"))) != before);
    CHECK(fx.cfg.findProfile("work")->fonts.value().size.pt == Catch::Approx(14.0));
    CHECK(fx.controller->hasChanges()); // there is a snapshot to go back to
}

TEST_CASE("SettingsController: one save refreshes the lists once", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    auto const spy = QSignalSpy { fx.controller.get(), &SettingsController::changed };

    REQUIRE(fx.controller->setProfileField("font_size", 15.0).has_value());
    fx.fire();

    CHECK(fx.reloads == 1);
    CHECK(spy.count() == 1);
}

TEST_CASE("SettingsController: setGlobalField refuses a value the loader would reject, before staging",
          "[settings]")
{
    auto fx = Fixture(BasicConfig);

    auto const invalid = fx.controller->setGlobalField("read_buffer_size", 0);
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error().kind == config::SettingsErrorKind::InvalidValue);
    CHECK(fx.scheduler->pendingDelay() == std::nullopt);

    auto const unknown = fx.controller->setGlobalField("no_such_setting", 1);
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().kind == config::SettingsErrorKind::UnknownSetting);
    CHECK(fx.scheduler->pendingDelay() == std::nullopt);
    CHECK_FALSE(fx.controller->hasChanges());
}

TEST_CASE("SettingsController: two global commits before the flush both land", "[settings]")
{
    // F1: the commits edit one settings.yml draft, so the second does not stage bytes without the first.
    auto fx = Fixture("default_profile: main\nreflow_on_resize: true\n");
    REQUIRE(fx.controller->setGlobalField("reflow_on_resize", false).has_value());
    REQUIRE(fx.controller->setGlobalField("snap_to_rows", true).has_value());
    REQUIRE(fx.controller->setDefaultProfile("main").has_value());
    fx.fire();

    CHECK(fx.cfg.reflowOnResize.value() == false);
    CHECK(fx.cfg.snapToRows.value() == true);
    CHECK(fx.cfg.guiManagedSettings.defaultProfile == std::optional<std::string> { "main" });
}

TEST_CASE("SettingsController: the app's own override goes through the one save model", "[settings]")
{
    // F20: the new-defaults banner's "Use New" writes settings.yml through this session. A global change
    // still pending lands with it, and a later flush does not take the app's own write for one made outside.
    auto fx = Fixture("default_profile: main\nreflow_on_resize: true\n");
    fx.commitGlobal("snap_to_rows", true);
    REQUIRE(fx.controller->setGlobalField("reflow_on_resize", false).has_value());

    REQUIRE(fx.controller->writeGlobalOverride(config::DefaultsEditionKey, "native2026").has_value());

    CHECK(fx.scheduler->pendingDelay() == std::nullopt); // written now, not after the debounce
    CHECK(fx.cfg.guiManagedSettings.globalOverrides.at(std::string { config::DefaultsEditionKey })
          == "native2026");
    CHECK(fx.cfg.reflowOnResize.value() == false);
    CHECK(fx.cfg.snapToRows.value() == true);

    fx.commitGlobal("snap_to_rows", false);
    CHECK(fx.cfg.snapToRows.value() == false);
    CHECK(fx.errors.isEmpty());

    SECTION("not under gui_config_locked")
    {
        auto locked = Fixture("default_profile: main\ngui_config_locked: true\n");
        auto const refused = locked.controller->writeGlobalOverride(config::DefaultsEditionKey, "native2026");
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().kind == config::SettingsErrorKind::Locked);
        CHECK_FALSE(locked.fileContent("settings.yml").has_value());
    }
}

TEST_CASE("SettingsFields: every global group row names a real key", "[settings]")
{
    // m5: the group table is separate from the descriptors, so a row for a key no descriptor has would sit
    // there unnoticed. A section row (ending in a dot) must cover at least one dotted key.
    auto const& descriptors = window::fields::globalFieldDescriptors();
    for (auto const& row: window::fields::globalFieldGroupRows())
    {
        auto const named = std::ranges::any_of(descriptors, [&](auto const& descriptor) {
            auto const key = descriptor.key.toStdString();
            return row.key.ends_with('.') ? key.starts_with(row.key) : key == row.key;
        });
        INFO("group row " << row.key);
        CHECK(named);
    }
}

TEST_CASE("SettingsController: the app's own override is judged by settings.yml alone", "[settings]")
{
    // m6: a colour scheme change still pending, whose file an editor changed since, would fail "Use New" if
    // the write flushed everything. It fails that scheme's own flush later, and nothing else.
    auto fx = Fixture(BasicConfig);
    fx.openSideScheme("mine");
    fx.controller->commitSchemeColor(QStringLiteral("background"), QStringLiteral("#102030"));
    REQUIRE(fx.controller->hasChanges());
    auto const schemePath = config::relativePathOf(config::colorSchemeFile("mine"));
    fx.handEdit(schemePath, "edited: elsewhere\n");

    auto const written = fx.controller->writeGlobalOverride(
        config::DefaultsEditionKey, config::configEnumToken(config::DefaultsEdition::Native2026));

    REQUIRE(written.has_value());
    CHECK(fx.errors.isEmpty());
    CHECK(fx.cfg.defaultsEdition == config::DefaultsEdition::Native2026);
    CHECK(fx.fileContent(schemePath) == "edited: elsewhere\n");
}

TEST_CASE("SettingsController: a hand edit made after the draft was opened is not written over", "[settings]")
{
    // I1: the draft was seeded before an editor saved the file; the first commit must not overwrite it.
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    auto handEdited = *fx.cfg.findProfile("work");
    handEdited.fonts.value().size.pt = 20.0;
    auto const handBytes = config::emitProfileYaml(handEdited);
    fx.handEdit(config::relativePathOf(config::profileFile("work")), handBytes);

    REQUIRE(fx.controller->setProfileField("font_size", 15.0).has_value());
    fx.fire();

    CHECK(fx.fileContent(config::relativePathOf(config::profileFile("work"))) == handBytes);
    REQUIRE_FALSE(fx.errors.isEmpty());
    CHECK(fx.errors.back().contains("changed outside Settings"));
    CHECK(rowWithKey(fx.controller->profileFields(), "font_size").value("value").toDouble()
          == Catch::Approx(20.0));
}

TEST_CASE("SettingsController: a hand edit of settings.yml after it was read is not written over",
          "[settings]")
{
    auto fx = Fixture("default_profile: main\nreflow_on_resize: true\n");
    fx.handEdit("settings.yml", "snap_to_rows: true\n");
    // No reload: the page's settings.yml draft is from before the edit.
    REQUIRE(fx.controller->setGlobalField("reflow_on_resize", false).has_value());
    fx.fire();
    CHECK(fx.fileContent("settings.yml") == "snap_to_rows: true\n");
    REQUIRE_FALSE(fx.errors.isEmpty());
    CHECK(fx.errors.back().contains("settings.yml changed outside Settings"));
}

TEST_CASE("SettingsController: a reset applies at once and shows the value in effect", "[settings]")
{
    // m6: a Reset is a click, not a drag; written at once, its row never shows the override it removed.
    auto fx = Fixture("default_profile: main\nreflow_on_resize: true\n");
    fx.commitGlobal("reflow_on_resize", false);
    REQUIRE(fx.cfg.reflowOnResize.value() == false);

    REQUIRE(fx.controller->resetGlobalField("reflow_on_resize").has_value());

    CHECK(fx.scheduler->pendingDelay() == std::nullopt);
    CHECK(fx.cfg.reflowOnResize.value() == true);
    auto const row = rowWithKey(fx.controller->globalFields(), "reflow_on_resize");
    CHECK(row.value("value").toBool() == true);
    CHECK_FALSE(row.value("overridden").toBool());
}

TEST_CASE(
    "SettingsController: a profile or scheme added this session can be renamed and deleted before it is "
    "written",
    "[settings]")
{
    // m3: a staged side file is a side file.
    auto fx = Fixture(BasicConfig);
    auto const copy = fx.controller->duplicateProfile("main");
    REQUIRE(copy.has_value());
    REQUIRE(fx.controller->renameProfile(*copy, "work").has_value());
    fx.fire();
    CHECK(fx.cfg.findProfile("work") != nullptr);
    CHECK(fx.cfg.findProfile(copy->toStdString()) == nullptr);

    auto const added = fx.controller->createProfile();
    REQUIRE(added.has_value());
    REQUIRE(fx.controller->deleteProfile(*added).has_value());
    fx.fire();
    CHECK(fx.cfg.findProfile(added->toStdString()) == nullptr);
    CHECK_FALSE(
        fx.fileContent(config::relativePathOf(config::profileFile(added->toStdString()))).has_value());

    auto const scheme = fx.controller->duplicateColorScheme("default");
    REQUIRE(scheme.has_value());
    REQUIRE(fx.controller->renameColorScheme(*scheme, "midnight").has_value());
    fx.fire();
    CHECK(fx.schemeFile("midnight").has_value());
    CHECK_FALSE(fx.schemeFile(scheme->toStdString()).has_value());
}

TEST_CASE("SettingsController: a rename whose source changed on disk writes no copy", "[settings]")
{
    // m7: the new file and the old one's removal are one change.
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    REQUIRE(fx.controller->renameProfile("work", "office").has_value());
    {
        auto out = std::ofstream(fx.configDir() / "profiles" / "work.yml", std::ios::app);
        out << "show_title_bar: false\n";
    }
    fx.fire();
    CHECK(fx.fileContent(std::filesystem::path { "profiles" } / "work.yml").has_value());
    CHECK_FALSE(fx.fileContent(std::filesystem::path { "profiles" } / "office.yml").has_value());
}

TEST_CASE("SettingsController: a name that would leave its directory is refused", "[settings]")
{
    // m8: a profile or scheme name is a file name inside profiles/ or colorschemes/, never a path.
    auto fx = Fixture("default_profile: main\n"
                      "profiles:\n"
                      "    main:\n"
                      "        show_title_bar: true\n"
                      "    \"../../x\":\n"
                      "        show_title_bar: true\n");
    fx.openSideProfile("work");
    fx.openSideScheme("midnight");
    auto const outside = [&fx] {
        auto const parent = fx.configDir().parent_path();
        return std::filesystem::exists(parent / "x.yml") || std::filesystem::exists(parent / "x copy.yml")
               || std::filesystem::exists(fx.configDir() / "x.yml")
               || std::filesystem::exists(fx.configDir() / "profiles" / "a")
               || std::filesystem::exists(fx.configDir() / "colorschemes" / "a")
               || std::filesystem::exists(fx.configDir() / "profiles" / ".hidden.yml")
               || std::filesystem::exists(fx.configDir() / "colorschemes" / ".hidden.yml");
    };

    for (auto const* name:
         { "../../x", "a/b", "a\\b", ".hidden", "a..b", "D:x", "C:x", "ab:c", "CON", "x.", "a\tb" })
    {
        INFO(name);
        auto const profile = fx.controller->renameProfile("work", QString::fromUtf8(name));
        REQUIRE_FALSE(profile.has_value());
        CHECK(profile.error().kind == config::SettingsErrorKind::InvalidValue);
        CHECK_FALSE(profile.error().message.empty());
        auto const scheme = fx.controller->renameColorScheme("midnight", QString::fromUtf8(name));
        REQUIRE_FALSE(scheme.has_value());
        CHECK(scheme.error().kind == config::SettingsErrorKind::InvalidValue);
    }
    auto const duplicated = fx.controller->duplicateProfile("../../x");
    REQUIRE_FALSE(duplicated.has_value());
    CHECK(duplicated.error().kind == config::SettingsErrorKind::InvalidValue);

    fx.fire();
    CHECK_FALSE(outside());
    CHECK(fx.cfg.findProfile("work") != nullptr);
}

TEST_CASE(
    "SettingsController: gui_config_locked set by a reload starts a locked session and re-seeds the drafts",
    "[settings]")
{
    // m4, m11: the session is rebuilt after the reload that runs inside it; the open draft turns read-only.
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    REQUIRE(fx.controller->setProfileField("font_size", 15.0).has_value());
    {
        auto out = std::ofstream(fx.configPath, std::ios::app);
        out << "gui_config_locked: true\n";
    }
    fx.reload();
    QCoreApplication::processEvents();

    CHECK(fx.controller->locked());
    CHECK(fx.controller->editingReadOnly());
    CHECK_FALSE(fx.controller->hasChanges());
    CHECK(fx.scheduler->pendingDelay() == std::nullopt); // the staged change is dropped, not written
    auto const refused = fx.controller->setProfileField("font_size", 16.0);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == config::SettingsErrorKind::Locked);
}

TEST_CASE("SettingsController: a global row shows the committed value before the flush", "[settings]")
{
    auto fx = Fixture("default_profile: main\nreflow_on_resize: true\n");
    auto const spy = QSignalSpy { fx.controller.get(), &SettingsController::globalFieldsChanged };

    REQUIRE(fx.controller->setGlobalField("reflow_on_resize", false).has_value());

    CHECK(spy.count() == 1);
    auto const row = rowWithKey(fx.controller->globalFields(), "reflow_on_resize");
    CHECK(row.value("value").toBool() == false);
    CHECK(row.value("overridden").toBool());
    CHECK(fx.cfg.reflowOnResize.value() == true); // not written yet
}

TEST_CASE("SettingsController: an unparsable settings.yml is not written over", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    REQUIRE(fx.store->write(config::guiSettingsFile(), "reflow_on_resize: [unclosed\n").has_value());
    fx.reload();
    REQUIRE(fx.cfg.guiSettingsFileHealth == config::GuiSettingsFileHealth::Unparsable);

    auto const refused = fx.controller->setGlobalField("reflow_on_resize", false);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == config::SettingsErrorKind::ReadOnly);
    CHECK(fx.scheduler->pendingDelay() == std::nullopt);
    CHECK(fx.fileContent("settings.yml") == "reflow_on_resize: [unclosed\n");
}

TEST_CASE("SettingsController: Revert after a contour.yml reload keeps the hand edit", "[settings]")
{
    // Review Focus 2: Revert puts back only the side files this session touched.
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    auto const original = fx.fileContent(config::relativePathOf(config::profileFile("work")));
    REQUIRE(fx.controller->setProfileField("font_size", 15.0).has_value());
    fx.fire();
    REQUIRE(fx.cfg.findProfile("work")->fonts.value().size.pt == Catch::Approx(15.0));

    {
        auto out = std::ofstream(fx.configPath, std::ios::app);
        out << "    extra:\n        show_title_bar: false\n";
    }
    fx.reload();
    REQUIRE(fx.cfg.findProfile("extra") != nullptr);

    fx.controller->revertChanges();

    CHECK(fx.fileContent(config::relativePathOf(config::profileFile("work"))) == original);
    CHECK(fx.cfg.findProfile("extra") != nullptr);
    CHECK_FALSE(fx.controller->hasChanges());
    CHECK(rowWithKey(fx.controller->profileFields(), "font_size").value("value").toDouble()
          == Catch::Approx(fx.cfg.findProfile("main")->fonts.value().size.pt));
    CHECK(fx.errors.isEmpty());
}

TEST_CASE("SettingsController: a side file changed on disk re-seeds the draft and says so", "[settings]")
{
    // Review Focus 1: a version written outside Settings wins, and the page shows it.
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    REQUIRE(fx.controller->setProfileField("font_size", 14.0).has_value());
    fx.fire();

    auto handEdited = *fx.cfg.findProfile("work");
    handEdited.fonts.value().size.pt = 20.0;
    {
        auto out = std::ofstream(fx.configDir() / config::relativePathOf(config::profileFile("work")));
        out << config::emitProfileYaml(handEdited);
    }
    REQUIRE(fx.controller->setProfileField("font_size", 15.0).has_value());
    fx.fire();

    REQUIRE_FALSE(fx.errors.isEmpty());
    CHECK(fx.errors.back().contains("changed outside Settings"));
    CHECK(fx.errors.back().contains("profiles/work.yml"));
    CHECK(rowWithKey(fx.controller->profileFields(), "font_size").value("value").toDouble()
          == Catch::Approx(20.0));
    CHECK(fx.cfg.findProfile("work")->fonts.value().size.pt == Catch::Approx(20.0));
}

TEST_CASE("SettingsController: addProfile writes New Profile and Revert removes it", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->addProfile();
    CHECK(fx.controller->editingProfile() == "New Profile");
    fx.fire();
    CHECK(rowNamed(fx.controller->profiles(), "New Profile").value("editable").toBool());
    CHECK(fx.controller->hasChanges());

    fx.controller->revertChanges();

    CHECK(rowNamed(fx.controller->profiles(), "New Profile").isEmpty());
    CHECK_FALSE(fx.fileContent(config::relativePathOf(config::profileFile("New Profile"))).has_value());
    CHECK(fx.controller->editingProfile().isEmpty());
    CHECK_FALSE(fx.controller->hasChanges());

    // A second "+" while the first is taken counts on.
    fx.controller->addProfile();
    fx.fire();
    fx.controller->addProfile();
    CHECK(fx.controller->editingProfile() == "New Profile 2");
}

TEST_CASE("SettingsController: a colour-scheme slot applies live", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideScheme("midnight");
    REQUIRE(fx.controller->setSchemeColor("background", "#101010").has_value());
    CHECK(fx.schemeFile("midnight")->defaultBackground != vtbackend::RGBColor { 0x10, 0x10, 0x10 });

    fx.fire();

    REQUIRE(fx.schemeFile("midnight").has_value());
    CHECK(fx.schemeFile("midnight")->defaultBackground == vtbackend::RGBColor { 0x10, 0x10, 0x10 });
}

TEST_CASE("SettingsController: a new profile with nothing to copy takes the config's edition", "[settings]")
{
    // BasicConfig states no defaults_edition, so it is Classic. With no default profile to copy, the new
    // profile is the built-in defaults -- of that edition, not of the newest.
    auto fx = Fixture(BasicConfig);
    REQUIRE(fx.cfg.defaultsEdition == config::DefaultsEdition::Classic);
    fx.cfg.defaultProfileName = std::string { "missing" };
    REQUIRE(fx.controller->createProfile().has_value());

    auto const fields = fx.controller->profileFields();
    CHECK(rowWithKey(fields, "terminal_size_columns").value("value").toInt() == 80);
    CHECK(rowWithKey(fields, "terminal_size_lines").value("value").toInt() == 25);
    CHECK(rowWithKey(fields, "dim_unfocused").value("value").toDouble() == Catch::Approx(0.0));
    CHECK(rowWithKey(fields, "bell_sound").value("value").toString() == "default");
    CHECK(rowWithKey(fields, "history_max_lines").value("value").toInt() == 1000);
}

TEST_CASE("SettingsController: a duplicate never takes a name another profile has", "[settings]")
{
    auto fx = Fixture("default_profile: main\n"
                      "profiles:\n"
                      "    main:\n"
                      "        show_title_bar: true\n"
                      "    main copy:\n"
                      "        show_title_bar: false\n");
    auto const copy = fx.controller->duplicateProfile("main");
    REQUIRE(copy.has_value());
    CHECK(*copy == "main copy 2");
}

TEST_CASE("SettingsController: delete removes a side-file profile", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");

    REQUIRE(fx.controller->deleteProfile("work").has_value());
    CHECK(fx.controller->editingProfile().isEmpty());
    fx.fire();
    CHECK(fx.cfg.findProfile("work") == nullptr);
    CHECK_FALSE(fx.fileContent(config::relativePathOf(config::profileFile("work"))).has_value());
}

TEST_CASE("SettingsController: a contour.yml profile cannot be deleted", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    auto const refused = fx.controller->deleteProfile("main");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == config::SettingsErrorKind::ReadOnly);
}

TEST_CASE("SettingsController: setDefaultProfile persists to settings.yml and overrides", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");

    REQUIRE(fx.controller->setDefaultProfile("work").has_value());
    CHECK(fx.controller->defaultProfile() == "work"); // the list follows the commit
    fx.fire();
    CHECK(fx.cfg.defaultProfileName.value() == "work");
    CHECK(fx.controller->defaultProfile() == "work");
    CHECK(fx.fileContent("settings.yml").has_value());
}

TEST_CASE("SettingsController: rename moves a side-file profile and follows the default + draft",
          "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    REQUIRE(fx.controller->setDefaultProfile("work").has_value());
    fx.fire();
    fx.controller->editProfile("work"); // the profile we are about to rename is the open draft

    REQUIRE(fx.controller->renameProfile("work", "office").has_value());
    fx.fire();

    // The side file moved, and the reloaded config carries the new name only.
    CHECK(fx.fileContent(std::filesystem::path { "profiles" } / "office.yml").has_value());
    CHECK_FALSE(fx.fileContent(std::filesystem::path { "profiles" } / "work.yml").has_value());
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
    fx.openSideProfile("work");

    CHECK_FALSE(fx.controller->renameProfile("main", "whatever").has_value()); // 'main' is from contour.yml
    auto const collision = fx.controller->renameProfile("work", "main"); // collides with contour.yml 'main'
    REQUIRE_FALSE(collision.has_value());
    CHECK(collision.error().kind == config::SettingsErrorKind::NameTaken);
    CHECK(fx.fileContent(std::filesystem::path { "profiles" } / "work.yml").has_value());
}

TEST_CASE("SettingsController: renaming onto a side-file profile is refused and does not clobber it",
          "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    fx.openSideProfile("home");
    REQUIRE(fx.controller->setProfileField("show_title_bar", false).has_value());
    fx.fire();

    CHECK_FALSE(fx.controller->renameProfile("home", "work").has_value());
    fx.fire();
    CHECK(fx.cfg.findProfile("work")->showTitleBar.value() == true);
    CHECK(fx.cfg.findProfile("home")->showTitleBar.value() == false);
}

TEST_CASE("SettingsController: rename moves a side-file color scheme with its draft", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideScheme("midnight");
    REQUIRE(fx.controller->setSchemeColor("background", "#101010").has_value());
    fx.fire();

    REQUIRE(fx.controller->renameColorScheme("midnight", "nightfall").has_value());
    fx.fire();
    CHECK(fx.schemeFile("nightfall").has_value());
    CHECK_FALSE(fx.schemeFile("midnight").has_value());
    CHECK(fx.schemeFile("nightfall")->defaultBackground == vtbackend::RGBColor { 0x10, 0x10, 0x10 });
    CHECK(fx.controller->editingScheme() == "nightfall");
}

TEST_CASE("SettingsController: rename keeps the colors of a side-file scheme named beyond ASCII",
          "[settings]")
{
    // A side-file scheme no profile uses is not loaded with the configuration, so the rename reads its colors
    // from its file.
    auto fx = Fixture(BasicConfig);
    auto palette = vtbackend::ColorPalette {};
    palette.defaultBackground = vtbackend::RGBColor { 0x10, 0x20, 0x30 };
    auto const name = std::string { vtpty::testing::NonAsciiNameUtf8 };
    REQUIRE(fx.store->write(config::colorSchemeFile(name), config::emitColorSchemeYaml(palette)).has_value());

    REQUIRE(fx.controller->renameColorScheme(QString::fromStdString(name), "nightfall").has_value());
    fx.fire();
    auto const renamed = fx.schemeFile("nightfall");
    REQUIRE(renamed.has_value());
    CHECK(renamed->defaultBackground == palette.defaultBackground);
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

TEST_CASE("SettingsController: a contour.yml color scheme takes no change, and its copy does", "[settings]")
{
    // An inline scheme shadows a side file at load, so writing one under its name would have no effect.
    auto fx = Fixture(InlineSchemeConfig);
    fx.controller->editColorScheme("solarized");
    auto const refused = fx.controller->setSchemeColor("background", "#111111");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == config::SettingsErrorKind::ReadOnly);
    fx.fire();
    CHECK_FALSE(fx.schemeFile("solarized").has_value());

    auto const copy = fx.controller->duplicateColorScheme("solarized");
    REQUIRE(copy.has_value());
    CHECK(*copy == "solarized copy");
    REQUIRE(fx.controller->setSchemeColor("background", "#111111").has_value());
    fx.fire();
    REQUIRE(fx.schemeFile("solarized copy").has_value());
    CHECK(fx.schemeFile("solarized copy")->defaultBackground == vtbackend::RGBColor { 0x11, 0x11, 0x11 });
}

TEST_CASE("SettingsController: renaming a color scheme onto a contour.yml scheme name is refused",
          "[settings]")
{
    auto fx = Fixture(InlineSchemeConfig);
    fx.openSideScheme("midnight");

    // Renaming it onto the inline 'solarized' would write a dead colorschemes/solarized.yml the loader
    // never resolves (the inline node wins); refuse it, and leave the source scheme untouched.
    CHECK_FALSE(fx.controller->renameColorScheme("midnight", "solarized").has_value());
    fx.fire();
    CHECK_FALSE(fx.schemeFile("solarized").has_value());
    CHECK(fx.schemeFile("midnight").has_value());
}

TEST_CASE("SettingsController: a non-side-file color scheme cannot be deleted", "[settings]")
{
    // deleteColorScheme must refuse a builtin/inline scheme (no side file): a removal treats an absent file
    // as success, so without the guard the controller would report a false 'deleted'.
    auto fx = Fixture(InlineSchemeConfig);
    CHECK_FALSE(fx.controller->deleteColorScheme("default").has_value());   // builtin
    CHECK_FALSE(fx.controller->deleteColorScheme("solarized").has_value()); // contour.yml inline
}

TEST_CASE("SettingsController: integer and bool profile fields round-trip", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    REQUIRE(fx.controller->setProfileField("slow_scrolling_time", 250).has_value());
    REQUIRE(fx.controller->setProfileField("maximized", true).has_value());
    fx.fire();

    auto const* work = fx.cfg.findProfile("work");
    REQUIRE(work != nullptr);
    CHECK(work->smoothLineScrolling.value() == std::chrono::milliseconds(250));
    CHECK(work->maximized.value() == true);
}

TEST_CASE("SettingsController: color-scheme selection supports a dark/light pair", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("dually");

    fx.controller->setColorSchemeMode("dual");
    CHECK(fx.controller->colorSchemeMode() == "dual");
    fx.controller->setColorSchemeLight("default");
    fx.controller->setColorSchemeDark("default");
    fx.fire();

    auto const* profile = fx.cfg.findProfile("dually");
    REQUIRE(profile != nullptr);
    CHECK(std::holds_alternative<config::DualColorConfig>(profile->colors.value()));
}

TEST_CASE("SettingsController: duplicate, edit and delete a color scheme", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    auto const mono = fx.controller->duplicateColorScheme("default");
    REQUIRE(mono.has_value());
    REQUIRE(fx.controller->setSchemeColor("background", "#123456").has_value());
    fx.fire();

    // The side file exists; re-opening it loads the color we set back.
    CHECK(fx.schemeFile(mono->toStdString()).has_value());
    fx.controller->editColorScheme(*mono);
    CHECK(rowWithKey(fx.controller->schemeColors(), "background").value("color").toString() == "#123456");

    REQUIRE(fx.controller->deleteColorScheme(*mono).has_value());
    fx.fire();
    CHECK_FALSE(fx.schemeFile(mono->toStdString()).has_value());
}

TEST_CASE("SettingsController: the command-block colour slots are editable and stay derived until set",
          "[settings]")
{
    using vtbackend::CommandBlockOutcome;
    auto fx = Fixture(BasicConfig);
    fx.openSideScheme("blocks");

    // Each slot shows the colour the palette resolves it to while the scheme leaves it unset.
    auto const palette = vtbackend::ColorPalette {};
    auto const colorOf = [&fx](QString const& key) {
        return rowWithKey(fx.controller->schemeColors(), key).value("color").toString();
    };
    CHECK(colorOf("block_status_success") == hexOf(palette.blockStatusColor(CommandBlockOutcome::Success)));
    CHECK(colorOf("block_status_failure") == hexOf(palette.blockStatusColor(CommandBlockOutcome::Failure)));
    CHECK(colorOf("block_status_running") == hexOf(palette.blockStatusColor(CommandBlockOutcome::Running)));
    for (auto const& [key, outcome]:
         { std::pair { "block_status_hover_success", CommandBlockOutcome::Success },
           std::pair { "block_status_hover_failure", CommandBlockOutcome::Failure },
           std::pair { "block_status_hover_running", CommandBlockOutcome::Running } })
    {
        INFO(key);
        CHECK(colorOf(QString::fromLatin1(key)) == hexOf(palette.blockStatusHoverColor(outcome)));
    }
    CHECK(colorOf("fold_marker") == hexOf(palette.foldMarkerColors().foreground));
    CHECK(colorOf("fold_marker_hover") == hexOf(palette.foldMarkerHoverColors().foreground));
    CHECK(colorOf("gutter_text") == hexOf(palette.gutterTextColor()));
    CHECK(colorOf("sticky_header_background") == hexOf(palette.stickyHeaderBackgroundColor()));
    CHECK(colorOf("sticky_header_separator") == hexOf(palette.stickyHeaderSeparatorColor()));

    REQUIRE(fx.controller->setSchemeColor("block_status_failure", "#ff0000").has_value());
    REQUIRE(fx.controller->setSchemeColor("block_status_hover_failure", "#ff8080").has_value());
    REQUIRE(fx.controller->setSchemeColor("fold_marker_hover", "#e0e0e0").has_value());
    REQUIRE(fx.controller->setSchemeColor("sticky_header_separator", "#123456").has_value());
    CHECK(colorOf("block_status_failure") == "#ff0000");
    fx.fire();

    auto const saved = fx.schemeFile("blocks");
    REQUIRE(saved.has_value());
    auto const statusOf = [&saved](CommandBlockOutcome outcome) {
        return saved->blockStatus.at(static_cast<std::size_t>(outcome));
    };
    CHECK(statusOf(CommandBlockOutcome::Failure) == vtbackend::RGBColor { 0xff, 0x00, 0x00 });
    CHECK(saved->blockStatusHover.at(static_cast<std::size_t>(CommandBlockOutcome::Failure))
          == vtbackend::RGBColor { 0xff, 0x80, 0x80 });
    REQUIRE(saved->foldMarkerHover.has_value());
    CHECK(saved->foldMarkerHover->foreground == vtbackend::RGBColor { 0xe0, 0xe0, 0xe0 });
    // On the page's own background: the gutter draws no band.
    CHECK(saved->foldMarkerHover->background == saved->defaultBackground);
    CHECK(saved->stickyHeaderSeparator == vtbackend::RGBColor { 0x12, 0x34, 0x56 });

    // The slots nobody edited are still unset, so they keep following the colours they derive from.
    // Seeding the draft with resolved values would have pinned colours the scheme never chose.
    CHECK_FALSE(statusOf(CommandBlockOutcome::Success).has_value());
    CHECK_FALSE(statusOf(CommandBlockOutcome::Running).has_value());
    CHECK_FALSE(
        saved->blockStatusHover.at(static_cast<std::size_t>(CommandBlockOutcome::Success)).has_value());
    CHECK_FALSE(saved->foldMarker.has_value());
    CHECK_FALSE(saved->gutterText.has_value());
    CHECK_FALSE(saved->stickyHeaderBackground.has_value());
}

TEST_CASE("SettingsController: a fold column set in the editor moves with the background it sits on",
          "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideScheme("folds");
    REQUIRE(fx.controller->setSchemeColor("fold_marker", "#808080").has_value());
    REQUIRE(fx.controller->setSchemeColor("background", "#203040").has_value());
    fx.fire();

    auto const saved = fx.schemeFile("folds");
    REQUIRE(saved.has_value());
    REQUIRE(saved->foldMarker.has_value());
    CHECK(saved->foldMarker->foreground == vtbackend::RGBColor { 0x80, 0x80, 0x80 });
    CHECK(saved->foldMarker->background == vtbackend::RGBColor { 0x20, 0x30, 0x40 });
}

TEST_CASE("SettingsController: an unset colour slot follows its source, and says so", "[settings]")
{
    using vtbackend::CommandBlockOutcome;
    auto fx = Fixture(BasicConfig);
    fx.openSideScheme("follows");
    auto const rowOf = [&fx](QString const& key) {
        return rowWithKey(fx.controller->schemeColors(), key);
    };
    auto const colorOf = [&rowOf](QString const& key) {
        return rowOf(key).value("color").toString();
    };
    auto const sourceOf = [&rowOf](QString const& key) {
        return rowOf(key).value("colorSource").toString();
    };

    CHECK(sourceOf("background") == "scheme");
    CHECK(sourceOf("normal_red") == "scheme");
    CHECK(sourceOf("block_status_failure") == "derived");
    CHECK(sourceOf("sticky_header_background") == "derived");

    // The failure colour derives from red ...
    auto expected = vtbackend::ColorPalette {};
    auto const failureBefore = colorOf("block_status_failure");
    REQUIRE(fx.controller->setSchemeColor("normal_red", "#aa1122").has_value());
    expected.palette.at(1) = vtbackend::RGBColor { 0xaa, 0x11, 0x22 };
    CHECK(colorOf("block_status_failure") == hexOf(expected.blockStatusColor(CommandBlockOutcome::Failure)));
    CHECK(colorOf("block_status_failure") != failureBefore);

    // ... and the sticky header's band from the background.
    auto const bandBefore = colorOf("sticky_header_background");
    REQUIRE(fx.controller->setSchemeColor("background", "#203040").has_value());
    expected.defaultBackground = vtbackend::RGBColor { 0x20, 0x30, 0x40 };
    CHECK(colorOf("sticky_header_background") == hexOf(expected.stickyHeaderBackgroundColor()));
    CHECK(colorOf("sticky_header_background") != bandBefore);
    CHECK(sourceOf("sticky_header_background") == "derived");
}

TEST_CASE("SettingsController: a derived colour is pinned in one edit, and follows again on reset",
          "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideScheme("pinned");
    auto const rowOf = [&fx](QString const& key) {
        return rowWithKey(fx.controller->schemeColors(), key);
    };
    auto const colorOf = [&rowOf](QString const& key) {
        return rowOf(key).value("color").toString();
    };
    auto const sourceOf = [&rowOf](QString const& key) {
        return rowOf(key).value("colorSource").toString();
    };
    auto const saved = [&fx]() {
        fx.fire();
        auto palette = fx.schemeFile("pinned");
        REQUIRE(palette.has_value());
        return *palette;
    };

    // Committing the colour a derived slot shows keeps that colour, whatever its source does next.
    auto const shown = colorOf("gutter_text");
    REQUIRE(fx.controller->setSchemeColor("gutter_text", shown).has_value());
    CHECK(sourceOf("gutter_text") == "pinned");
    REQUIRE(fx.controller->setSchemeColor("foreground", "#00ff00").has_value());
    CHECK(colorOf("gutter_text") == shown);
    auto const pinned = saved();
    REQUIRE(pinned.gutterText.has_value());
    CHECK(hexOf(*pinned.gutterText) == shown);

    // Reset lets it follow again.
    fx.controller->resetSchemeColor("gutter_text");
    CHECK(sourceOf("gutter_text") == "derived");
    auto expected = vtbackend::ColorPalette {};
    expected.defaultForeground = vtbackend::RGBColor { 0x00, 0xff, 0x00 };
    CHECK(colorOf("gutter_text") == hexOf(expected.gutterTextColor()));
    CHECK_FALSE(saved().gutterText.has_value());

    // A slot the scheme always sets has nothing to follow.
    auto const background = colorOf("background");
    fx.controller->resetSchemeColor("background");
    CHECK(colorOf("background") == background);
    CHECK(sourceOf("background") == "scheme");
}

TEST_CASE("SettingsController: remember window position persists and resets", "[settings][window-position]")
{
    auto fx = Fixture("remember_window_position: false\n");
    CHECK_FALSE(fx.cfg.rememberWindowPosition.value());
    fx.commitGlobal("remember_window_position", true);
    CHECK(fx.cfg.rememberWindowPosition.value());
    fx.resetGlobal("remember_window_position");
    CHECK_FALSE(fx.cfg.rememberWindowPosition.value());
}

TEST_CASE("SettingsController: global overrides write settings.yml, apply, and reset", "[settings]")
{
    auto fx = Fixture("default_profile: main\n"
                      "reflow_on_resize: true\n");
    auto const configDir = std::filesystem::path(fx.dir.path().toStdU16String());

    fx.commitGlobal("reflow_on_resize", false);
    CHECK(fx.cfg.reflowOnResize.value() == false);
    CHECK_FALSE(fx.cfg.snapToRows.value());
    fx.commitGlobal("snap_to_rows", true);
    CHECK(fx.cfg.snapToRows.value());
    fx.resetGlobal("snap_to_rows");
    CHECK_FALSE(fx.cfg.snapToRows.value());
    fx.commitGlobal("read_buffer_size", 32768);
    CHECK(fx.cfg.ptyReadBufferSize.value() == 32768);
    fx.commitGlobal("word_delimiters", "abc");
    CHECK(fx.cfg.wordDelimiters.value() == "abc");
    CHECK(std::filesystem::exists(configDir / "settings.yml"));

    // The overridden field is flagged as such in the model.
    auto overridden = false;
    for (auto const& row: fx.controller->globalFields())
        if (row.toMap().value("key").toString() == "reflow_on_resize")
            overridden = row.toMap().value("overridden").toBool();
    CHECK(overridden);

    // Reset falls back to the contour.yml value.
    fx.resetGlobal("reflow_on_resize");
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
    CHECK(options == QStringList { "terminal", "system", "dark", "light" });

    // Selecting a value persists it as the corresponding enum on the live config.
    fx.commitGlobal("theme", "dark");
    CHECK(fx.cfg.theme.value() == config::GuiTheme::Dark);

    fx.commitGlobal("theme", "system");
    CHECK(fx.cfg.theme.value() == config::GuiTheme::System);

    fx.commitGlobal("theme", "terminal");
    CHECK(fx.cfg.theme.value() == config::GuiTheme::Terminal);
}

TEST_CASE("SettingsController: the tab bar fields are global and offer their table's tokens", "[settings]")
{
    auto fx = Fixture(BasicConfig);

    // The options come from contour/TabBarMode.h, so the page can only ever offer tokens the
    // configuration reader accepts -- asserted against the table itself rather than a second literal
    // list, which is the whole point of the table.
    auto expected = QStringList {};
    for (auto const& info: config::configEnumValues<config::TabBarVisibility>())
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

    fx.commitGlobal("tab_bar_visibility", "Multiple");
    CHECK(fx.cfg.tabBarVisibility.value() == config::TabBarVisibility::Multiple);

    fx.commitGlobal("tab_bar_position", "Bottom");
    CHECK(fx.cfg.tabBarPosition.value() == config::TabBarPosition::Bottom);

    // They are no longer profile fields at all.
    auto profileKeys = QStringList {};
    fx.controller->editProfile("main");
    for (auto const& row: fx.controller->profileFields())
        profileKeys.push_back(row.toMap().value("key").toString());
    CHECK(!profileKeys.contains("tab_bar_position"));
    CHECK(!profileKeys.contains("tab_bar_visibility"));
}

TEST_CASE("SettingsController: the chrome style is a global field offering its table's tokens", "[settings]")
{
    auto fx = Fixture(BasicConfig);

    // Same discipline as the tab bar rows above: the options come from contour/UiStyle.h, so the page
    // cannot offer a token the configuration reader would then reject.
    auto expected = QStringList {};
    for (auto const& info: config::configEnumValues<config::UiStyle>())
        expected.push_back(QString::fromUtf8(info.token.data(), static_cast<qsizetype>(info.token.size())));

    auto options = QStringList {};
    auto type = QString {};
    auto keys = QStringList {};
    for (auto const& row: fx.controller->globalFields())
    {
        keys.push_back(row.toMap().value("key").toString());
        if (row.toMap().value("key").toString() == "ui_style")
        {
            type = row.toMap().value("type").toString();
            options = row.toMap().value("options").toStringList();
        }
    }
    CHECK(type == "enum");
    CHECK(options == expected);
    CHECK(keys.contains("ui_font_family"));
    CHECK(keys.contains("ui_font_size"));

    fx.commitGlobal("ui_style", "terminal");
    CHECK(fx.cfg.uiStyle.value() == config::UiStyle::Terminal);

    fx.commitGlobal("ui_font_family", "JetBrains Mono");
    CHECK(fx.cfg.uiFontFamily.value() == "JetBrains Mono");

    fx.commitGlobal("ui_font_size", 11.0);
    CHECK(fx.cfg.uiFontSize.value() == 11.0);

    // Resetting drops the override, so the value falls back to what contour.yml says.
    fx.resetGlobal("ui_style");
    CHECK(fx.cfg.uiStyle.value() == config::UiStyle::Native);
}

TEST_CASE("SettingsController: the window control style is a global field offering its table's tokens",
          "[settings]")
{
    auto fx = Fixture(BasicConfig);

    // Same discipline as ui_style above: the options come from contour/config/WindowControlStyle.hpp,
    // so the page cannot offer a token the configuration reader would then reject -- and a style
    // added to that table shows up in the settings page with neither file touched.
    auto expected = QStringList {};
    for (auto const& info: config::configEnumValues<config::WindowControlStyle>())
        expected.push_back(QString::fromUtf8(info.token.data(), static_cast<qsizetype>(info.token.size())));

    auto options = QStringList {};
    auto type = QString {};
    auto help = QString {};
    for (auto const& row: fx.controller->globalFields())
        if (row.toMap().value("key").toString() == "window_control_style")
        {
            type = row.toMap().value("type").toString();
            options = row.toMap().value("options").toStringList();
            help = row.toMap().value("help").toString();
        }

    CHECK(type == "enum");
    CHECK(options == expected);
    // Unlike its ui_* neighbours this one applies live, so its help must NOT carry their restart
    // note -- a false "takes effect after restart" is worse than none, because the user then closes
    // and reopens the window to get something they already had.
    CHECK(!help.contains("restart"));

    fx.commitGlobal("window_control_style", "macos");
    CHECK(fx.cfg.windowControlStyle.value() == config::WindowControlStyle::MacOS);

    // Resetting drops the override, so the value falls back to what contour.yml says -- here the
    // shipped default, which is the host-resolving one.
    fx.resetGlobal("window_control_style");
    CHECK(fx.cfg.windowControlStyle.value() == config::WindowControlStyle::Auto);
}

TEST_CASE("SettingsController: chrome_density offers both densities and round-trips", "[settings]")
{
    auto fx = Fixture(BasicConfig);

    auto expected = QStringList {};
    for (auto const& info: config::configEnumValues<config::ChromeDensity>())
        expected.push_back(QString::fromUtf8(info.token.data(), static_cast<qsizetype>(info.token.size())));

    auto options = QStringList {};
    auto type = QString {};
    for (auto const& row: fx.controller->globalFields())
        if (row.toMap().value("key").toString() == "chrome_density")
        {
            type = row.toMap().value("type").toString();
            options = row.toMap().value("options").toStringList();
        }

    CHECK(type == "enum");
    CHECK(options == expected);

    fx.commitGlobal("chrome_density", "minimal");
    CHECK(fx.cfg.chromeDensity.value() == config::ChromeDensity::Minimal);

    fx.resetGlobal("chrome_density");
    CHECK(fx.cfg.chromeDensity.value() == config::ChromeDensity::Comfortable);
}

TEST_CASE("SettingsController: secure_input offers its modes and round-trips", "[settings][secureinput]")
{
    auto fx = Fixture(BasicConfig);

    auto expected = QStringList {};
    for (auto const& info: config::configEnumValues<config::SecureInputMode>())
        expected.push_back(QString::fromUtf8(info.token.data(), static_cast<qsizetype>(info.token.size())));

    auto const row = rowWithKey(fx.controller->globalFields(), "secure_input");
    CHECK(row.value("type").toString() == "enum");
    CHECK(row.value("options").toStringList() == expected);
    CHECK(row.value("group").toInt() == static_cast<int>(SettingsGroup::GeneralAdvanced));

    // The edition's default is stated by contour.yml's loader; the override says the other one.
    auto const before = fx.cfg.secureInput.value();
    auto const other = before == config::SecureInputMode::Off ? "auto" : "off";
    fx.commitGlobal("secure_input", other);
    CHECK(fx.cfg.secureInput.value() != before);

    fx.resetGlobal("secure_input");
    CHECK(fx.cfg.secureInput.value() == before);
}

TEST_CASE("SettingsController: every global field's override survives save and reload", "[settings]")
{
    // A global row is two halves in two files: the descriptor here, which writes the override, and a
    // loader line in mergeGuiManagedSideFiles, which applies it. A row whose second half is missing
    // saves without complaint and then does nothing -- after the reload the page shows the old value,
    // with a Reset button for a change that never happened. Driving every row through the real store
    // and the real reload keeps the two halves in step, including for rows added later.
    auto fx = Fixture(BasicConfig);
    auto rows = 0;
    for (auto const& raw: fx.controller->globalFields())
    {
        auto const row = raw.toMap();
        auto const key = row.value("key").toString();
        if (row.value("controlName").toString() == "shortcut")
        {
            // A chord is recorded as Qt's key and modifiers and shown as the platform spells it, so the row's
            // value is not what was committed: the same two halves, driven through the recorder's door.
            INFO("global field: " << key.toStdString());
            REQUIRE(fx.controller->setHotkey(Qt::Key_QuoteLeft, Qt::ControlModifier).isEmpty());
            fx.fire();
            auto const recorded = rowWithKey(fx.controller->globalFields(), key);
            CHECK_FALSE(recorded.value("value").toString().isEmpty());
            CHECK(recorded.value("overridden").toBool());
            ++rows;
            continue;
        }
        auto const wanted = differentValue(row);
        INFO("global field: " << key.toStdString() << " -> " << wanted.toString().toStdString());
        fx.commitGlobal(key, wanted);
        auto const after = rowWithKey(fx.controller->globalFields(), key);
        CHECK(after.value("value") == wanted);
        CHECK(after.value("overridden").toBool());
        ++rows;
    }
    CHECK(rows > 0);
}

TEST_CASE("SettingsController: the merge of settings.yml applies every global row", "[settings]")
{
    // A row whose override settings.yml does not apply would save and then do nothing.
    auto fx = Fixture(BasicConfig);
    for (auto const& raw: fx.controller->globalFields())
    {
        auto const key = raw.toMap().value("key").toString().toStdString();
        INFO(key);
        CHECK(config::globalOverrideFor(key) != nullptr);
    }
}

TEST_CASE("SettingsController: an override the loader refused is not shown as overridden", "[settings]")
{
    // settings.yml is hand-editable. An override the loader cannot read leaves the value contour.yml gives,
    // so a Reset button beside it would offer to undo a change that never took.
    auto fx = Fixture(BasicConfig);
    {
        auto out = std::ofstream(std::filesystem::path(fx.dir.path().toStdU16String()) / "settings.yml");
        out << "reflow_on_resize: maybe\n"
               "gutter:\n"
               "    line_numbers: relative\n"
               "    timestamps: maybe\n"
               "theme: dark\n";
    }
    fx.reload();

    auto const rows = fx.controller->globalFields();
    auto const overridden = [&rows](QString const& key) {
        return rowWithKey(rows, key).value("overridden").toBool();
    };
    CHECK(fx.cfg.reflowOnResize.value() == true);
    CHECK_FALSE(overridden("reflow_on_resize"));
    CHECK(fx.cfg.gutter.value().timestamps == false);
    CHECK_FALSE(overridden("gutter.timestamps"));
    // A leaf beside a refused one says whether it took itself: it does where its section's loader reads each
    // leaf under a guard of its own, and not where the whole section is put back.
    CHECK(overridden("gutter.line_numbers")
          == (fx.cfg.gutter.value().lineNumbers == vtbackend::LineNumberMode::Relative));
    // An override that took is shown as one.
    CHECK(fx.cfg.theme.value() == config::GuiTheme::Dark);
    CHECK(overridden("theme"));
}

TEST_CASE("SettingsController: an override spelled otherwise than the page writes it is shown when it took",
          "[settings]")
{
    // The loader reads a bool, a number or an enum in more than one spelling. One that took is overridden,
    // whatever its spelling, and offers its Reset.
    auto fx = Fixture(BasicConfig);
    {
        auto out = std::ofstream(std::filesystem::path(fx.dir.path().toStdU16String()) / "settings.yml");
        out << "reflow_on_resize: False\n"
               "read_buffer_size: 0x10000\n"
               "theme: Dark\n"
               "gutter:\n"
               "    line_numbers: Relative\n";
    }
    fx.reload();
    REQUIRE(fx.cfg.reflowOnResize.value() == false);
    REQUIRE(fx.cfg.ptyReadBufferSize.value() == 0x10000);
    REQUIRE(fx.cfg.theme.value() == config::GuiTheme::Dark);
    REQUIRE(fx.cfg.gutter.value().lineNumbers == vtbackend::LineNumberMode::Relative);

    auto const rows = fx.controller->globalFields();
    for (auto const* key: { "reflow_on_resize", "read_buffer_size", "theme", "gutter.line_numbers" })
    {
        INFO(key);
        CHECK(rowWithKey(rows, QString::fromLatin1(key)).value("overridden").toBool());
    }
}

TEST_CASE("SettingsController: a dotted global field overrides one leaf of its section", "[settings]")
{
    auto fx = Fixture(R"(
default_profile: main
gutter:
    timestamps: true
    timestamp_format: "%H:%M"
folding:
    show_markers: false
profiles:
    main:
        show_title_bar: true
)");
    auto const configDir = std::filesystem::path(fx.dir.path().toStdU16String());

    fx.commitGlobal("gutter.line_numbers", "relative");
    fx.commitGlobal("folding.enabled", false);

    // The overrides took ...
    CHECK(fx.cfg.gutter.value().lineNumbers == vtbackend::LineNumberMode::Relative);
    CHECK(fx.cfg.folding.value().enabled == false);
    // ... and nothing else in either section moved.
    CHECK(fx.cfg.gutter.value().timestamps == true);
    CHECK(fx.cfg.gutter.value().timestampFormat == "%H:%M");
    CHECK(fx.cfg.folding.value().showMarkers == false);

    // settings.yml reads like contour.yml: a section holding the one leaf, never a literal dotted key.
    // Closed again at once: Windows refuses to replace a file that is open, and Reset below rewrites it.
    auto const written = [&configDir]() {
        auto file = std::ifstream(configDir / "settings.yml");
        return YAML::Load(file);
    }();
    REQUIRE(written["gutter"].IsMap());
    CHECK(written["gutter"]["line_numbers"].as<std::string>() == "relative");
    CHECK(written["gutter"].size() == std::size_t { 1 });
    CHECK_FALSE(written["gutter.line_numbers"].IsDefined());

    // Only the overridden row says so, and Reset brings back what contour.yml says (here: the default).
    CHECK(rowWithKey(fx.controller->globalFields(), "gutter.line_numbers").value("overridden").toBool());
    CHECK_FALSE(rowWithKey(fx.controller->globalFields(), "gutter.timestamps").value("overridden").toBool());
    fx.resetGlobal("gutter.line_numbers");
    CHECK(fx.cfg.gutter.value().lineNumbers == vtbackend::LineNumberMode::Off);
    CHECK(fx.cfg.gutter.value().timestamps == true);
}

TEST_CASE("SettingsController: the command-block enum rows offer the loader's own tokens", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    auto const rows = fx.controller->globalFields();

    // Spelled as the configuration spells them, and taken from the table the loader parses with.
    CHECK(tokensOf<vtbackend::LineNumberMode>() == QStringList { "off", "absolute", "relative", "hybrid" });
    CHECK(rowWithKey(rows, "gutter.line_numbers").value("options").toStringList()
          == tokensOf<vtbackend::LineNumberMode>());
    CHECK(tokensOf<vtbackend::FoldJumpBehavior>() == QStringList { "expand", "skip" });
    CHECK(rowWithKey(rows, "folding.on_jump_into_fold").value("options").toStringList()
          == tokensOf<vtbackend::FoldJumpBehavior>());

    // A token the loader would reject is refused rather than written.
    CHECK_FALSE(fx.controller->setGlobalField("gutter.line_numbers", "sideways").has_value());
    CHECK_FALSE(
        rowWithKey(fx.controller->globalFields(), "gutter.line_numbers").value("overridden").toBool());
}

TEST_CASE("SettingsController: an invalid timestamp format is reported and not saved", "[settings]")
{
    auto fx = Fixture(BasicConfig);

    fx.controller->commitGlobalField("gutter.timestamp_format", "%K");
    fx.controller->commitGlobalField("gutter.timestamp_format", "{%H}");
    fx.controller->commitGlobalField("gutter.timestamp_format", "");
    REQUIRE(fx.errors.size() == qsizetype { 3 });
    for (auto const& message: fx.errors)
        CHECK(message.contains("timestamp format"));
    CHECK(fx.cfg.gutter.value().timestampFormat == "%H:%M:%S");
    CHECK_FALSE(
        rowWithKey(fx.controller->globalFields(), "gutter.timestamp_format").value("overridden").toBool());

    fx.commitGlobal("gutter.timestamp_format", "%H:%M");
    CHECK(fx.cfg.gutter.value().timestampFormat == "%H:%M");
}

TEST_CASE("SettingsController: every global row refuses a value its loader cannot take", "[settings]")
{
    auto fx = Fixture(BasicConfig);

    // A negative size or count wraps where it is used, a negative font size draws nothing, and a word no
    // option is spelled as is no value at all.
    for (auto const& [key, value]: std::array {
             std::pair { "read_buffer_size", QVariant(0) },
             std::pair { "command_palette_recent_count", QVariant(-1) },
             std::pair { "ui_font_size", QVariant(-1.0) },
             std::pair { "theme", QVariant(QStringLiteral("sepia")) },
             std::pair { "tab_bar_position", QVariant(QStringLiteral("middle")) },
             std::pair { "text_scaling_method", QVariant(QStringLiteral("blur")) },
             std::pair { "renderer.gpu", QVariant(QStringLiteral("fastest")) },
         })
    {
        INFO(key);
        fx.errors.clear();
        fx.controller->commitGlobalField(QString::fromLatin1(key), value);
        CHECK(fx.errors.size() == qsizetype { 1 });
        CHECK_FALSE(
            rowWithKey(fx.controller->globalFields(), QString::fromLatin1(key)).value("overridden").toBool());
    }

    // Zero recent commands is a choice: it turns the list off.
    fx.commitGlobal("command_palette_recent_count", 0);
    CHECK(fx.cfg.commandPaletteRecentCount.value() == 0);
}

TEST_CASE("SettingsController: the numeric command-block rows are range-checked", "[settings]")
{
    auto fx = Fixture(BasicConfig);

    CHECK_FALSE(fx.controller->setGlobalField("gutter.line_number_width", 2).has_value());
    CHECK_FALSE(fx.controller->setGlobalField("gutter.line_number_width", 11).has_value());
    fx.commitGlobal("gutter.line_number_width", 10);
    CHECK(fx.cfg.gutter.value().lineNumberWidth == 10);

    CHECK_FALSE(fx.controller->setGlobalField("command_blocks.max_records", 0).has_value());
    // The loader clamps above config::MaxCommandBlockRecords; the page refuses instead of saving a value
    // that would silently load as something else.
    CHECK_FALSE(fx.controller
                    ->setGlobalField("command_blocks.max_records",
                                     static_cast<int>(config::MaxCommandBlockRecords) + 1)
                    .has_value());
    fx.commitGlobal("command_blocks.max_records", 1);
    CHECK(fx.cfg.commandBlocks.value().maxRecords == std::size_t { 1 });
}

TEST_CASE("SettingsController: the command-block global rows say when and where they apply", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    auto const helpOf = [&fx](QString const& key) {
        return rowWithKey(fx.controller->globalFields(), key).value("help").toString();
    };

    // The dot marks every finished command that shows no fold control, which is all of them while the fold
    // markers are off -- not only one that printed nothing.
    auto const exitStatus = helpOf("gutter.exit_status");
    CHECK(exitStatus.startsWith("Color each"));
    CHECK(exitStatus.contains("shows no fold control"));
    CHECK(exitStatus.contains("any one while fold markers are off"));

    // A running session keeps the store it was opened with.
    CHECK(helpOf("command_blocks.max_records").contains("sessions opened afterwards"));
}

TEST_CASE("SettingsController: a blank pager is refused, and says how to go back to the configured one",
          "[settings]")
{
    auto fx = Fixture(BasicConfig);
    auto const configured = fx.cfg.commandBlocks.value().pager;

    // Every OpenCommandOutput without a program of its own would then fail for want of one.
    fx.controller->commitGlobalField("command_blocks.pager", "");
    fx.controller->commitGlobalField("command_blocks.pager", "   ");
    REQUIRE(fx.errors.size() == qsizetype { 2 });
    CHECK(fx.errors.first().contains("Reset"));
    CHECK(fx.cfg.commandBlocks.value().pager == configured);
    CHECK_FALSE(
        rowWithKey(fx.controller->globalFields(), "command_blocks.pager").value("overridden").toBool());

    fx.commitGlobal("command_blocks.pager", "most");
    CHECK(fx.cfg.commandBlocks.value().pager == "most");
}

TEST_CASE("SettingsController: the pager's format is a global row", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    auto const row = rowWithKey(fx.controller->globalFields(), "command_blocks.pager_format");
    REQUIRE(!row.isEmpty());
    CHECK(row.value("type").toString() == "enum");
    CHECK(row.value("options").toStringList() == QStringList { "sgr", "plain" });
    CHECK(row.value("value").toString() == "sgr");
    // Each value says what it does to the file the pager is given.
    auto const help = row.value("help").toString();
    CHECK(help.contains("renders colors"));
    CHECK(help.contains("sgr keeps"));
    CHECK(help.contains("plain writes"));

    fx.commitGlobal("command_blocks.pager_format", "plain");
    CHECK(fx.cfg.commandBlocks.value().pagerFormat == actions::OutputFormat::Plain);
    CHECK_FALSE(fx.controller->setGlobalField("command_blocks.pager_format", "html").has_value());
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

TEST_CASE("SettingsController: a keybinding's trigger names its click count and its whole character",
          "[settings][semanticblocks]")
{
    // Ctrl+triple-click is bound here, beside the Ctrl+click it extends: the one Contour selects a command's
    // output with by default is a built-in fallback, never a row of the configuration.
    auto fx = Fixture(
        "default_profile: main\n"
        "profiles:\n"
        "    main:\n"
        "        shell: /bin/sh\n"
        "input_mapping:\n"
        "    - { mods: [Control], mouse: Left, action: FollowHyperlink }\n"
        "    - { mods: [Control], mouse: Left, clicks: 3, action: SelectCommandBlock, target: pointer,"
        " part: output }\n"
        "    - { mods: [Control], key: '\xF0\x9F\x98\x80', action: ScreenshotVT }\n");
    auto triggers = QStringList {};
    for (auto const& row: fx.controller->keybindings())
        triggers.append(row.toMap().value("trigger").toString());
    INFO(triggers.join(QStringLiteral(" | ")).toStdString());
    auto const triggersOf = [&fx](QString const& action) {
        auto found = QStringList {};
        for (auto const& raw: fx.controller->keybindings())
            if (raw.toMap().value("action").toString().startsWith(action))
                found.append(raw.toMap().value("trigger").toString());
        return found;
    };

    // The count is what tells the two mouse rows apart, and it reads " x3": ASCII, as
    // vtbackend::clickCountSuffix spells it, not a multiplication sign.
    auto const tripleClick = triggersOf(QStringLiteral("SelectCommandBlock"));
    REQUIRE(tripleClick.size() == qsizetype { 1 });
    CHECK(tripleClick.first().endsWith(QStringLiteral("Left x3")));
    CHECK(triggersOf(QStringLiteral("FollowHyperlink")).contains(tripleClick.first().chopped(3)));
    // U+1F600, not its low 16 bits.
    CHECK(std::ranges::any_of(triggers, [](QString const& trigger) {
        return trigger.endsWith(QString::fromUcs4(U"\U0001F600", 1));
    }));
}

TEST_CASE("SettingsController: the bindings viewer lists the built-in bindings no configured row shadows",
          "[settings][semanticblocks]")
{
    /// The rows of the bindings viewer whose trigger is @p trigger.
    auto const rowsFor = [](SettingsController const& controller, QString const& trigger) {
        auto found = QVariantList {};
        for (auto const& raw: controller.keybindings())
            if (raw.toMap().value("trigger").toString() == trigger)
                found.push_back(raw);
        return found;
    };

    SECTION("a default configuration")
    {
        // No configuration lists these, so without the built-in rows the viewer would not show how to select
        // a command's output at all.
        auto fx = Fixture(BasicConfig);
        auto const builtIn = [&](QString const& trigger, QString const& action) {
            auto const rows = rowsFor(*fx.controller, trigger);
            INFO(trigger.toStdString());
            REQUIRE(rows.size() == qsizetype { 1 });
            CHECK(rows.first().toMap().value("action").toString().startsWith(action));
            CHECK(rows.first().toMap().value("builtIn").toBool());
        };
        builtIn(QStringLiteral("Control+Left x3"), QStringLiteral("SelectCommandBlock"));
        builtIn(QStringLiteral("Right"), QStringLiteral("OpenContextMenu"));
        builtIn(QStringLiteral("WheelLeft"), QStringLiteral("SwitchToTabLeft"));
        builtIn(QStringLiteral("Shift+Control+G"), QStringLiteral("OpenCommandOutput"));

        // The defaults list Ctrl+Tab as well, so its built-in twin is shadowed and the chord is listed once.
        auto const tab = rowsFor(*fx.controller, QStringLiteral("Control+Tab"));
        REQUIRE(tab.size() == qsizetype { 1 });
        CHECK_FALSE(tab.first().toMap().value("builtIn").toBool());

        // What the configuration lists is not marked built-in.
        auto configured = 0;
        for (auto const& raw: fx.controller->keybindings())
            if (!raw.toMap().value("builtIn").toBool())
                ++configured;
        CHECK(configured > 0);
    }

    SECTION("a configured row shadows the built-in one, and a switched-off built-in row is not listed")
    {
        auto fx =
            Fixture("default_profile: main\n"
                    "tab_switch_on_horizontal_wheel: false\n"
                    "profiles:\n"
                    "    main:\n"
                    "        shell: /bin/sh\n"
                    "input_mapping:\n"
                    "    - { mods: [Control], key: Tab, action: ScreenshotVT }\n"
                    "    - { mods: [Control], mouse: Left, clicks: 3, action: CopySelection }\n"
                    "    - { mods: [Control], mouse: Left, clicks: 3, action: ClearHistoryAndReset }\n");
        auto const tab = rowsFor(*fx.controller, QStringLiteral("Control+Tab"));
        REQUIRE(tab.size() == qsizetype { 1 });
        CHECK(tab.first().toMap().value("action").toString() == "ScreenshotVT");
        CHECK_FALSE(tab.first().toMap().value("builtIn").toBool());
        CHECK(rowsFor(*fx.controller, QStringLiteral("WheelLeft")).isEmpty());

        // A binding that fires two actions shows both, in the order they run.
        auto const tripleClick = rowsFor(*fx.controller, QStringLiteral("Control+Left x3"));
        REQUIRE(tripleClick.size() == qsizetype { 1 });
        CHECK(tripleClick.first().toMap().value("action").toString()
              == "CopySelection, ClearHistoryAndReset");

        // Ctrl+Shift+Tab is a chord of its own, so its built-in row stays.
        CHECK(rowsFor(*fx.controller, QStringLiteral("Shift+Control+Tab")).size() == qsizetype { 1 });
    }

    SECTION("a configured row only for some modes leaves the built-in row for the rest")
    {
        auto fx = Fixture("default_profile: main\n"
                          "profiles:\n"
                          "    main:\n"
                          "        shell: /bin/sh\n"
                          "input_mapping:\n"
                          "    - { mods: [Control], key: Tab, mode: 'Insert', action: ScreenshotVT }\n");
        CHECK(rowsFor(*fx.controller, QStringLiteral("Control+Tab")).size() == qsizetype { 2 });
    }

    SECTION("configured rows that cover every mode only together shadow the built-in row")
    {
        // Neither row covers every mode alone: one answers on the alternate screen, the other off it.
        auto fx = Fixture(
            "default_profile: main\n"
            "profiles:\n"
            "    main:\n"
            "        shell: /bin/sh\n"
            "input_mapping:\n"
            "    - { mods: [Control], key: Tab, mode: 'AltScreen', action: ScreenshotVT }\n"
            "    - { mods: [Control], key: Tab, mode: '~AltScreen', action: ClearHistoryAndReset }\n");
        auto const tab = rowsFor(*fx.controller, QStringLiteral("Control+Tab"));
        CHECK(tab.size() == qsizetype { 2 });
        CHECK(std::ranges::none_of(
            tab, [](QVariant const& row) { return row.toMap().value("builtIn").toBool(); }));
    }

    SECTION("a configured row for the modes the built-in row answers in shadows it")
    {
        // The built-in Ctrl+triple-click answers on the primary screen alone, so a row for the primary screen
        // is all it takes.
        auto fx = Fixture(
            "default_profile: main\n"
            "profiles:\n"
            "    main:\n"
            "        shell: /bin/sh\n"
            "input_mapping:\n"
            "    - { mods: [Control], mouse: Left, clicks: 3, mode: '~AltScreen', action: CopySelection }\n");
        auto const tripleClick = rowsFor(*fx.controller, QStringLiteral("Control+Left x3"));
        REQUIRE(tripleClick.size() == qsizetype { 1 });
        CHECK_FALSE(tripleClick.first().toMap().value("builtIn").toBool());
    }
}

TEST_CASE("SettingsController: a locked configuration stages nothing", "[settings]")
{
    // Review Focus 3: gui_config_locked refuses every change before anything is staged, and says why.
    auto fx = Fixture("default_profile: main\ngui_config_locked: true\n"
                      "profiles:\n    main:\n        show_title_bar: true\n");
    CHECK(fx.controller->locked() == true);
    REQUIRE(fx.store->write(config::profileFile("work"), config::emitProfileYaml(*fx.cfg.findProfile("main")))
                .has_value());
    REQUIRE(fx.store
                ->write(config::colorSchemeFile("midnight"),
                        config::emitColorSchemeYaml(vtbackend::ColorPalette {}))
                .has_value());
    fx.reload();
    fx.controller->editProfile("work");
    fx.controller->editColorScheme("midnight");
    CHECK(fx.controller->editingReadOnly());

    fx.controller->commitProfileField("font_size", 14.0);
    fx.controller->commitGlobalField("reflow_on_resize", false);
    fx.controller->commitGlobalReset("reflow_on_resize");
    fx.controller->commitSchemeColor("background", "#101010");
    fx.controller->duplicate("main");
    fx.controller->addProfile();
    fx.controller->duplicateScheme("midnight");
    fx.controller->requestDeleteProfile("work");
    fx.controller->requestRenameProfile("work", "office");
    fx.controller->requestDefaultProfile("work");
    fx.controller->requestDeleteScheme("midnight");
    fx.controller->requestRenameScheme("midnight", "nightfall");

    CHECK(fx.scheduler->pendingDelay() == std::nullopt);
    CHECK(fx.scheduler->scheduleCount() == 0);
    CHECK_FALSE(fx.controller->hasChanges());
    REQUIRE(fx.errors.size() == 12);
    for (auto const& error: fx.errors)
        CHECK(error == QString::fromUtf8(config::SettingsLockedMessage));
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
    fx.openSideProfile("work");

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
    fx.openSideProfile("work");

    for (auto const& raw: fx.controller->profileFields())
    {
        auto const row = raw.toMap();
        if (row.value("type").toString() != "enum")
            continue;

        auto const key = row.value("key").toString();
        for (auto const& option: row.value("options").toStringList())
        {
            REQUIRE(fx.controller->setProfileField(key, option).has_value());
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
    fx.openSideProfile("work");

    auto const row = rowWithKey(fx.controller->profileFields(), "history_max_lines");
    REQUIRE(!row.isEmpty());
    CHECK(row.value("value").toInt() == -1);

    SECTION("and writing -1 back keeps it unlimited")
    {
        REQUIRE(fx.controller->setProfileField("history_max_lines", -1).has_value());
        CHECK(rowWithKey(fx.controller->profileFields(), "history_max_lines").value("value").toInt() == -1);
    }

    SECTION("while a finite limit is still a finite limit")
    {
        REQUIRE(fx.controller->setProfileField("history_max_lines", 4200).has_value());
        CHECK(rowWithKey(fx.controller->profileFields(), "history_max_lines").value("value").toInt() == 4200);
    }
}

TEST_CASE("SettingsController: profile fields are grouped for the settings page", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");

    auto groups = QStringList {};
    for (auto const& raw: fx.controller->profileFields())
    {
        auto const row = raw.toMap();
        auto const group = row.value("groupTitle").toString();
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

TEST_CASE("SettingsController: the command-block profile fields round-trip through a saved profile",
          "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("blocks");

    REQUIRE(fx.controller->setProfileField("scrollbar_marks_commands", false).has_value());
    REQUIRE(fx.controller->setProfileField("sticky_header_mode", "always").has_value());
    REQUIRE(fx.controller->setProfileField("sticky_header_show_evicted", false).has_value());
    REQUIRE(fx.controller->setProfileField("notify_on_command_finish_when", "hidden").has_value());
    REQUIRE(fx.controller->setProfileField("notify_on_command_finish_min_duration", 42).has_value());
    REQUIRE(fx.controller->setProfileField("notify_on_command_finish_outcome", "failure").has_value());
    REQUIRE(fx.controller->setProfileField("notify_on_command_finish_action", "notify_bell").has_value());
    REQUIRE(fx.controller->setProfileField("notify_on_command_finish_clear_on", "next").has_value());
    fx.fire();

    auto const* blocks = fx.cfg.findProfile("blocks");
    REQUIRE(blocks != nullptr);
    using vtbackend::ScrollbarMarkSource;
    auto const marks = blocks->scrollbar.value().marks;
    CHECK(marks.test(ScrollbarMarkSource::Failures));
    CHECK_FALSE(marks.test(ScrollbarMarkSource::Commands));
    CHECK(marks.test(ScrollbarMarkSource::UserMarks));
    CHECK(blocks->stickyHeader.value().mode == vtbackend::StickyHeaderMode::Always);
    CHECK(blocks->stickyHeader.value().showEvicted == false);
    auto const& notify = blocks->notifyOnCommandFinish.value();
    CHECK(notify.when == config::NotifyWhen::Hidden);
    CHECK(notify.minDuration == std::chrono::seconds(42));
    CHECK(notify.outcome == config::NotifyOutcome::Failure);
    CHECK(notify.action == config::NotifyAction::NotifyBell);
    CHECK(notify.clearOn == config::NotifyClearOn::Next);

    // The page shows what was written.
    auto const fields = fx.controller->profileFields();
    CHECK(rowWithKey(fields, "scrollbar_marks_commands").value("value").toBool() == false);
    CHECK(rowWithKey(fields, "sticky_header_mode").value("value").toString() == "always");
    CHECK(rowWithKey(fields, "sticky_header_show_evicted").value("value").toBool() == false);
    CHECK(rowWithKey(fields, "notify_on_command_finish_min_duration").value("value").toInt() == 42);

    SECTION("every scrollbar source off is an empty list, not the default")
    {
        REQUIRE(fx.controller->setProfileField("scrollbar_marks_failures", false).has_value());
        REQUIRE(fx.controller->setProfileField("scrollbar_marks_running", false).has_value());
        REQUIRE(fx.controller->setProfileField("scrollbar_marks_user_marks", false).has_value());
        REQUIRE(fx.controller->setProfileField("scrollbar_marks_search_matches", false).has_value());
        fx.fire();
        auto const* saved = fx.cfg.findProfile("blocks");
        REQUIRE(saved != nullptr);
        CHECK(saved->scrollbar.value().marks.none());
    }

    SECTION("a negative minimum duration is stored as zero")
    {
        REQUIRE(fx.controller->setProfileField("notify_on_command_finish_min_duration", -5).has_value());
        CHECK(rowWithKey(fx.controller->profileFields(), "notify_on_command_finish_min_duration")
                  .value("value")
                  .toInt()
              == 0);
    }
}

TEST_CASE("SettingsController: the command-block profile fields form their own group", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");

    auto keys = QStringList {};
    for (auto const& raw: fx.controller->profileFields())
        if (raw.toMap().value("groupTitle").toString() == "Command blocks")
            keys.push_back(raw.toMap().value("key").toString());
    CHECK(keys
          == QStringList { "scrollbar_marks_failures",
                           "scrollbar_marks_commands",
                           "scrollbar_marks_running",
                           "scrollbar_marks_user_marks",
                           "scrollbar_marks_search_matches",
                           "sticky_header_mode",
                           "sticky_header_show_evicted",
                           "notify_on_command_finish_when",
                           "notify_on_command_finish_min_duration",
                           "notify_on_command_finish_outcome",
                           "notify_on_command_finish_action",
                           "notify_on_command_finish_clear_on" });

    // The enum rows offer exactly the documented spellings -- which are the loader's tables' tokens, since
    // the rows read their options from those tables.
    auto const fields = fx.controller->profileFields();
    auto const optionsOf = [&fields](QString const& key) {
        return rowWithKey(fields, key).value("options").toStringList();
    };
    CHECK(optionsOf("sticky_header_mode") == QStringList { "never", "scrolled", "always" });
    CHECK(optionsOf("notify_on_command_finish_when")
          == QStringList { "never", "unfocused", "hidden", "always" });
    CHECK(optionsOf("notify_on_command_finish_outcome") == QStringList { "any", "failure" });
    CHECK(optionsOf("notify_on_command_finish_action") == QStringList { "notify", "bell", "notify_bell" });
    CHECK(optionsOf("notify_on_command_finish_clear_on") == QStringList { "focus", "next", "never" });

    // The marks are drawn on the scrollbar, so the position row says where it is by default -- and that an
    // older contour.yml says otherwise -- and that hiding it hides them too.
    auto const positionHelp = rowWithKey(fields, "scrollbar_position").value("help").toString();
    CHECK(positionHelp.contains("Right is the default for new configurations"));
    CHECK(positionHelp.contains("written by an earlier version keeps hidden"));
    CHECK(positionHelp.contains("Hidden also hides the command marks"));

    // The commands source marks the commands the failures source does not: a failed command has one mark.
    auto const commandsHelp = rowWithKey(fields, "scrollbar_marks_commands").value("help").toString();
    CHECK(commandsHelp.contains("did not fail"));
    CHECK(rowWithKey(fields, "scrollbar_marks_running").value("help").toString().contains("still running"));

    // A pane is hidden wherever it is off screen -- behind another tab, a zoomed sibling or a minimised
    // window -- not only when its tab is not shown.
    auto const whenHelp = rowWithKey(fields, "notify_on_command_finish_when").value("help").toString();
    CHECK(whenHelp.contains("off screen"));
    CHECK(whenHelp.contains("zoomed sibling"));
}

// }}}
// {{{ Indicator status line bridge

TEST_CASE("SettingsController: the indicator bridge round-trips a segment unchanged", "[settings]")
{
    // The whole point of the bridge: opening the settings page parses each segment into items, and saving
    // serializes them back. A segment nobody edited must come back byte-identical, or merely visiting the
    // page rewrites the user's profile.
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");

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
    fx.openSideProfile("work");

    auto const items = fx.controller->parseIndicatorSegment(fx.controller->indicatorSegment(0));
    auto tabs = QVariantMap {};
    for (auto const& raw: items)
        if (raw.toMap().value("type").toString() == "Tabs")
            tabs = raw.toMap();

    REQUIRE(!tabs.isEmpty());
    CHECK(tabs.value("hasActiveColor").toBool());
    CHECK(tabs.value("activeColor").toString() == "#FFFF00");

    // ... and it is still there after a save.
    auto const readBack =
        fx.controller->parseIndicatorSegment(fx.controller->serializeIndicatorSegment(items));
    auto tabsAgain = QVariantMap {};
    for (auto const& raw: readBack)
        if (raw.toMap().value("type").toString() == "Tabs")
            tabsAgain = raw.toMap();
    CHECK(tabsAgain.value("activeColor").toString() == "#FFFF00");
}

TEST_CASE("SettingsController: the indicator bridge exposes every flag the template supports", "[settings]")
{
    // Four of the thirteen used to be exposed, so a profile using Underline or Inverse lost it on save.
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");

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
    auto const readBack =
        fx.controller->parseIndicatorSegment(fx.controller->serializeIndicatorSegment(items));
    REQUIRE(readBack.size() == 1);
    auto const after = readBack.first().toMap().value("flags").toMap();
    CHECK(after.value("Underline").toBool());
    CHECK(after.value("Inverse").toBool());
    CHECK(after.value("CrossedOut").toBool());
}

TEST_CASE("SettingsController: the indicator bridge keeps a Command's Interval and comma commands",
          "[settings]")
{
    // An Interval= has no control in the page, so the only way it survives an edit is by being carried
    // through the row; and a command containing a comma is only expressible as the {Command:<raw>}
    // shorthand, which the serializer has to choose for it.
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");

    auto items = fx.controller->parseIndicatorSegment("{Command:Program=x,Interval=30} {Clock}");
    REQUIRE(items.size() == 3);
    auto const command = items.first().toMap();
    CHECK(command.value("command").toString() == "x");
    CHECK(command.value("interval").toInt() == 30);

    // An unrelated edit, the way the editor does it: the rows are copied, one moves, the segment is
    // serialized again.
    items.move(0, 2);
    auto const edited = fx.controller->serializeIndicatorSegment(items);
    CHECK(edited.contains("Interval=30"));
    auto const reread = fx.controller->parseIndicatorSegment(edited);
    REQUIRE(reread.size() == 3);
    CHECK(reread.last().toMap().value("command").toString() == "x");
    CHECK(reread.last().toMap().value("interval").toInt() == 30);

    auto const withComma = fx.controller->parseIndicatorSegment("{Command:git log -1 --format=%h,%s}");
    REQUIRE(withComma.size() == 1);
    CHECK(withComma.first().toMap().value("command").toString() == "git log -1 --format=%h,%s");
    auto const once = fx.controller->serializeIndicatorSegment(withComma);
    auto const again = fx.controller->parseIndicatorSegment(once);
    REQUIRE(again.size() == 1);
    CHECK(again.first().toMap().value("command").toString() == "git log -1 --format=%h,%s");
    CHECK(fx.controller->serializeIndicatorSegment(again) == once);
}

TEST_CASE("SettingsController: an extension item keeps its name and styles through an edit", "[settings]")
{
    // The name is the item's payload: a reader that dropped it would make the writer delete the segment.
    auto fx = Fixture(BasicConfig);
    for (auto const& text: { QString("{Extension:git}"), QString("{Extension:Name=git,Bold,Left=[}") })
    {
        INFO(text.toStdString());
        auto const items = fx.controller->parseIndicatorSegment(text);
        REQUIRE(items.size() == 1);
        CHECK(items.first().toMap().value("name").toString() == "git");
        CHECK(fx.controller->serializeIndicatorSegment(items) == text);
    }
}

TEST_CASE("SettingsController: the indicator bridge keeps role colours", "[settings]")
{
    // A role is a word, not a hex colour: a bridge that read it through a hex parser would save it as black.
    auto fx = Fixture(BasicConfig);
    auto const original =
        QString("{VTType:Color=#C0C030}{Clock:Color=muted}{Title:Color=#000000,BackgroundColor=accent}");

    auto const items = fx.controller->parseIndicatorSegment(original);
    REQUIRE(items.size() == 3);
    auto const vtType = items[0].toMap();
    auto const clock = items[1].toMap();
    auto const title = items[2].toMap();
    CHECK(vtType.value("hasColor").toBool());
    CHECK(vtType.value("color").toString() == "#C0C030");
    CHECK(clock.value("hasColor").toBool());
    CHECK(clock.value("color").toString() == "muted");
    CHECK(title.value("hasBgColor").toBool());
    CHECK(title.value("bgColor").toString() == "accent");

    CHECK(fx.controller->serializeIndicatorSegment(items) == original);

    // A colour the page cannot read is left unset, not written as black.
    auto broken = clock;
    broken["color"] = QString("#zz");
    CHECK(fx.controller->serializeIndicatorSegment({ broken }) == "{Clock}");
}

TEST_CASE("SettingsController: the indicator bridge keeps every payload", "[settings][statusline]")
{
    // Every item is round-tripped through the bridge on every edit of its segment, so a payload the reader
    // does not hand to QML is one the writer deletes from the user's profile.
    auto fx = Fixture(BasicConfig);
    for (auto const& text: { QString("{InputMode:Chip,Hide=Insert}"),
                             QString("{InputMode:Bold,Left= ,Right= ,Chip,Hide=Insert}"),
                             QString("{Cwd:MaxWidth=20}"),
                             QString("{LastCommand:MaxWidth=50}"),
                             QString("{Context:Verbosity=Full,MaxWidth=10}") })
    {
        INFO(text.toStdString());
        CHECK(fx.controller->serializeIndicatorSegment(fx.controller->parseIndicatorSegment(text)) == text);
    }

    // The keys QML reads: hide names the mode, chip is the look, maxWidth the column budget.
    auto const inputMode =
        fx.controller->parseIndicatorSegment("{InputMode:Chip,Hide=Insert}").first().toMap();
    CHECK(inputMode.value("hide").toString() == "Insert");
    CHECK(inputMode.value("chip").toBool());
    auto const plain = fx.controller->parseIndicatorSegment("{InputMode}").first().toMap();
    CHECK(plain.value("hide").toString().isEmpty());
    CHECK_FALSE(plain.value("chip").toBool());
    CHECK(fx.controller->parseIndicatorSegment("{Cwd:MaxWidth=20}").first().toMap().value("maxWidth").toInt()
          == 20);

    // The roles the colour fields accept, in the table's order.
    auto const roles = fx.controller->indicatorColorRoles();
    REQUIRE(static_cast<std::size_t>(roles.size())
            == vtbackend::StatusLineDefinitions::ColorRoleNames.size());
    CHECK(roles.first() == "accent");
}

TEST_CASE("SettingsController: the indicator bridge resolves role colours for the preview",
          "[settings][statusline]")
{
    // A role is a word, and a QML colour property cannot take "accent": the preview reads the resolved
    // hex the bridge hands it beside the role, from the profile's own scheme.
    auto fx = Fixture(R"(
default_profile: main
profiles:
    main:
        colors: contour-light
)");
    fx.controller->editProfile("main");

    auto scheme = vtbackend::ColorPalette {};
    REQUIRE(vtbackend::defaultColorPalettes("contour-light", scheme));

    auto const rows = fx.controller->parseIndicatorSegment(
        "{Clock:Color=muted,BackgroundColor=accent}{VTType:Color=#C0C030}");
    REQUIRE(rows.size() == 2);
    auto const clock = rows[0].toMap();
    CHECK(clock.value("colorPreview").toString() == hexOf(scheme.indexedColor(8)));
    CHECK(clock.value("bgColorPreview").toString() == hexOf(scheme.indexedColor(4)));
    CHECK(rows[1].toMap().value("colorPreview").toString() == "#c0c030");
    // Unset stays unset: no preview colour to paint.
    CHECK(rows[1].toMap().value("bgColorPreview").toString().isEmpty());
}

TEST_CASE("SettingsController: a light/dark profile previews its roles in the appearance you are in",
          "[settings][statusline]")
{
    // The terminal draws a light/dark profile with the scheme of the current appearance, so the preview has
    // to as well: the dark scheme's colours on a light page are what the terminal never shows.
    auto constexpr Yaml = std::string_view { R"(
default_profile: main
profiles:
    main:
        colors:
            light: contour-light
            dark: contour-dark
)" };
    auto const segment = QString("{Cwd:Color=success}{SearchPrompt:Color=warning}");
    auto const previewsOf = [&](SettingsController const& controller) {
        auto const rows = controller.parseIndicatorSegment(segment);
        REQUIRE(rows.size() == 2);
        return std::pair { rows[0].toMap().value("colorPreview").toString(),
                           rows[1].toMap().value("colorPreview").toString() };
    };
    auto const schemeColors = [](std::string_view name) {
        auto scheme = vtbackend::ColorPalette {};
        REQUIRE(vtbackend::defaultColorPalettes(name, scheme));
        return std::pair { hexOf(scheme.indexedColor(2)), hexOf(scheme.indexedColor(3)) };
    };

    SECTION("light")
    {
        auto fx = Fixture(Yaml, nullptr, vtbackend::ColorPreference::Light);
        fx.controller->editProfile("main");
        CHECK(previewsOf(*fx.controller) == schemeColors("contour-light"));
        CHECK(previewsOf(*fx.controller) == std::pair { QString("#2a7a1f"), QString("#8a6100") });
    }

    SECTION("dark")
    {
        auto fx = Fixture(Yaml, nullptr, vtbackend::ColorPreference::Dark);
        fx.controller->editProfile("main");
        CHECK(previewsOf(*fx.controller) == schemeColors("contour-dark"));
    }

    SECTION("a change of appearance repaints the preview")
    {
        auto fx = Fixture(Yaml, nullptr, vtbackend::ColorPreference::Dark);
        fx.controller->editProfile("main");
        auto changes = 0;
        QObject::connect(fx.controller.get(), &SettingsController::draftChanged, [&changes] { ++changes; });
        fx.controller->setColorPreference(vtbackend::ColorPreference::Light);
        CHECK(changes == 1);
        CHECK(previewsOf(*fx.controller) == schemeColors("contour-light"));
        // The same appearance again is no change.
        fx.controller->setColorPreference(vtbackend::ColorPreference::Light);
        CHECK(changes == 1);
    }
}

TEST_CASE("SettingsController: a draft scheme switch previews the newly named scheme",
          "[settings][statusline]")
{
    // The draft names a scheme before it is saved; the preview follows the name, not the palette the
    // profile was loaded with.
    auto fx = Fixture(R"(
default_profile: main
profiles:
    main:
        colors: contour-dark
)",
                      nullptr,
                      vtbackend::ColorPreference::Light);
    fx.openSideProfile("work");
    auto const successPreview = [&] {
        return fx.controller->parseIndicatorSegment("{Cwd:Color=success}")
            .first()
            .toMap()
            .value("colorPreview")
            .toString();
    };
    auto const successOf = [](std::string_view name) {
        auto scheme = vtbackend::ColorPalette {};
        REQUIRE(vtbackend::defaultColorPalettes(name, scheme));
        return hexOf(scheme.indexedColor(2));
    };
    REQUIRE(successOf("contour-light") != successOf("contour-dark"));
    CHECK(successPreview() == successOf("contour-dark"));

    fx.controller->setColorScheme("contour-light");
    CHECK(successPreview() == successOf("contour-light"));

    // A light/dark pair made from it previews the light one, the appearance being light.
    fx.controller->setColorSchemeMode("dual");
    fx.controller->setColorSchemeDark("contour-dark");
    CHECK(successPreview() == successOf("contour-light"));
    fx.controller->setColorSchemeLight("contour-dark");
    CHECK(successPreview() == successOf("contour-dark"));
}

TEST_CASE("SettingsController: a scheme written under the preview's name repaints the preview",
          "[settings][statusline]")
{
    // The preview reads a scheme once per name; a save of that scheme must not leave it on the old colours.
    auto fx = Fixture(BasicConfig);
    fx.openSideScheme("midnight");
    REQUIRE(fx.controller->setSchemeColor("normal_green", "#102030").has_value());
    fx.fire();

    fx.openSideProfile("work");
    fx.controller->setColorScheme("midnight");
    auto const successPreview = [&] {
        return fx.controller->parseIndicatorSegment("{Cwd:Color=success}")
            .first()
            .toMap()
            .value("colorPreview")
            .toString();
    };
    CHECK(successPreview() == "#102030");

    fx.controller->editColorScheme("midnight");
    REQUIRE(fx.controller->setSchemeColor("normal_green", "#405060").has_value());
    fx.fire();
    CHECK(successPreview() == "#405060");
}

TEST_CASE("SettingsController: the native2026 default goes through the bridge unchanged",
          "[settings][statusline]")
{
    // A first launch's status line, opened and saved in the editor, must come back byte for byte.
    auto fx = Fixture(BasicConfig);
    auto const& segments = config::editionDefaults(config::DefaultsEdition::Native2026).statusLine;
    for (auto const segment: { segments.left, segments.middle, segments.right })
    {
        auto const text = QString::fromUtf8(segment.data(), static_cast<qsizetype>(segment.size()));
        INFO(text.toStdString());
        CHECK(fx.controller->serializeIndicatorSegment(fx.controller->parseIndicatorSegment(text)) == text);
    }
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

        // Text, Command and Extension need their payload attribute to be recognized at all; the rest stand
        // alone.
        auto const templateText = [&type] {
            if (type == "Text")
                return QString("{Text:text=x}");
            if (type == "Command")
                return QString("{Command:Program=true}");
            if (type == "Extension")
                return QString("{Extension:git}");
            return QString("{%1}").arg(type);
        }();
        auto const items = fx.controller->parseIndicatorSegment(templateText);
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
    fx.openSideProfile("work");

    CHECK(fx.controller->indicatorSegment(-1).isEmpty());
    CHECK(fx.controller->indicatorSegment(3).isEmpty());

    // An out-of-range write is dropped rather than landing on a neighbouring segment.
    auto const middle = fx.controller->indicatorSegment(1);
    fx.controller->setIndicatorSegment(7, "{Clock}");
    CHECK(fx.controller->indicatorSegment(1) == middle);
}

// }}}

TEST_CASE("SettingsController: the GPU row shows titles and stores ids", "[settings][gpu]")
{
    auto fx = Fixture(BasicConfig, hybridGpus());
    auto const row = rowWithKey(fx.controller->globalFields(), "renderer.gpu");
    CHECK(row.value("value").toString() == "auto");
    CHECK(row.value("options").toStringList()
          == QStringList { "auto", "integrated", "discrete", "8086:a788", "10de:2820" });
    CHECK(row.value("optionLabels").toStringList()
          == QStringList { "Automatic (system default)",
                           "Integrated GPU",
                           "Discrete GPU",
                           "Intel integrated GPU",
                           "NVIDIA GeForce RTX 4070 Max-Q / Mobile" });

    fx.commitGlobal("renderer.gpu", "10de:2820");
    CHECK(fx.cfg.renderer.value().gpu.id == config::PciId { 0x10de, 0x2820 });
    CHECK(rowWithKey(fx.controller->globalFields(), "renderer.gpu").value("overridden").toBool());
}

TEST_CASE("SettingsController: a configured GPU that is gone stays visible and selected", "[settings][gpu]")
{
    auto fx = Fixture(R"(
default_profile: main
renderer:
    gpu: 1002:7340
profiles:
    main:
        show_title_bar: true
)",
                      hybridGpus());
    auto const row = rowWithKey(fx.controller->globalFields(), "renderer.gpu");
    CHECK(row.value("value").toString() == "1002:7340");
    CHECK(row.value("options").toStringList().last() == "1002:7340");
    CHECK(row.value("optionLabels").toStringList().last() == "Unavailable GPU (1002:7340)");
}

TEST_CASE("SettingsController: a hand-listed enum row labels each option with its value", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->editProfile("main");
    auto const row = rowWithKey(fx.controller->profileFields(), "pixel_reporting");
    CHECK(row.value("optionLabels").toStringList() == row.value("options").toStringList());
}

TEST_CASE("SettingsController: Show links edits hyperlink_decoration.mode", "[settings][links]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    REQUIRE(fx.controller->setProfileField("hyperlink_decoration_mode", "never").has_value());
    auto const fields = fx.controller->profileFields();
    auto const row = rowWithKey(fields, "hyperlink_decoration_mode");
    CHECK(row.value("value").toString() == "never");
    CHECK(row.value("groupTitle").toString() == "Links");
    CHECK(row.value("options").toStringList() == QStringList { "always", "hover", "modifier", "never" });
}

TEST_CASE(
    "SettingsController: Show links reads 'modifier' under the native2026 defaults, unless the file pins it",
    "[settings][links]")
{
    auto const shownBy = [](std::string_view yaml) {
        auto fx = Fixture(yaml);
        fx.controller->editProfile("main");
        return rowWithKey(fx.controller->profileFields(), "hyperlink_decoration_mode")
            .value("value")
            .toString();
    };
    CHECK(shownBy("defaults_edition: native2026\ndefault_profile: main\nprofiles:\n    main:\n"
                  "        show_title_bar: true\n")
          == "modifier");
    // What a contour.yml generated before the edition existed holds: the value, written out.
    CHECK(shownBy("defaults_edition: native2026\ndefault_profile: main\nprofiles:\n    main:\n"
                  "        hyperlink_decoration:\n            mode: always\n")
          == "always");
    // The classic edition keeps its own default.
    CHECK(shownBy(std::string_view { BasicConfig }) == "always");
}

TEST_CASE("SettingsController: link detection switches edit link_detection", "[settings][links]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    auto const field = [&](QString const& key) {
        return rowWithKey(fx.controller->profileFields(), key);
    };

    struct Expected
    {
        QString key;
        bool value;
        QString labelPart;
    };
    auto const expected = std::array {
        Expected { "link_detection_primary_urls", true, "URLs" },
        Expected { "link_detection_primary_file_paths", true, "file paths" },
        Expected { "link_detection_other_urls", true, "URLs" },
        Expected { "link_detection_other_file_paths", false, "file paths" },
    };
    for (auto const& row: expected)
    {
        INFO(row.key.toStdString());
        REQUIRE_FALSE(field(row.key).isEmpty());
        CHECK(field(row.key).value("groupTitle").toString() == "Links");
        CHECK(field(row.key).value("type").toString() == "bool");
        CHECK(field(row.key).value("value").toBool() == row.value);
        // The settings search (QML) matches on label, help text and key.
        CHECK(field(row.key).value("label").toString().contains(row.labelPart));
        CHECK(field(row.key).value("help").toString().endsWith("OSC 8 hyperlinks are always links."));
    }

    REQUIRE(fx.controller->setProfileField("link_detection_other_file_paths", true).has_value());
    CHECK(field("link_detection_other_file_paths").value("value").toBool());
    CHECK(field("link_detection_primary_file_paths").value("value").toBool());
}

TEST_CASE("SettingsController: Open files with validates its placeholders inline", "[settings][links]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    auto const field = [&](QString const& key) {
        return rowWithKey(fx.controller->profileFields(), key);
    };

    REQUIRE_FALSE(field("file_opener_command").isEmpty());
    CHECK(field("file_opener_command").value("groupTitle").toString() == "Links");
    CHECK(field("file_opener_command").value("problem").toString().isEmpty()); // empty: the system default

    REQUIRE(fx.controller->setProfileField("file_opener_command", "code {file}").has_value());
    CHECK(field("file_opener_command").value("value").toString() == "code {file}");
    CHECK(field("file_opener_command").value("problem").toString().contains("placeholder"));

    REQUIRE(fx.controller->setProfileField("file_opener_command", "code {path").has_value());
    CHECK(field("file_opener_command").value("problem").toString().contains("'{'"));

    REQUIRE(fx.controller->setProfileField("file_opener_command", "code --goto {path}:{line}:{column}")
                .has_value());
    CHECK(field("file_opener_command").value("problem").toString().isEmpty());

    // Every row says whether it has a problem, so the page binds one property for all of them.
    for (auto const& row: fx.controller->profileFields())
        CHECK(row.toMap().contains("problem"));
}

TEST_CASE("SettingsController: an invalid file_opener is saved as written, and its row says what is wrong",
          "[settings][links]")
{
    // The reader keeps such a command and the click opens with the system's default application instead: a
    // refusal to save would guard nothing, and would block every other edit of the profile until the
    // command is mended.
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    REQUIRE(fx.controller->setProfileField("file_opener_command", "code {file}").has_value());
    fx.fire();
    REQUIRE(fx.cfg.findProfile("work") != nullptr);
    CHECK(fx.cfg.findProfile("work")->fileOpener.value().command == "code {file}");

    // An unrelated edit, saved under the profile's own name, writes the command back unchanged.
    fx.controller->editProfile("work");
    CHECK(rowWithKey(fx.controller->profileFields(), "file_opener_command")
              .value("problem")
              .toString()
              .contains("placeholder"));
    REQUIRE(fx.controller->setProfileField("file_opener_placement", "tab").has_value());
    fx.fire();
    CHECK(fx.cfg.findProfile("work")->fileOpener.value().command == "code {file}");
    CHECK(fx.cfg.findProfile("work")->fileOpener.value().placement == contour::actions::OutputPlacement::Tab);
}

TEST_CASE("SettingsController: Open in edits file_opener.placement", "[settings][links]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    auto const row = [&] {
        return rowWithKey(fx.controller->profileFields(), "file_opener_placement");
    };
    CHECK(row().value("value").toString() == "detached");
    CHECK(row().value("options").toStringList() == QStringList { "split", "tab", "detached" });
    REQUIRE(fx.controller->setProfileField("file_opener_placement", "tab").has_value());
    CHECK(row().value("value").toString() == "tab");
}

// {{{ Link patterns

namespace
{

/// Link pattern row @p index of @p controller.
[[nodiscard]] QVariantMap linkRow(SettingsController const& controller, int index)
{
    return controller.linkPatterns().at(index).toMap();
}

/// How many rows the built-in patterns take, ahead of the profile's own.
[[nodiscard]] int builtinRowCount()
{
    return static_cast<int>(vtbackend::builtinLinkPatternNames().size());
}

/// The row of the built-in pattern @p name.
[[nodiscard]] int builtinRow(std::string_view name)
{
    auto const names = vtbackend::builtinLinkPatternNames();
    return static_cast<int>(std::ranges::find(names, name) - names.begin());
}

constexpr auto JiraRegex = std::string_view { R"(\b[A-Z]+-\d+\b)" };

} // namespace

TEST_CASE("SettingsController: link patterns list the built-ins first, then the profile's",
          "[settings][links]")
{
    auto fx =
        Fixture("default_profile: main\n"
                "profiles:\n"
                "  main:\n"
                "    hint_patterns:\n"
                "      - { name: jira, regex: '[A-Z]+-[0-9]+', clickable: true, url: 'https://j/{0}' }\n"
                "      - { name: githash, clickable: true }\n");
    fx.controller->editProfile("main");
    auto const& c = *fx.controller;
    auto const builtins = builtinRowCount();
    // The githash override shows on its built-in's row, not as a row of its own.
    REQUIRE(c.linkPatterns().size() == builtins + 1);
    auto const names = vtbackend::builtinLinkPatternNames();
    for (auto const index: std::views::iota(0, builtins))
    {
        auto const row = linkRow(c, index);
        auto const name = names[static_cast<std::size_t>(index)];
        CAPTURE(name);
        CHECK(row.value("name").toString().toStdString() == name);
        CHECK(row.value("builtin").toBool());
        CHECK(row.value("regex").toString().toStdString()
              == vtbackend::LinkPatterns::builtins()->find(name)->source);
        // A clickable githash makes no link: a bare hash names no scheme (§7.6). Said on its row.
        if (name == "githash")
            CHECK(row.value("problem").toString().contains("scheme"));
        else
            CHECK(row.value("problem").toString().isEmpty());
    }
    CHECK(linkRow(c, builtinRow("url")).value("kind").toString() == "url");
    CHECK(linkRow(c, builtinRow("filepath")).value("kind").toString() == "path");
    CHECK(linkRow(c, builtinRow("filepath")).value("clickable").toBool()); // the built-in's default
    CHECK(linkRow(c, builtinRow("githash")).value("clickable").toBool());  // the profile's override
    CHECK_FALSE(linkRow(c, builtinRow("ipv4")).value("clickable").toBool());

    auto const jira = linkRow(c, builtins);
    CHECK(jira.value("name").toString() == "jira");
    CHECK(jira.value("regex").toString() == "[A-Z]+-[0-9]+");
    CHECK(jira.value("clickable").toBool());
    CHECK(jira.value("url").toString() == "https://j/{0}");
    CHECK_FALSE(jira.value("builtin").toBool());
    CHECK(jira.value("kind").toString() == "custom");
    CHECK(jira.value("problem").toString().isEmpty());
}

TEST_CASE("SettingsController: toggling a built-in writes a name-only override, and back removes it",
          "[settings][links]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    auto const filepath = builtinRow("filepath");
    auto edited = linkRow(*fx.controller, filepath);
    edited["clickable"] = false;
    fx.controller->setLinkPattern(filepath, edited);
    CHECK_FALSE(linkRow(*fx.controller, filepath).value("clickable").toBool());
    CHECK(linkRow(*fx.controller, filepath).value("problem").toString().isEmpty()); // a name-only override
    CHECK(fx.controller->linkPatterns().size() == builtinRowCount());               // and no row of its own

    fx.fire();
    REQUIRE(fx.cfg.findProfile("work") != nullptr);
    CHECK(fx.cfg.findProfile("work")->hintPatterns.value()
          == std::vector {
              config::HintPatternConfig { .name = "filepath", .regex = {}, .clickable = false, .url = {} } });

    // Back to the built-in's own default: nothing is left in the override, so it goes.
    edited["clickable"] = true;
    fx.controller->setLinkPattern(filepath, edited);
    CHECK(linkRow(*fx.controller, filepath).value("clickable").toBool());
    fx.fire();
    CHECK(fx.cfg.findProfile("work")->hintPatterns.value().empty());
}

TEST_CASE("SettingsController: user link patterns are added, edited, checked and removed",
          "[settings][links]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    auto const index = builtinRowCount();
    fx.controller->addLinkPattern();
    REQUIRE(fx.controller->linkPatterns().size() == index + 1);
    auto edited = linkRow(*fx.controller, index);
    CHECK_FALSE(edited.value("builtin").toBool());
    CHECK(edited.value("clickable").toBool());
    CHECK(edited.value("regex").toString().isEmpty());
    CHECK(edited.value("problem").toString().contains("needs a regex")); // until it has one

    edited["name"] = "jira";
    edited["regex"] = "([a-";
    fx.controller->setLinkPattern(index, edited);
    CHECK(linkRow(*fx.controller, index).value("name").toString() == "jira");
    CHECK(linkRow(*fx.controller, index).value("problem").toString().contains("invalid regex"));

    edited["regex"] = QString::fromUtf8(JiraRegex.data(), static_cast<qsizetype>(JiraRegex.size()));
    edited["url"] = "https://j/{1}";
    fx.controller->setLinkPattern(index, edited);
    CHECK(linkRow(*fx.controller, index).value("problem").toString().contains("capture group"));

    edited["url"] = "https://j/{0}";
    fx.controller->setLinkPattern(index, edited);
    CHECK(linkRow(*fx.controller, index).value("problem").toString().isEmpty());
    CHECK(linkRow(*fx.controller, index).value("url").toString() == "https://j/{0}");

    // Each pattern added gets a name of its own.
    fx.controller->addLinkPattern();
    fx.controller->addLinkPattern();
    REQUIRE(fx.controller->linkPatterns().size() == index + 3);
    CHECK(linkRow(*fx.controller, index + 1).value("name")
          != linkRow(*fx.controller, index + 2).value("name"));

    fx.controller->removeLinkPattern(index + 1);
    fx.controller->removeLinkPattern(index + 1);
    fx.controller->removeLinkPattern(index);
    CHECK(fx.controller->linkPatterns().size() == index);
}

TEST_CASE("SettingsController: a user link pattern cannot take another pattern's name", "[settings][links]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    auto const index = builtinRowCount();
    fx.controller->addLinkPattern();
    fx.controller->addLinkPattern();
    auto const before = linkRow(*fx.controller, index).value("name");
    for (auto const& taken:
         { QStringLiteral("url"), linkRow(*fx.controller, index + 1).value("name").toString() })
    {
        CAPTURE(taken.toStdString());
        auto edited = linkRow(*fx.controller, index);
        edited["name"] = taken;
        fx.errors.clear();
        fx.controller->setLinkPattern(index, edited);
        CHECK(linkRow(*fx.controller, index).value("name") == before);
        REQUIRE(fx.errors.size() == qsizetype { 1 });
        CHECK(fx.errors.back().contains(taken));
    }
    CHECK(fx.controller->linkPatterns().size() == index + 2);
}

TEST_CASE(
    "SettingsController: a link pattern in error is held back, and the profile's other edits are written",
    "[settings][links]")
{
    // m5: the reader would skip a pattern without a regex, so it is not written until it has one; its row
    // says why. Every other edit of the profile is written meanwhile.
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    auto const index = builtinRowCount();
    fx.controller->addLinkPattern();
    auto edited = linkRow(*fx.controller, index);
    edited["name"] = "jira";
    fx.controller->setLinkPattern(index, edited);
    CHECK(fx.errors.isEmpty()); // its row says what is wrong while the regex is still to be typed
    CHECK_FALSE(linkRow(*fx.controller, index).value("problem").toString().isEmpty());
    REQUIRE(fx.controller->setProfileField("show_title_bar", false).has_value());
    CHECK(fx.controller->hasChanges());
    fx.fire();
    CHECK(fx.cfg.findProfile("work")->showTitleBar.value() == false);
    CHECK(fx.cfg.findProfile("work")->hintPatterns.value().empty()); // the pattern in error was not written
    CHECK(linkRow(*fx.controller, index).value("name").toString() == "jira"); // and is still being edited

    edited["regex"] = QString::fromUtf8(JiraRegex.data(), static_cast<qsizetype>(JiraRegex.size()));
    edited["url"] = "https://j/{0}";
    fx.controller->setLinkPattern(index, edited);
    fx.fire();
    REQUIRE(fx.cfg.findProfile("work") != nullptr);
    CHECK(fx.cfg.findProfile("work")->hintPatterns.value()
          == std::vector { config::HintPatternConfig { .name = "jira",
                                                       .regex = std::string { JiraRegex },
                                                       .clickable = true,
                                                       .url = "https://j/{0}" } });
}

TEST_CASE("SettingsController: a built-in's broken override shows its problem and does not block a save",
          "[settings][links]")
{
    // The page cannot change a built-in's regex, and the loader keeps the entry and reports it at every load:
    // refusing the save would leave a profile based on this one unsavable for an unrelated change.
    auto fx = Fixture("default_profile: main\n"
                      "profiles:\n"
                      "  main:\n"
                      "    hint_patterns:\n"
                      "      - { name: filepath, regex: '\\S+\\.cpp' }\n");
    fx.openSideProfile("work");
    CHECK(linkRow(*fx.controller, builtinRow("filepath")).value("problem").toString().contains("group 1"));

    REQUIRE(fx.controller->setProfileField("show_title_bar", false).has_value());
    fx.fire();
    CHECK(fx.errors.isEmpty());
    REQUIRE(fx.cfg.findProfile("work") != nullptr);
    CHECK(fx.cfg.findProfile("work")->hintPatterns.value()
          == std::vector { config::HintPatternConfig {
              .name = "filepath", .regex = R"(\S+\.cpp)", .clickable = {}, .url = {} } });
    CHECK(linkRow(*fx.controller, builtinRow("filepath")).value("problem").toString().contains("group 1"));
}

TEST_CASE("SettingsController: a built-in row's Clickable is the one the terminal uses", "[settings][links]")
{
    // Two entries for one built-in: the later one wins in the terminal, so on the page too.
    auto fx = Fixture("default_profile: main\n"
                      "profiles:\n"
                      "  main:\n"
                      "    hint_patterns:\n"
                      "      - { name: githash, clickable: true }\n"
                      "      - { name: githash, clickable: false }\n");
    fx.controller->editProfile("main");
    CHECK_FALSE(linkRow(*fx.controller, builtinRow("githash")).value("clickable").toBool());
}

TEST_CASE("SettingsController: a built-in's Clickable switch holds over hand-written duplicate entries",
          "[settings][links]")
{
    // The terminal merges the entries by name, an earlier entry's clickable standing where a later one gives
    // none: switching it off must clear it wherever it is set, or the row snaps back.
    auto fx = Fixture("default_profile: main\n"
                      "profiles:\n"
                      "  main:\n"
                      "    hint_patterns:\n"
                      "      - { name: githash, clickable: true }\n"
                      "      - { name: githash, regex: '\\b[0-9a-f]{8}\\b' }\n");
    fx.openSideProfile("work");
    auto const githash = builtinRow("githash");
    REQUIRE(linkRow(*fx.controller, githash).value("clickable").toBool());

    auto edited = linkRow(*fx.controller, githash);
    edited["clickable"] = false;
    fx.controller->setLinkPattern(githash, edited);
    CHECK_FALSE(linkRow(*fx.controller, githash).value("clickable").toBool());
    CHECK(linkRow(*fx.controller, githash).value("regex").toString() == R"(\b[0-9a-f]{8}\b)");

    edited["clickable"] = true;
    fx.controller->setLinkPattern(githash, edited);
    CHECK(linkRow(*fx.controller, githash).value("clickable").toBool());
}

TEST_CASE("SettingsController: the link pattern preview matches as the terminal does", "[settings][links]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    auto const preview = fx.controller->previewLinkPattern(
        QVariantMap {
            { "name", "jira" },
            { "regex", QString::fromUtf8(JiraRegex.data(), static_cast<qsizetype>(JiraRegex.size())) },
            { "clickable", true },
            { "url", "https://j/{0}" },
            { "builtin", false } },
        QStringLiteral("ä ABC-1, XY-22."));
    CHECK(preview.value("problem").toString().isEmpty());
    CHECK(preview.value("note").toString().isEmpty());
    auto const spans = preview.value("spans").toList();
    REQUIRE(spans.size() == 2);
    CHECK(spans[0].toMap().value("start").toInt() == 2); // UTF-16 offsets: after "ä "
    CHECK(spans[0].toMap().value("end").toInt() == 7);
    CHECK(spans[0].toMap().value("target").toString() == "https://j/ABC-1");
    CHECK(spans[1].toMap().value("start").toInt() == 9);
    CHECK(spans[1].toMap().value("end").toInt() == 14);
    CHECK(spans[1].toMap().value("target").toString() == "https://j/XY-22");
}

TEST_CASE("SettingsController: the link pattern preview marks a match the printed text cannot make a link",
          "[settings][links]")
{
    // §7.6, §9: a clickable pattern without a scheme of its own is a link only where the printed text names
    // one Contour opens. The terminal underlines nothing elsewhere, and the preview says so.
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    auto const preview =
        fx.controller->previewLinkPattern(QVariantMap { { "name", "doc" },
                                                        { "regex", R"(\S+\.pdf)" },
                                                        { "clickable", true },
                                                        { "url", "" },
                                                        { "builtin", false } },
                                          QStringLiteral("https://ok.example/x.pdf smb://evil/y.pdf"));
    CHECK(preview.value("problem").toString().contains("scheme")); // the load's warning
    CHECK(preview.value("note").toString().contains("no link"));
    auto const spans = preview.value("spans").toList();
    REQUIRE(spans.size() == 2);
    CHECK_FALSE(spans[0].toMap().value("refused").toBool());
    CHECK(spans[1].toMap().value("refused").toBool());
}

TEST_CASE("SettingsController: the link pattern preview says what is wrong, and when a path is shown",
          "[settings][links]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");

    auto const broken = fx.controller->previewLinkPattern(
        QVariantMap { { "name", "bad" }, { "regex", "([a-" }, { "clickable", true }, { "builtin", false } },
        QStringLiteral("abc"));
    CHECK(broken.value("problem").toString().contains("invalid regex"));
    CHECK(broken.value("spans").toList().isEmpty());

    auto const path = fx.controller->previewLinkPattern(linkRow(*fx.controller, builtinRow("filepath")),
                                                        QStringLiteral("see src/main.cpp:12 now"));
    CHECK(path.value("problem").toString().isEmpty());
    CHECK(path.value("note").toString().contains("once they are found to exist"));
    // Without the disk, every word that could name a file is shown, the bare ones too; the path is the
    // target, its location left out.
    auto const spans = path.value("spans").toList();
    REQUIRE(spans.size() == 3);
    CHECK(spans[1].toMap().value("start").toInt() == 4);
    CHECK(spans[1].toMap().value("target").toString() == "src/main.cpp");
}

TEST_CASE("SettingsController: link patterns of a read-only profile cannot change", "[settings][links]")
{
    auto fx = Fixture("default_profile: main\n"
                      "profiles:\n"
                      "  main:\n"
                      "    hint_patterns:\n"
                      "      - { name: jira, regex: '[A-Z]+-[0-9]+' }\n");
    fx.controller->editProfile("main"); // from contour.yml: read-only
    REQUIRE(fx.controller->editingReadOnly());
    auto const before = fx.controller->linkPatterns();
    auto const index = builtinRowCount();
    REQUIRE(before.size() == index + 1);

    fx.controller->addLinkPattern();
    auto edited = linkRow(*fx.controller, index);
    edited["regex"] = "x";
    fx.controller->setLinkPattern(index, edited);
    auto toggled = linkRow(*fx.controller, builtinRow("filepath"));
    toggled["clickable"] = false;
    fx.controller->setLinkPattern(builtinRow("filepath"), toggled);
    fx.controller->removeLinkPattern(index);
    CHECK(fx.controller->linkPatterns() == before);
}

// }}}

// {{{ Sections and groups

TEST_CASE("SettingsController: every field group appears exactly once", "[settings][sections]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->editProfile("main");

    auto named = std::set<int> {};
    auto const collect = [&named](QVariantList const& rows, SettingsScope scope) {
        for (auto const& raw: rows)
        {
            auto const row = raw.toMap();
            INFO(row.value("key").toString().toStdString());
            REQUIRE(row.contains("group"));
            auto const index = row.value("group").toInt();
            REQUIRE(index >= 0);
            auto const groupIndex = static_cast<std::size_t>(index);
            REQUIRE(groupIndex < SettingsGroupCount);
            auto const& group = settingsGroupRow(static_cast<SettingsGroup>(index));
            CHECK(group.scope == scope);
            CHECK(row.value("groupTitle").toString()
                  == QString::fromUtf8(group.title.data(), static_cast<qsizetype>(group.title.size())));
            named.insert(index);
        }
    };
    collect(fx.controller->globalFields(), SettingsScope::Global);
    collect(fx.controller->profileFields(), SettingsScope::Profile);
    // A host's rows, those of both transports.
    fx.openSideHost("starfive", "url: ssh://starfive\n");
    collect(fx.controller->hostFields(), SettingsScope::Host);
    REQUIRE(fx.controller->setHostField("url", "tls://starfive:7297").has_value());
    collect(fx.controller->hostFields(), SettingsScope::Host);
    // The two groups that are pages rather than field rows.
    named.insert(static_cast<int>(SettingsGroup::KeyboardBindings));
    named.insert(static_cast<int>(SettingsGroup::ProfileColors));

    auto all = std::set<int> {};
    for (auto const& row: SettingsGroupRows)
        all.insert(static_cast<int>(row.group));
    CHECK(named == all);

    CHECK(rowWithKey(fx.controller->profileFields(), "option_as_alt").value("group").toInt()
          == static_cast<int>(SettingsGroup::ProfileKeyboard));
    CHECK(rowWithKey(fx.controller->globalFields(), "chrome_density").value("group").toInt()
          == static_cast<int>(SettingsGroup::AppearanceChrome));
}

TEST_CASE("SettingsController: the sections and the sidebar come from the table", "[settings][sections]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");

    auto const sections = fx.controller->sections();
    auto const sectionCount = static_cast<std::size_t>(sections.size());
    REQUIRE(sectionCount == SettingsSectionCount);
    for (auto const index: std::views::iota(std::size_t { 0 }, SettingsSectionCount))
    {
        auto const section = sections.at(static_cast<qsizetype>(index)).toMap();
        auto const& row = SettingsSectionRows.at(index);
        CHECK(section.value("id").toString().toStdString() == row.id);
        CHECK(section.value("title").toString().toStdString() == row.title);
        CHECK(section.value("icon").toInt() == static_cast<int>(row.icon));
        auto const groups = section.value("groups").toList();
        auto const groupCount = static_cast<std::size_t>(groups.size());
        auto const tableCount = static_cast<std::size_t>(std::ranges::distance(groupsOf(row.section)));
        CHECK(groupCount == tableCount);
        auto const links = section.value("links").toList();
        CHECK(static_cast<std::size_t>(links.size())
              == static_cast<std::size_t>(std::ranges::distance(linksOf(row.section))));
        for (auto const& raw: links)
        {
            auto const link = raw.toMap();
            auto const found = std::ranges::find(
                SettingsPageLinks, link.value("target").toString().toStdString(), &SettingsPageLink::id);
            REQUIRE(found != SettingsPageLinks.end());
            CHECK(link.value("title").toString().toStdString() == found->title);
        }
    }

    auto const sidebar = fx.controller->sidebar();
    auto const sidebarCount = static_cast<std::size_t>(sidebar.size());
    REQUIRE(sidebarCount == SettingsSectionCount + 2 + 1); // main, work, edit
    CHECK(sidebar.at(0).toMap().value("kind").toString() == "section");
    // The profiles follow the Profiles section's row, and the Hosts section's row follows them.
    auto const profilesAt = static_cast<qsizetype>(SettingsSection::Profiles);
    CHECK(sidebar.at(profilesAt).toMap().value("id").toString() == "profiles");
    auto const work = sidebar.at(profilesAt + 2).toMap();
    CHECK(sidebar.at(profilesAt + 3).toMap().value("id").toString() == "hosts");
    CHECK(work.value("kind").toString() == "profile");
    CHECK(work.value("name").toString() == "work");
    CHECK(work.value("editable").toBool());
    CHECK(work.contains("tabColor"));
    auto const edit = sidebar.back().toMap();
    CHECK(edit.value("kind").toString() == "edit");
    CHECK(edit.value("title").toString() == QStringLiteral("Edit contour.yml…"));
}

TEST_CASE("SettingsController: search finds a profile field and a global one", "[settings][sections]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->editProfile("main");

    auto const found = fx.controller->searchSettings("scrollback");
    REQUIRE_FALSE(found.isEmpty());
    auto const first = found.first().toMap();
    CHECK(first.value("key").toString() == "history_max_lines");
    CHECK(first.value("scope").toString() == "profile");
    CHECK(first.value("section").toString() == "profiles");
    CHECK(first.value("group").toInt() == static_cast<int>(SettingsGroup::ProfileHistory));

    auto const gutter = fx.controller->searchSettings("GUTTER.");
    REQUIRE_FALSE(gutter.isEmpty());
    for (auto const& raw: gutter)
    {
        CHECK(raw.toMap().value("scope").toString() == "global");
        CHECK(raw.toMap().value("section").toString() == "shell-integration");
    }
    CHECK(fx.controller->searchSettings("  ").isEmpty());
}

TEST_CASE("SettingsController: search finds the status line page by its link",
          "[settings][sections][statusline]")
{
    auto fx = Fixture(BasicConfig);

    // Without a profile open: the link is a page, not a row of the draft.
    auto const found = fx.controller->searchSettings("status line");
    auto const link =
        std::ranges::find_if(found, [](QVariant const& raw) { return raw.toMap().contains("target"); });
    REQUIRE(link != found.end());
    auto const row = link->toMap();
    CHECK(row.value("target").toString() == "default-profile-status-line");
    CHECK(row.value("label").toString() == "Status Line");
    CHECK(row.value("section").toString() == "appearance");
    CHECK(row.value("scope").toString() == "global");
}

// }}}

// {{{ Controls

namespace
{
/// The value type a row's "type" token names.
[[nodiscard]] std::optional<SettingsValueType> valueTypeOf(QVariantMap const& row)
{
    return settingsValueTypeFromToken(row.value("type").toString().toStdString());
}
} // namespace

TEST_CASE("SettingsController: every row names a control its type accepts", "[settings][controls]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->editProfile("main");
    auto rows = fx.controller->globalFields();
    rows.append(fx.controller->profileFields());
    REQUIRE_FALSE(rows.isEmpty());
    for (auto const& raw: rows)
    {
        auto const row = raw.toMap();
        INFO(row.value("key").toString().toStdString());
        auto const type = valueTypeOf(row);
        REQUIRE(type.has_value());
        REQUIRE(row.contains("control"));
        auto const control = static_cast<SettingsControl>(row.value("control").toInt());
        CHECK(controlAccepts(control, *type));
        CHECK(row.value("controlName").toString().toStdString() == settingsControlToken(control));
        if (*type == SettingsValueType::Enum)
            CHECK(row.value("optionLabels").toStringList().size()
                  == row.value("options").toStringList().size());
        if (*type == SettingsValueType::Int || *type == SettingsValueType::Double)
        {
            CHECK(row.contains("min"));
            CHECK(row.contains("max"));
            CHECK(row.value("min").toDouble() <= row.value("max").toDouble());
            CHECK(row.value("step").toDouble() > 0.0);
            auto const unit = static_cast<NumberUnit>(row.value("unit").toInt());
            CHECK(row.value("unitText").toString().toStdString() == numberUnitRow(unit).suffix);
            CHECK(row.value("displayFactor").toDouble() == numberUnitRow(unit).displayFactor);
        }
    }
}

TEST_CASE("SettingsController: the rows the canvas shows take its controls", "[settings][controls]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->editProfile("main");
    auto const globals = fx.controller->globalFields();
    auto const fields = fx.controller->profileFields();

    SECTION("the theme offers the Phase 0 labels")
    {
        auto const theme = rowWithKey(globals, "theme");
        CHECK(theme.value("options").toStringList() == QStringList { "terminal", "system", "dark", "light" });
        CHECK(theme.value("optionLabels").toStringList()
              == QStringList { "Match terminal", "Match system", "Dark", "Light" });
        // "Match terminal" is fourteen characters: too long for a segment.
        CHECK(theme.value("control").toInt() == static_cast<int>(SettingsControl::PopUp));
    }

    SECTION("dimming is a slider in percent")
    {
        auto const dim = rowWithKey(fields, "dim_unfocused");
        CHECK(dim.value("label").toString() == "Dim inactive panes");
        CHECK(dim.value("help").toString() == "How far a pane without focus steps back.");
        CHECK(dim.value("control").toInt() == static_cast<int>(SettingsControl::Slider));
        CHECK(dim.value("unit").toInt() == static_cast<int>(NumberUnit::Percent));
        CHECK(dim.value("unitText").toString() == "%");
        CHECK(dim.value("displayFactor").toDouble() == 100.0);
        CHECK(dim.value("min").toDouble() == 0.0);
        CHECK(dim.value("max").toDouble() == 1.0);
    }

    SECTION("the scrollback is a stepper in lines that goes down to unlimited")
    {
        auto const scrollback = rowWithKey(fields, "history_max_lines");
        CHECK(scrollback.value("label").toString() == "Scrollback lines");
        CHECK(scrollback.value("control").toInt() == static_cast<int>(SettingsControl::Stepper));
        CHECK(scrollback.value("unit").toInt() == static_cast<int>(NumberUnit::Lines));
        CHECK(scrollback.value("min").toDouble() == -1.0);
    }

    SECTION("the font family is a font picker")
    {
        auto const font = rowWithKey(fields, "font_family");
        CHECK(font.value("label").toString() == "Font");
        CHECK(font.value("control").toInt() == static_cast<int>(SettingsControl::FontPicker));
    }

    SECTION("the tab colour is a colour well, empty for none")
    {
        auto const tabColor = rowWithKey(fields, "tab_color");
        CHECK(tabColor.value("control").toInt() == static_cast<int>(SettingsControl::ColorWell));
        CHECK(tabColor.value("value").toString().isEmpty());
        CHECK(tabColor.value("group").toInt() == static_cast<int>(SettingsGroup::ProfileWindow));
    }

    SECTION("a global with bounds publishes them")
    {
        auto const width = rowWithKey(globals, "gutter.block_column_width");
        CHECK(width.value("min").toDouble() == static_cast<double>(vtbackend::MinBlockColumnWidth));
        CHECK(width.value("max").toDouble() == static_cast<double>(vtbackend::MaxBlockColumnWidth));
        auto const uiFontSize = rowWithKey(globals, "ui_font_size");
        CHECK(uiFontSize.value("unit").toInt() == static_cast<int>(NumberUnit::Points));
        CHECK(uiFontSize.value("control").toInt() == static_cast<int>(SettingsControl::Stepper));
    }

    SECTION("a profile enum takes the labels of its table")
    {
        auto const sticky = rowWithKey(fields, "sticky_header_mode");
        CHECK(sticky.value("optionLabels").toStringList().size()
              == sticky.value("options").toStringList().size());
        CHECK(sticky.value("optionLabels").toStringList() != sticky.value("options").toStringList());
    }
}

TEST_CASE("SettingsController: a tab colour round-trips through the side file", "[settings][controls]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");

    REQUIRE(fx.controller->setProfileField("tab_color", QStringLiteral("#2e3440")).has_value());
    fx.fire();
    auto const* work = fx.cfg.findProfile("work");
    REQUIRE(work != nullptr);
    CHECK(work->tabColor.value() == vtbackend::RGBColor { 0x2e, 0x34, 0x40 });

    CHECK(rowWithKey(fx.controller->profileFields(), "tab_color").value("value").toString() == "#2e3440");
    REQUIRE(fx.controller->setProfileField("tab_color", QString {}).has_value());
    fx.fire();
    CHECK_FALSE(fx.cfg.findProfile("work")->tabColor.value().has_value());

    // A value that is no colour changes nothing.
    auto const ignored = fx.controller->setProfileField("tab_color", QStringLiteral("#12345"));
    CHECK(ignored.has_value());
    fx.fire();
    CHECK_FALSE(fx.cfg.findProfile("work")->tabColor.value().has_value());
}

TEST_CASE("SettingsController: a profile row carries what its rule offers", "[settings][controls]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");

    auto const main = rowNamed(fx.controller->profiles(), "main");
    CHECK_FALSE(main.value("editable").toBool());
    CHECK(main.value("banner").toString() == "Defined in contour.yml");
    auto const mainActions = main.value("actions").toMap();
    CHECK(mainActions.value("duplicate").toBool());
    CHECK(mainActions.value("setDefault").toBool());
    CHECK_FALSE(mainActions.value("delete").toBool());
    CHECK_FALSE(mainActions.value("rename").toBool());
    CHECK_FALSE(mainActions.value("edit").toBool());

    auto const work = rowNamed(fx.controller->profiles(), "work");
    CHECK(work.value("editable").toBool());
    CHECK(work.value("banner").toString().isEmpty());
    auto const workActions = work.value("actions").toMap();
    CHECK(workActions.size() == static_cast<qsizetype>(ProfileActionCount));
    for (auto const& offered: workActions)
        CHECK(offered.toBool());
}

TEST_CASE("SettingsController: Use New brings a side file's spelled old status line along",
          "[settings][controls]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    REQUIRE(fx.cfg.defaultsEdition == config::DefaultsEdition::Classic);
    auto const classic = config::editionDefaults(config::DefaultsEdition::Classic).statusLine;
    auto const native = config::editionDefaults(config::DefaultsEdition::Native2026).statusLine;
    REQUIRE(fx.cfg.findProfile("work")->statusLine.value().indicator.left == classic.left);

    // The left segment is the user's own: it stays.
    fx.controller->setIndicatorSegment(0, QStringLiteral("{Clock}"));
    fx.fire();

    REQUIRE(fx.controller
                ->writeGlobalOverride(config::DefaultsEditionKey,
                                      config::configEnumToken(config::DefaultsEdition::Native2026))
                .has_value());
    REQUIRE(fx.cfg.defaultsEdition == config::DefaultsEdition::Native2026);
    auto const& indicator = fx.cfg.findProfile("work")->statusLine.value().indicator;
    CHECK(indicator.left == "{Clock}");
    CHECK(indicator.middle == native.middle);
    CHECK(indicator.right == native.right);

    // One session change: Revert puts the side file and settings.yml back together.
    fx.controller->revertChanges();
    CHECK(fx.errors.isEmpty());
    CHECK(fx.cfg.defaultsEdition == config::DefaultsEdition::Classic);
    CHECK(fx.cfg.findProfile("work")->statusLine.value().indicator.middle == classic.middle);
}

// }}}

// {{{ Drafts are seeded from what the configuration loaded

TEST_CASE("SettingsController: a hand edit made before a profile opens is not written over", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    auto const* main = fx.cfg.findProfile("main");
    REQUIRE(main != nullptr);
    auto edited = *main; // a copy: the reload below replaces the configuration
    REQUIRE(fx.store->write(config::profileFile("work"), config::emitProfileYaml(edited)).has_value());
    fx.reload();
    edited.maximized = true;
    auto const handEdited = config::emitProfileYaml(edited);
    fx.handEdit(config::relativePathOf(config::profileFile("work")), handEdited);

    fx.controller->editProfile("work");
    fx.controller->commitProfileField("show_title_bar", false);
    fx.fire();

    CHECK(fx.fileContent(config::relativePathOf(config::profileFile("work"))) == handEdited);
    REQUIRE(fx.errors.size() == 1);
    CHECK(fx.errors.front().contains("changed outside Settings"));
}

TEST_CASE("SettingsController: a hand edit made before a scheme opens is kept", "[settings]")
{
    // The scheme draft is parsed from the bytes the session records as seen, whatever the configuration
    // holds of the scheme.
    // No profile draws with it: one that did would follow it, which is not what this case is about.
    auto fx = Fixture(BasicConfig);
    auto palette = vtbackend::ColorPalette {};
    REQUIRE(
        fx.store->write(config::colorSchemeFile("mine"), config::emitColorSchemeYaml(palette)).has_value());
    fx.reload();
    palette.defaultForeground = vtbackend::RGBColor { 0x12, 0x34, 0x56 };
    fx.handEdit(config::relativePathOf(config::colorSchemeFile("mine")),
                config::emitColorSchemeYaml(palette));

    fx.controller->editColorScheme("mine");
    REQUIRE(fx.controller->setSchemeColor("background", "#101010").has_value());
    fx.fire();

    auto const written = fx.schemeFile("mine");
    REQUIRE(written.has_value());
    CHECK(written->defaultForeground == vtbackend::RGBColor { 0x12, 0x34, 0x56 });
}

namespace
{
/// Writes side-file profile `work` as a copy of `main`, reloads, then hand-edits it (maximized) without a
/// reload, as an editor does between Settings' load and its next write. The profile is never opened.
/// @return The hand-edited bytes.
std::string handEditedUnopenedProfile(Fixture& fx)
{
    auto const* main = fx.cfg.findProfile("main");
    REQUIRE(main != nullptr);
    auto edited = *main; // a copy: the reload below replaces the configuration
    REQUIRE(fx.store->write(config::profileFile("work"), config::emitProfileYaml(edited)).has_value());
    fx.reload();
    edited.maximized = true;
    auto handEdited = config::emitProfileYaml(edited);
    fx.handEdit(config::relativePathOf(config::profileFile("work")), handEdited);
    return handEdited;
}
} // namespace

TEST_CASE("SettingsController: renaming a profile that was never opened keeps a hand edit", "[settings]")
{
    // N1: the rename stages the configuration's bytes, so a hand edit made since the load is a conflict.
    auto fx = Fixture(BasicConfig);
    auto const handEdited = handEditedUnopenedProfile(fx);

    REQUIRE(fx.controller->renameProfile("work", "office").has_value());
    fx.fire();

    CHECK(fx.fileContent(config::relativePathOf(config::profileFile("work"))) == handEdited);
    CHECK_FALSE(fx.fileContent(config::relativePathOf(config::profileFile("office"))).has_value());
    REQUIRE(fx.errors.size() == 1);
    CHECK(fx.errors.front().contains("changed outside Settings"));
}

TEST_CASE("SettingsController: renaming a scheme that was never opened carries a hand edit", "[settings]")
{
    // N1: the rename reads the side file as it is, as opening it does, and records those same bytes.
    // No profile draws with it: one that did would follow it, which is not what this case is about.
    auto fx = Fixture(BasicConfig);
    auto palette = vtbackend::ColorPalette {};
    REQUIRE(
        fx.store->write(config::colorSchemeFile("mine"), config::emitColorSchemeYaml(palette)).has_value());
    fx.reload();
    palette.defaultForeground = vtbackend::RGBColor { 0x12, 0x34, 0x56 };
    fx.handEdit(config::relativePathOf(config::colorSchemeFile("mine")),
                config::emitColorSchemeYaml(palette));

    REQUIRE(fx.controller->renameColorScheme("mine", "mine2").has_value());
    fx.fire();

    CHECK(fx.errors.isEmpty());
    auto const renamed = fx.schemeFile("mine2");
    REQUIRE(renamed.has_value());
    CHECK(renamed->defaultForeground == vtbackend::RGBColor { 0x12, 0x34, 0x56 });
    CHECK_FALSE(fx.fileContent(config::relativePathOf(config::colorSchemeFile("mine"))).has_value());
}

TEST_CASE("SettingsController: Use New keeps a hand edit of a profile that was never opened", "[settings]")
{
    // N1: Use New rewrites the side-file profiles that spell the old edition's status line, from the
    // configuration's bytes.
    auto fx = Fixture(BasicConfig);
    auto const handEdited = handEditedUnopenedProfile(fx);

    auto const written = fx.controller->writeGlobalOverride(
        config::DefaultsEditionKey, config::configEnumToken(config::DefaultsEdition::Native2026));

    REQUIRE_FALSE(written.has_value());
    CHECK(written.error().kind == config::SettingsErrorKind::ChangedOnDisk);
    CHECK(fx.fileContent(config::relativePathOf(config::profileFile("work"))) == handEdited);
}

TEST_CASE("SettingsController: a profile saved by hand in a state that does not parse is kept", "[settings]")
{
    // N3: the flush finds the conflict, the reload cannot parse the file, and the draft still shows
    // Settings' own edit: the next commit must not write it over the hand edit.
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    fx.controller->commitProfileField("show_title_bar", false);
    auto const path = config::relativePathOf(config::profileFile("work"));
    fx.handEdit(path, "this: [is not\n  a profile");
    fx.fire();
    REQUIRE(fx.errors.size() == 1);
    CHECK(fx.errors.front().contains("changed outside Settings"));

    fx.controller->commitProfileField("show_title_bar", true);
    fx.fire();

    CHECK(fx.fileContent(path) == "this: [is not\n  a profile");
}

TEST_CASE("SettingsController: a profile deleted by hand is not re-created by the next commit", "[settings]")
{
    // N3, the deleted file.
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    fx.controller->commitProfileField("show_title_bar", false);
    auto const path = config::relativePathOf(config::profileFile("work"));
    std::filesystem::remove(fx.configDir() / path);
    fx.fire();
    REQUIRE(fx.errors.size() == 1);

    fx.controller->commitProfileField("show_title_bar", true);
    fx.fire();

    CHECK_FALSE(fx.fileContent(path).has_value());
}

TEST_CASE("SettingsController: a profile name that folds equal to another is refused", "[settings]")
{
    // N5: on Windows and macOS `Office` and `office` are one file, so a pending write and an existing name
    // both count.
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    fx.openSideProfile("other");
    REQUIRE(fx.controller->renameProfile("work", "Office").has_value()); // pending, not on disk yet

    auto const pending = fx.controller->renameProfile("other", "office");
    REQUIRE_FALSE(pending.has_value());
    CHECK(pending.error().kind == config::SettingsErrorKind::NameTaken);

    auto const existing = fx.controller->renameProfile("other", "MAIN");
    REQUIRE_FALSE(existing.has_value());
    CHECK(existing.error().kind == config::SettingsErrorKind::NameTaken);

    CHECK(fx.controller->renameProfile("other", "elsewhere").has_value());
}

TEST_CASE("SettingsController: a rename that only changes letter case is refused, and says why", "[settings]")
{
    // On a case-insensitive file system the new file is the old one, which the rename then removes.
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    auto const refused = fx.controller->renameProfile("work", "Work");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == config::SettingsErrorKind::NameTaken);
    CHECK(refused.error().message.contains("letter case"));
    fx.fire();
    CHECK(fx.cfg.findProfile("work") != nullptr);
}

TEST_CASE("SettingsController: a rename announces the new name with the draft already moved", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    auto editingWhenAnnounced = QString {};
    auto const connection = QObject::connect(fx.controller.get(),
                                             &SettingsController::profileRenamed,
                                             fx.controller.get(),
                                             [&](QString const& oldName, QString const& newName) {
                                                 CHECK(oldName == "work");
                                                 CHECK(newName == "office");
                                                 editingWhenAnnounced = fx.controller->editingProfile();
                                             });
    auto const spy = QSignalSpy { fx.controller.get(), &SettingsController::profileRenamed };
    REQUIRE(fx.controller->renameProfile("work", "office").has_value());
    CHECK(spy.count() == 1);
    CHECK(editingWhenAnnounced == "office");
    QObject::disconnect(connection);
}

TEST_CASE("SettingsController: a scheme name that folds equal to another is refused", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideScheme("one");
    fx.openSideScheme("two");
    REQUIRE(fx.controller->renameColorScheme("one", "Third").has_value()); // pending, not on disk yet

    auto const pending = fx.controller->renameColorScheme("two", "third");
    REQUIRE_FALSE(pending.has_value());
    CHECK(pending.error().kind == config::SettingsErrorKind::NameTaken);

    auto const existing = fx.controller->renameColorScheme("two", "DEFAULT");
    REQUIRE_FALSE(existing.has_value());
    CHECK(existing.error().kind == config::SettingsErrorKind::NameTaken);
}

TEST_CASE("SettingsController: a hand edit of settings.yml before Settings opens is not written over",
          "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.handEdit("settings.yml", "theme: light\n");
    fx.restartController(); // Settings opens late, over the configuration loaded before the edit

    REQUIRE(fx.controller->setGlobalField("reflow_on_resize", false).has_value());
    fx.fire();

    CHECK(fx.fileContent("settings.yml") == "theme: light\n");
    REQUIRE(fx.errors.size() == 1);
    CHECK(fx.errors.front().contains("changed outside Settings"));
}

TEST_CASE("SettingsController: a commit after a reload of a hand edit lands beside it", "[settings]")
{
    // I3: the reload read the hand edit, so it is no conflict.
    auto fx = Fixture(BasicConfig);
    fx.commitGlobal("reflow_on_resize", false);
    auto const written = fx.fileContent("settings.yml");
    REQUIRE(written.has_value());
    fx.handEdit("settings.yml", *written + "theme: light\n");
    fx.reload();

    fx.commitGlobal("snap_to_rows", true);

    CHECK(fx.errors.isEmpty());
    CHECK(fx.cfg.theme.value() == config::GuiTheme::Light);
    CHECK(fx.cfg.snapToRows.value());
    CHECK_FALSE(fx.cfg.reflowOnResize.value());
}

TEST_CASE("SettingsController: an open profile follows a reload of a hand edit", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    auto const* work = fx.cfg.findProfile("work");
    REQUIRE(work != nullptr);
    auto edited = *work;
    REQUIRE_FALSE(edited.maximized.value());
    edited.maximized = true;
    fx.handEdit(config::relativePathOf(config::profileFile("work")), config::emitProfileYaml(edited));
    fx.reload();

    CHECK(rowWithKey(fx.controller->profileFields(), "maximized").value("value").toBool());
    fx.controller->commitProfileField("show_title_bar", false);
    fx.fire();
    CHECK(fx.errors.isEmpty());
    CHECK(fx.cfg.findProfile("work")->maximized.value());
    CHECK_FALSE(fx.cfg.findProfile("work")->showTitleBar.value());
}

// }}}

TEST_CASE("settings: the font picker lists the injected families, and the bundled one alone without a source",
          "[settings][fonts]")
{
    auto fx = Fixture(BasicConfig);
    CHECK(fx.controller->fontFamilies()
          == QStringList {
              QString::fromUtf8(platform::BundledMonospaceFamily.data(),
                                static_cast<qsizetype>(platform::BundledMonospaceFamily.size())) });

    auto source = std::make_shared<FakeFontFamilySource>();
    source->families = { "JetBrains Mono", "Iosevka" };
    fx.fontFamilies = source;
    fx.restartController();
    fx.controller->refresh();
    CHECK(fx.controller->fontFamilies() == QStringList { "JetBrains Mono", "Iosevka" });
}

TEST_CASE("SettingsController: option_as_alt is a segmented row of the four settings",
          "[settings][optionasalt]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->addProfile();
    fx.scheduler->fire();
    REQUIRE_FALSE(fx.controller->editingReadOnly());

    auto const row = rowWithKey(fx.controller->profileFields(), "option_as_alt");
    REQUIRE_FALSE(row.isEmpty());
    CHECK(row.value("label").toString() == "Use Option as Meta");
    CHECK(row.value("help").toString()
          == "The other Option key keeps typing accents like \u00e9 and \u00fc.");
    CHECK(row.value("options").toStringList() == QStringList { "off", "left", "right", "both" });
    CHECK(row.value("optionLabels").toStringList() == QStringList { "Off", "Left", "Right", "Both" });
    CHECK(row.value("control").toInt() == static_cast<int>(SettingsControl::Segmented));
    CHECK(row.value("controlName").toString() == "segmented");
    CHECK(row.value("value").toString() == "off");
    CHECK(row.value("group").toInt() == static_cast<int>(SettingsGroup::ProfileKeyboard));

    fx.controller->commitProfileField("option_as_alt", "left");
    fx.scheduler->fire();
    auto const written = fx.fileContent("profiles/New Profile.yml");
    REQUIRE(written.has_value());
    CHECK(written->contains("option_as_alt: left\n"));
    CHECK(fx.cfg.findProfile("New Profile")->optionAsAlt.value() == config::OptionAsAlt::Left);
    CHECK(rowWithKey(fx.controller->profileFields(), "option_as_alt").value("value").toString() == "left");
}

TEST_CASE("SettingsController: input_method_editor_style offers both styles and is written to the profile",
          "[settings][preedit]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->addProfile();
    fx.scheduler->fire();
    REQUIRE_FALSE(fx.controller->editingReadOnly());

    auto expected = QStringList {};
    for (auto const& info: config::configEnumValues<vtbackend::PreeditStyle>())
        expected.push_back(QString::fromUtf8(info.token.data(), static_cast<qsizetype>(info.token.size())));
    REQUIRE(expected == QStringList { "highlighted", "inline" });

    auto const row = rowWithKey(fx.controller->profileFields(), "input_method_editor_style");
    REQUIRE_FALSE(row.isEmpty());
    CHECK(row.value("options").toStringList() == expected);
    CHECK(row.value("group").toInt() == static_cast<int>(SettingsGroup::ProfileBehaviour));
    auto const before = fx.cfg.findProfile("New Profile")->inputMethodEditorStyle.value();
    CHECK(row.value("value").toString()
          == QString::fromUtf8(config::configEnumToken(before).data(),
                               static_cast<qsizetype>(config::configEnumToken(before).size())));

    auto const other = before == vtbackend::PreeditStyle::Inline ? "highlighted" : "inline";
    fx.controller->commitProfileField("input_method_editor_style", other);
    fx.scheduler->fire();
    auto const written = fx.fileContent("profiles/New Profile.yml");
    REQUIRE(written.has_value());
    CHECK(written->contains(std::format("input_method_editor_style: {}\n", other)));
    CHECK(fx.cfg.findProfile("New Profile")->inputMethodEditorStyle.value() != before);
}

TEST_CASE(
    "SettingsController: scrollbar.mode is an enum row of the three preferences, written to the profile",
    "[settings][scrollbar]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->addProfile();
    fx.scheduler->fire();
    REQUIRE_FALSE(fx.controller->editingReadOnly());

    auto expected = QStringList {};
    for (auto const& info: config::configEnumValues<config::ScrollBarModePreference>())
        expected.push_back(QString::fromUtf8(info.token.data(), static_cast<qsizetype>(info.token.size())));
    REQUIRE(expected.size() == 3);

    auto const row = rowWithKey(fx.controller->profileFields(), "scrollbar_mode");
    REQUIRE_FALSE(row.isEmpty());
    CHECK(row.value("options").toStringList() == expected);
    CHECK(row.value("group").toInt() == static_cast<int>(SettingsGroup::ProfileHistory));
    // A new profile takes its edition's mode, whatever that is: the row shows it.
    auto const before = fx.cfg.findProfile("New Profile")->scrollbar.value().mode;
    auto const beforeToken = config::configEnumToken(before);
    CHECK(row.value("value").toString().toStdString() == beforeToken);
    auto const other = before == config::ScrollBarModePreference::Always ? "overlay" : "always";

    fx.controller->commitProfileField("scrollbar_mode", other);
    fx.scheduler->fire();
    auto const written = fx.fileContent("profiles/New Profile.yml");
    REQUIRE(written.has_value());
    CHECK(written->contains(std::format("mode: {}", other)));
    CHECK(fx.cfg.findProfile("New Profile")->scrollbar.value().mode != before);
    CHECK(rowWithKey(fx.controller->profileFields(), "scrollbar_mode").value("value").toString().toStdString()
          == other);
}

// {{{ An unflushed edit survives switching to another draft

namespace
{

/// Writes side-file profiles and schemes @p names without opening any, and reloads.
void writeSideProfiles(Fixture& fx, std::initializer_list<std::string> names)
{
    auto const* main = fx.cfg.findProfile("main");
    REQUIRE(main != nullptr);
    for (auto const& name: names)
        REQUIRE(fx.store->write(config::profileFile(name), config::emitProfileYaml(*main)).has_value());
    fx.reload();
}

} // namespace

TEST_CASE("SettingsController: renaming a profile within the debounce after switching away keeps its edit",
          "[settings][debounce]")
{
    auto fx = Fixture(BasicConfig);
    writeSideProfiles(fx, { "work", "home" });
    fx.controller->editProfile("work");
    REQUIRE(fx.controller->setProfileField("show_title_bar", false).has_value());
    fx.controller->editProfile("home"); // within the debounce: the scheduler has not fired

    REQUIRE(fx.controller->renameProfile("work", "office").has_value());
    fx.fire();

    CHECK(fx.errors.isEmpty());
    REQUIRE(fx.cfg.findProfile("office") != nullptr);
    CHECK(fx.cfg.findProfile("office")->showTitleBar.value() == false);
    CHECK(fx.cfg.findProfile("work") == nullptr);
}

TEST_CASE("SettingsController: Use New within the debounce after switching away keeps a profile's edit",
          "[settings][debounce]")
{
    auto fx = Fixture(BasicConfig);
    writeSideProfiles(fx, { "work", "home" });
    fx.controller->editProfile("work");
    REQUIRE(fx.controller->setProfileField("show_title_bar", false).has_value());
    fx.controller->setIndicatorSegment(0, QStringLiteral("{Clock}"));
    fx.controller->editProfile("home");

    REQUIRE(fx.controller
                ->writeGlobalOverride(config::DefaultsEditionKey,
                                      config::configEnumToken(config::DefaultsEdition::Native2026))
                .has_value());
    fx.fire();

    CHECK(fx.errors.isEmpty());
    CHECK(fx.cfg.findProfile("work")->showTitleBar.value() == false);
    CHECK(fx.cfg.findProfile("work")->statusLine.value().indicator.left == "{Clock}");
}

TEST_CASE("SettingsController: reopening a profile within the debounce shows and keeps its edit",
          "[settings][debounce]")
{
    auto fx = Fixture(BasicConfig);
    writeSideProfiles(fx, { "work", "home" });
    fx.controller->editProfile("work");
    REQUIRE(fx.controller->setProfileField("show_title_bar", false).has_value());
    fx.controller->editProfile("home");
    fx.controller->editProfile("work");

    CHECK(rowWithKey(fx.controller->profileFields(), "show_title_bar").value("value").toBool() == false);
    REQUIRE(fx.controller->setProfileField("maximized", false).has_value());
    fx.fire();

    CHECK(fx.errors.isEmpty());
    CHECK(fx.cfg.findProfile("work")->showTitleBar.value() == false);
    CHECK(fx.cfg.findProfile("work")->maximized.value() == false);
}

TEST_CASE("SettingsController: renaming a scheme within the debounce after switching away keeps its edit",
          "[settings][debounce]")
{
    auto fx = Fixture(BasicConfig);
    for (auto const* name: { "midnight", "dawn" })
        REQUIRE(fx.store
                    ->write(config::colorSchemeFile(name),
                            config::emitColorSchemeYaml(vtbackend::ColorPalette {}))
                    .has_value());
    fx.reload();
    fx.controller->editColorScheme("midnight");
    REQUIRE(fx.controller->setSchemeColor("background", "#101010").has_value());
    fx.controller->editColorScheme("dawn");

    REQUIRE(fx.controller->renameColorScheme("midnight", "nightfall").has_value());
    fx.fire();

    CHECK(fx.errors.isEmpty());
    REQUIRE(fx.schemeFile("nightfall").has_value());
    CHECK(fx.schemeFile("nightfall")->defaultBackground == vtbackend::RGBColor { 0x10, 0x10, 0x10 });
}

TEST_CASE("SettingsController: reopening a scheme within the debounce shows its edit", "[settings][debounce]")
{
    auto fx = Fixture(BasicConfig);
    for (auto const* name: { "midnight", "dawn" })
        REQUIRE(fx.store
                    ->write(config::colorSchemeFile(name),
                            config::emitColorSchemeYaml(vtbackend::ColorPalette {}))
                    .has_value());
    fx.reload();
    fx.controller->editColorScheme("midnight");
    REQUIRE(fx.controller->setSchemeColor("background", "#101010").has_value());
    fx.controller->editColorScheme("dawn");
    fx.controller->editColorScheme("midnight");

    auto const colors = fx.controller->schemeColors();
    auto const background = std::ranges::find_if(
        colors, [](QVariant const& row) { return row.toMap().value("key").toString() == "background"; });
    REQUIRE(background != colors.end());
    CHECK(background->toMap().value("color").toString().toLower() == "#101010");
    fx.fire();
    CHECK(fx.errors.isEmpty());
    REQUIRE(fx.schemeFile("midnight").has_value());
    CHECK(fx.schemeFile("midnight")->defaultBackground == vtbackend::RGBColor { 0x10, 0x10, 0x10 });
}

// }}}

// {{{ The Colors page: gallery, readout and what a profile's use of a scheme allows

namespace
{
/// The colour (as "#rrggbb") of @p slot in the open scheme's rows.
[[nodiscard]] QString slotColor(SettingsController const& controller, QString const& slot)
{
    return rowWithKey(controller.schemeColors(), slot).value("color").toString();
}

/// Writes side-file profile @p name, a copy of contour.yml's `main` drawn with @p colors, and reloads.
void writeProfileDrawnWith(Fixture& fx, std::string const& name, config::ColorConfig colors)
{
    auto profile = *fx.cfg.findProfile("main");
    profile.colors = std::move(colors);
    REQUIRE(fx.store->write(config::profileFile(name), config::emitProfileYaml(profile)).has_value());
    fx.reload();
}

/// Writes side-file color scheme @p name with the default palette, and reloads.
void writeScheme(Fixture& fx, std::string const& name)
{
    REQUIRE(
        fx.store
            ->write(config::colorSchemeFile(name), config::emitColorSchemeYaml(vtbackend::ColorPalette {}))
            .has_value());
    fx.reload();
}
} // namespace

TEST_CASE("SettingsController: the scheme gallery lists every scheme with its colours", "[settings][schemes]")
{
    auto fx = Fixture(BasicConfig);
    writeScheme(fx, "midnight");

    auto const gallery = fx.controller->schemeGallery();
    auto names = QStringList {};
    for (auto const& raw: gallery)
        names << raw.toMap().value("name").toString();
    auto expected = QStringList {};
    for (auto const& raw: fx.controller->colorSchemes())
        expected << raw.toMap().value("name").toString();
    CHECK(names == expected);
    CHECK(names.contains("midnight"));
    CHECK(names.contains("default"));

    for (auto const& raw: gallery)
    {
        auto const card = raw.toMap();
        INFO(card.value("name").toString().toStdString());
        CHECK(card.value("background").toString().size() == 7);
        CHECK(card.value("foreground").toString().size() == 7);
        auto const normal = card.value("normal").toStringList();
        CHECK(normal.size() == 8);
        for (auto const& color: normal)
            CHECK(QColor::isValidColorName(color));
    }

    // Each card says whether it is one Settings made, and so can edit.
    auto const cardOf = [&](QString const& name) {
        for (auto const& raw: gallery)
            if (raw.toMap().value("name").toString() == name)
                return raw.toMap();
        return QVariantMap {};
    };
    CHECK(cardOf("midnight").value("editable").toBool());
    CHECK_FALSE(cardOf("default").value("editable").toBool());
    CHECK(cardOf("default").value("background").toString()
          == QString::asprintf("#%02x%02x%02x",
                               vtbackend::ColorPalette {}.defaultBackground.red,
                               vtbackend::ColorPalette {}.defaultBackground.green,
                               vtbackend::ColorPalette {}.defaultBackground.blue));
}

TEST_CASE("SettingsController: the gallery follows a colour as it is edited, and shows a new scheme at once",
          "[settings][schemes]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideScheme("midnight");
    REQUIRE(fx.controller->setSchemeColor("background", "#102030").has_value());

    auto const backgroundOf = [&](QString const& name) {
        for (auto const& raw: fx.controller->schemeGallery())
            if (raw.toMap().value("name").toString() == name)
                return raw.toMap().value("background").toString();
        return QString {};
    };
    CHECK(backgroundOf("midnight") == "#102030"); // before the debounce writes it

    // A duplicate is staged, not yet written, and is on the page nonetheless.
    auto const copy = fx.controller->duplicateColorScheme("midnight");
    REQUIRE(copy.has_value());
    CHECK(backgroundOf(*copy) == "#102030");
}

TEST_CASE("SettingsController: every slot says what its colour is read against", "[settings][schemes]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideScheme("midnight");
    auto const background = slotColor(*fx.controller, "background");

    auto const rows = fx.controller->schemeColors();
    REQUIRE_FALSE(rows.isEmpty());
    CHECK(rowWithKey(rows, "background").value("against").toString().isEmpty());
    for (auto const& raw: rows)
    {
        auto const row = raw.toMap();
        INFO(row.value("key").toString().toStdString());
        REQUIRE(row.contains("against"));
        if (row.value("key").toString() == "background")
            continue;
        // What it sits on is a colour the scheme has; no slot is judged against itself or against nothing.
        CHECK(QColor::isValidColorName(row.value("against").toString()));
    }
    // The text slots sit on the background.
    for (auto const* slot: { "foreground", "normal_red", "normal_white", "bright_black", "bright_white" })
        CHECK(rowWithKey(rows, slot).value("against").toString() == background);
}

TEST_CASE("SettingsController: every slot sits in a block of the editor", "[settings][schemes]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideScheme("midnight");

    auto counts = std::map<QString, int> {};
    for (auto const& raw: fx.controller->schemeColors())
        ++counts[raw.toMap().value("group").toString()];
    CHECK(counts["base"] == 2);
    CHECK(counts["normal"] == 8);
    CHECK(counts["bright"] == 8);
    CHECK(counts["interface"] > 0);
    CHECK(counts.size() == 4);
}

TEST_CASE("SettingsController: contrastReadout answers the page's ratio and verdict", "[settings][schemes]")
{
    auto fx = Fixture(BasicConfig);

    auto const widest = fx.controller->contrastReadout("#000000", "#ffffff");
    CHECK(widest.value("text").toString() == "21.00 : 1, passes");
    CHECK(widest.value("passes").toBool());

    auto const narrow = fx.controller->contrastReadout("#777777", "#ffffff");
    CHECK(narrow.value("text").toString() == "4.47 : 1, fails"); // 4.478, truncated
    CHECK_FALSE(narrow.value("passes").toBool());

    // A half-typed colour is no reading.
    CHECK(fx.controller->contrastReadout("#12", "#ffffff").value("text").toString().isEmpty());
}

TEST_CASE("SettingsController: a colour scheme a profile draws with cannot be deleted", "[settings][schemes]")
{
    auto fx = Fixture(BasicConfig);
    writeScheme(fx, "midnight");
    writeScheme(fx, "dawn");
    writeProfileDrawnWith(fx, "work", config::SimpleColorConfig { .colorScheme = "midnight" });
    writeProfileDrawnWith(
        fx, "dually", config::DualColorConfig { .colorSchemeLight = "dawn", .colorSchemeDark = "default" });

    for (auto const* name: { "midnight", "dawn" })
    {
        INFO(name);
        auto const refused = fx.controller->deleteColorScheme(name);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().kind == config::SettingsErrorKind::InUse);
        CHECK(refused.error().message.contains(name == std::string_view { "midnight" } ? "work" : "dually"));
    }
    fx.fire();
    CHECK(fx.schemeFile("midnight").has_value());
    CHECK(fx.schemeFile("dawn").has_value());

    // The page shows the reason as it does for any refusal.
    fx.controller->requestDeleteScheme("midnight");
    REQUIRE_FALSE(fx.errors.isEmpty());
    CHECK(fx.errors.back().contains("work"));

    // The draft being edited counts, before it is written: a scheme a profile is about to leave is free.
    fx.controller->editProfile("work");
    fx.controller->setColorScheme("default");
    REQUIRE(fx.controller->deleteColorScheme("midnight").has_value());
    fx.fire();
    CHECK_FALSE(fx.schemeFile("midnight").has_value());
}

TEST_CASE("SettingsController: a profile that is about to use a scheme keeps it from being deleted",
          "[settings][schemes]")
{
    auto fx = Fixture(BasicConfig);
    writeScheme(fx, "midnight");
    fx.openSideProfile("work");
    fx.controller->setColorScheme("midnight"); // staged, not yet written

    auto const refused = fx.controller->deleteColorScheme("midnight");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == config::SettingsErrorKind::InUse);
}

TEST_CASE("SettingsController: renaming a scheme moves the profiles that draw with it", "[settings][schemes]")
{
    auto fx = Fixture(BasicConfig);
    writeScheme(fx, "midnight");
    writeScheme(fx, "dawn");
    writeProfileDrawnWith(fx, "work", config::SimpleColorConfig { .colorScheme = "midnight" });
    writeProfileDrawnWith(
        fx, "dually", config::DualColorConfig { .colorSchemeLight = "dawn", .colorSchemeDark = "midnight" });
    fx.controller->editProfile("work"); // one of them is open, with its draft

    REQUIRE(fx.controller->renameColorScheme("midnight", "nightfall").has_value());
    fx.fire();

    CHECK(fx.errors.isEmpty());
    CHECK(fx.schemeFile("nightfall").has_value());
    CHECK_FALSE(fx.schemeFile("midnight").has_value());
    auto const* work = fx.cfg.findProfile("work");
    REQUIRE(work != nullptr);
    CHECK(std::get<config::SimpleColorConfig>(work->colors.value()).colorScheme == "nightfall");
    auto const* dually = fx.cfg.findProfile("dually");
    REQUIRE(dually != nullptr);
    auto const& dual = std::get<config::DualColorConfig>(dually->colors.value());
    CHECK(dual.colorSchemeLight == "dawn"); // not the one renamed
    CHECK(dual.colorSchemeDark == "nightfall");
    CHECK(fx.controller->colorScheme() == "nightfall"); // the open draft moved too
}

TEST_CASE("SettingsController: a scheme a contour.yml profile draws with cannot be renamed",
          "[settings][schemes]")
{
    // contour.yml is never written, so the profile would keep naming a scheme that is gone.
    auto fx = Fixture(std::string_view { R"(
default_profile: main
profiles:
    main:
        colors: midnight
)" });
    writeScheme(fx, "midnight");

    auto const refused = fx.controller->renameColorScheme("midnight", "nightfall");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == config::SettingsErrorKind::InUse);
    CHECK(refused.error().message.contains("main"));
    fx.fire();
    CHECK(fx.schemeFile("midnight").has_value());
    CHECK_FALSE(fx.schemeFile("nightfall").has_value());
}

// }}}

// }}}
// {{{ A failed or conflicting direct flush re-seeds the drafts, as the debounced one does

namespace
{

/// What an editor leaves behind when it saves a profile in a state that does not parse.
constexpr auto UnparsableProfile = std::string_view { "{{{ not yaml\n" };

} // namespace

TEST_CASE("SettingsController: a scheme delete that flushes a conflicting profile closes its draft",
          "[settings][debounce]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideScheme("unused");
    fx.openSideProfile("work");
    fx.controller->commitProfileField("show_title_bar", false);
    auto const workFile = config::relativePathOf(config::profileFile("work"));
    fx.handEdit(workFile, UnparsableProfile);

    fx.controller->requestDeleteScheme("unused"); // flushes work within the debounce: a conflict

    REQUIRE_FALSE(fx.errors.isEmpty());
    CHECK(fx.errors.back().contains("changed outside Settings"));
    CHECK(fx.controller->editingProfile().isEmpty());
    fx.controller->commitProfileField("maximized", false);
    fx.fire();
    CHECK(fx.fileContent(workFile) == UnparsableProfile);
}

TEST_CASE("SettingsController: opening a profile whose pending flush conflicts closes the stale draft",
          "[settings][debounce]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    fx.controller->commitProfileField("show_title_bar", false);
    auto const workFile = config::relativePathOf(config::profileFile("work"));
    fx.handEdit(workFile, UnparsableProfile);

    fx.controller->editProfile("work");

    REQUIRE_FALSE(fx.errors.isEmpty());
    CHECK(fx.controller->editingProfile().isEmpty());
    fx.controller->commitProfileField("maximized", false);
    fx.fire();
    CHECK(fx.fileContent(workFile) == UnparsableProfile);
}

TEST_CASE("SettingsController: a failed Use New leaves nothing staged for the next write",
          "[settings][debounce]")
{
    auto fx = Fixture(BasicConfig);
    fx.openSideProfile("work");
    // settings.yml takes no write, as on a full or read-only disk: nothing of the change lands. The store
    // writes through a file system that fails by injection, which holds on every platform and as root.
    auto const settingsTarget = fx.configDir() / "settings.yml";
    auto const faulty = vthost::testing::FaultyFileSystem { [&](auto op, std::filesystem::path const& path) {
        return op == vthost::testing::FaultOp::Rename && path == settingsTarget;
    } };
    fx.store = std::make_shared<config::FileGuiConfigStore>(faulty, fx.configDir());
    fx.restartController();
    auto const written = fx.controller->writeGlobalOverride(
        config::DefaultsEditionKey, config::configEnumToken(config::DefaultsEdition::Native2026));
    REQUIRE_FALSE(written.has_value()); // a full disk, as far as the store can tell

    fx.controller->commitGlobalField("reflow_on_resize", false);
    fx.fire();

    // Nothing reached settings.yml, neither the refused edition nor the field committed after it.
    CHECK_FALSE(faulty.readFile(settingsTarget).has_value());
    CHECK(fx.cfg.defaultsEdition == config::DefaultsEdition::Classic);
}

// }}}
// {{{ Deleting what is not open is judged by what Settings last loaded

TEST_CASE("SettingsController: deleting an unopened profile keeps a hand edit made since the load",
          "[settings]")
{
    auto fx = Fixture(BasicConfig);
    writeSideProfiles(fx, { "work" });
    auto const workFile = config::relativePathOf(config::profileFile("work"));
    fx.handEdit(workFile, "show_title_bar: false\n");

    fx.controller->requestDeleteProfile("work");
    fx.fire();

    CHECK(fx.fileContent(workFile) == "show_title_bar: false\n");
    REQUIRE_FALSE(fx.errors.isEmpty());
    CHECK(fx.errors.back().contains("changed outside Settings"));
}

TEST_CASE("SettingsController: renaming an unopened scheme keeps a hand edit that does not parse",
          "[settings]")
{
    auto fx = Fixture(BasicConfig);
    writeScheme(fx, "midnight");
    // "work" draws with midnight, and the configuration keeps no copy of it to fall back on: the next test
    // covers the one name it does keep.
    writeProfileDrawnWith(fx, "work", config::SimpleColorConfig { .colorScheme = "midnight" });
    auto const schemeFile = config::relativePathOf(config::colorSchemeFile("midnight"));
    auto const workFile = config::relativePathOf(config::profileFile("work"));
    auto const workBefore = fx.fileContent(workFile);
    fx.handEdit(schemeFile, "{{{ half edited\n");

    auto const renamed = fx.controller->renameColorScheme("midnight", "nightfall");
    fx.fire();

    REQUIRE_FALSE(renamed.has_value());
    CHECK(renamed.error().kind == config::SettingsErrorKind::ChangedOnDisk);
    CHECK(fx.fileContent(schemeFile) == "{{{ half edited\n");
    CHECK_FALSE(fx.fileContent(config::relativePathOf(config::colorSchemeFile("nightfall"))).has_value());
    CHECK(fx.fileContent(workFile) == workBefore);
}

TEST_CASE("SettingsController: renaming an unopened scheme the configuration also knows keeps a hand edit "
          "that does not parse",
          "[settings]")
{
    // The configuration's own table of schemes holds no side file's palette -- a profile carries the one it
    // resolved -- except the built-in `default`, which it always has. A side file shadowing that name is the
    // one rename that has an older palette to fall back on, and that fallback wrote the new file from it and
    // removed the file holding the hand edit.
    // contour.yml's own profile draws with another scheme, so `default` is free to be renamed.
    auto fx = Fixture(std::string_view { R"(
default_profile: main
profiles:
    main:
        colors: monokai
)" });
    writeScheme(fx, "midnight");
    writeProfileDrawnWith(fx, "work", config::SimpleColorConfig { .colorScheme = "midnight" });
    // The premise: a scheme a profile draws with is not in the configuration's table.
    CHECK_FALSE(fx.cfg.colorschemes.value().contains("midnight"));
    REQUIRE(fx.cfg.colorschemes.value().contains("default"));

    REQUIRE(fx.store
                ->write(config::colorSchemeFile("default"),
                        config::emitColorSchemeYaml(vtbackend::ColorPalette {}))
                .has_value());
    writeProfileDrawnWith(fx, "dawn", config::SimpleColorConfig { .colorScheme = "default" });
    auto const schemeFile = config::relativePathOf(config::colorSchemeFile("default"));
    auto const dawnFile = config::relativePathOf(config::profileFile("dawn"));
    auto const dawnBefore = fx.fileContent(dawnFile);
    fx.handEdit(schemeFile, "{{{ half edited\n");

    auto const renamed = fx.controller->renameColorScheme("default", "nightfall");
    fx.fire();

    REQUIRE_FALSE(renamed.has_value());
    CHECK(renamed.error().kind == config::SettingsErrorKind::ChangedOnDisk);
    CHECK(fx.fileContent(schemeFile) == "{{{ half edited\n");
    CHECK_FALSE(fx.fileContent(config::relativePathOf(config::colorSchemeFile("nightfall"))).has_value());
    CHECK(fx.fileContent(dawnFile) == dawnBefore);
}

TEST_CASE("SettingsController: deleting an unopened scheme keeps a hand edit made since the load",
          "[settings]")
{
    auto fx = Fixture(BasicConfig);
    REQUIRE(fx.store
                ->write(config::colorSchemeFile("midnight"),
                        config::emitColorSchemeYaml(vtbackend::ColorPalette {}))
                .has_value());
    fx.reload();
    auto const schemeFile = config::relativePathOf(config::colorSchemeFile("midnight"));
    fx.handEdit(schemeFile, "edited: elsewhere\n");

    fx.controller->requestDeleteScheme("midnight");
    fx.fire();

    CHECK(fx.fileContent(schemeFile) == "edited: elsewhere\n");
    REQUIRE_FALSE(fx.errors.isEmpty());
    CHECK(fx.errors.back().contains("changed outside Settings"));
}

TEST_CASE("SettingsController: deleting an unopened profile or scheme removes what was loaded", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    writeSideProfiles(fx, { "work" });
    REQUIRE(fx.store
                ->write(config::colorSchemeFile("midnight"),
                        config::emitColorSchemeYaml(vtbackend::ColorPalette {}))
                .has_value());
    fx.reload();

    fx.controller->requestDeleteProfile("work");
    fx.controller->requestDeleteScheme("midnight");
    fx.fire();

    CHECK(fx.errors.isEmpty());
    CHECK_FALSE(fx.fileContent(config::relativePathOf(config::profileFile("work"))).has_value());
    CHECK_FALSE(fx.fileContent(config::relativePathOf(config::colorSchemeFile("midnight"))).has_value());
}

// }}}
// {{{ The settle in rename and Use New: a pending write for a file that is not open

namespace
{

/// Stages a scheme rename that moves the unopened profile "work": its rewrite is now pending, and no draft
/// is open to flush it.
void stageSchemeRenameMovingUnopenedProfile(Fixture& fx)
{
    writeScheme(fx, "midnight");
    writeProfileDrawnWith(fx, "work", config::SimpleColorConfig { .colorScheme = "midnight" });
    REQUIRE(fx.controller->editingProfile().isEmpty());
    REQUIRE(fx.controller->renameColorScheme("midnight", "nightfall").has_value());
    REQUIRE(fx.controller->hasChanges());
}

[[nodiscard]] std::string schemeDrawnBy(Fixture const& fx, std::string const& profile)
{
    auto const* found = fx.cfg.findProfile(profile);
    REQUIRE(found != nullptr);
    return std::get<config::SimpleColorConfig>(found->colors.value()).colorScheme;
}

} // namespace

TEST_CASE("SettingsController: renaming a profile settles what is pending for it first",
          "[settings][debounce]")
{
    auto fx = Fixture(BasicConfig);
    stageSchemeRenameMovingUnopenedProfile(fx);

    REQUIRE(fx.controller->renameProfile("work", "office").has_value());
    fx.fire();

    CHECK(fx.errors.isEmpty());
    CHECK(schemeDrawnBy(fx, "office") == "nightfall");
    CHECK(fx.schemeFile("nightfall").has_value());
    CHECK_FALSE(fx.schemeFile("midnight").has_value());
}

TEST_CASE("SettingsController: Use New settles what is pending for an unopened profile first",
          "[settings][debounce]")
{
    auto fx = Fixture(BasicConfig);
    stageSchemeRenameMovingUnopenedProfile(fx);

    REQUIRE(fx.controller
                ->writeGlobalOverride(config::DefaultsEditionKey,
                                      config::configEnumToken(config::DefaultsEdition::Native2026))
                .has_value());
    fx.fire();

    CHECK(fx.errors.isEmpty());
    CHECK(schemeDrawnBy(fx, "work") == "nightfall");
}

TEST_CASE("SettingsController: renaming a scheme settles what is pending for it first",
          "[settings][debounce]")
{
    // No profile draws with it, so nothing else flushes the pending rename of the scheme to "nightfall".
    auto fx = Fixture(BasicConfig);
    writeScheme(fx, "midnight");
    REQUIRE(fx.controller->renameColorScheme("midnight", "nightfall").has_value());
    REQUIRE(fx.controller->hasChanges());

    REQUIRE(fx.controller->renameColorScheme("nightfall", "dusk").has_value());
    fx.fire();

    CHECK(fx.errors.isEmpty());
    CHECK(fx.schemeFile("dusk").has_value());
    CHECK_FALSE(fx.schemeFile("nightfall").has_value());
    CHECK_FALSE(fx.schemeFile("midnight").has_value());
}

// }}}
// {{{ The scheme list follows what is staged

namespace
{

[[nodiscard]] QStringList schemeNamesOf(Fixture const& fx)
{
    auto names = QStringList {};
    for (auto const& row: fx.controller->colorSchemes())
        names.push_back(row.toMap().value("name").toString());
    return names;
}

} // namespace

TEST_CASE("SettingsController: the scheme list follows a staged rename and delete", "[settings][schemes]")
{
    auto fx = Fixture(BasicConfig);
    writeScheme(fx, "midnight");
    writeScheme(fx, "dawn");
    REQUIRE(schemeNamesOf(fx).contains("midnight"));

    REQUIRE(fx.controller->renameColorScheme("midnight", "nightfall").has_value());
    // Not yet written: the old name is already gone from the list, the new one already in it.
    CHECK_FALSE(schemeNamesOf(fx).contains("midnight"));
    CHECK(schemeNamesOf(fx).contains("nightfall"));

    REQUIRE(fx.controller->deleteColorScheme("dawn").has_value());
    CHECK_FALSE(schemeNamesOf(fx).contains("dawn"));

    fx.fire();
    CHECK_FALSE(schemeNamesOf(fx).contains("midnight"));
    CHECK(schemeNamesOf(fx).contains("nightfall"));
    CHECK_FALSE(schemeNamesOf(fx).contains("dawn"));
}

// }}}
// {{{ A link pattern in error is held back on every write path

namespace
{

/// A name no documentation comment of the emitted YAML mentions.
constexpr auto HeldBackName = std::string_view { "unfinished-pattern" };

/// Opens side-file profile "work" with a link pattern that is named but has no regex yet: its row says
/// why, and no write may carry it.
void openWorkWithLinkPatternInError(Fixture& fx)
{
    fx.openSideProfile("work");
    auto const index = builtinRowCount();
    fx.controller->addLinkPattern();
    auto edited = linkRow(*fx.controller, index);
    edited["name"] = QString::fromUtf8(HeldBackName.data(), static_cast<qsizetype>(HeldBackName.size()));
    fx.controller->setLinkPattern(index, edited);
    REQUIRE_FALSE(linkRow(*fx.controller, index).value("problem").toString().isEmpty());
}

} // namespace

TEST_CASE("SettingsController: renaming a profile does not write its link pattern in error",
          "[settings][links]")
{
    auto fx = Fixture(BasicConfig);
    openWorkWithLinkPatternInError(fx);

    REQUIRE(fx.controller->renameProfile("work", "office").has_value());
    fx.fire();

    REQUIRE(fx.cfg.findProfile("office") != nullptr);
    // The file itself, not what the reader makes of it: the reader skips a pattern without a regex.
    auto const written = fx.fileContent(config::relativePathOf(config::profileFile("office")));
    REQUIRE(written.has_value());
    CHECK_FALSE(written->contains(HeldBackName));
    // And it is still being edited.
    CHECK(linkRow(*fx.controller, builtinRowCount()).value("name").toString().toStdString() == HeldBackName);
}

TEST_CASE("SettingsController: duplicating a profile does not write its link pattern in error",
          "[settings][links]")
{
    auto fx = Fixture(BasicConfig);
    openWorkWithLinkPatternInError(fx);

    auto const copy = fx.controller->duplicateProfile("work");
    REQUIRE(copy.has_value());
    fx.fire();

    auto const written = fx.fileContent(config::relativePathOf(config::profileFile(copy->toStdString())));
    REQUIRE(written.has_value());
    CHECK_FALSE(written->contains(HeldBackName));
}

TEST_CASE("SettingsController: Use New does not write the open profile's link pattern in error",
          "[settings][links]")
{
    auto fx = Fixture(BasicConfig);
    openWorkWithLinkPatternInError(fx);
    // The indicator spells out the old edition's default, so the switch rewrites the profile.
    auto const before = fx.cfg.defaultsEdition;
    REQUIRE(before == config::DefaultsEdition::Classic);

    REQUIRE(fx.controller
                ->writeGlobalOverride(config::DefaultsEditionKey,
                                      config::configEnumToken(config::DefaultsEdition::Native2026))
                .has_value());
    fx.fire();

    auto const written = fx.fileContent(config::relativePathOf(config::profileFile("work")));
    REQUIRE(written.has_value());
    CHECK_FALSE(written->contains(HeldBackName));
}

// {{{ Shell Integration: the Install rows
namespace
{
using config::RcState;
using config::ShellKind;
using platform::RcInspection;
using platform::ShellRcError;

/// @return The row of @p shell in @p fx's shellIntegrationRows.
[[nodiscard]] QVariantMap shellRowOf(Fixture const& fx, ShellKind shell)
{
    for (auto const& row: fx.controller->shellIntegrationRows())
        if (row.toMap().value(QStringLiteral("shell")).toInt() == std::to_underlying(shell))
            return row.toMap();
    return {};
}

/// Scripts the editor as the page's brief has it: zsh installed, bash absent, fish unreadable.
void scriptShells(Fixture& fx)
{
    fx.shellEditor.inspections[ShellKind::Zsh] =
        RcInspection { .file = home() / ".zshrc", .state = RcState::Installed };
    fx.shellEditor.inspections[ShellKind::Bash] =
        RcInspection { .file = home() / ".bashrc", .state = RcState::Absent };
    fx.shellEditor.inspections[ShellKind::Fish] = std::unexpected(ShellRcError::Unreadable);
    fx.controller->refresh();
}
} // namespace

TEST_CASE("SettingsController: the Install rows list every shell with what its startup file is",
          "[settings][shellintegration]")
{
    auto fx = Fixture(BasicConfig);
    scriptShells(fx);

    auto const rows = fx.controller->shellIntegrationRows();
    REQUIRE(static_cast<std::size_t>(rows.size()) == config::ShellRows.size());
    // In ShellRows order.
    for (auto const index: std::views::iota(std::size_t { 0 }, config::ShellRows.size()))
        CHECK(rows[static_cast<qsizetype>(index)].toMap().value(QStringLiteral("shell")).toInt()
              == std::to_underlying(config::ShellRows[index].kind));

    auto const zsh = shellRowOf(fx, ShellKind::Zsh);
    CHECK(zsh.value(QStringLiteral("name")).toString() == QStringLiteral("zsh"));
    CHECK(zsh.value(QStringLiteral("status")).toString() == QStringLiteral("Installed in ~/.zshrc"));
    CHECK_FALSE(zsh.value(QStringLiteral("canInstall")).toBool());

    auto const bash = shellRowOf(fx, ShellKind::Bash);
    CHECK(bash.value(QStringLiteral("status")).toString() == QStringLiteral("Not installed"));
    CHECK(bash.value(QStringLiteral("canInstall")).toBool());

    auto const fish = shellRowOf(fx, ShellKind::Fish);
    CHECK(fish.value(QStringLiteral("status")).toString()
          == QStringLiteral("Unknown: the file could not be read"));
    CHECK_FALSE(fish.value(QStringLiteral("canInstall")).toBool());
}

TEST_CASE("SettingsController: a cut-short install shows the line to delete and offers no install",
          "[settings][shellintegration]")
{
    auto fx = Fixture(BasicConfig);
    auto const cut = std::string { R"(if [ "$TERM_PROGRAM" = contour ]; then eval "$('/opt/c)" };
    fx.shellEditor.inspections[ShellKind::Zsh] =
        RcInspection { .file = home() / ".zshrc", .state = RcState::Partial, .partialLine = cut };
    fx.controller->refresh();

    auto const zsh = shellRowOf(fx, ShellKind::Zsh);
    CHECK(zsh.value(QStringLiteral("status")).toString().contains(QStringLiteral("~/.zshrc")));
    CHECK(zsh.value(QStringLiteral("deleteLine")).toString() == QString::fromStdString(cut));
    CHECK_FALSE(zsh.value(QStringLiteral("canInstall")).toBool());
    // Nothing else has a line to delete.
    CHECK(shellRowOf(fx, ShellKind::Bash).value(QStringLiteral("deleteLine")).toString().isEmpty());
}

TEST_CASE("SettingsController: the Install row names the line and the file it would add to",
          "[settings][shellintegration]")
{
    auto fx = Fixture(BasicConfig);
    scriptShells(fx);

    CHECK(fx.controller->shellIntegrationLine(std::to_underlying(ShellKind::Bash)).toStdString()
          == window::shellRcLine(ShellKind::Bash, contourBinary()).value());
    CHECK(fx.controller->shellIntegrationRcPath(std::to_underlying(ShellKind::Bash))
          == QStringLiteral("~/.bashrc"));
    // No line or file for what is no shell, or a shell whose file cannot be named.
    CHECK(fx.controller->shellIntegrationLine(std::to_underlying(ShellKind::Unknown)).isEmpty());
    CHECK(fx.controller->shellIntegrationRcPath(std::to_underlying(ShellKind::Fish)).isEmpty());
}

TEST_CASE("SettingsController: the confirmation says what the card's Confirm stage says",
          "[settings][shellintegration]")
{
    auto fx = Fixture(BasicConfig);
    scriptShells(fx);

    auto const confirm = fx.controller->shellIntegrationConfirm(std::to_underlying(ShellKind::Bash));
    CHECK(confirm.value(QStringLiteral("title")).toString() == QStringLiteral("Add this line to ~/.bashrc?"));
    CHECK(confirm.value(QStringLiteral("line")).toString().toStdString()
          == window::shellRcLine(ShellKind::Bash, contourBinary()).value());
    CHECK(confirm.value(QStringLiteral("primary")).toString() == QStringLiteral("Add Line"));
    CHECK(confirm.value(QStringLiteral("secondary")).toString() == QStringLiteral("Cancel"));
    CHECK(fx.controller->shellIntegrationConfirm(std::to_underlying(ShellKind::Unknown)).isEmpty());
}

TEST_CASE("SettingsController: installing from Settings appends the line through the editor and refreshes",
          "[settings][shellintegration]")
{
    auto fx = Fixture(BasicConfig);
    scriptShells(fx);
    auto const bash = std::to_underlying(ShellKind::Bash);

    fx.controller->installShellIntegration(bash);
    REQUIRE(fx.shellEditor.installs.size() == 1);
    CHECK(fx.shellEditor.installs.front().shell == ShellKind::Bash);
    CHECK(fx.shellEditor.installs.front().line
          == window::shellRcLine(ShellKind::Bash, contourBinary()).value());
    CHECK(fx.errors.isEmpty());

    // Once the editor reports the file changed, the row says so.
    CHECK(shellRowOf(fx, ShellKind::Bash).value(QStringLiteral("status")).toString()
          == QStringLiteral("Not installed"));
    fx.shellEditor.inspections[ShellKind::Bash] =
        RcInspection { .file = home() / ".bashrc", .state = RcState::Installed };
    fx.controller->installShellIntegration(bash);
    CHECK(shellRowOf(fx, ShellKind::Bash).value(QStringLiteral("status")).toString()
          == QStringLiteral("Installed in ~/.bashrc"));
}

TEST_CASE("SettingsController: an install the editor refuses is reported with the same words as the toast",
          "[settings][shellintegration]")
{
    for (auto const error: { ShellRcError::Superuser, ShellRcError::Changed, ShellRcError::PartialWrite })
    {
        CAPTURE(std::to_underlying(error));
        auto fx = Fixture(BasicConfig);
        scriptShells(fx);
        fx.shellEditor.installResult = std::unexpected(error);

        fx.controller->installShellIntegration(std::to_underlying(ShellKind::Bash));
        REQUIRE(fx.errors.size() == 1);
        CHECK(fx.errors.front().toStdString() == window::shellIntegrationFailedText("~/.bashrc", error));
    }
}

TEST_CASE("SettingsController: an install of no shell writes nothing", "[settings][shellintegration]")
{
    auto fx = Fixture(BasicConfig);
    scriptShells(fx);
    fx.controller->installShellIntegration(std::to_underlying(ShellKind::Unknown));
    fx.controller->installShellIntegration(99);
    CHECK(fx.shellEditor.installs.empty());
}
// }}}

TEST_CASE("SettingsController: the Install rows read the default profile's startup-file environment",
          "[settings][shellintegration]")
{
    auto fx = Fixture(BasicConfig);
    auto* const profile = fx.cfg.findProfile("main");
    REQUIRE(profile != nullptr);
    profile->shell.value().env["ZDOTDIR"] = "/zdot";
    profile->shell.value().removedEnvironment.emplace_back("XDG_CONFIG_HOME");
    auto const expected = platform::RcEnvironment { { "ZDOTDIR", "/zdot" }, { "XDG_CONFIG_HOME", "" } };

    // What the pane's card asks the editor with, the page asks it with too: both name the same file.
    fx.shellEditor.inspectedEnvironments.clear();
    fx.controller->refresh();
    REQUIRE_FALSE(fx.shellEditor.inspectedEnvironments.empty());
    for (auto const& environment: fx.shellEditor.inspectedEnvironments)
        CHECK(environment == expected);

    fx.shellEditor.inspections[ShellKind::Bash] =
        RcInspection { .file = home() / ".bashrc", .state = RcState::Absent };
    fx.controller->installShellIntegration(std::to_underlying(ShellKind::Bash));
    REQUIRE(fx.shellEditor.installs.size() == 1);
    CHECK(fx.shellEditor.installs.front().sessionEnvironment == expected);
}

TEST_CASE("SettingsController: Install is offered only where the card would offer it",
          "[settings][shellintegration]")
{
    struct Case
    {
        config::HostPlatform host;
        BinaryReach reach;
        InstallBlock block;
    };
    auto const cases = std::array {
        Case { config::HostPlatform::Other, BinaryReach::Reachable, InstallBlock::None },
        Case { config::HostPlatform::Windows, BinaryReach::Reachable, InstallBlock::WindowsHost },
        Case { config::HostPlatform::Other, BinaryReach::Unreachable, InstallBlock::BinaryUnreachable },
    };
    for (auto const& c: cases)
    {
        auto fx = Fixture(BasicConfig);
        fx.shellHost = c.host;
        fx.binaryReach = c.reach;
        fx.restartController(); // the host is read when the controller is built
        scriptShells(fx);

        auto const bash = shellRowOf(fx, ShellKind::Bash);
        auto const offered = c.block == InstallBlock::None;
        CHECK(bash.value(QStringLiteral("canInstall")).toBool() == offered);
        if (!offered)
            CHECK(bash.value(QStringLiteral("status")).toString()
                  == QString::fromUtf8(
                      InstallBlockRows[std::to_underlying(c.block)].text.data(),
                      static_cast<qsizetype>(InstallBlockRows[std::to_underlying(c.block)].text.size())));
        // The same facts through offerFor(): never a Card where the page offers no Install.
        auto const card = offerFor({ .shell = ShellKind::Bash,
                                     .locality = RcLocality::ThisMachine,
                                     .host = c.host,
                                     .binary = c.reach,
                                     .dismissal = OfferDismissal::Open,
                                     .rc = RcState::Absent,
                                     .slot = CardSlot::Free });
        CHECK((card == ShellIntegrationOffer::Card) == offered);

        // An install the page does not offer is refused too, writing nothing.
        fx.controller->installShellIntegration(std::to_underlying(ShellKind::Bash));
        CHECK(fx.shellEditor.installs.size() == (offered ? 1U : 0U));
        // A file that is already installed keeps saying so, whatever the host.
        CHECK(shellRowOf(fx, ShellKind::Zsh).value(QStringLiteral("status")).toString()
              == QStringLiteral("Installed in ~/.zshrc"));
    }
}

TEST_CASE("SettingsController: setHotkey stages the chord as contour.yml spells it",
          "[settings][quickterminal]")
{
    auto fx = Fixture("default_profile: main\n");
    CHECK(fx.controller->setHotkey(Qt::Key_QuoteLeft, Qt::ControlModifier).isEmpty());
    fx.fire();

    // What is on disk is the nested override, a mapping, as contour.yml spells a hotkey.
    auto const settings = fx.fileContent("settings.yml");
    REQUIRE(settings.has_value());
    auto const document = YAML::Load(*settings);
    REQUIRE(document["quick_terminal"]["hotkey"].IsMap());
    CHECK(document["quick_terminal"]["hotkey"]["mods"][0].as<std::string>() == "Control");
    CHECK(document["quick_terminal"]["hotkey"]["key"].as<std::string>() == "`");

    // And it is what the loader reads back, and the row shows.
    CHECK(
        fx.cfg.quickTerminal.value().hotkey
        == std::optional { config::HotkeyChord { .modifiers = vtbackend::Modifier::Control, .key = U'`' } });
    auto const row = rowWithKey(fx.controller->globalFields(), "quick_terminal.hotkey");
    CHECK(row.value("controlName").toString() == "shortcut");
    CHECK(row.value("value").toString().contains('`'));
    CHECK(row.value("overridden").toBool());
    CHECK(fx.errors.isEmpty());
}

TEST_CASE("SettingsController: a pager that looks like a mapping is written as the string it is",
          "[settings][quickterminal]")
{
    auto fx = Fixture("default_profile: main\n");
    fx.commitGlobal("command_blocks.pager", "{ less -R; }");
    CHECK(fx.cfg.commandBlocks.value().pager == "{ less -R; }");
    fx.fire();
    auto const settings = fx.fileContent("settings.yml");
    REQUIRE(settings.has_value());
    auto const document = YAML::Load(*settings);
    REQUIRE(document["command_blocks"]["pager"].IsScalar());
    CHECK(document["command_blocks"]["pager"].as<std::string>() == "{ less -R; }");

    // Beside the hotkey, which is written as a mapping, the pager stays a string through the next save.
    CHECK(fx.controller->setHotkey(Qt::Key_QuoteLeft, Qt::ControlModifier).isEmpty());
    fx.fire();
    auto const both = YAML::Load(*fx.fileContent("settings.yml"));
    CHECK(both["quick_terminal"]["hotkey"].IsMap());
    REQUIRE(both["command_blocks"]["pager"].IsScalar());
    CHECK(both["command_blocks"]["pager"].as<std::string>() == "{ less -R; }");

    // A cleared hotkey leaves nothing of its section behind.
    fx.controller->clearHotkey();
    fx.fire();
    CHECK_FALSE(YAML::Load(*fx.fileContent("settings.yml"))["quick_terminal"].IsDefined());
}

TEST_CASE("SettingsController: setHotkey refuses what cannot be a hotkey, and stages nothing",
          "[settings][quickterminal]")
{
    auto fx = Fixture("default_profile: main\n");
    fx.menuBar = command::MenuBarPlacement::Global;
    fx.restartController();

    // No modifier held that a typing key would not have.
    auto const noModifier = fx.controller->setHotkey(Qt::Key_F12, Qt::NoModifier);
    CHECK(noModifier.toStdString()
          == std::string { hotkeyRefusalText(config::HotkeyChordError::NoModifier) });
    CHECK(fx.controller->setHotkey(Qt::Key_F12, Qt::ShiftModifier) == noModifier);

    // A key the table has no row for.
    CHECK(fx.controller->setHotkey(Qt::Key_Backspace, Qt::ControlModifier).toStdString()
          == std::string { hotkeyRefusalText(platform::HotkeyError::UnsupportedKey) });

    // A chord the menu bar keeps (Hide, spelled as Super+H on every host).
    CHECK(fx.controller->setHotkey(Qt::Key_H, Qt::MetaModifier).toStdString()
          == std::string { hotkeyRefusalText(platform::HotkeyError::MenuOwned) });

    // The refusal shows under the row, and nothing was written.
    auto const row = rowWithKey(fx.controller->globalFields(), "quick_terminal.hotkey");
    CHECK(row.value("problem").toString().toStdString()
          == std::string { hotkeyRefusalText(platform::HotkeyError::MenuOwned) });
    CHECK_FALSE(fx.scheduler->pendingDelay().has_value());
    CHECK_FALSE(fx.fileContent("settings.yml").has_value());

    // Off a global menu bar the same chord is fine.
    fx.menuBar = command::MenuBarPlacement::None;
    fx.restartController();
    CHECK(fx.controller->setHotkey(Qt::Key_H, Qt::MetaModifier).isEmpty());
}

TEST_CASE("SettingsController: clearHotkey drops the override", "[settings][quickterminal]")
{
    auto fx = Fixture("default_profile: main\n");
    CHECK(fx.controller->setHotkey(Qt::Key_QuoteLeft, Qt::ControlModifier).isEmpty());
    fx.fire();
    REQUIRE(fx.cfg.quickTerminal.value().hotkey.has_value());

    fx.controller->clearHotkey();
    fx.fire();
    CHECK_FALSE(fx.cfg.quickTerminal.value().hotkey.has_value());
    auto const row = rowWithKey(fx.controller->globalFields(), "quick_terminal.hotkey");
    CHECK(row.value("value").toString().isEmpty());
    CHECK_FALSE(row.value("overridden").toBool());
}

TEST_CASE("SettingsController: the recorder's chord goes through the page's own commit",
          "[settings][quickterminal]")
{
    auto fx = Fixture("default_profile: main\n");
    fx.controller->commitGlobalField("quick_terminal.hotkey",
                                     QVariantMap { { "key", static_cast<int>(Qt::Key_Space) },
                                                   { "modifiers", static_cast<int>(Qt::AltModifier) } });
    fx.fire();
    CHECK(fx.cfg.quickTerminal.value().hotkey
          == std::optional { config::HotkeyChord { .modifiers = vtbackend::Modifier::Alt, .key = U' ' } });
}

TEST_CASE("SettingsController: the focus-loss switch is a bool row, and the operating system's refusal shows",
          "[settings][quickterminal]")
{
    auto fx = Fixture("default_profile: main\n");
    auto const row = rowWithKey(fx.controller->globalFields(), "quick_terminal.hide_on_focus_loss");
    REQUIRE_FALSE(row.isEmpty());
    CHECK(row.value("controlName").toString() == "switch");
    CHECK(row.value("value").toBool());

    fx.commitGlobal("quick_terminal.hide_on_focus_loss", false);
    CHECK(fx.cfg.quickTerminal.value().focusLoss == config::QuickTerminalFocusLoss::Stay);

    // The application reports the desktop's refusal, and the row shows it until the next edit.
    fx.controller->setHotkeyProblem("Another application already uses that shortcut.");
    CHECK(rowWithKey(fx.controller->globalFields(), "quick_terminal.hotkey").value("problem").toString()
          == "Another application already uses that shortcut.");
    CHECK(fx.controller->setHotkey(Qt::Key_QuoteLeft, Qt::ControlModifier).isEmpty());
    CHECK(rowWithKey(fx.controller->globalFields(), "quick_terminal.hotkey")
              .value("problem")
              .toString()
              .isEmpty());
}

TEST_CASE("SettingsController: General's Quick Terminal group holds the hotkey and the focus-loss switch",
          "[settings][quickterminal][settingssections]")
{
    auto keys = std::vector<std::string> {};
    for (auto const& descriptor: fields::globalFieldDescriptors())
        if (descriptor.group == SettingsGroup::GeneralQuickTerminal)
            keys.push_back(descriptor.key.toStdString());
    CHECK(keys == std::vector<std::string> { "quick_terminal.hotkey", "quick_terminal.hide_on_focus_loss" });
    CHECK(settingsGroupRow(SettingsGroup::GeneralQuickTerminal).section == SettingsSection::General);
}

TEST_CASE("SettingsController: a profile's program, arguments and working directory are written and reload",
          "[settings][shell]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->addProfile();
    fx.scheduler->fire();
    REQUIRE_FALSE(fx.controller->editingReadOnly());

    for (auto const* key: { "shell", "arguments", "initial_working_directory" })
    {
        INFO(key);
        auto const row = rowWithKey(fx.controller->profileFields(), key);
        REQUIRE_FALSE(row.isEmpty());
        CHECK(row.value("group").toInt() == static_cast<int>(SettingsGroup::ProfileShell));
        CHECK(row.value("groupTitle").toString() == "Shell");
    }
    CHECK(rowWithKey(fx.controller->profileFields(), "initial_working_directory").value("value").toString()
          == "~");

    fx.controller->commitProfileField("shell", "/bin/zsh");
    fx.controller->commitProfileField("arguments", "-l -c \"echo hi there\"");
    fx.controller->commitProfileField("initial_working_directory", "/tmp");
    fx.scheduler->fire();

    auto const written = fx.fileContent("profiles/New Profile.yml");
    REQUIRE(written.has_value());
    CHECK(written->contains("shell: /bin/zsh"));

    auto const* profile = fx.cfg.findProfile("New Profile");
    REQUIRE(profile != nullptr);
    auto const& shell = profile->shell.value();
    CHECK(shell.program == "/bin/zsh");
    CHECK(shell.arguments == std::vector<std::string> { "-l", "-c", "echo hi there" });
    CHECK(shell.workingDirectory == std::filesystem::path { "/tmp" });

    auto const& fields = fx.controller->profileFields();
    CHECK(rowWithKey(fields, "shell").value("value").toString() == "/bin/zsh");
    CHECK(rowWithKey(fields, "arguments").value("value").toString() == "-l -c \"echo hi there\"");
    CHECK(rowWithKey(fields, "initial_working_directory").value("value").toString() == "/tmp");
}

TEST_CASE("SettingsController: search finds the shell rows", "[settings][shell]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->editProfile("main");

    auto const found = fx.controller->searchSettings("working directory");
    REQUIRE_FALSE(found.isEmpty());
    auto const first = found.first().toMap();
    CHECK(first.value("key").toString() == "initial_working_directory");
    CHECK(first.value("group").toInt() == static_cast<int>(SettingsGroup::ProfileShell));
}

// {{{ Hosts

namespace
{

constexpr auto HostsConfig = std::string_view { R"(
default_profile: main
profiles:
    main:
        show_title_bar: true
    remote:
        show_title_bar: false
hosts:
    home:
        url: "ssh://homepc"
)" };

/// The host side file @p name, as written; nullopt when there is none.
[[nodiscard]] std::optional<config::HostDraft> hostFileOf(Fixture const& fx, std::string const& name)
{
    return fx.fileContent(config::relativePathOf(config::hostFile(name)))
        .and_then(config::parseHostDraftYaml);
}

/// The keys of @p rows, in order.
[[nodiscard]] QStringList keysOf(QVariantList const& rows)
{
    auto keys = QStringList {};
    for (auto const& row: rows)
        keys.push_back(row.toMap().value("key").toString());
    return keys;
}

} // namespace

TEST_CASE("SettingsController: hosts list with provenance and transport", "[settings][hosts]")
{
    auto fx = Fixture(HostsConfig);
    fx.openSideHost("build-box", "url: tls://10.0.0.7:7297\ntoken_file: /tmp/build-box.token\n");

    auto const home = rowNamed(fx.controller->hosts(), "home");
    CHECK(home.value("origin").toString() == "main");
    CHECK_FALSE(home.value("editable").toBool());
    CHECK(home.value("transport").toString() == "SSH");
    CHECK(home.value("actions").toMap().value("duplicate").toBool());
    CHECK_FALSE(home.value("actions").toMap().value("rename").toBool());

    auto const box = rowNamed(fx.controller->hosts(), "build-box");
    CHECK(box.value("origin").toString() == "side");
    CHECK(box.value("editable").toBool());
    CHECK(box.value("transport").toString() == "TLS");
    CHECK(box.value("actions").toMap().value("attach").toBool());
}

TEST_CASE("SettingsController: the sidebar lists the hosts beneath the Hosts section", "[settings][hosts]")
{
    auto fx = Fixture(HostsConfig);
    auto const sidebar = fx.controller->sidebar();
    auto const hostsAt = std::ranges::find_if(
        sidebar, [](QVariant const& row) { return row.toMap().value("id").toString() == "hosts"; });
    REQUIRE(hostsAt != sidebar.end());
    REQUIRE(std::next(hostsAt) != sidebar.end());
    auto const home = std::next(hostsAt)->toMap();
    CHECK(home.value("kind").toString() == "host");
    CHECK(home.value("name").toString() == "home");
    CHECK(home.value("transport").toString() == "SSH");
}

TEST_CASE("SettingsController: a host's rows follow its address", "[settings][hosts]")
{
    auto fx = Fixture(HostsConfig);
    fx.openSideHost("starfive", "url: ssh://me@starfive\nprofile: remote\n");

    CHECK(keysOf(fx.controller->hostFields())
          == QStringList { "url", "transport", "remote_command", "profile" });
    auto const command = rowWithKey(fx.controller->hostFields(), "remote_command");
    CHECK(command.value("placeholder").toString() == "contour daemon --stdio");
    CHECK(command.value("value").toString().isEmpty());
    auto const profile = rowWithKey(fx.controller->hostFields(), "profile");
    CHECK(profile.value("value").toString() == "remote");
    CHECK(profile.value("controlName").toString() == "popUp");
    CHECK(profile.value("options").toStringList().contains(QString {}));
    CHECK(profile.value("optionLabels").toStringList().front() == "Default");

    // Typed over to a tls:// address, the SSH group gives way to TLS, and the token is required.
    REQUIRE(fx.controller->setHostField("url", "tls://starfive:7297").has_value());
    CHECK(keysOf(fx.controller->hostFields())
          == QStringList { "url", "transport", "token_file", "tls_ca", "profile" });
    CHECK(rowWithKey(fx.controller->hostFields(), "transport").value("value").toString() == "TLS");
    CHECK(rowWithKey(fx.controller->hostFields(), "token_file").value("problem").toString().toStdString()
          == config::describe(config::HostEntryError::TokenRequired));
    CHECK(rowWithKey(fx.controller->hostFields(), "tls_ca").value("placeholder").toString()
          == "System trust");
    CHECK_FALSE(fx.controller->hostAttachable());

    // An address that names no transport keeps the last one's rows, and says what is wrong under it.
    REQUIRE(fx.controller->setHostField("url", "starfive:22").has_value());
    CHECK(keysOf(fx.controller->hostFields()).contains("token_file"));
    CHECK(rowWithKey(fx.controller->hostFields(), "url").value("problem").toString().toStdString()
          == config::describe(config::HostUrlError::MissingScheme));
}

TEST_CASE("SettingsController: an invalid host page saves nothing", "[settings][hosts]")
{
    auto fx = Fixture(HostsConfig);
    fx.openSideHost("starfive", "url: ssh://me@starfive\n");

    REQUIRE(fx.controller->setHostField("url", "starfive:22").has_value());
    fx.fire();
    CHECK(hostFileOf(fx, "starfive")->url == "ssh://me@starfive");

    // A tls:// host without its token is no host the loader takes either.
    REQUIRE(fx.controller->setHostField("url", "tls://starfive:7297").has_value());
    fx.fire();
    CHECK(hostFileOf(fx, "starfive")->url == "ssh://me@starfive");
    CHECK(fx.cfg.hosts.value().at("starfive").url.scheme == config::HostScheme::Ssh);

    // With the token, it is.
    REQUIRE(fx.controller->setHostField("token_file", "~/starfive.token").has_value());
    fx.fire();
    CHECK(hostFileOf(fx, "starfive")
          == config::HostDraft { .url = "tls://starfive:7297", .tokenFile = "~/starfive.token" });
    CHECK(fx.cfg.hosts.value().at("starfive").url.scheme == config::HostScheme::Tls);
    CHECK(fx.controller->hostAttachable());
}

TEST_CASE("SettingsController: a new host is written once its address is usable", "[settings][hosts]")
{
    auto fx = Fixture(BasicConfig);
    REQUIRE(fx.controller->hosts().isEmpty());

    auto const created = fx.controller->createHost();
    REQUIRE(created.has_value());
    CHECK(*created == "New Host");
    CHECK(fx.controller->editingHost() == "New Host");
    CHECK(rowNamed(fx.controller->hosts(), "New Host").value("editable").toBool());
    fx.fire();
    CHECK_FALSE(hostFileOf(fx, "New Host").has_value());

    REQUIRE(fx.controller->setHostField("url", "ssh://me@starfive").has_value());
    fx.fire();
    CHECK(hostFileOf(fx, "New Host") == config::HostDraft { .url = "ssh://me@starfive" });
    CHECK(fx.cfg.hosts.value().contains("New Host"));
    CHECK(fx.cfg.hostOrigins.at("New Host") == config::SettingsOrigin::SideFile);

    // Revert takes it away again.
    fx.controller->revertChanges();
    CHECK_FALSE(hostFileOf(fx, "New Host").has_value());
}

TEST_CASE("SettingsController: a new host left before it was usable is dropped", "[settings][hosts]")
{
    auto fx = Fixture(HostsConfig);
    REQUIRE(fx.controller->createHost().has_value());
    fx.controller->editHost("home");
    CHECK(rowNamed(fx.controller->hosts(), "New Host").isEmpty());
}

TEST_CASE("SettingsController: a contour.yml host is read-only, and its duplicate is not",
          "[settings][hosts]")
{
    auto fx = Fixture(HostsConfig);
    fx.controller->editHost("home");
    CHECK(fx.controller->editingHostReadOnly());
    auto const refused = fx.controller->setHostField("url", "ssh://elsewhere");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == config::SettingsErrorKind::ReadOnly);
    CHECK_FALSE(fx.controller->deleteHost("home").has_value());
    CHECK_FALSE(fx.controller->renameHost("home", "house").has_value());

    auto const copy = fx.controller->duplicateHost("home");
    REQUIRE(copy.has_value());
    CHECK(*copy == "home copy");
    CHECK_FALSE(fx.controller->editingHostReadOnly());
    fx.fire();
    CHECK(hostFileOf(fx, "home copy") == config::HostDraft { .url = "ssh://homepc" });
    CHECK(fx.fileContent("contour.yml")->contains("ssh://homepc"));
}

TEST_CASE("SettingsController: rename moves a host's side file; a case-only rename is refused",
          "[settings][hosts]")
{
    auto fx = Fixture(HostsConfig);
    fx.openSideHost("starfive", "url: ssh://me@starfive\n");

    auto const caseOnly = fx.controller->renameHost("starfive", "StarFive");
    REQUIRE_FALSE(caseOnly.has_value());
    CHECK(caseOnly.error().kind == config::SettingsErrorKind::NameTaken);
    auto const taken = fx.controller->renameHost("starfive", "HOME");
    REQUIRE_FALSE(taken.has_value());
    CHECK(taken.error().kind == config::SettingsErrorKind::NameTaken);

    auto renamed = std::vector<std::pair<QString, QString>> {};
    QObject::connect(fx.controller.get(),
                     &SettingsController::hostRenamed,
                     [&](QString const& from, QString const& to) { renamed.emplace_back(from, to); });
    REQUIRE(fx.controller->renameHost("starfive", "sf").has_value());
    CHECK(renamed == std::vector<std::pair<QString, QString>> { { "starfive", "sf" } });
    CHECK(fx.controller->editingHost() == "sf");
    fx.fire();
    CHECK_FALSE(hostFileOf(fx, "starfive").has_value());
    CHECK(hostFileOf(fx, "sf") == config::HostDraft { .url = "ssh://me@starfive" });
    CHECK(fx.cfg.hosts.value().contains("sf"));
    CHECK_FALSE(fx.cfg.hosts.value().contains("starfive"));
}

TEST_CASE("SettingsController: delete removes a host's side file", "[settings][hosts]")
{
    auto fx = Fixture(HostsConfig);
    fx.openSideHost("starfive", "url: ssh://me@starfive\n");
    REQUIRE(fx.controller->deleteHost("starfive").has_value());
    CHECK(fx.controller->editingHost().isEmpty());
    CHECK(rowNamed(fx.controller->hosts(), "starfive").isEmpty());
    fx.fire();
    CHECK_FALSE(hostFileOf(fx, "starfive").has_value());
    CHECK_FALSE(fx.cfg.hosts.value().contains("starfive"));
}

TEST_CASE("SettingsController: Attach runs AttachToHost for the host, after what is staged is written",
          "[settings][hosts]")
{
    auto fx = Fixture(HostsConfig);
    fx.openSideHost("starfive", "url: ssh://me@starfive\n");
    REQUIRE(fx.controller->setHostField("remote_command", "/opt/contour daemon --stdio").has_value());

    REQUIRE(fx.controller->attachToHost("starfive").has_value());
    REQUIRE(fx.ranActions.size() == 1);
    auto const* attach = std::get_if<actions::AttachToHost>(&fx.ranActions.front());
    REQUIRE(attach != nullptr);
    CHECK(attach->name == "starfive");
    // The window reads the host from the configuration, which already has the edit.
    CHECK(fx.cfg.hosts.value().at("starfive").remoteCommand == "/opt/contour daemon --stdio");

    // Not while the page shows a problem.
    REQUIRE(fx.controller->setHostField("url", "starfive").has_value());
    auto const refused = fx.controller->attachToHost("starfive");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == config::SettingsErrorKind::InvalidValue);
    CHECK(fx.ranActions.size() == 1);
}

TEST_CASE("SettingsController: Choose… commits the file chosen, and a cancel nothing", "[settings][hosts]")
{
    auto fx = Fixture(HostsConfig);
    fx.openSideHost("build-box", "url: tls://10.0.0.7:7297\ntoken_file: /tmp/old.token\n");

    fx.controller->chooseHostFile("token_file");
    CHECK(fx.fileDialogTitles == std::vector<std::string> { "Token file" });
    CHECK(rowWithKey(fx.controller->hostFields(), "token_file").value("value").toString()
          == "/tmp/old.token");

    fx.chosenFile = std::filesystem::path { "/tmp/new.token" };
    fx.controller->chooseHostFile("token_file");
    CHECK(rowWithKey(fx.controller->hostFields(), "token_file").value("value").toString()
          == "/tmp/new.token");
    CHECK(rowWithKey(fx.controller->hostFields(), "token_file").value("canChoose").toBool());
}

TEST_CASE("SettingsController: search finds a host's rows", "[settings][hosts]")
{
    auto fx = Fixture(HostsConfig);
    for (auto const* query: { "token", "remote command", "Address" })
    {
        INFO(query);
        auto const found = fx.controller->searchSettings(QString::fromUtf8(query));
        REQUIRE_FALSE(found.isEmpty());
        CHECK(found.front().toMap().value("section").toString() == "hosts");
        CHECK(found.front().toMap().value("scope").toString() == "host");
    }
}

TEST_CASE("SettingsController: hosts are read-only under gui_config_locked", "[settings][hosts]")
{
    auto fx = Fixture(std::string { HostsConfig } + "gui_config_locked: true\n");
    CHECK_FALSE(fx.controller->createHost().has_value());
    CHECK_FALSE(
        rowNamed(fx.controller->hosts(), "home").value("actions").toMap().value("duplicate").toBool());
}

// }}}
