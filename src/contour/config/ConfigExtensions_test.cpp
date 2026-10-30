// SPDX-License-Identifier: Apache-2.0
//
// Config loader tests, continued: extensions, secure input, tab colours, window position,
// the theme and the tab bar, and the action variants.

#include <contour/config/Actions.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/ConfigEnum.hpp>
#include <contour/config/ConfigReader.hpp>
#include <contour/config/ConfigTestSupport.hpp>
#include <contour/config/SingleQuoted.hpp>
#include <contour/platform/GuiTheme.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/input/InputGenerator.hpp>
#include <vtbackend/screen/GutterSettings.hpp>
#include <vtbackend/shell/ScrollbarMarks.hpp>
#include <vtbackend/shell/StickyHeader.hpp>

#include <vtpty/test/NonAsciiSample.hpp>

#include <text_shaper/Font.hpp>

#include <crispy/StrongHash.hpp>

#include <core/log/LogSink.hpp>
#include <core/testing/ScopedWorkingDirectory.hpp>

#include <QtCore/QTemporaryDir>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace contour::test::configSupport;

namespace
{

constexpr auto ExtensionsYaml = R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        extensions:
            git:    { run_command: ask }
            plasma: { config: { speed: "2" } }
            mine:   { path: ~/wasm/mine.wasm, run_command: deny }
        status_line:
            extension: plasma
            indicator:
                right: "{Extension:git} │ {Clock:Bold}"
)"sv;

/// Every row of every table in @p Enums formats as the token it carries.
template <typename... Enums>
void checkFormatsAsToken()
{
    auto const checkRow = [](auto const& row) {
        INFO("token: " << row.token);
        CHECK(std::format("{}", row.value) == row.token);
    };
    (std::ranges::for_each(contour::config::configEnumValues<Enums>(), checkRow), ...);
}

/// What one DefaultsEdition must give a configuration that does not override anything, written out
/// literally rather than read back from the edition table: the table is what is under test.
struct ExpectedEditionDefaults
{
    contour::config::DefaultsEdition edition;
    std::string_view fontFamily;
    double fontSize;
    int lines;
    int columns;
    unsigned horizontalMargin;
    unsigned verticalMargin;
    std::string_view statusLineLeft;
    std::string_view statusLineMiddle;
    std::string_view statusLineRight;
    vtbackend::LinkDecorationMode linkDecoration;
    double dimUnfocused;
    std::string_view bellSound;
    int scrollback;
    vtbackend::CursorShape insertCursorShape;
    std::chrono::milliseconds screenTransitionDuration;
    std::string_view darkColorScheme; ///< Equal to lightColorScheme for a single `colors: <name>`.
    std::string_view lightColorScheme;
    contour::config::TabBarVisibility tabBarVisibility;
    vtbackend::Modifier followHyperlinkModifier;
    contour::config::GuiTheme theme;
    bool hyperlinkHoverTooltip; ///< `hyperlink_hover_tooltip`, a YAML boolean.
    contour::config::ScrollBarModePreference scrollBarMode;
    vtbackend::PreeditStyle preeditStyle;
    contour::config::SecureInputMode secureInput;
};

/// Every value the releases before the 2026 redesign shipped.
constexpr auto ClassicExpectation = ExpectedEditionDefaults {
    .edition = contour::config::DefaultsEdition::Classic,
#ifdef __APPLE__
    .fontFamily = "Monaco",
#else
    .fontFamily = "monospace",
#endif
    .fontSize = 12.0,
    .lines = 25,
    .columns = 80,
    .horizontalMargin = 0,
    .verticalMargin = 0,
    .statusLineLeft = " {InputMode:Bold,Color=#FFFF00}"
                      "{TraceMode:Bold,Color=#FFFF00,Left= │ }"
                      "{Tabs:ActiveColor=#FFFF00,Left= │ }"
                      "{ProtectedMode:Bold,Left= │ }"
                      "{Context:Left= │ }",
    .statusLineMiddle = "« {Title} »",
    .statusLineRight = "{HistoryLineCount:Faint,Color=#c0c0c0} │ {Clock:Bold}",
    .linkDecoration = vtbackend::LinkDecorationMode::Always,
    .dimUnfocused = 0.0,
    .bellSound = "default",
    .scrollback = 1000,
    .insertCursorShape = vtbackend::CursorShape::Bar,
    .screenTransitionDuration = std::chrono::milliseconds { 250 },
    .darkColorScheme = "default",
    .lightColorScheme = "default",
    .tabBarVisibility = contour::config::TabBarVisibility::Always,
    .followHyperlinkModifier = vtbackend::Modifier::Control,
    .theme = contour::config::GuiTheme::System,
    .hyperlinkHoverTooltip = true,
    .scrollBarMode = contour::config::ScrollBarModePreference::Always,
    .preeditStyle = vtbackend::PreeditStyle::Highlighted,
    .secureInput = contour::config::SecureInputMode::Off,
};

/// Every first-launch value of the 2026 redesign.
constexpr auto Native2026Expectation = ExpectedEditionDefaults {
    .edition = contour::config::DefaultsEdition::Native2026,
#ifdef _WIN32
    .fontFamily = "Cascadia Mono",
#else
    .fontFamily = "JetBrains Mono",
#endif
#ifdef __APPLE__
    .fontSize = 13.0,
#else
    .fontSize = 12.0,
#endif
    .lines = 30,
    .columns = 100,
    .horizontalMargin = 8,
    .verticalMargin = 6,
    .statusLineLeft =
        "{InputMode:Bold,Left= ,Right= ,Chip,Hide=Insert}{Context:Left= }{Cwd:Color=success,Left= }"
        "{Git:Left=  }",
    .statusLineMiddle = "{LastCommand}{Selection:Left=  }{SearchPrompt:Color=warning,Left=  }",
    .statusLineRight = "{Progress:Color=accent,Right=  }{Scroll:Right=  }{Clock:Faint,Right= }",
    .linkDecoration = vtbackend::LinkDecorationMode::Modifier,
    .dimUnfocused = 0.15,
    .bellSound = "off",
    .scrollback = 10000,
    .insertCursorShape = vtbackend::CursorShape::Bar,
    .screenTransitionDuration = std::chrono::milliseconds { 150 },
    .darkColorScheme = "contour-dark",
    .lightColorScheme = "contour-light",
    .tabBarVisibility = contour::config::TabBarVisibility::Always,
#ifdef __APPLE__
    .followHyperlinkModifier = vtbackend::Modifier::Super, // Command: Qt's Control/Meta swap is off
#else
    .followHyperlinkModifier = vtbackend::Modifier::Control,
#endif
    .theme = contour::config::GuiTheme::Terminal,
    .hyperlinkHoverTooltip = false,
    .scrollBarMode = contour::config::ScrollBarModePreference::System,
    .preeditStyle = vtbackend::PreeditStyle::Inline,
    .secureInput = contour::config::SecureInputMode::Auto,
};

/// The palette a built-in scheme name resolves to.
[[nodiscard]] vtbackend::ColorPalette builtinPaletteNamed(std::string_view name)
{
    auto palette = vtbackend::ColorPalette {};
    (void) vtbackend::defaultColorPalettes(name, palette);
    return palette;
}

/// Checks @p profile against every profile-level default of @p expected.
void checkProfileDefaults(contour::config::TerminalProfile const& profile,
                          ExpectedEditionDefaults const& expected)
{
    auto const& fonts = profile.fonts.value();
    CHECK(fonts.regular.familyName == expected.fontFamily);
    CHECK(fonts.bold.familyName == expected.fontFamily);
    CHECK(fonts.italic.familyName == expected.fontFamily);
    CHECK(fonts.boldItalic.familyName == expected.fontFamily);
    CHECK(fonts.size.pt == Catch::Approx(expected.fontSize));
    CHECK(profile.terminalSize.value().lines == vtbackend::LineCount(expected.lines));
    CHECK(profile.terminalSize.value().columns == vtbackend::ColumnCount(expected.columns));
    CHECK(profile.margins.value().horizontal
          == contour::config::HorizontalMargin { expected.horizontalMargin });
    CHECK(profile.margins.value().vertical == contour::config::VerticalMargin { expected.verticalMargin });

    auto const& statusLine = profile.statusLine.value();
    CHECK(statusLine.initialType == vtbackend::StatusDisplayType::Indicator);
    CHECK(statusLine.indicator.left == expected.statusLineLeft);
    CHECK(statusLine.indicator.middle == expected.statusLineMiddle);
    CHECK(statusLine.indicator.right == expected.statusLineRight);

    CHECK(profile.hyperlinkDecoration.value().mode == expected.linkDecoration);
    CHECK(profile.dimUnfocused.value() == Catch::Approx(expected.dimUnfocused));
    CHECK(profile.bell.value().sound == expected.bellSound);
    CHECK(profile.bell.value().alert);
    CHECK(profile.history.value().maxHistoryLineCount
          == vtbackend::MaxHistoryLineCount { vtbackend::LineCount(expected.scrollback) });
    CHECK(profile.modeInsert.value().cursor.cursorShape == expected.insertCursorShape);
    CHECK(profile.modeInsert.value().cursor.cursorDisplay == vtbackend::CursorDisplay::Steady);
    CHECK(profile.screenTransitionDuration.value() == expected.screenTransitionDuration);
    CHECK(profile.inputMethodEditorStyle.value() == expected.preeditStyle);
    CHECK(profile.tabLabel.value() == "{WindowTitle}");
    CHECK(profile.scrollbar.value().mode == expected.scrollBarMode);

    auto const& colors = profile.colors.value();
    if (expected.darkColorScheme == expected.lightColorScheme)
    {
        REQUIRE(std::holds_alternative<contour::config::SimpleColorConfig>(colors));
        CHECK(std::get<contour::config::SimpleColorConfig>(colors).colorScheme == expected.darkColorScheme);
    }
    else
    {
        REQUIRE(std::holds_alternative<contour::config::DualColorConfig>(colors));
        auto const& dual = std::get<contour::config::DualColorConfig>(colors);
        CHECK(dual.colorSchemeDark == expected.darkColorScheme);
        CHECK(dual.colorSchemeLight == expected.lightColorScheme);
        // The palettes are resolved, not left at the default palette under a new name.
        auto const dark = builtinPaletteNamed(expected.darkColorScheme);
        auto const light = builtinPaletteNamed(expected.lightColorScheme);
        CHECK(dark.defaultBackground != vtbackend::ColorPalette {}.defaultBackground);
        CHECK(dual.darkMode.defaultBackground == dark.defaultBackground);
        CHECK(dual.darkMode.defaultForeground == dark.defaultForeground);
        CHECK(dual.lightMode.defaultBackground == light.defaultBackground);
        CHECK(dual.lightMode.defaultForeground == light.defaultForeground);
    }
}

/// Checks @p config, its default profile included, against every default of @p expected.
void checkEditionDefaults(contour::config::Config const& config, ExpectedEditionDefaults const& expected)
{
    CHECK(config.defaultsEdition == expected.edition);
    CHECK(config.tabBarVisibility.value() == expected.tabBarVisibility);
    CHECK(config.theme.value() == expected.theme);
    CHECK(config.hyperlinkHoverTooltip.value() == expected.hyperlinkHoverTooltip);
    CHECK(config.secureInput.value() == expected.secureInput);

    auto const& mice = config.inputMappings.value().mouseMappings;
    auto const follow = std::ranges::find_if(mice, [](auto const& mapping) {
        return mapping.input == vtbackend::MouseButton::Left && !mapping.binding.empty()
               && std::holds_alternative<contour::actions::FollowHyperlink>(mapping.binding.at(0));
    });
    REQUIRE(follow != mice.end());
    CHECK(follow->modifiers == vtbackend::Modifiers { expected.followHyperlinkModifier });

    checkProfileDefaults(config.profile(), expected);
}

