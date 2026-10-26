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
#include <contour/display/GpuInventory.hpp>
#include <contour/window/SettingsController.hpp>

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

#include <yaml-cpp/yaml.h>

#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
/// at the config directory, and an apply callback that reloads the Config from disk. The whole
/// create/edit/save/delete workflow runs against real files under a temp directory.
struct Fixture
{
    QTemporaryDir dir;
    config::Config cfg;
    std::filesystem::path configPath;
    std::shared_ptr<config::FileGuiConfigStore> store;
    std::unique_ptr<SettingsController> controller;
    QStringList errors; ///< What the controller reported through errorOccurred, in order.

    explicit Fixture(std::string_view yaml,
                     std::shared_ptr<display::IGpuInventory const> gpus = nullptr,
                     vtbackend::ColorPreference appearance = vtbackend::ColorPreference::Dark)
    {
        configPath = writeConfig(dir, yaml);
        // Through the path overload, as production loads: it reads the file's defaults_edition first.
        cfg = config::loadConfigFromFile(configPath);
        store =
            std::make_shared<config::FileGuiConfigStore>(std::filesystem::path(dir.path().toStdU16String()));
        controller =
            std::make_unique<SettingsController>([this]() -> config::Config const& { return cfg; },
                                                 store,
                                                 [this]() { cfg = config::loadConfigFromFile(configPath); },
                                                 std::move(gpus),
                                                 appearance);
        QObject::connect(controller.get(),
                         &SettingsController::errorOccurred,
                         [this](QString const& message) { errors.push_back(message); });
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
    CHECK(std::filesystem::exists(std::filesystem::path(fx.dir.path().toStdU16String()) / "profiles"
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

TEST_CASE("SettingsController: a new profile with nothing to copy takes the config's edition", "[settings]")
{
    // BasicConfig states no defaults_edition, so it is Classic. With neither the named profile nor the
    // default one to copy, the draft is the built-in defaults -- of that edition, not of the newest.
    auto fx = Fixture(BasicConfig);
    REQUIRE(fx.cfg.defaultsEdition == config::DefaultsEdition::Classic);
    fx.cfg.defaultProfileName = std::string { "missing" };
    fx.controller->newProfile("also-missing");

    auto const fields = fx.controller->profileFields();
    CHECK(rowWithKey(fields, "terminal_size_columns").value("value").toInt() == 80);
    CHECK(rowWithKey(fields, "terminal_size_lines").value("value").toInt() == 25);
    CHECK(rowWithKey(fields, "dim_unfocused").value("value").toDouble() == Catch::Approx(0.0));
    CHECK(rowWithKey(fields, "bell_sound").value("value").toString() == "default");
    CHECK(rowWithKey(fields, "history_max_lines").value("value").toInt() == 1000);
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
    CHECK(std::filesystem::exists(std::filesystem::path(fx.dir.path().toStdU16String()) / "profiles"
                                  / "work.yml")
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
    CHECK(std::filesystem::exists(std::filesystem::path(fx.dir.path().toStdU16String()) / "settings.yml"));
}

TEST_CASE("SettingsController: rename moves a side-file profile and follows the default + draft",
          "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");
    REQUIRE(fx.controller->saveProfileAs("work"));
    REQUIRE(fx.controller->setDefaultProfile("work"));
    fx.controller->editProfile("work"); // the profile we are about to rename is the open draft

    auto const dir = std::filesystem::path(fx.dir.path().toStdU16String());
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
    CHECK(std::filesystem::exists(std::filesystem::path(fx.dir.path().toStdU16String()) / "profiles"
                                  / "work.yml"));
}

TEST_CASE("SettingsController: rename moves a side-file color scheme with its draft", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->newColorScheme("");
    fx.controller->setSchemeColor("background", "#101010");
    REQUIRE(fx.controller->saveColorScheme("midnight"));

    auto const dir = std::filesystem::path(fx.dir.path().toStdU16String());
    REQUIRE(std::filesystem::exists(dir / "colorschemes" / "midnight.yml"));

    fx.controller->editColorScheme("midnight");
    REQUIRE(fx.controller->renameColorScheme("midnight", "nightfall"));
    CHECK(std::filesystem::exists(dir / "colorschemes" / "nightfall.yml"));
    CHECK_FALSE(std::filesystem::exists(dir / "colorschemes" / "midnight.yml"));
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
    REQUIRE(fx.store->saveColorScheme(name, palette).has_value());

    REQUIRE(fx.controller->renameColorScheme(QString::fromStdString(name), "nightfall"));
    auto const renamed = config::loadColorSchemeFile(std::filesystem::path(fx.dir.path().toStdU16String())
                                                     / "colorschemes" / "nightfall.yml");
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

TEST_CASE("SettingsController: saving a color scheme named like a contour.yml scheme is refused",
          "[settings]")
{
    // An inline scheme shadows a side file at load, so a GUI save under that name would silently have no
    // effect. Refuse it (and write nothing), mirroring Save As's contour.yml guard for profiles.
    auto fx = Fixture(InlineSchemeConfig);
    fx.controller->newColorScheme("");
    fx.controller->setSchemeColor("background", "#111111");
    CHECK(fx.controller->saveColorScheme("solarized") == false);
    CHECK_FALSE(std::filesystem::exists(std::filesystem::path(fx.dir.path().toStdU16String()) / "colorschemes"
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
    auto const dir = std::filesystem::path(fx.dir.path().toStdU16String());
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
    CHECK(std::filesystem::exists(std::filesystem::path(fx.dir.path().toStdU16String()) / "colorschemes"
                                  / "mono.yml"));
    fx.controller->editColorScheme("mono");
    auto background = QString {};
    for (auto const& row: fx.controller->schemeColors())
        if (row.toMap().value("key").toString() == "background")
            background = row.toMap().value("color").toString();
    CHECK(background == "#123456");

    REQUIRE(fx.controller->deleteColorScheme("mono"));
    CHECK(std::filesystem::exists(std::filesystem::path(fx.dir.path().toStdU16String()) / "colorschemes"
                                  / "mono.yml")
          == false);
}

TEST_CASE("SettingsController: the command-block colour slots are editable and stay derived until set",
          "[settings]")
{
    using vtbackend::CommandBlockOutcome;
    auto fx = Fixture(BasicConfig);
    fx.controller->newColorScheme("");

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

    fx.controller->setSchemeColor("block_status_failure", "#ff0000");
    fx.controller->setSchemeColor("block_status_hover_failure", "#ff8080");
    fx.controller->setSchemeColor("fold_marker_hover", "#e0e0e0");
    fx.controller->setSchemeColor("sticky_header_separator", "#123456");
    CHECK(colorOf("block_status_failure") == "#ff0000");
    REQUIRE(fx.controller->saveColorScheme("blocks"));

    auto const saved = config::loadColorSchemeFile(std::filesystem::path(fx.dir.path().toStdU16String())
                                                   / "colorschemes" / "blocks.yml");
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
    fx.controller->newColorScheme("");
    fx.controller->setSchemeColor("fold_marker", "#808080");
    fx.controller->setSchemeColor("background", "#203040");
    REQUIRE(fx.controller->saveColorScheme("folds"));

    auto const saved = config::loadColorSchemeFile(std::filesystem::path(fx.dir.path().toStdU16String())
                                                   / "colorschemes" / "folds.yml");
    REQUIRE(saved.has_value());
    REQUIRE(saved->foldMarker.has_value());
    CHECK(saved->foldMarker->foreground == vtbackend::RGBColor { 0x80, 0x80, 0x80 });
    CHECK(saved->foldMarker->background == vtbackend::RGBColor { 0x20, 0x30, 0x40 });
}

TEST_CASE("SettingsController: an unset colour slot follows its source, and says so", "[settings]")
{
    using vtbackend::CommandBlockOutcome;
    auto fx = Fixture(BasicConfig);
    fx.controller->newColorScheme("");
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
    fx.controller->setSchemeColor("normal_red", "#aa1122");
    expected.palette.at(1) = vtbackend::RGBColor { 0xaa, 0x11, 0x22 };
    CHECK(colorOf("block_status_failure") == hexOf(expected.blockStatusColor(CommandBlockOutcome::Failure)));
    CHECK(colorOf("block_status_failure") != failureBefore);

    // ... and the sticky header's band from the background.
    auto const bandBefore = colorOf("sticky_header_background");
    fx.controller->setSchemeColor("background", "#203040");
    expected.defaultBackground = vtbackend::RGBColor { 0x20, 0x30, 0x40 };
    CHECK(colorOf("sticky_header_background") == hexOf(expected.stickyHeaderBackgroundColor()));
    CHECK(colorOf("sticky_header_background") != bandBefore);
    CHECK(sourceOf("sticky_header_background") == "derived");
}

TEST_CASE("SettingsController: a derived colour is pinned in one edit, and follows again on reset",
          "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->newColorScheme("");
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
        REQUIRE(fx.controller->saveColorScheme("pinned"));
        auto palette = config::loadColorSchemeFile(std::filesystem::path(fx.dir.path().toStdU16String())
                                                   / "colorschemes" / "pinned.yml");
        REQUIRE(palette.has_value());
        return *palette;
    };

    // Committing the colour a derived slot shows keeps that colour, whatever its source does next.
    auto const shown = colorOf("gutter_text");
    fx.controller->setSchemeColor("gutter_text", shown);
    CHECK(sourceOf("gutter_text") == "pinned");
    fx.controller->setSchemeColor("foreground", "#00ff00");
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
    REQUIRE(fx.controller->setGlobalField("remember_window_position", true));
    CHECK(fx.cfg.rememberWindowPosition.value());
    REQUIRE(fx.controller->resetGlobalField("remember_window_position"));
    CHECK_FALSE(fx.cfg.rememberWindowPosition.value());
}

TEST_CASE("SettingsController: global overrides write settings.yml, apply, and reset", "[settings]")
{
    auto fx = Fixture("default_profile: main\n"
                      "reflow_on_resize: true\n");
    auto const configDir = std::filesystem::path(fx.dir.path().toStdU16String());

    REQUIRE(fx.controller->setGlobalField("reflow_on_resize", false));
    CHECK(fx.cfg.reflowOnResize.value() == false);
    CHECK_FALSE(fx.cfg.snapToRows.value());
    REQUIRE(fx.controller->setGlobalField("snap_to_rows", true));
    CHECK(fx.cfg.snapToRows.value());
    REQUIRE(fx.controller->resetGlobalField("snap_to_rows"));
    CHECK_FALSE(fx.cfg.snapToRows.value());
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
    CHECK(options == QStringList { "terminal", "system", "dark", "light" });

    // Selecting a value persists it as the corresponding enum on the live config.
    REQUIRE(fx.controller->setGlobalField("theme", "dark"));
    CHECK(fx.cfg.theme.value() == config::GuiTheme::Dark);

    REQUIRE(fx.controller->setGlobalField("theme", "system"));
    CHECK(fx.cfg.theme.value() == config::GuiTheme::System);

    REQUIRE(fx.controller->setGlobalField("theme", "terminal"));
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

    REQUIRE(fx.controller->setGlobalField("ui_style", "terminal"));
    CHECK(fx.cfg.uiStyle.value() == config::UiStyle::Terminal);

    REQUIRE(fx.controller->setGlobalField("ui_font_family", "JetBrains Mono"));
    CHECK(fx.cfg.uiFontFamily.value() == "JetBrains Mono");

    REQUIRE(fx.controller->setGlobalField("ui_font_size", 11.0));
    CHECK(fx.cfg.uiFontSize.value() == 11.0);

    // Resetting drops the override, so the value falls back to what contour.yml says.
    fx.controller->resetGlobalField("ui_style");
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

    REQUIRE(fx.controller->setGlobalField("window_control_style", "macos"));
    CHECK(fx.cfg.windowControlStyle.value() == config::WindowControlStyle::MacOS);

    // Resetting drops the override, so the value falls back to what contour.yml says -- here the
    // shipped default, which is the host-resolving one.
    fx.controller->resetGlobalField("window_control_style");
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

    REQUIRE(fx.controller->setGlobalField("chrome_density", "minimal"));
    CHECK(fx.cfg.chromeDensity.value() == config::ChromeDensity::Minimal);

    fx.controller->resetGlobalField("chrome_density");
    CHECK(fx.cfg.chromeDensity.value() == config::ChromeDensity::Comfortable);
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
        auto const wanted = differentValue(row);
        INFO("global field: " << key.toStdString() << " -> " << wanted.toString().toStdString());
        REQUIRE(fx.controller->setGlobalField(key, wanted));
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
    fx.cfg = config::Config {};
    config::loadConfigFromFile(fx.cfg, fx.configPath);

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
    fx.cfg = config::Config {};
    config::loadConfigFromFile(fx.cfg, fx.configPath);
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

    REQUIRE(fx.controller->setGlobalField("gutter.line_numbers", "relative"));
    REQUIRE(fx.controller->setGlobalField("folding.enabled", false));

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
    REQUIRE(fx.controller->resetGlobalField("gutter.line_numbers"));
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
    CHECK_FALSE(fx.controller->setGlobalField("gutter.line_numbers", "sideways"));
    CHECK_FALSE(
        rowWithKey(fx.controller->globalFields(), "gutter.line_numbers").value("overridden").toBool());
}

TEST_CASE("SettingsController: an invalid timestamp format is reported and not saved", "[settings]")
{
    auto fx = Fixture(BasicConfig);

    CHECK_FALSE(fx.controller->setGlobalField("gutter.timestamp_format", "%K"));
    CHECK_FALSE(fx.controller->setGlobalField("gutter.timestamp_format", "{%H}"));
    CHECK_FALSE(fx.controller->setGlobalField("gutter.timestamp_format", ""));
    REQUIRE(fx.errors.size() == qsizetype { 3 });
    for (auto const& message: fx.errors)
        CHECK(message.contains("timestamp format"));
    CHECK(fx.cfg.gutter.value().timestampFormat == "%H:%M:%S");
    CHECK_FALSE(
        rowWithKey(fx.controller->globalFields(), "gutter.timestamp_format").value("overridden").toBool());

    REQUIRE(fx.controller->setGlobalField("gutter.timestamp_format", "%H:%M"));
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
        CHECK_FALSE(fx.controller->setGlobalField(QString::fromLatin1(key), value));
        CHECK(fx.errors.size() == qsizetype { 1 });
        CHECK_FALSE(
            rowWithKey(fx.controller->globalFields(), QString::fromLatin1(key)).value("overridden").toBool());
    }

    // Zero recent commands is a choice: it turns the list off.
    REQUIRE(fx.controller->setGlobalField("command_palette_recent_count", 0));
    CHECK(fx.cfg.commandPaletteRecentCount.value() == 0);
}

TEST_CASE("SettingsController: the numeric command-block rows are range-checked", "[settings]")
{
    auto fx = Fixture(BasicConfig);

    CHECK_FALSE(fx.controller->setGlobalField("gutter.line_number_width", 2));
    CHECK_FALSE(fx.controller->setGlobalField("gutter.line_number_width", 11));
    REQUIRE(fx.controller->setGlobalField("gutter.line_number_width", 10));
    CHECK(fx.cfg.gutter.value().lineNumberWidth == 10);

    CHECK_FALSE(fx.controller->setGlobalField("command_blocks.max_records", 0));
    // The loader clamps above config::MaxCommandBlockRecords; the page refuses instead of saving a value
    // that would silently load as something else.
    CHECK_FALSE(fx.controller->setGlobalField("command_blocks.max_records",
                                              static_cast<int>(config::MaxCommandBlockRecords) + 1));
    REQUIRE(fx.controller->setGlobalField("command_blocks.max_records", 1));
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
    CHECK_FALSE(fx.controller->setGlobalField("command_blocks.pager", ""));
    CHECK_FALSE(fx.controller->setGlobalField("command_blocks.pager", "   "));
    REQUIRE(fx.errors.size() == qsizetype { 2 });
    CHECK(fx.errors.first().contains("Reset"));
    CHECK(fx.cfg.commandBlocks.value().pager == configured);
    CHECK_FALSE(
        rowWithKey(fx.controller->globalFields(), "command_blocks.pager").value("overridden").toBool());

    REQUIRE(fx.controller->setGlobalField("command_blocks.pager", "most"));
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

    REQUIRE(fx.controller->setGlobalField("command_blocks.pager_format", "plain"));
    CHECK(fx.cfg.commandBlocks.value().pagerFormat == actions::OutputFormat::Plain);
    CHECK_FALSE(fx.controller->setGlobalField("command_blocks.pager_format", "html"));
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

TEST_CASE("SettingsController: the command-block profile fields round-trip through a saved profile",
          "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");

    fx.controller->setProfileField("scrollbar_marks_commands", false);
    fx.controller->setProfileField("sticky_header_mode", "always");
    fx.controller->setProfileField("sticky_header_show_evicted", false);
    fx.controller->setProfileField("notify_on_command_finish_when", "hidden");
    fx.controller->setProfileField("notify_on_command_finish_min_duration", 42);
    fx.controller->setProfileField("notify_on_command_finish_outcome", "failure");
    fx.controller->setProfileField("notify_on_command_finish_action", "notify_bell");
    fx.controller->setProfileField("notify_on_command_finish_clear_on", "next");
    REQUIRE(fx.controller->saveProfileAs("blocks"));

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

    // The page reads back what was saved (saveProfileAs re-opens the reloaded profile).
    auto const fields = fx.controller->profileFields();
    CHECK(rowWithKey(fields, "scrollbar_marks_commands").value("value").toBool() == false);
    CHECK(rowWithKey(fields, "sticky_header_mode").value("value").toString() == "always");
    CHECK(rowWithKey(fields, "sticky_header_show_evicted").value("value").toBool() == false);
    CHECK(rowWithKey(fields, "notify_on_command_finish_min_duration").value("value").toInt() == 42);

    SECTION("all three scrollbar sources off is an empty list, not the default")
    {
        fx.controller->setProfileField("scrollbar_marks_failures", false);
        fx.controller->setProfileField("scrollbar_marks_user_marks", false);
        REQUIRE(fx.controller->saveProfile());
        auto const* saved = fx.cfg.findProfile("blocks");
        REQUIRE(saved != nullptr);
        CHECK(saved->scrollbar.value().marks.none());
    }

    SECTION("a negative minimum duration is stored as zero")
    {
        fx.controller->setProfileField("notify_on_command_finish_min_duration", -5);
        CHECK(rowWithKey(fx.controller->profileFields(), "notify_on_command_finish_min_duration")
                  .value("value")
                  .toInt()
              == 0);
    }
}

TEST_CASE("SettingsController: the command-block profile fields form their own group", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");

    auto keys = QStringList {};
    for (auto const& raw: fx.controller->profileFields())
        if (raw.toMap().value("group").toString() == "Command blocks")
            keys.push_back(raw.toMap().value("key").toString());
    CHECK(keys
          == QStringList { "scrollbar_marks_failures",
                           "scrollbar_marks_commands",
                           "scrollbar_marks_user_marks",
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
    CHECK(commandsHelp.contains("still running"));

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
    fx.controller->newProfile("main");

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
    fx.controller->editProfile("main");
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
    fx.controller->newProfile("main");

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

    REQUIRE(fx.controller->setGlobalField("renderer.gpu", "10de:2820"));
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

TEST_CASE("SettingsController: plain enum rows label each option with its value", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    auto const row = rowWithKey(fx.controller->globalFields(), "theme");
    CHECK(row.value("optionLabels").toStringList() == row.value("options").toStringList());
}

TEST_CASE("SettingsController: Show links edits hyperlink_decoration.mode", "[settings][links]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");
    fx.controller->setProfileField("hyperlink_decoration_mode", "never");
    auto const fields = fx.controller->profileFields();
    auto const row = rowWithKey(fields, "hyperlink_decoration_mode");
    CHECK(row.value("value").toString() == "never");
    CHECK(row.value("group").toString() == "Links");
    CHECK(row.value("options").toStringList() == QStringList { "always", "hover", "modifier", "never" });
}

TEST_CASE("SettingsController: link detection switches edit link_detection", "[settings][links]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");
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
        CHECK(field(row.key).value("group").toString() == "Links");
        CHECK(field(row.key).value("type").toString() == "bool");
        CHECK(field(row.key).value("value").toBool() == row.value);
        // The settings search (QML) matches on label, help text and key.
        CHECK(field(row.key).value("label").toString().contains(row.labelPart));
        CHECK(field(row.key).value("help").toString().endsWith("OSC 8 hyperlinks are always links."));
    }

    fx.controller->setProfileField("link_detection_other_file_paths", true);
    CHECK(field("link_detection_other_file_paths").value("value").toBool());
    CHECK(field("link_detection_primary_file_paths").value("value").toBool());
}

TEST_CASE("SettingsController: Open files with validates its placeholders inline", "[settings][links]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");
    auto const field = [&](QString const& key) {
        return rowWithKey(fx.controller->profileFields(), key);
    };

    REQUIRE_FALSE(field("file_opener_command").isEmpty());
    CHECK(field("file_opener_command").value("group").toString() == "Links");
    CHECK(field("file_opener_command").value("problem").toString().isEmpty()); // empty: the system default

    fx.controller->setProfileField("file_opener_command", "code {file}");
    CHECK(field("file_opener_command").value("value").toString() == "code {file}");
    CHECK(field("file_opener_command").value("problem").toString().contains("placeholder"));

    fx.controller->setProfileField("file_opener_command", "code {path");
    CHECK(field("file_opener_command").value("problem").toString().contains("'{'"));

    fx.controller->setProfileField("file_opener_command", "code --goto {path}:{line}:{column}");
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
    fx.controller->newProfile("main");
    fx.controller->setProfileField("file_opener_command", "code {file}");
    REQUIRE(fx.controller->saveProfileAs("work"));
    REQUIRE(fx.cfg.findProfile("work") != nullptr);
    CHECK(fx.cfg.findProfile("work")->fileOpener.value().command == "code {file}");

    // An unrelated edit, saved under the profile's own name, writes the command back unchanged.
    fx.controller->editProfile("work");
    CHECK(rowWithKey(fx.controller->profileFields(), "file_opener_command")
              .value("problem")
              .toString()
              .contains("placeholder"));
    fx.controller->setProfileField("file_opener_placement", "tab");
    REQUIRE(fx.controller->saveProfile());
    CHECK(fx.cfg.findProfile("work")->fileOpener.value().command == "code {file}");
    CHECK(fx.cfg.findProfile("work")->fileOpener.value().placement == contour::actions::OutputPlacement::Tab);
}

TEST_CASE("SettingsController: Open in edits file_opener.placement", "[settings][links]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");
    auto const row = [&] {
        return rowWithKey(fx.controller->profileFields(), "file_opener_placement");
    };
    CHECK(row().value("value").toString() == "detached");
    CHECK(row().value("options").toStringList() == QStringList { "split", "tab", "detached" });
    fx.controller->setProfileField("file_opener_placement", "tab");
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
    fx.controller->newProfile("main");
    auto const filepath = builtinRow("filepath");
    auto edited = linkRow(*fx.controller, filepath);
    edited["clickable"] = false;
    fx.controller->setLinkPattern(filepath, edited);
    CHECK_FALSE(linkRow(*fx.controller, filepath).value("clickable").toBool());
    CHECK(linkRow(*fx.controller, filepath).value("problem").toString().isEmpty()); // a name-only override
    CHECK(fx.controller->linkPatterns().size() == builtinRowCount());               // and no row of its own

    REQUIRE(fx.controller->saveProfileAs("work"));
    REQUIRE(fx.cfg.findProfile("work") != nullptr);
    CHECK(fx.cfg.findProfile("work")->hintPatterns.value()
          == std::vector {
              config::HintPatternConfig { .name = "filepath", .regex = {}, .clickable = false, .url = {} } });

    // Back to the built-in's own default: nothing is left in the override, so it goes.
    edited["clickable"] = true;
    fx.controller->setLinkPattern(filepath, edited);
    CHECK(linkRow(*fx.controller, filepath).value("clickable").toBool());
    REQUIRE(fx.controller->saveProfile());
    CHECK(fx.cfg.findProfile("work")->hintPatterns.value().empty());
}

TEST_CASE("SettingsController: user link patterns are added, edited, checked and removed",
          "[settings][links]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");
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
    fx.controller->newProfile("main");
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

TEST_CASE("SettingsController: a profile with a link pattern in error is not saved, and says which",
          "[settings][links]")
{
    // The reader would skip a pattern without a regex: refused, as a field in error is.
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");
    auto const index = builtinRowCount();
    fx.controller->addLinkPattern();
    auto edited = linkRow(*fx.controller, index);
    edited["name"] = "jira";
    fx.controller->setLinkPattern(index, edited);
    CHECK_FALSE(fx.controller->saveProfileAs("work"));
    CHECK(fx.cfg.findProfile("work") == nullptr);
    REQUIRE_FALSE(fx.errors.isEmpty());
    CHECK(fx.errors.back().contains("jira"));

    edited["regex"] = QString::fromUtf8(JiraRegex.data(), static_cast<qsizetype>(JiraRegex.size()));
    edited["url"] = "https://j/{0}";
    fx.controller->setLinkPattern(index, edited);
    REQUIRE(fx.controller->saveProfileAs("work"));
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
    fx.controller->newProfile("main");
    CHECK(linkRow(*fx.controller, builtinRow("filepath")).value("problem").toString().contains("group 1"));

    fx.controller->setProfileField("show_title_bar", false);
    REQUIRE(fx.controller->saveProfileAs("work"));
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
    fx.controller->newProfile("main");
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
    fx.controller->newProfile("main");
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
    fx.controller->newProfile("main");
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
    fx.controller->newProfile("main");

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