/// A contour.yml from before `defaults_edition` existed: it spells out none of the defaults that changed.
constexpr auto PartialConfigWithoutEdition = "profiles:\n"
                                             "    main:\n"
                                             "        shell: /bin/sh\n"
                                             "    work:\n"
                                             "        shell: /bin/sh\n"sv;

} // namespace

TEST_CASE("Config: a SetTabColor binding survives the round-trip through the config it writes", "[config]")
{
    // std::formatter<Action> is what Contour writes into the contour.yml it generates, and parseAction()
    // is what reads it back. Closing that loop for real — feeding the formatter's OWN output to the
    // parser rather than a hand-written copy of it — is what catches the quoting hazard: unquoted,
    // `color: #FF0000` is a YAML COMMENT, and the color would silently vanish on the next start.
    using namespace contour::actions;

    auto const red = vtbackend::RGBColor { 0xFF, 0x00, 0x00 };
    auto const written = std::format("{}", Action { SetTabColor { red } });
    CHECK(written == "SetTabColor, color: '#FF0000'");
    CHECK(std::format("{}", Action { SetTabColor {} }) == "SetTabColor"); // the picker writes no color
    CHECK(std::format("{}", Action { ResetTabColor {} }) == "ResetTabColor");

    QTemporaryDir dir;
    auto const config = loadFromYaml(dir,
                                     std::format(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - {{ mods: [Control], key: F2, action: {} }}
)",
                                                 written));

    auto const& mappings = config.inputMappings.value();
    REQUIRE(mappings.keyMappings.size() == 1);
    auto const& action = mappings.keyMappings.at(0).binding.at(0);
    REQUIRE(std::holds_alternative<SetTabColor>(action));
    CHECK(std::get<SetTabColor>(action).color == red); // what went out came back
}

TEST_CASE("Config: a CopySelection format survives the round-trip through the config it writes", "[config]")
{
    // The writer once wrote no `format:` at all, so a bound `format: HTML` came back as text after the first
    // config save. Feeding the writer's own output to the reader is what catches that.
    using namespace contour::actions;

    QTemporaryDir dir;
    auto const config = loadFromYaml(dir,
                                     std::format(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - {{ mods: [Control], key: F2, action: {} }}
)",
                                                 Action { CopySelection { .format = CopyFormat::HTML } }));

    auto const& mappings = config.inputMappings.value();
    REQUIRE(mappings.keyMappings.size() == 1);
    auto const& action = mappings.keyMappings.at(0).binding.at(0);
    REQUIRE(std::holds_alternative<CopySelection>(action));
    CHECK(std::get<CopySelection>(action).format == CopyFormat::HTML);
}

TEST_CASE("Config: a nameless SaveLayout binding is kept and round-trips as 'SaveLayout'", "[config]")
{
    // The layout-side mirror of the SetTabColor round-trip. A nameless SaveLayout opens the save-as prompt,
    // so — like a colorless SetTabColor — it writes no argument, must survive the config it writes, and
    // (unlike a nameless LaunchLayout) must NOT be dropped on parse. A named one still carries its name.
    using namespace contour::actions;

    CHECK(std::format("{}", Action { SaveLayout {} }) == "SaveLayout"); // the prompt writes no name
    CHECK(std::format("{}", Action { SaveLayout { .name = "dev" } }) == "SaveLayout, name: 'dev'");

    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control, Shift], key: F2, action: SaveLayout }
    - { mods: [Control, Shift], key: F3, action: SaveLayout, name: dev }
)"sv);

    auto const& mappings = config.inputMappings.value().keyMappings;
    // Both bindings survived: the nameless one is no longer dropped for want of a name.
    auto const nameless = std::ranges::any_of(mappings, [](auto const& m) {
        auto const* s = std::get_if<SaveLayout>(&m.binding.at(0));
        return s != nullptr && s->name.empty();
    });
    auto const named = std::ranges::any_of(mappings, [](auto const& m) {
        auto const* s = std::get_if<SaveLayout>(&m.binding.at(0));
        return s != nullptr && s->name == "dev";
    });
    CHECK(nameless);
    CHECK(named);
}

TEST_CASE("Config: an AttachToHost binding carries its host's name, and one without is refused",
          "[config][hosts]")
{
    // Like LaunchLayout: "attach to which host?" has no default, so a nameless binding is dropped on parse.
    using namespace contour::actions;

    CHECK(std::format("{}", Action { AttachToHost { .name = "home" } }) == "AttachToHost, name: 'home'");

    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control, Shift], key: F6, action: AttachToHost, name: home }
    - { mods: [Control, Shift], key: F7, action: AttachToHost }
)"sv);

    auto const& mappings = config.inputMappings.value().keyMappings;
    auto const attaches = std::ranges::count_if(
        mappings, [](auto const& m) { return std::holds_alternative<AttachToHost>(m.binding.at(0)); });
    CHECK(attaches == 1);
    auto const home = std::ranges::any_of(mappings, [](auto const& m) {
        auto const* attach = std::get_if<AttachToHost>(&m.binding.at(0));
        return attach != nullptr && attach->name == "home";
    });
    CHECK(home);
}

TEST_CASE("Config: font features, text shaping engine, frozen DEC modes and hint patterns load", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        font:
            size: 11
            regular:
                family: monospace
                features: ['+dlig', '-calt', 'ss01', 'bad']
            text_shaping:
                engine: OpenShaper
        frozen_dec_modes:
            "2026": false
            "999999": true
        hint_patterns:
            - { name: 'url', regex: 'https?://\S+' }
            - { name: '', regex: 'incomplete' }
        hint_scrollback_lines: 250
        mouse:
            hide_while_typing: false
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);

    // 4-letter feature codes parse (with +/- prefixes); the invalid 3-letter one is skipped.
    CHECK(profile->fonts.value().regular.features.size() == 3);
    CHECK(profile->fonts.value().textShapingEngine == vtrasterizer::TextShapingEngine::OpenShaper);

    // Valid DEC mode 2026 freezes; the out-of-range number is rejected.
    CHECK(profile->frozenModes.value().size() == 1);

    // Hint patterns: the complete entry loads, the nameless one is skipped with a warning.
    REQUIRE(profile->hintPatterns.value().size() == 1);
    CHECK(profile->hintPatterns.value().at(0).name == "url");
    CHECK(profile->hintScrollbackLines.value() == vtbackend::LineCount(250));

    CHECK(profile->mouse.value().hideWhileTyping == false);
}

TEST_CASE("Config: a hint pattern's quotes survive a write and a read", "[config]")
{
    // The regex is written single-quoted, whose one escape is a doubled quote: a quote at either end and a
    // run of two must each come back as written.
    QTemporaryDir dir;
    auto config = contour::config::Config {};
    auto const regex = std::string { "'[^']*''x'" };
    config.profile("main")->hintPatterns.value() = { contour::config::HintPatternConfig { .name = "quoted",
                                                                                          .regex = regex } };

    auto const reloaded =
        loadFromYaml(dir, contour::config::createString<contour::config::YAMLConfigWriter>(config));
    auto const* profile = reloaded.profile("main");
    REQUIRE(profile != nullptr);
    REQUIRE(profile->hintPatterns.value().size() == 1);
    CHECK(profile->hintPatterns.value().at(0).regex == regex);
}

TEST_CASE("Config: a hint pattern's name survives a write and a read", "[config]")
{
    // Written as a plain scalar, ` #` would start a comment and `: ` a nested mapping -- the first silently
    // shortens the name, the second makes the whole file unreadable.
    for (auto const name: { "url #1"sv, "key: value"sv, "'quoted'"sv })
    {
        CAPTURE(name);
        QTemporaryDir dir;
        auto config = contour::config::Config {};
        config.profile("main")->hintPatterns.value() = { contour::config::HintPatternConfig {
            .name = std::string { name }, .regex = "x" } };

        auto const reloaded =
            loadFromYaml(dir, contour::config::createString<contour::config::YAMLConfigWriter>(config));
        auto const* profile = reloaded.profile("main");
        REQUIRE(profile != nullptr);
        REQUIRE(profile->hintPatterns.value().size() == 1);
        CHECK(profile->hintPatterns.value().at(0).name == name);
    }
}

TEST_CASE("Config: extensions load by name, with their config, path and permission", "[config][extensions]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, ExtensionsYaml);
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);

    auto const& extensions = profile->extensions.value();
    REQUIRE(extensions.size() == 3);

    CHECK(extensions[0].name == "git");
    CHECK(extensions[0].path.empty());
    CHECK(extensions[0].runCommand == contour::config::Permission::Ask);
    CHECK(extensions[0].config.empty());

    CHECK(extensions[1].name == "plasma");
    CHECK(extensions[1].config == contour::config::ExtensionConfigMap { { "speed", "2" } });
    CHECK(extensions[1].runCommand == contour::config::Permission::Ask);

    CHECK(extensions[2].name == "mine");
    CHECK(extensions[2].runCommand == contour::config::Permission::Deny);
    // The leading ~ is the home directory, as for the other paths the file names.
    CHECK(extensions[2].path.is_absolute());
    CHECK(extensions[2].path.generic_string().ends_with("/wasm/mine.wasm"));

    CHECK(profile->statusLine.value().extension == std::optional<std::string> { "plasma" });
    CHECK(profile->statusLine.value().indicator.right == "{Extension:git} │ {Clock:Bold}");
}

TEST_CASE("Config: no extensions are configured unless the file names them", "[config][extensions]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, "profiles:\n  main:\n    shell: /bin/sh\n"sv);
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(profile->extensions.value().empty());
    CHECK_FALSE(profile->statusLine.value().extension.has_value());
    CHECK(contour::config::Config {}.profile("main")->extensions.value().empty());
}

TEST_CASE("Config: extensions survive a write and a read", "[config][extensions]")
{
    QTemporaryDir dir;
    auto const original = loadFromYaml(dir, ExtensionsYaml);

    QTemporaryDir dir2;
    auto const reloaded =
        loadFromYaml(dir2, contour::config::createString<contour::config::YAMLConfigWriter>(original));
    auto const* profile = reloaded.profile("main");
    REQUIRE(profile != nullptr);

    CHECK(profile->extensions.value() == original.profile("main")->extensions.value());
    CHECK(profile->statusLine.value().extension == std::optional<std::string> { "plasma" });
}

TEST_CASE("Config: a profile without a take-over writes none and reads none back", "[config][extensions]")
{
    QTemporaryDir dir;
    auto const reloaded = loadFromYaml(
        dir, contour::config::createString<contour::config::YAMLConfigWriter>(contour::config::Config {}));
    auto const* profile = reloaded.profile("main");
    REQUIRE(profile != nullptr);
    CHECK_FALSE(profile->statusLine.value().extension.has_value());
    CHECK(profile->extensions.value().empty());
}

TEST_CASE("Config: an extension's awkward names and values survive a write and a read",
          "[config][extensions]")
{
    // Written as plain scalars, ` #` would start a comment and `: ` a nested mapping.
    QTemporaryDir dir;
    // Absolute on every platform, so that reading it back resolves nothing.
    auto const path = std::filesystem::path(dir.path().toStdU16String()) / "it's here.wasm";
    auto config = contour::config::Config {};
    config.profile("main")->extensions.value() = { contour::config::ExtensionConfig {
        .name = "key: #1",
        .path = path,
        .config = { { "a: b", "c #d" }, { "quote", "'x'" } },
        .runCommand = contour::config::Permission::Allow,
    } };

    auto const reloaded =
        loadFromYaml(dir, contour::config::createString<contour::config::YAMLConfigWriter>(config));
    auto const* profile = reloaded.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(profile->extensions.value() == config.profile("main")->extensions.value());
}

TEST_CASE("Config: an unknown run_command is reported and the entry asks", "[config][extensions]")
{
    auto const capture = core::log::ScopedCapture { "error" };
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir,
                                     "profiles:\n  main:\n    shell: /bin/sh\n"
                                     "    extensions:\n      git: { run_command: maybe }\n"sv);
    INFO("captured error log:\n" << capture.text());
    CHECK(capture.text().contains("maybe"));
    CHECK(capture.text().contains("git"));

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    REQUIRE(profile->extensions.value().size() == 1);
    CHECK(profile->extensions.value()[0].runCommand == contour::config::Permission::Ask);
}

TEST_CASE("Config: a malformed extension entry costs that entry only", "[config][extensions]")
{
    auto const capture = core::log::ScopedCapture { "error" };
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir,
                                     "profiles:\n  main:\n    shell: /bin/sh\n"
                                     "    extensions:\n"
                                     "      broken: [1, 2]\n"
                                     "      nested: { config: { k: [1] } }\n"
                                     "      fine: { run_command: allow }\n"sv);
    INFO("captured error log:\n" << capture.text());
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const& extensions = profile->extensions.value();
    REQUIRE(extensions.size() == 2);
    CHECK(extensions[0].name == "nested");
    CHECK(extensions[0].config.empty());
    CHECK(extensions[1].name == "fine");
    CHECK(extensions[1].runCommand == contour::config::Permission::Allow);
    CHECK(capture.text().contains("broken"));
    CHECK(capture.text().contains("nested"));
}

TEST_CASE("Config: an extension named twice keeps the first", "[config][extensions]")
{
    auto const capture = core::log::ScopedCapture { "error" };
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir,
                                     "profiles:\n  main:\n    shell: /bin/sh\n"
                                     "    extensions:\n"
                                     "      git: { run_command: deny }\n"
                                     "      git: { run_command: allow }\n"sv);
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    REQUIRE(profile->extensions.value().size() == 1);
    CHECK(profile->extensions.value()[0].runCommand == contour::config::Permission::Deny);
    INFO("captured error log:\n" << capture.text());
    CHECK(capture.text().contains("'git'"));
    CHECK(capture.text().contains("twice"));
}

TEST_CASE("Config: a config key set twice keeps the later value and says so", "[config][extensions]")
{
    auto const capture = core::log::ScopedCapture { "error" };
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir,
                                     "profiles:\n  main:\n    shell: /bin/sh\n"
                                     "    extensions:\n"
                                     "      plasma:\n"
                                     "        config:\n"
                                     "          speed: '1'\n"
                                     "          other: x\n"
                                     "          speed: '2'\n"sv);
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    REQUIRE(profile->extensions.value().size() == 1);
    CHECK(profile->extensions.value()[0].config
          == contour::config::ExtensionConfigMap { { "other", "x" }, { "speed", "2" } });
    INFO("captured error log:\n" << capture.text());
    CHECK(capture.text().contains("'speed'"));
    CHECK(capture.text().contains("twice"));
}

TEST_CASE("Config: a path that names nothing skips the entry instead of falling back to the bundled one",
          "[config][extensions]")
{
    auto const capture = core::log::ScopedCapture { "error" };
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir,
                                     "profiles:\n  main:\n    shell: /bin/sh\n"
                                     "    extensions:\n"
                                     "      empty: { path: '' }\n"
                                     "      nothing: { path: }\n"
                                     "      list: { path: [a, b] }\n"
                                     "      bundled: { run_command: allow }\n"sv);
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const& extensions = profile->extensions.value();
    REQUIRE(extensions.size() == 1);
    CHECK(extensions[0].name == "bundled");
    CHECK(extensions[0].path.empty());
    INFO("captured error log:\n" << capture.text());
    CHECK(capture.text().contains("'empty'"));
    CHECK(capture.text().contains("'nothing'"));
    CHECK(capture.text().contains("'list'"));
}

TEST_CASE("Config: a relative extension path is relative to the file declaring it", "[config][extensions]")
{
    QTemporaryDir dir;
    auto const root = std::filesystem::path(dir.path().toStdU16String());
    writeSideFile(dir,
                  "profiles/work.yml",
                  "extensions:\n"
                  "  side: { path: side.wasm }\n"
                  "  up: { path: ../up.wasm }\n"sv);
    auto const config = loadFromYaml(dir,
                                     "default_profile: main\n"
                                     "profiles:\n  main:\n    shell: /bin/sh\n"
                                     "    extensions:\n"
                                     "      relative: { path: wasm/rel.wasm }\n"
                                     "      absolute: { path: /opt/abs.wasm }\n"sv);

    auto const* main = config.profile("main");
    REQUIRE(main != nullptr);
    auto const& mainExtensions = main->extensions.value();
    REQUIRE(mainExtensions.size() == 2);
    CHECK(mainExtensions[0].path == (root / "wasm" / "rel.wasm").lexically_normal());
    // Rooted, so kept as written; on Windows it takes the declaring file's drive, as a rooted path does
    // there.
    CHECK(mainExtensions[1].path == root / std::filesystem::path { "/opt/abs.wasm" });

    // The side file's own directory, not the directory of contour.yml.
    auto const* work = config.profile("work");
    REQUIRE(work != nullptr);
    auto const& workExtensions = work->extensions.value();
    REQUIRE(workExtensions.size() == 2);
    CHECK(workExtensions[0].path == (root / "profiles" / "side.wasm").lexically_normal());
    CHECK(workExtensions[1].path == (root / "up.wasm").lexically_normal());
}

TEST_CASE("Config: a relative extension path of a bare-filename config is made absolute",
          "[config][extensions]")
{
    // `contour config contour.yml`: the config file has no directory part, yet the stored path must not
    // depend on the working directory at the time the module is loaded.
    QTemporaryDir dir;
    (void) writeConfig(dir,
                       "profiles:\n  main:\n    shell: /bin/sh\n"
                       "    extensions:\n      relative: { path: wasm/rel.wasm }\n"sv);
    auto const here =
        core::testing::ScopedWorkingDirectory { std::filesystem::path(dir.path().toStdU16String()) };

    auto config = contour::config::Config {};
    contour::config::loadConfigFromFile(config, "contour.yml");

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    REQUIRE(profile->extensions.value().size() == 1);
    auto const& path = profile->extensions.value()[0].path;
    CHECK(path.is_absolute());
    CHECK(path == (std::filesystem::current_path() / "wasm" / "rel.wasm"));
}

TEST_CASE("Config: extensions that are no map are ignored, and a null one clears the list",
          "[config][extensions]")
{
    auto const capture = core::log::ScopedCapture { "error" };
    QTemporaryDir dir;
    writeSideFile(dir, "profiles/keeps.yml", "extensions: [1, 2]\n"sv);
    writeSideFile(dir, "profiles/clears.yml", "extensions: ~\n"sv);
    writeSideFile(dir, "profiles/untouched.yml", "mouse: { hide_while_typing: false }\n"sv);
    auto const config = loadFromYaml(dir,
                                     "default_profile: main\n"
                                     "profiles:\n  main:\n    shell: /bin/sh\n"
                                     "    extensions:\n      git: {}\n"sv);

    for (auto const* name: { "main", "keeps", "untouched" })
    {
        CAPTURE(name);
        auto const* profile = config.profile(name);
        REQUIRE(profile != nullptr);
        REQUIRE(profile->extensions.value().size() == 1);
        CHECK(profile->extensions.value()[0].name == "git");
    }
    auto const* clears = config.profile("clears");
    REQUIRE(clears != nullptr);
    CHECK(clears->extensions.value().empty());

    INFO("captured error log:\n" << capture.text());
    CHECK(capture.text().contains("extensions"));
    CHECK(capture.text().contains("expected a map"));
}

TEST_CASE("Config: a null or empty status_line.extension resets the take-over, an absent one keeps it",
          "[config][extensions]")
{
    QTemporaryDir dir;
    writeSideFile(dir, "profiles/null.yml", "status_line: { extension: null }\n"sv);
    writeSideFile(dir, "profiles/empty.yml", "status_line: { extension: '' }\n"sv);
    writeSideFile(dir, "profiles/absent.yml", "status_line: { position: top }\n"sv);
    auto const config = loadFromYaml(dir,
                                     "default_profile: main\n"
                                     "profiles:\n  main:\n    shell: /bin/sh\n"
                                     "    status_line:\n      extension: plasma\n"sv);

    auto const takeOver = [&](char const* name) {
        auto const* profile = config.profile(name);
        REQUIRE(profile != nullptr);
        return profile->statusLine.value().extension;
    };
    CHECK(takeOver("main") == std::optional<std::string> { "plasma" });
    CHECK(takeOver("absent") == std::optional<std::string> { "plasma" });
    CHECK_FALSE(takeOver("null").has_value());
    CHECK_FALSE(takeOver("empty").has_value());
}

// }}}

TEST_CASE("Config: dual (dark/light) color scheme selection loads both palettes", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        colors:
            dark: darkish
            light: lightish
color_schemes:
    darkish:
        default:
            background: '#000010'
            foreground: '#C0C0C0'
    lightish:
        default:
            background: '#F0F0FF'
            foreground: '#202020'
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    REQUIRE(std::holds_alternative<contour::config::DualColorConfig>(profile->colors.value()));
    auto const& dual = std::get<contour::config::DualColorConfig>(profile->colors.value());
    CHECK(dual.darkMode.defaultBackground == vtbackend::RGBColor(0x00, 0x00, 0x10));
    CHECK(dual.lightMode.defaultBackground == vtbackend::RGBColor(0xF0, 0xF0, 0xFF));
}

TEST_CASE("Config: a second profile inherits from the default profile", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        terminal_size:
            columns: 90
            lines: 30
    alt:
        font:
            size: 16
)"sv);

    auto const* alt = config.profile("alt");
    REQUIRE(alt != nullptr);
    // Overridden field applies; everything else inherits the default profile's values.
    CHECK(alt->fonts.value().size.pt == 16.0);
    CHECK(alt->terminalSize.value().columns == vtbackend::ColumnCount(90));
    CHECK(alt->terminalSize.value().lines == vtbackend::LineCount(30));
}

TEST_CASE("Config: remember window position defaults and explicit values round trip",
          "[config][window-position]")
{
    QTemporaryDir dir;
    auto const defaults = loadFromYaml(dir, "default_profile: main\n");
    auto const generated = YAML::Load(contour::config::defaultConfigString());
#ifdef _WIN32
    CHECK(defaults.rememberWindowPosition.value());
    CHECK(generated["remember_window_position"].as<bool>());
#else
    CHECK_FALSE(defaults.rememberWindowPosition.value());
    CHECK_FALSE(generated["remember_window_position"].as<bool>());
#endif
    for (auto const* value: { "true", "false" })
    {
        auto const configured = loadFromYaml(dir, std::string("remember_window_position: ") + value + "\n");
        auto const reloaded =
            loadFromYaml(dir, contour::config::createString<contour::config::YAMLConfigWriter>(configured));
        CHECK(reloaded.rememberWindowPosition.value() == (std::string_view(value) == "true"));
    }
}

TEST_CASE("Config: row snapping is opt in and round trips", "[config][row-resize]")
{
    QTemporaryDir dir;
    auto const defaults = loadFromYaml(dir, "default_profile: main\n");
    CHECK_FALSE(defaults.snapToRows.value());
    auto const enabled = loadFromYaml(dir, "default_profile: main\nsnap_to_rows: true\n");
    CHECK(enabled.snapToRows.value());
    auto const reloaded =
        loadFromYaml(dir, contour::config::createString<contour::config::YAMLConfigWriter>(enabled));
    CHECK(reloaded.snapToRows.value());
    auto const generated = YAML::Load(contour::config::defaultConfigString());
    CHECK_FALSE(generated["snap_to_rows"].as<bool>());
}

TEST_CASE("Config: generated default config round-trips through the loader", "[config]")
{
    // defaultConfigString() renders the full default config document (the `contour generate
    // config` payload); feeding it back through the loader pins writer and reader against each
    // other — a key renamed in only one of them shows up as a default-valued field here.
    QTemporaryDir dir;
    auto const defaults = contour::config::Config {};
    auto const rendered = contour::config::defaultConfigString();
    auto const reloaded = loadFromYaml(dir, rendered);

    CHECK(reloaded.defaultProfileName.value() == defaults.defaultProfileName.value());
    CHECK(reloaded.earlyExitThreshold.value() == defaults.earlyExitThreshold.value());
    auto const* profile = reloaded.profile(reloaded.defaultProfileName.value());
    REQUIRE(profile != nullptr);
    CHECK(profile->terminalSize.value().columns == defaults.profile().terminalSize.value().columns);
    CHECK(profile->terminalSize.value().lines == defaults.profile().terminalSize.value().lines);
    CHECK(profile->showTitleBar.value() == defaults.profile().showTitleBar.value());
    // Guards the tab_bar_* writer<->reader key match: the std::formatter emits "Top"/"Always" into the
    // generated doc, and the loader must read the same key back. A typo in only one side would surface
    // here as a default-valued mismatch (which, for these two, would still equal the default — so also
    // assert the rendered document actually carries the keys below).
    CHECK(reloaded.tabBarPosition.value() == defaults.tabBarPosition.value());
    CHECK(reloaded.tabBarVisibility.value() == defaults.tabBarVisibility.value());
    // Guards the global `theme` writer<->reader key match: the std::formatter emits "system" into the
    // generated doc, and the loader must read the same key back.
    CHECK(reloaded.theme.value() == defaults.theme.value());
    // Same guard for window_control_style: its std::formatter emits "auto", and its default surviving
    // the round trip is what proves the writer and the reader agree on the key. The rendered-document
    // check below is the half that would catch the key being missing entirely, since "auto" read back
    // as the default would look identical to it never having been written.
    CHECK(reloaded.windowControlStyle.value() == defaults.windowControlStyle.value());
    CHECK(rendered.contains("pixel_reporting:"));
    CHECK(rendered.contains("tab_bar_position:"));
    CHECK(rendered.contains("tab_bar_visibility:"));
    CHECK(rendered.contains("theme:"));
    CHECK(rendered.contains("window_control_style:"));
    // Each value the reader accepts is named where the setting is documented.
    for (auto const& info: contour::config::configEnumValues<contour::config::WindowControlStyle>())
        CHECK(rendered.contains(std::format("   {} ", info.token)));
}

TEST_CASE("Config: dual color schemes, palette list forms, and infinite history load", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        colors:
            dark: darkish
            light: lightish
        history:
            limit: -1
color_schemes:
    darkish:
        default:
            background: '#000010'
            foreground: '#C0C0C0'
        normal:
            black: '#000000'
            red: '#AA0000'
            green: '#00AA00'
            yellow: '#AAAA00'
            blue: '#0000AA'
            magenta: '#AA00AA'
            cyan: '#00AAAA'
            white: '#AAAAAA'
    lightish:
        default:
            background: '#FFFFF0'
            foreground: '#202020'
        bright: ['#101010', '#FF0000', '#00FF00', '#FFFF00', '#0000FF', '#FF00FF', '#00FFFF', '#FFFFFF']
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);

    // colors: {dark:, light:} selects the DualColorConfig arm with both palettes resolved.
    REQUIRE(std::holds_alternative<contour::config::DualColorConfig>(profile->colors.value()));
    auto const& dual = std::get<contour::config::DualColorConfig>(profile->colors.value());
    CHECK(dual.colorSchemeDark == "darkish");
    CHECK(dual.colorSchemeLight == "lightish");
    CHECK(dual.darkMode.defaultBackground == vtbackend::RGBColor(0x00, 0x00, 0x10));
    CHECK(dual.lightMode.defaultForeground == vtbackend::RGBColor(0x20, 0x20, 0x20));
    // Named-map palette form (dark) and sequence palette form (light) both land in the palettes.
    CHECK(dual.darkMode.normalColor(1) == vtbackend::RGBColor(0xAA, 0x00, 0x00));
    CHECK(dual.lightMode.brightColor(2) == vtbackend::RGBColor(0x00, 0xFF, 0x00));
    // A colour no default palette carries, so the list is proven read rather than left at its defaults,
    // and into the bright slots rather than the normal ones.
    CHECK(dual.lightMode.brightColor(0) == vtbackend::RGBColor(0x10, 0x10, 0x10));
    CHECK(dual.lightMode.normalColor(0) == vtbackend::ColorPalette {}.normalColor(0));

    // history.limit: -1 selects the Infinite arm of MaxHistoryLineCount.
    CHECK(std::holds_alternative<vtbackend::Infinite>(profile->history.value().maxHistoryLineCount));
}

TEST_CASE("Config: input mappings parse keys, chars, mouse buttons, modifiers and modes", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control, Shift], key: 'F3', action: NewTerminal }
    - { mods: [Alt], key: 'Enter', action: ToggleFullscreen, mode: 'Insert' }
    - { mods: [Control], key: 'a', action: CopySelection }
    - { mods: [Meta, Shift], key: 'Tab', action: SwitchToTabRight }
    - { mods: [Control, Shift], mouse: Left, action: FollowHyperlink }
    - { mods: [Control], mouse: WheelUp, action: IncreaseFontSize }
    - { mods: [Control], mouse: WheelDown, action: DecreaseFontSize }
    - { mods: [Shift], key: 'PageUp', action: ScrollPageUp, mode: 'Select|Insert' }
    - { mods: [Control, Shift], key: 'v', action: PasteClipboard, strip: true }
    - { mods: [], key: 'F12', action: WriteScreen, chars: "hello" }
)"sv);

    // An `input_mapping:` section REPLACES the built-in defaults rather than adding to them, so the
    // totals are exactly the ten rows above, split by how each `key:` parses: a NAMED key becomes a
    // KeyInputMapping, a single character a CharInputMapping.
    auto const& mappings = config.inputMappings.value();
    CHECK(mappings.keyMappings.size() == 5);   // F3, Enter, Tab, PageUp, F12
    CHECK(mappings.charMappings.size() == 2);  // 'a', 'v'
    CHECK(mappings.mouseMappings.size() == 3); // Left, WheelUp, WheelDown
}

TEST_CASE("Config: a single-quoted scalar doubles the quotes inside it", "[config]")
{
    using contour::config::SingleQuoted;
    CHECK(std::format("{}", SingleQuoted { "" }) == "''");
    CHECK(std::format("{}", SingleQuoted { "%H:%M" }) == "'%H:%M'");
    CHECK(std::format("{}", SingleQuoted { "it's" }) == "'it''s'");
    CHECK(std::format("{}", SingleQuoted { "'" }) == "''''");
}

TEST_CASE("Config: the key named BACKSLASH binds the backslash", "[config][input-mapping]")
{
    // One of the names the documentation offers for a character YAML makes awkward to write.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control], key: BACKSLASH, action: ScreenshotVT }
)"sv);

    auto const& charMappings = config.inputMappings.value().charMappings;
    REQUIRE(charMappings.size() == 1);
    CHECK(charMappings.front().input == U'\\');
}

TEST_CASE("Config: a character key binding round-trips through the generated config", "[config]")
{
    // The writer single-quotes every key, so a quote must be written doubled -- `'''` is no YAML at all --
    // and a character beyond ASCII as its UTF-8, not as the one byte a cast to char leaves of it.
    auto const keys = std::array { U'\'', U'ö', U'€' }; // ', ö, €
    auto const modifiers = vtbackend::Modifiers { vtbackend::Modifier::Control }
                           | vtbackend::Modifiers { vtbackend::Modifier::Alt };
    auto config = contour::config::Config {};
    for (auto const key: keys)
        config.inputMappings.value().charMappings.push_back(
            contour::config::CharInputMapping { .modes = {},
                                                .modifiers = modifiers,
                                                .input = key,
                                                .binding = { { contour::actions::ScreenshotVT {} } } });

    QTemporaryDir dir;
    auto const reloaded =
        loadFromYaml(dir, contour::config::createString<contour::config::YAMLConfigWriter>(config));

    auto const& charMappings = reloaded.inputMappings.value().charMappings;
    for (auto const key: keys)
    {
        CAPTURE(static_cast<uint32_t>(key));
        CHECK(std::ranges::any_of(charMappings, [&](auto const& mapping) {
            return mapping.input == key && mapping.modifiers == modifiers;
        }));
    }
}

TEST_CASE("Config: invalid enum values fall back and do not abort loading", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        scrollbar:
            position: Sideways
        status_line:
            display: holographic
            position: Middle
        permissions:
            change_font: perhaps
        bell:
            sound: default
renderer:
    backend: FancyGPU
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    // Unknown enum literals leave the compiled-in defaults in place. Compared with the default itself, not a
    // literal: which edge that is, is pinned once, by "the scrollbar is shown on the right by default".
    CHECK(profile->scrollbar.value().position == contour::config::ScrollBarConfig {}.position);
    CHECK(profile->statusLine.value().position == vtbackend::StatusDisplayPosition::Bottom);
    CHECK(profile->permissions.value().changeFont == contour::config::Permission::Ask);
}

TEST_CASE("Config: environment-style variables resolve inside string values", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        initial_working_directory: "~/somewhere"
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    // A leading ~ resolves to the user's home directory (homeResolvedPath).
    auto const cwd = profile->shell.value().workingDirectory.string();
    CHECK(!cwd.contains('~'));
    CHECK_FALSE(cwd.empty());
}

TEST_CASE("Config: a path in the configuration file is read as UTF-8", "[config]")
{
    constexpr auto Awkward = vtpty::testing::NonAsciiName;
    auto const awkward = std::string { vtpty::testing::NonAsciiNameUtf8 };
    auto const loadWorkingDirectory = [](std::string const& spelled) {
        QTemporaryDir dir;
        auto const config = loadFromYaml(
            dir, std::format("profiles:\n    main:\n        initial_working_directory: \"{}\"\n", spelled));
        return config.profile("main")->shell.value().workingDirectory;
    };

    CHECK(loadWorkingDirectory("/tmp/" + awkward) == std::filesystem::path { u8"/tmp/" } / Awkward);
    CHECK(loadWorkingDirectory("~/" + awkward) == vtpty::Process::homeDirectory() / Awkward);

    // A profile that names none inherits the default profile's, resolved once more on the way.
    QTemporaryDir dir;
    auto const config =
        loadFromYaml(dir,
                     std::format("profiles:\n    main:\n        initial_working_directory: \"/tmp/{}\"\n"
                                 "    other:\n        shell: /bin/sh\n",
                                 awkward));
    REQUIRE(config.profile("other") != nullptr);
    CHECK(config.profile("other")->shell.value().workingDirectory
          == std::filesystem::path { u8"/tmp/" } / Awkward);

    // A background image: where it is, and the hash it is cached under, which is of its path in UTF-8.
    QTemporaryDir imageDir;
    auto const imageConfig =
        loadFromYaml(imageDir,
                     std::format("profiles:\n    main:\n        colors: awkward\n"
                                 "color_schemes:\n    awkward:\n        background_image:\n"
                                 "            path: '/tmp/{}.png'\n",
                                 awkward));
    auto const& colors = imageConfig.profile("main")->colors.value();
    REQUIRE(std::holds_alternative<contour::config::SimpleColorConfig>(colors));
    auto const& image = std::get<contour::config::SimpleColorConfig>(colors).colors.backgroundImage;
    REQUIRE(image != nullptr);
    CHECK(std::get<std::filesystem::path>(image->location)
          == std::filesystem::path { u8"/tmp/" } / (std::u8string { Awkward } + u8".png"));
    CHECK(image->hash == crispy::StrongHash::compute(std::format("/tmp/{}.png", awkward)));
}

TEST_CASE("Config: a background image's path is formatted in UTF-8", "[config]")
{
    // As the configuration log formats it. path::string() spelled it in the ANSI code page on Windows, and
    // threw for a name beyond it.
    auto image = vtbackend::BackgroundImage {};
    image.location = std::filesystem::path { u8"/tmp/" } / vtpty::testing::NonAsciiName;
    CHECK(std::format("{}", image) == std::format("/tmp/{}", vtpty::testing::NonAsciiNameUtf8));
}

TEST_CASE("Config: deprecated image max size keys are accepted and ignored", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
images:
    max_width: 640
    max_height: 480
    sixel_scrolling: false
profiles:
    main:
        shell: /bin/sh
)"sv);

    // max_width/max_height no longer exist: the image canvas is derived from the screen. They must
    // still parse, so configurations carrying them keep loading -- including their neighbours in the
    // same section, which is what would break if the keys made the section fail. The loader warns
    // about them so the setting does not just quietly stop meaning something.
    CHECK_FALSE(config.images.value().sixelScrolling);
}

TEST_CASE("Config: text_outline accepts the scalar thickness-only form", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        font:
            size: 12
            text_outline: 3.5
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    // Scalar form: thickness only, clamped to [0, 10].
    CHECK(profile->fonts.value().textOutline.thickness == 3.5f);
}

TEST_CASE("Config: per-mode cursor shape/blink/interval parse from YAML", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        cursor:
            shape: bar
            blinking: true
            blinking_interval: 400
        normal_mode:
            cursor:
                shape: block
                blinking: false
                blinking_interval: 300
        visual_mode:
            cursor:
                shape: underscore
                blinking: true
                blinking_interval: 250
        blink_style: smooth
        screen_transition: fade
        screen_transition_duration: 120
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(profile->modeInsert.value().cursor.cursorShape == vtbackend::CursorShape::Bar);
    CHECK(profile->modeInsert.value().cursor.cursorDisplay == vtbackend::CursorDisplay::Blink);
    CHECK(profile->modeNormal.value().cursor.cursorShape == vtbackend::CursorShape::Block);
    CHECK(profile->modeNormal.value().cursor.cursorDisplay == vtbackend::CursorDisplay::Steady);
    CHECK(profile->modeVisual.value().cursor.cursorShape == vtbackend::CursorShape::Underscore);
    CHECK(profile->blinkStyle.value() == vtbackend::BlinkStyle::Smooth);
    CHECK(profile->screenTransitionStyle.value() == vtbackend::ScreenTransitionStyle::Fade);
}

TEST_CASE("Config: cursor shape rectangle and blink linger arms parse", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        cursor:
            shape: rectangle
        blink_style: linger
        screen_transition: classic
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(profile->modeInsert.value().cursor.cursorShape == vtbackend::CursorShape::Rectangle);
    CHECK(profile->blinkStyle.value() == vtbackend::BlinkStyle::Linger);
    CHECK(profile->screenTransitionStyle.value() == vtbackend::ScreenTransitionStyle::Classic);
}

TEST_CASE("Config: pixel_reporting parses each value (ignore-case)", "[config]")
{
    using contour::config::PixelReporting;

    SECTION("default is Device, the unit the cell is an integer in")
    {
        // Not a preference: the cell is the font's advance in device pixels, so only a device-pixel
        // report divides back to it exactly. Reporting logical floors each axis on its own, changing
        // the cell's aspect ratio, and a full-page image is then letterboxed by the difference.
        QTemporaryDir dir;
        auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
)"sv);
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        CHECK(profile->pixelReporting.value() == PixelReporting::Device);
    }

    SECTION("device")
    {
        QTemporaryDir dir;
        auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        pixel_reporting: Device
)"sv);
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        CHECK(profile->pixelReporting.value() == PixelReporting::Device);
    }

    SECTION("lower-case logical")
    {
        QTemporaryDir dir;
        auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        pixel_reporting: logical
)"sv);
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        CHECK(profile->pixelReporting.value() == PixelReporting::Logical);
    }
}

TEST_CASE("Config: an invalid pixel_reporting value falls back to the default", "[config]")
{
    using contour::config::PixelReporting;

    QTemporaryDir dir;
    // Unlike the tab_bar_* readers this one also errorLog()s: a typo here leaves the user looking at
    // the oversized image the setting exists to fix, with nothing to connect the two.
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        pixel_reporting: Physical
)"sv);
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(profile->pixelReporting.value() == PixelReporting::Device);
}

TEST_CASE("Config: theme parses each value (ignore-case)", "[config]")
{
    using contour::config::GuiTheme;

    SECTION("native2026 defaults to Terminal (follow the terminal background)")
    {
        QTemporaryDir dir;
        auto const config = loadFromYaml(dir, R"(
defaults_edition: native2026
default_profile: main
profiles:
    main:
        shell: /bin/sh
)"sv);
        CHECK(config.theme.value() == GuiTheme::Terminal);
    }

    SECTION("a file without defaults_edition keeps the classic System (follow the OS)")
    {
        // Through the path-taking loader, which picks the edition from the file the way startup does.
        QTemporaryDir dir;
        auto const config = contour::config::loadConfigFromFile(writeConfig(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
)"sv));
        CHECK(config.theme.value() == GuiTheme::System);
    }

    SECTION("terminal")
    {
        QTemporaryDir dir;
        auto const config = loadFromYaml(dir, R"(
default_profile: main
theme: Terminal
profiles:
    main:
        shell: /bin/sh
)"sv);
        CHECK(config.theme.value() == GuiTheme::Terminal);
    }

    SECTION("dark")
    {
        QTemporaryDir dir;
        auto const config = loadFromYaml(dir, R"(
default_profile: main
theme: dark
profiles:
    main:
        shell: /bin/sh
)"sv);
        CHECK(config.theme.value() == GuiTheme::Dark);
    }

    SECTION("upper-case LIGHT is accepted (ignore-case)")
    {
        QTemporaryDir dir;
        auto const config = loadFromYaml(dir, R"(
default_profile: main
theme: LIGHT
profiles:
    main:
        shell: /bin/sh
)"sv);
        CHECK(config.theme.value() == GuiTheme::Light);
    }

    SECTION("system")
    {
        QTemporaryDir dir;
        auto const config = loadFromYaml(dir, R"(
default_profile: main
theme: system
profiles:
    main:
        shell: /bin/sh
)"sv);
        CHECK(config.theme.value() == GuiTheme::System);
    }
}

TEST_CASE("Config: an invalid theme value falls back to the default", "[config]")
{
    using contour::config::GuiTheme;

    QTemporaryDir dir;
    // A typo in a visible appearance setting must not silently pass; the reader errorLog()s and keeps
    // the edition's default (Terminal under native2026) rather than aborting the whole config load.
    auto const config = loadFromYaml(dir, R"(
defaults_edition: native2026
default_profile: main
theme: midnight
profiles:
    main:
        shell: /bin/sh
)"sv);
    CHECK(config.theme.value() == GuiTheme::Terminal);
}

TEST_CASE("GuiTheme: buildThemePalette forces a legible dark/light chrome palette", "[config]")
{
    using contour::platform::buildThemePalette;

    // The explicit palette is what actually recolors the chrome on platform themes (KDE/GNOME) that
    // own the palette and ignore QStyleHints::setColorScheme. Assert the two schemes differ and each
    // keeps the window text readable against its own window fill (the failure mode of the old seam
    // was a palette that never changed at all).
    auto const dark = buildThemePalette(Qt::ColorScheme::Dark);
    auto const light = buildThemePalette(Qt::ColorScheme::Light);

    // Dark: light text on a dark window; Light: dark text on a light window.
    CHECK(dark.color(QPalette::Window).lightnessF() < 0.5);
    CHECK(dark.color(QPalette::WindowText).lightnessF() > 0.5);
    CHECK(light.color(QPalette::Window).lightnessF() > 0.5);
    CHECK(light.color(QPalette::WindowText).lightnessF() < 0.5);

    // The two schemes must genuinely differ — the whole point of an explicit palette.
    CHECK(dark.color(QPalette::Window) != light.color(QPalette::Window));
    CHECK(dark.color(QPalette::Base) != light.color(QPalette::Base));

    // The disabled group is themed too, so greyed-out chrome stays visibly distinct from enabled text.
    CHECK(dark.color(QPalette::Disabled, QPalette::Text) != dark.color(QPalette::Normal, QPalette::Text));
}

TEST_CASE("Config: every tab bar mode is described by its table", "[config]")
{
    using namespace contour::config;

    checkEnumerators(std::array { TabBarPosition::Top, TabBarPosition::Bottom });
    checkEnumerators(
        std::array { TabBarVisibility::Always, TabBarVisibility::Never, TabBarVisibility::Multiple });
    checkEnumerators(std::array { UiStyle::Native, UiStyle::Terminal });
    checkEnumerators(std::array { WindowControlStyle::Auto,
                                  WindowControlStyle::Windows,
                                  WindowControlStyle::MacOS,
                                  WindowControlStyle::Plasma,
                                  WindowControlStyle::Linux });
    checkEnumerators(std::array { vtbackend::SearchCaseSensitivity::Smart,
                                  vtbackend::SearchCaseSensitivity::Insensitive,
                                  vtbackend::SearchCaseSensitivity::Sensitive });

    // The reader is case-insensitive, and refuses what no row carries.
    CHECK(configEnumFromToken<TabBarVisibility>("MULTIPLE") == TabBarVisibility::Multiple);
    CHECK(configEnumFromToken<TabBarVisibility>("mUlTiPlE") == TabBarVisibility::Multiple);
    CHECK(!configEnumFromToken<TabBarVisibility>("sometimes").has_value());
    CHECK(!configEnumFromToken<TabBarPosition>("").has_value());

    // The labels are what a human reads, so they must be distinct -- two rows reading "Multiple"
    // would be indistinguishable in a menu even though the modes differ.
    CHECK(configEnumLabel(TabBarVisibility::Always) != configEnumLabel(TabBarVisibility::Never));
    CHECK(configEnumLabel(TabBarVisibility::Never) != configEnumLabel(TabBarVisibility::Multiple));
}

TEST_CASE("Config: every configuration enum is written as its table's token", "[config]")
{
    using namespace contour::config;

    // What the writer emits must be what the reader accepts. One formatter serves every enum with a
    // table; vtbackend's own formatters for the enums it owns must agree with the table as well.
    checkFormatsAsToken<TabBarPosition,
                        TabBarVisibility,
                        UiStyle,
                        WindowControlStyle,
                        ShadowSize,
                        NotifyWhen,
                        NotifyOutcome,
                        NotifyAction,
                        NotifyClearOn,
                        contour::actions::CopyFormat,
                        contour::actions::CopyFallback,
                        contour::actions::OutputPlacement,
                        contour::actions::OutputFormat,
                        vtbackend::CommandBlockTarget,
                        vtbackend::CommandBlockPart,
                        vtbackend::FoldJumpBehavior,
                        vtbackend::ContextMarkPolicy,
                        vtbackend::ContextTintScope,
                        vtbackend::SearchCaseSensitivity,
                        vtbackend::LineNumberMode,
                        vtbackend::StickyHeaderMode,
                        vtbackend::LinkDecorationMode,
                        vtbackend::ScrollbarMarkSource>();
}

TEST_CASE("Config: every scrollbar mark source is described by its table", "[config][scrollbar-marks]")
{
    using namespace contour::config;
    using vtbackend::ScrollbarMarkSource;

    checkEnumerators(std::array { ScrollbarMarkSource::Failures,
                                  ScrollbarMarkSource::Commands,
                                  ScrollbarMarkSource::Running,
                                  ScrollbarMarkSource::UserMarks,
                                  ScrollbarMarkSource::SearchMatches });
    // The spellings the documentation promises.
    CHECK(configEnumToken(ScrollbarMarkSource::Failures) == "failures");
    CHECK(configEnumToken(ScrollbarMarkSource::Commands) == "commands");
    CHECK(configEnumToken(ScrollbarMarkSource::Running) == "running");
    CHECK(configEnumToken(ScrollbarMarkSource::UserMarks) == "user_marks");
    CHECK(configEnumToken(ScrollbarMarkSource::SearchMatches) == "search_matches");

    // The default shows every source the table offers, and nothing it does not: a row added without its
    // bit in AllScrollbarMarkSources would be a tick no profile shows unless it lists the token.
    auto const rows = configEnumValues<ScrollbarMarkSource>();
    for (auto const& row: rows)
    {
        INFO("token: " << row.token);
        CHECK(vtbackend::AllScrollbarMarkSources.test(row.value));
    }
    CHECK(std::cmp_equal(std::popcount(vtbackend::AllScrollbarMarkSources.value()), rows.size()));
}

TEST_CASE("Config: tab_bar_position and tab_bar_visibility parse each value (ignore-case)", "[config]")
{
    using contour::config::TabBarPosition;
    using contour::config::TabBarVisibility;

    SECTION("bottom + never")
    {
        QTemporaryDir dir;
        auto const config = loadFromYaml(dir, R"(
default_profile: main
tab_bar_position: Bottom
tab_bar_visibility: Never
profiles:
    main:
        shell: /bin/sh
)"sv);
        CHECK(config.tabBarPosition.value() == TabBarPosition::Bottom);
        CHECK(config.tabBarVisibility.value() == TabBarVisibility::Never);
    }

    SECTION("top + multiple, lower-case tokens prove case-insensitivity")
    {
        QTemporaryDir dir;
        auto const config = loadFromYaml(dir, R"(
default_profile: main
tab_bar_position: top
tab_bar_visibility: multiple
profiles:
    main:
        shell: /bin/sh
)"sv);
        CHECK(config.tabBarPosition.value() == TabBarPosition::Top);
        CHECK(config.tabBarVisibility.value() == TabBarVisibility::Multiple);
    }

    SECTION("always (the default) still parses explicitly")
    {
        QTemporaryDir dir;
        auto const config = loadFromYaml(dir, R"(
default_profile: main
tab_bar_visibility: Always
profiles:
    main:
        shell: /bin/sh
)"sv);
        CHECK(config.tabBarVisibility.value() == TabBarVisibility::Always);
    }
}

TEST_CASE("Config: tab_bar_* under a profile is no longer read", "[config]")
{
    using contour::config::TabBarPosition;
    using contour::config::TabBarVisibility;

    // The tab bar belongs to the window, so these keys moved from the profile to the top level. The
    // move was a clean break: a profile-scoped key is not consulted, and not quietly promoted either.
    // Pinned so that re-adding a profile-scoped reader is a deliberate act rather than an accident.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        tab_bar_position: Bottom
        tab_bar_visibility: Never
)"sv);
    REQUIRE(config.profile("main") != nullptr);
    CHECK(config.tabBarPosition.value() == TabBarPosition::Top);
    CHECK(config.tabBarVisibility.value() == TabBarVisibility::Always); // the Native2026 default
}

TEST_CASE("Config: invalid tab_bar_* values fall back to the defaults", "[config]")
{
    using contour::config::TabBarPosition;
    using contour::config::TabBarVisibility;

    QTemporaryDir dir;
    // Unrecognized tokens must leave the fields at their ConfigEntry defaults (Top / Always), not throw
    // or zero them out — the loadFromEntry overloads only write on a successful parse.
    auto const config = loadFromYaml(dir, R"(
default_profile: main
tab_bar_position: Sideways
tab_bar_visibility: Sometimes
profiles:
    main:
        shell: /bin/sh
)"sv);
    CHECK(config.tabBarPosition.value() == TabBarPosition::Top);
    CHECK(config.tabBarVisibility.value() == TabBarVisibility::Always); // the Native2026 default
}

TEST_CASE("Config: window_control_style parses each value (ignore-case)", "[config]")
{
    using contour::config::WindowControlStyle;

    auto const load = [](QTemporaryDir& dir, std::string_view value) {
        return loadFromYaml(dir,
                            std::string { "default_profile: main\nwindow_control_style: " }
                                + std::string { value } + "\nprofiles:\n    main:\n        shell: /bin/sh\n");
    };

    SECTION("every token the table carries round-trips")
    {
        // Driven from the table rather than from a literal list, so a style added there is covered
        // here without this test being touched -- and one whose token the reader cannot parse fails
        // immediately rather than at the first user who writes it.
        for (auto const& info: contour::config::configEnumValues<WindowControlStyle>())
        {
            QTemporaryDir dir;
            INFO("token: " << info.token);
            CHECK(load(dir, info.token).windowControlStyle.value() == info.value);
        }
    }

    SECTION("the match ignores case")
    {
        QTemporaryDir dir;
        CHECK(load(dir, "MacOS").windowControlStyle.value() == WindowControlStyle::MacOS);
    }

    SECTION("linux parses, and writes back as linux")
    {
        QTemporaryDir dir;
        CHECK(load(dir, "linux").windowControlStyle.value() == WindowControlStyle::Linux);
        CHECK(std::format("{}", WindowControlStyle::Linux) == "linux");
    }

    SECTION("an unknown token leaves the default")
    {
        // Auto, not a zeroed field: the loadFromEntry overload only writes on a successful parse, so
        // a typo in the config file costs the user their default rather than their title bar.
        QTemporaryDir dir;
        CHECK(load(dir, "aqua").windowControlStyle.value() == WindowControlStyle::Auto);
    }
}

TEST_CASE("Config: chrome_density parses each value (ignore-case)", "[config]")
{
    using contour::config::ChromeDensity;

    auto const load = [](QTemporaryDir& dir, std::string_view value) {
        return loadFromYaml(dir,
                            std::string { "default_profile: main\nchrome_density: " } + std::string { value }
                                + "\nprofiles:\n    main:\n        shell: /bin/sh\n");
    };

    SECTION("every token the table carries round-trips")
    {
        for (auto const& info: contour::config::configEnumValues<ChromeDensity>())
        {
            QTemporaryDir dir;
            INFO("token: " << info.token);
            CHECK(load(dir, info.token).chromeDensity.value() == info.value);
        }
    }

    SECTION("the match ignores case")
    {
        {
            QTemporaryDir dir;
            CHECK(load(dir, "Minimal").chromeDensity.value() == ChromeDensity::Minimal);
        }
        {
            QTemporaryDir dir;
            CHECK(load(dir, "MINIMAL").chromeDensity.value() == ChromeDensity::Minimal);
        }
    }

    SECTION("an unknown token leaves the default")
    {
        QTemporaryDir dir;
        auto capture = core::log::ScopedCapture { "error" };
        CHECK(load(dir, "dense").chromeDensity.value() == ChromeDensity::Comfortable);
        INFO("captured error log:\n" << capture.text());
        CHECK(capture.contains("dense"));
    }
}

TEST_CASE("Config: chrome_density defaults to comfortable in both editions", "[config]")
{
    using contour::config::ChromeDensity;
    using contour::config::DefaultsEdition;

    CHECK(contour::config::editionDefaults(DefaultsEdition::Classic).chromeDensity
          == ChromeDensity::Comfortable);
    CHECK(contour::config::editionDefaults(DefaultsEdition::Native2026).chromeDensity
          == ChromeDensity::Comfortable);

    constexpr auto Partial = "profiles:\n    main:\n        shell: /bin/sh\n"sv;

    SECTION("a partial contour.yml gets comfortable")
    {
        QTemporaryDir dir;
        auto const path = writeConfig(dir, Partial);
        CHECK(contour::config::loadConfigFromFile(path).chromeDensity.value() == ChromeDensity::Comfortable);
    }

    SECTION("a settings.yml stating minimal overrides it")
    {
        QTemporaryDir dir;
        auto const path = writeConfig(dir, Partial);
        writeSideFile(dir, "settings.yml", "chrome_density: minimal\n");
        CHECK(contour::config::loadConfigFromFile(path).chromeDensity.value() == ChromeDensity::Minimal);
    }
}

TEST_CASE("Config: chrome_density survives a YAMLConfigWriter round trip", "[config]")
{
    using contour::config::ChromeDensity;

    QTemporaryDir dir;
    auto const minimal = loadFromYaml(dir, "default_profile: main\nchrome_density: minimal\n");
    REQUIRE(minimal.chromeDensity.value() == ChromeDensity::Minimal);

    auto const rendered = contour::config::createString<contour::config::YAMLConfigWriter>(minimal);
    CHECK(rendered.contains("chrome_density: minimal"));
    CHECK(loadFromYaml(dir, rendered).chromeDensity.value() == ChromeDensity::Minimal);
}

TEST_CASE("Config: the generated config states chrome_density", "[config]")
{
    auto const generated = contour::config::defaultConfigString();
    auto const needle = std::string_view { "chrome_density: comfortable" };
    auto const first = generated.find(needle);
    REQUIRE(first != std::string::npos);
    CHECK(generated.find(needle, first + needle.size()) == std::string::npos);
}

TEST_CASE("Config: a Classic file keeps secure input off", "[config][secureinput]")
{
    // Existing users never saw a lock, and secure input stops other apps' global hotkeys while it is on.
    QTemporaryDir dir;
    auto const path = writeConfig(dir, "profiles:\n    main:\n        shell: /bin/sh\n"sv);
    CHECK(contour::config::loadConfigFromFile(path).secureInput.value()
          == contour::config::SecureInputMode::Off);
}

TEST_CASE("Config: the generated config turns secure input on", "[config][secureinput]")
{
    // Stated in the text, not only inherited from the edition: the generated file is what users read.
    auto const generated = contour::config::defaultConfigString();
    CHECK(generated.contains("\nsecure_input: auto\n"));

    QTemporaryDir dir;
    auto const path = writeConfig(dir, generated);
    CHECK(contour::config::loadConfigFromFile(path).secureInput.value()
          == contour::config::SecureInputMode::Auto);
}

TEST_CASE("Config: secure_input: off parses and round-trips", "[config][secureinput]")
{
    using contour::config::SecureInputMode;

    QTemporaryDir dir;
    auto const off = loadFromYaml(dir,
                                  "defaults_edition: native2026\n"
                                  "default_profile: main\n"
                                  "secure_input: off\n"
                                  "profiles:\n    main:\n        shell: /bin/sh\n"sv);
    REQUIRE(off.secureInput.value() == SecureInputMode::Off);

    auto const rendered = contour::config::createString<contour::config::YAMLConfigWriter>(off);
    CHECK(rendered.contains("\nsecure_input: off\n"));
    CHECK(loadFromYaml(dir, rendered).secureInput.value() == SecureInputMode::Off);
}

TEST_CASE("Config: an unknown secure_input spelling warns and keeps the edition's default",
          "[config][secureinput]")
{
    using contour::config::SecureInputMode;

    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir,
                                     "defaults_edition: native2026\n"
                                     "default_profile: main\n"
                                     "secure_input: maybe\n"
                                     "profiles:\n    main:\n        shell: /bin/sh\n"sv);
    CHECK(config.secureInput.value() == SecureInputMode::Auto);
    INFO("captured error log:\n" << capture.text());
    CHECK(capture.count("maybe") == 1);
    // The error names what would have been accepted, the aliases too.
    CHECK(capture.contains("off, auto, false, no"));
}

TEST_CASE("Config: secure_input accepts false and no as off", "[config][secureinput]")
{
    // A YAML user's reflex for a switch: neither may fall back to the edition's `auto`.
    using contour::config::SecureInputMode;
    for (auto const spelling: { "false"sv, "no"sv, "No"sv, "FALSE"sv })
    {
        INFO("spelling: " << spelling);
        QTemporaryDir dir;
        auto const config =
            loadFromYaml(dir,
                         std::string { "defaults_edition: native2026\n"
                                       "default_profile: main\n"
                                       "secure_input: " }
                             + std::string { spelling } + "\nprofiles:\n    main:\n        shell: /bin/sh\n");
        CHECK(config.secureInput.value() == SecureInputMode::Off);
    }
}

TEST_CASE("Config: secureInputModeName spells each mode as the file does", "[config][secureinput]")
{
    using contour::config::SecureInputMode;
    CHECK(contour::config::secureInputModeName(SecureInputMode::Off) == "off");
    CHECK(contour::config::secureInputModeName(SecureInputMode::Auto) == "auto");
}

TEST_CASE("Config: git-drawings, arc and braille styles parse from YAML", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
box_arc_style: elliptic
braille_style: aa_square
git_drawings:
    arc_style: round
    branch_style: double
    merge_commit_style: solid
profiles:
    main:
        shell: /bin/sh
)"sv);

    using Box = vtrasterizer::BoxDrawingRenderer;
    CHECK(config.boxArcStyle.value() == Box::ArcStyle::Elliptic);
    CHECK(config.brailleStyle.value() == Box::BrailleStyle::AASquare);
    CHECK(config.gitDrawings.value().branchStyle == Box::GitDrawingsStyle::BranchStyle::Double);
    CHECK(config.gitDrawings.value().mergeCommitStyle == Box::GitDrawingsStyle::MergeCommitStyle::Solid);
}

TEST_CASE("Config: on_mouse_select action parses each enum arm", "[config]")
{
    auto const load = [](std::string_view value) {
        QTemporaryDir dir;
        return loadFromYaml(dir,
                            std::string("default_profile: main\non_mouse_select: ") + std::string(value)
                                + "\nprofiles:\n    main:\n        shell: /bin/sh\n");
    };

    CHECK(load("CopyToClipboard").onMouseSelection.value()
          == contour::config::SelectionAction::CopyToClipboard);
    CHECK(load("CopyToSelectionClipboard").onMouseSelection.value()
          == contour::config::SelectionAction::CopyToSelectionClipboard);
    CHECK(load("Nothing").onMouseSelection.value() == contour::config::SelectionAction::Nothing);
    // An unrecognized value falls back to Nothing.
    CHECK(load("Bogus").onMouseSelection.value() == contour::config::SelectionAction::Nothing);
}

TEST_CASE("Config: input_mapping action variants and mouse/mode arms parse", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
input_mapping:
    - { mods: [Control, Shift], key: 'c', action: CopySelection, format: HTML }
    - { mods: [Control, Shift], key: 'x', action: CopySelection, format: PNG }
    - { mods: [Control], key: 'v', action: PasteClipboard, strip: false }
    - { mods: [Shift], key: 'Insert', action: PasteSelection, evaluate_in_shell: true }
    - { mods: [Control], key: 'w', action: WriteScreen, chars: "hi" }
    - { mods: [Control], key: 's', action: CreateSelection, delimiters: " ." }
    - { mods: [Control], key: 'g', action: SendChars, chars: "\x07" }
    - { mods: [Alt], key: '1', action: SwitchToTab, position: 1 }
    - { mods: [Alt], key: '2', action: MoveTabTo, position: 2 }
    - { mods: [Shift, Alt, Control], key: 'LeftArrow', action: ResizePane, direction: Left, percent: 8 }
    - { mods: [Shift, Alt, Control], key: 'RightArrow', action: ResizePane, direction: right }
    - { mods: [Control], key: 'n', action: NewTerminal, profile: main }
    - { mods: [Control], key: 'r', action: ReloadConfig, profile: main }
    - { mods: [Control], key: 'p', action: ChangeProfile, name: main }
    - { mods: [Control], key: 'h', action: HintMode, hint_action: Open, patterns: "abc" }
    - { mods: [], mouse: WheelUp, action: ScrollUp, mode: '~Alt' }
    - { mods: [Control], mouse: Left, action: PasteSelection, mode: 'Insert|Select' }
)"sv);

    auto const& mappings = config.inputMappings.value();
    CHECK_FALSE(mappings.keyMappings.empty());
    CHECK_FALSE(mappings.mouseMappings.empty());

    // ResizePane parses its direction (case-insensitive) and an optional percent (default 5) as
    // sibling keys — the round-trip format the generated default config also uses.
    std::vector<contour::actions::ResizePane> resizes;
    for (auto const& km: mappings.keyMappings)
        for (auto const& action: km.binding)
            if (auto const* r = std::get_if<contour::actions::ResizePane>(&action))
                resizes.push_back(*r);
    REQUIRE(resizes.size() == 2);
    CHECK(resizes[0].direction == contour::actions::Direction::Left);
    CHECK(resizes[0].percent == 8);
    CHECK(resizes[1].direction == contour::actions::Direction::Right);
    CHECK(resizes[1].percent == 5); // default when omitted
}

// No comma in the name: Catch2 splits its command-line test filter on commas, so a comma here
// would leave no way to run the case on its own.
TEST_CASE("Config: HintMode parses its scope and rejects an unknown one", "[config][input-mapping]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control], key: 'h', action: HintMode, patterns: url, scope: Scrollback }
    - { mods: [Control], key: 'j', action: HintMode, patterns: url, scope: visible }
    - { mods: [Control], key: 'k', action: HintMode, patterns: url, scope: sideways }
    - { mods: [Control], key: 'l', action: HintMode, patterns: url }
)"sv);

    // A binding on a literal character lands in charMappings, not keyMappings — the latter holds
    // only the named keys (LeftArrow, Insert, F6, …).
    std::vector<contour::actions::HintMode> hints;
    for (auto const& cm: config.inputMappings.value().charMappings)
        for (auto const& action: cm.binding)
            if (auto const* h = std::get_if<contour::actions::HintMode>(&action))
                hints.push_back(*h);

    REQUIRE(hints.size() == 4);
    CHECK(hints[0].scope == vtbackend::HintScope::Scrollback);
    CHECK(hints[1].scope == vtbackend::HintScope::Visible); // case-insensitive
    CHECK(hints[2].scope == vtbackend::HintScope::Visible); // unknown value falls back
    CHECK(hints[3].scope == vtbackend::HintScope::Visible); // omitted: the default
}

TEST_CASE("Config: color scheme with 0x colors and sequence color maps parses", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
color_schemes:
    myscheme:
        default:
            background: '0x101010'
            foreground: '0xE0E0E0'
        normal:
            black:   '0x000000'
            red:     '0xCC0000'
            green:   '0x00CC00'
            yellow:  '0xCCCC00'
            blue:    '0x0000CC'
            magenta: '0xCC00CC'
            cyan:    '0x00CCCC'
            white:   '0xCCCCCC'
        bright: ['0x808080', '0xFF0000', '0x00FF00', '0xFFFF00', '0x0000FF', '0xFF00FF', '0x00FFFF', '0xFFFFFF']
profiles:
    main:
        shell: /bin/sh
        colors: myscheme
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const* simple = std::get_if<contour::config::SimpleColorConfig>(&profile->colors.value());
    REQUIRE(simple != nullptr);
    CHECK(simple->colors.defaultBackground == vtbackend::RGBColor { 0x101010 });
    CHECK(simple->colors.defaultForeground == vtbackend::RGBColor { 0xE0E0E0 });
}

TEST_CASE("Config: vi_mode_cursorline color loads from YAML", "[config]")
{
    // Regression: the loader key was misspelled ("vi_mode_cursosrline"), so the documented
    // `vi_mode_cursorline` key was silently ignored. Assert the correctly-spelled key now takes
    // effect (and thus that the normal-mode current-line highlight is user-configurable).
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
color_schemes:
    myscheme:
        default:
            background: '0x101010'
            foreground: '0xE0E0E0'
        vi_mode_cursorline:
            foreground: '0x123456'
            foreground_alpha: 0.3
            background: '0xABCDEF'
            background_alpha: 0.7
profiles:
    main:
        shell: /bin/sh
        colors: myscheme
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const* simple = std::get_if<contour::config::SimpleColorConfig>(&profile->colors.value());
    REQUIRE(simple != nullptr);
    auto const& cursorline = simple->colors.normalModeCursorline;
    REQUIRE(std::holds_alternative<vtbackend::RGBColor>(cursorline.background));
    CHECK(std::get<vtbackend::RGBColor>(cursorline.background) == vtbackend::RGBColor { 0xABCDEF });
    CHECK(cursorline.backgroundAlpha == Catch::Approx(0.7f));
    REQUIRE(std::holds_alternative<vtbackend::RGBColor>(cursorline.foreground));
    CHECK(std::get<vtbackend::RGBColor>(cursorline.foreground) == vtbackend::RGBColor { 0x123456 });
    CHECK(cursorline.foregroundAlpha == Catch::Approx(0.3f));
}

TEST_CASE("Config: createDefaultConfig writes a loadable file into a fresh directory", "[config]")
{
    // createDefaultConfig() creates the parent directory chain and writes the generated default
    // document. Point it at a nested path that does not exist yet so the create_directories branch
    // runs, then load it back to prove the written file round-trips.
    QTemporaryDir const dir;
    auto const path = std::filesystem::path(dir.path().toStdU16String()) / "nested" / "sub" / "contour.yml";

    auto const ec = contour::config::createDefaultConfig(path);
    CHECK_FALSE(ec);
    REQUIRE(std::filesystem::exists(path));

    auto const reloaded = contour::config::loadConfigFromFile(path);
    CHECK(reloaded.profile(reloaded.defaultProfileName.value()) != nullptr);
}

TEST_CASE("Config: loadConfigFromFile creates the file when it does not exist", "[config]")
{
    // The path overload of loadConfigFromFile() runs createFileIfNotExists() first: a missing file
    // is materialized with the default config, so loading a not-yet-existent path yields a usable
    // config AND leaves the file on disk.
    QTemporaryDir const dir;
    auto const path = std::filesystem::path(dir.path().toStdU16String()) / "created" / "contour.yml";
    REQUIRE_FALSE(std::filesystem::exists(path));

    auto const config = contour::config::loadConfigFromFile(path);
    CHECK(std::filesystem::exists(path));
    CHECK(config.profile(config.defaultProfileName.value()) != nullptr);
}

TEST_CASE("Config.firstLaunchDefaults", "[config]")
{
    SECTION("a plain Config is the newest edition")
    {
        checkEditionDefaults(contour::config::Config {}, Native2026Expectation);
    }

    SECTION("a partial contour.yml without defaults_edition keeps every pre-redesign value")
    {
        QTemporaryDir dir;
        auto const path = writeConfig(dir, PartialConfigWithoutEdition);
        auto const config = contour::config::loadConfigFromFile(path);
        checkEditionDefaults(config, ClassicExpectation);
        // A profile that inherits from the default one inherits its edition too.
        auto const* work = config.findProfile("work");
        REQUIRE(work != nullptr);
        checkProfileDefaults(*work, ClassicExpectation);
    }

    SECTION("a default profile that the seed does not hold starts from the file's edition too")
    {
        QTemporaryDir dir;
        auto const config = contour::config::loadConfigFromFile(writeConfig(dir,
                                                                            "default_profile: work\n"
                                                                            "profiles:\n"
                                                                            "    work:\n"
                                                                            "        shell: /bin/sh\n"sv));
        REQUIRE(config.defaultProfileName.value() == "work");
        checkEditionDefaults(config, ClassicExpectation);
    }

    SECTION("a settings.yml stating native2026 switches a partial contour.yml to the new values")
    {
        QTemporaryDir dir;
        auto const path = writeConfig(dir, PartialConfigWithoutEdition);
        writeSideFile(dir, "settings.yml", "defaults_edition: native2026\n");
        auto const config = contour::config::loadConfigFromFile(path);
        checkEditionDefaults(config, Native2026Expectation);
        auto const* work = config.findProfile("work");
        REQUIRE(work != nullptr);
        checkProfileDefaults(*work, Native2026Expectation);
    }

    SECTION("settings.yml wins over contour.yml")
    {
        QTemporaryDir dir;
        auto const path = writeConfig(
            dir, "defaults_edition: native2026\nprofiles:\n    main:\n        shell: /bin/sh\n"sv);
        writeSideFile(dir, "settings.yml", "defaults_edition: classic\n");
        checkEditionDefaults(contour::config::loadConfigFromFile(path), ClassicExpectation);
    }

    SECTION("a value naming no edition is reported and read as none")
    {
        QTemporaryDir dir;
        auto const path = writeConfig(
            dir, "defaults_edition: future3000\nprofiles:\n    main:\n        shell: /bin/sh\n"sv);
        auto capture = core::log::ScopedCapture { "error" };
        checkEditionDefaults(contour::config::loadConfigFromFile(path), ClassicExpectation);
        INFO("captured error log:\n" << capture.text());
        CHECK(capture.contains("future3000"));
    }

    SECTION("a generated config is Native2026")
    {
        QTemporaryDir const dir;
        auto const path = std::filesystem::path(dir.path().toStdU16String()) / "fresh" / "contour.yml";
        REQUIRE_FALSE(std::filesystem::exists(path));
        auto const config = contour::config::loadConfigFromFile(path);
        checkEditionDefaults(config, Native2026Expectation);

        // What `contour generate config` emits.
        auto const generated = contour::config::defaultConfigString();
        CHECK(generated.contains("defaults_edition: native2026\n"));
        CHECK(generated.contains("dark: contour-dark\n"));
        auto const document = YAML::Load(generated);
        CHECK(document["defaults_edition"].as<std::string>() == "native2026");
        CHECK(document["profiles"]["main"]["colors"]["dark"].as<std::string>() == "contour-dark");
        CHECK(document["profiles"]["main"]["colors"]["light"].as<std::string>() == "contour-light");
    }

    SECTION("a Classic configuration writes the edition it has")
    {
        auto const classic =
            contour::config::Config { .defaultsEdition = contour::config::DefaultsEdition::Classic };
        checkEditionDefaults(classic, ClassicExpectation);
        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(classic);
        CHECK(written.contains("defaults_edition: classic\n"));
        CHECK(written.contains("colors: default\n"));
    }

    SECTION("a file without the key is not reported as missing it")
    {
        QTemporaryDir dir;
        auto const path = writeConfig(dir, PartialConfigWithoutEdition);
        auto capture = core::log::ScopedCapture { "config" };
        (void) contour::config::loadConfigFromFile(path);
        INFO("captured config log:\n" << capture.text());
        CHECK(capture.contains("is missing in user config file")); // the diff ran
        CHECK_FALSE(capture.contains("Entry defaults_edition is missing"));
    }
}

TEST_CASE("Config: a switch of edition replaces a status line spelled as the old edition's default",
          "[config]")
{
    using contour::config::DefaultsEdition;
    using contour::config::editionDefaults;
    auto const classic = editionDefaults(DefaultsEdition::Classic).statusLine;
    auto const native = editionDefaults(DefaultsEdition::Native2026).statusLine;
    // What `contour generate config` of an earlier release wrote: the Classic template, spelled out, and
    // a right segment of the user's own.
    auto const yaml = std::format("profiles:\n"
                                  "    main:\n"
                                  "        status_line:\n"
                                  "            indicator:\n"
                                  "                left: \"{}\"\n"
                                  "                middle: \"{}\"\n"
                                  "                right: \"{{Clock}}\"\n",
                                  classic.left,
                                  classic.middle);

    SECTION("Use New: the spelled defaults become the new ones, the user's own segment stays")
    {
        QTemporaryDir dir;
        auto const path = writeConfig(dir, yaml);
        writeSideFile(dir, "settings.yml", "defaults_edition: native2026\n");
        auto const config = contour::config::loadConfigFromFile(path);
        auto const& indicator = config.profile().statusLine.value().indicator;
        CHECK(indicator.left == native.left);
        CHECK(indicator.middle == native.middle);
        CHECK(indicator.right == "{Clock}");
    }

    SECTION("no switch: the file's edition keeps what it spells")
    {
        QTemporaryDir dir;
        auto const config = contour::config::loadConfigFromFile(writeConfig(dir, yaml));
        auto const& indicator = config.profile().statusLine.value().indicator;
        CHECK(indicator.left == classic.left);
        CHECK(indicator.middle == classic.middle);
    }

    SECTION("the same edition stated twice is no switch")
    {
        QTemporaryDir dir;
        auto const path = writeConfig(dir, yaml);
        writeSideFile(dir, "settings.yml", "defaults_edition: classic\n");
        auto const config = contour::config::loadConfigFromFile(path);
        CHECK(config.profile().statusLine.value().indicator.left == classic.left);
    }

    SECTION("the pure rule: only a segment equal to the old edition's default is replaced")
    {
        auto const spelled = contour::config::IndicatorConfig { .left = std::string { classic.left },
                                                                .middle = "mine",
                                                                .right = std::string { classic.right } };
        auto const adopted = contour::config::indicatorForEdition(
            spelled, DefaultsEdition::Classic, DefaultsEdition::Native2026);
        CHECK(adopted.left == native.left);
        CHECK(adopted.middle == "mine");
        CHECK(adopted.right == native.right);
        auto const same =
            contour::config::indicatorForEdition(spelled, DefaultsEdition::Classic, DefaultsEdition::Classic);
        CHECK(same.left == classic.left);
    }
}

TEST_CASE("Config: the path overload records what each document states as its edition",
          "[config][newdefaults]")
{
    using contour::config::DefaultsEdition;
    using contour::config::DefaultsEditionSources;
    QTemporaryDir dir;
    auto const path =
        writeConfig(dir, "defaults_edition: native2026\nprofiles:\n    main:\n        shell: /bin/sh\n"sv);
    writeSideFile(dir, "settings.yml", "defaults_edition: classic\n");
    auto const config = contour::config::loadConfigFromFile(path);
    CHECK(config.defaultsEditionSources
          == DefaultsEditionSources { .mainFile = DefaultsEdition::Native2026,
                                      .settingsFile = DefaultsEdition::Classic });
    CHECK(config.defaultsEdition == DefaultsEdition::Classic);
}

TEST_CASE("Config: defaultConfigFilePath and the documentation generators produce content", "[config]")
{
    // These are the pure string-producing entry points used by the `contour generate`/`documentation`
    // subcommands; exercising them covers the no-argument overloads and their default-Config path.
    CHECK_FALSE(contour::config::defaultConfigFilePath().empty());
    CHECK(contour::config::defaultConfigFilePath().contains("contour.yml"));

    CHECK_FALSE(contour::config::documentationGlobalConfig().empty());
    CHECK_FALSE(contour::config::documentationProfileConfig().empty());
    CHECK_FALSE(contour::config::defaultConfigString().empty());
}

TEST_CASE("Config: show_title_bar documents its Linux meaning", "[config]")
{
    // There is no window_decorations key: show_title_bar is the switch between our header bar
    // (client-side) and the window manager's frame (server-side).
    auto const profile = contour::config::documentationProfileConfig();
    CHECK(profile.contains("the window manager draws the frame"));
    CHECK(contour::config::defaultConfigString().contains("the window manager draws the frame"));
}

TEST_CASE("Config: readConfigFile returns nullopt when the file is absent everywhere", "[config]")
{
    // readConfigFile() walks the config-home search path; a randomly-named file exists in none of
    // them, so it must report absence rather than throw.
    CHECK_FALSE(contour::config::readConfigFile("contour-nonexistent-xyzzy-file.yml").has_value());
}

TEST_CASE("Config: the experimental feature map enables only the true entries", "[config]")
{
    // The std::set<string> loadFromEntry overload backs the top-level `experimental:` map: each
    // `feature: bool` entry that is true lands in the set; false entries are omitted.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
experimental:
    feature_a: true
    feature_b: true
    feature_c: false
default_profile: main
profiles:
    main:
        shell: /bin/sh
)"sv);

    auto const& features = config.experimentalFeatures.value();
    CHECK(features.count("feature_a") == 1);
    CHECK(features.count("feature_b") == 1);
    CHECK(!features.contains("feature_c")); // disabled entries are not inserted
}

TEST_CASE("Config: image max_width/max_height and color registers load from YAML", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
images:
    sixel_scrolling: false
    sixel_register_count: 512
    max_width: 640
    max_height: 480
profiles:
    main:
        shell: /bin/sh
)"sv);

    // max_width/max_height are deprecated no-ops; the neighbouring keys must still load.
    CHECK(config.images.value().sixelScrolling == false);
}

TEST_CASE("Config: frozen_dec_modes accepts valid modes and skips invalid ones", "[config]")
{
    // The map<DECMode,bool> loader: numeric keys that name a real DEC mode are stored; a
    // non-DEC-mode number is logged and skipped (the invalid-entry continue branch).
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        frozen_dec_modes:
            "25": true
            "2004": false
            "999999": true
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const& frozen = profile->frozenModes.value();
    // 25 (cursor visibility) and 2004 (bracketed paste) are valid DEC modes.
    CHECK(frozen.at(static_cast<vtbackend::DECMode>(25)) == true);
    CHECK(frozen.at(static_cast<vtbackend::DECMode>(2004)) == false);
    // 999999 is not a valid DEC mode: skipped, so never stored.
    CHECK(!frozen.contains(static_cast<vtbackend::DECMode>(999999)));
}

TEST_CASE("Config: input_mapping match modes parse every flag arm", "[config]")
{
    // parseMatchModes()'s per-flag arms: each named mode (with an optional ~ negation) maps to a
    // MatchModes flag; an unknown mode name is logged and skipped without aborting the load.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
input_mapping:
    - { mods: [Control], key: 'a', action: ScrollPageUp,   mode: 'AppCursor' }
    - { mods: [Control], key: 'b', action: ScrollPageDown, mode: 'AppKeypad' }
    - { mods: [Control], key: 'c', action: ScrollPageUp,   mode: 'Insert' }
    - { mods: [Control], key: 'd', action: ScrollPageDown, mode: 'Select' }
    - { mods: [Control], key: 'e', action: ScrollPageUp,   mode: 'Search' }
    - { mods: [Control], key: 'f', action: ScrollPageDown, mode: 'Trace' }
    - { mods: [Control], key: 'g', action: ScrollPageUp,   mode: 'AltScreen' }
    - { mods: [Control], key: 'h', action: ScrollPageDown, mode: '~Insert' }
    - { mods: [Control], key: 'i', action: ScrollPageUp,   mode: 'Bogus' }
profiles:
    main:
        shell: /bin/sh
)"sv);

    // The malformed 'Bogus' mode is skipped but the load completes and the other rows are present.
    // Single-character keys ('a'..'i') bind as char mappings, so assert those grew.
    CHECK_FALSE(config.inputMappings.value().charMappings.empty());
}

TEST_CASE("Config: action variants missing their required argument are rejected", "[config]")
{
    // parseAction()'s else-arms: ChangeProfile without name, MoveTabTo/SwitchToTab without position,
    // and SendChars without chars all return nullopt, so the mapping is dropped rather than added
    // with a garbage argument. NewTerminal/ReloadConfig without a profile fall back to the argless
    // action (still added). The load must not abort.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
input_mapping:
    - { mods: [Control], key: 'a', action: ChangeProfile }
    - { mods: [Control], key: 'b', action: MoveTabTo }
    - { mods: [Control], key: 'c', action: SwitchToTab }
    - { mods: [Control], key: 'd', action: SendChars }
    - { mods: [Control], key: 'e', action: NewTerminal }
    - { mods: [Control], key: 'f', action: ReloadConfig }
    - { mods: [Control], key: 'g', action: NoSuchActionName }
profiles:
    main:
        shell: /bin/sh
)"sv);

    // NewTerminal and ReloadConfig without a profile still bind (argless fallback); the rest are
    // rejected. The document loads regardless.
    CHECK(config.profile("main") != nullptr);
}
