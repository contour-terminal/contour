// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the YAML configuration loader (Config.cpp) — the largest previously untested
// surface of the GUI layer. Each case writes an inline YAML document to a temp file, loads it
// through the production loadConfigFromFile(), and asserts the parsed ConfigEntry values, so a
// renamed/retyped YAML key or a broken loadFromEntry overload fails here instead of silently
// falling back to defaults at runtime.

#include <contour/Actions.h>
#include <contour/Config.h>
#include <contour/GuiConfigStore.h>

#include <vtbackend/Color.h>
#include <vtbackend/primitives.h>

#include <QtCore/QTemporaryDir>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>
#include <variant>
#include <vector>

using namespace std::string_view_literals;

namespace
{

/// Writes @p yaml to a fresh file inside @p dir and returns its path.
[[nodiscard]] std::filesystem::path writeConfig(QTemporaryDir& dir, std::string_view yaml)
{
    auto const path = std::filesystem::path(dir.path().toStdString()) / "contour.yml";
    {
        auto out = std::ofstream(path);
        out << yaml;
    }
    return path;
}

/// Loads a config from inline YAML through the production file loader.
[[nodiscard]] contour::config::Config loadFromYaml(QTemporaryDir& dir, std::string_view yaml)
{
    auto config = contour::config::Config {};
    contour::config::loadConfigFromFile(config, writeConfig(dir, yaml));
    return config;
}

/// Writes @p content to a GUI side file at @p relativePath under @p dir (the config directory),
/// creating intermediate directories (e.g. `profiles/`, `colorschemes/`) as needed.
void writeSideFile(QTemporaryDir& dir, std::filesystem::path const& relativePath, std::string_view content)
{
    auto const path = std::filesystem::path(dir.path().toStdString()) / relativePath;
    std::filesystem::create_directories(path.parent_path());
    auto out = std::ofstream(path);
    out << content;
}

} // namespace

TEST_CASE("Config: global scalars load from YAML", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
early_exit_threshold: 11
live_config: true
reflow_on_resize: false
profiles:
    main:
        shell: /bin/sh
)"sv);

    CHECK(config.defaultProfileName.value() == "main");
    CHECK(config.earlyExitThreshold.value() == 11);
    CHECK(config.live.value() == true);
    CHECK(config.reflowOnResize.value() == false);
}

TEST_CASE("Config: profile knobs load from YAML", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        arguments: ['-c', 'true']
        show_title_bar: true
        dim_unfocused: 0.25
        size_indicator_on_resize: false
        fullscreen: true
        maximized: true
        terminal_size:
            columns: 132
            lines: 43
        margins:
            horizontal: 6
            vertical: 3
        font:
            size: 14
        bell:
            sound: "off"
            alert: false
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(profile->shell.value().program == "/bin/sh");
    REQUIRE(profile->shell.value().arguments.size() == 2);
    CHECK(profile->shell.value().arguments[0] == "-c");
    CHECK(profile->showTitleBar.value() == true);
    CHECK(profile->dimUnfocused.value() == 0.25);
    CHECK(profile->sizeIndicatorOnResize.value() == false);
    CHECK(profile->fullscreen.value() == true);
    CHECK(profile->maximized.value() == true);
    CHECK(profile->terminalSize.value().columns == vtbackend::ColumnCount(132));
    CHECK(profile->terminalSize.value().lines == vtbackend::LineCount(43));
    CHECK(profile->margins.value().horizontal.value == 6);
    CHECK(profile->margins.value().vertical.value == 3);
    CHECK(profile->fonts.value().size.pt == 14.0);
    CHECK(profile->bell.value().sound == "off");
    CHECK(profile->bell.value().alert == false);
}

TEST_CASE("Config: a layout with plain leaf tabs parses", "[config][layout]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
layouts:
    work:
        tabs:
            - title: "editor"
              color: "#d75f00"
              directory: "/tmp"
              command: "nvim"
              arguments: ["."]
            - title: "claude"
              command: "claude"
)"sv);

    auto const& layouts = config.layouts.value();
    REQUIRE(layouts.contains("work"));
    auto const& work = layouts.at("work");
    REQUIRE(work.tabs.size() == 2);

    auto const& t0 = work.tabs[0];
    CHECK(t0.title == "editor");
    REQUIRE(t0.color.has_value());
    CHECK(*t0.color == vtbackend::RGBColor { "#d75f00" });
    CHECK(t0.root.isLeaf());
    REQUIRE(t0.root.command.has_value());
    CHECK(*t0.root.command == "nvim");
    REQUIRE(t0.root.arguments.size() == 1);
    CHECK(t0.root.arguments[0] == ".");
    REQUIRE(t0.root.directory.has_value());
    CHECK(t0.root.directory->generic_string() == "/tmp");

    auto const& t1 = work.tabs[1];
    CHECK(t1.title == "claude");
    CHECK(t1.root.isLeaf());
    CHECK(*t1.root.command == "claude");
    CHECK_FALSE(t1.root.directory.has_value());
}

TEST_CASE("Config: shellSplit tokenizes a command line respecting quotes", "[config][layout]")
{
    using contour::config::shellSplit;
    using V = std::vector<std::string>;
    CHECK(shellSplit("emacs -nw") == V { "emacs", "-nw" });
    CHECK(shellSplit("  git   log --oneline ") == V { "git", "log", "--oneline" });
    CHECK(shellSplit("nvim") == V { "nvim" });
    CHECK(shellSplit("") == V {});
    CHECK(shellSplit("   ") == V {});
    // Double quotes group spaces into one token; the quotes themselves are stripped.
    CHECK(shellSplit(R"("/opt/my app/emacs" -nw)") == V { "/opt/my app/emacs", "-nw" });
    // Single quotes are literal (no inner processing).
    CHECK(shellSplit(R"(sh -c 'echo hi there')") == V { "sh", "-c", "echo hi there" });
    // A backslash escapes the next character outside quotes.
    CHECK(shellSplit(R"(a\ b c)") == V { "a b", "c" });
}

TEST_CASE("Config: a layout command is shell-split into program and arguments", "[config][layout]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
layouts:
    work:
        tabs:
            - command: "/usr/bin/emacs -nw"
            - command: "git log --oneline"
              arguments: ["--graph"]
)"sv);
    auto const& work = config.layouts.value().at("work");
    REQUIRE(work.tabs.size() == 2);

    // The command string is split: the first word is the program, the rest are arguments.
    REQUIRE(work.tabs[0].root.command.has_value());
    CHECK(*work.tabs[0].root.command == "/usr/bin/emacs");
    REQUIRE(work.tabs[0].root.arguments.size() == 1);
    CHECK(work.tabs[0].root.arguments[0] == "-nw");

    // Words from `command` come first, then any explicit `arguments:` entries are appended.
    CHECK(*work.tabs[1].root.command == "git");
    REQUIRE(work.tabs[1].root.arguments.size() == 3);
    CHECK(work.tabs[1].root.arguments[0] == "log");
    CHECK(work.tabs[1].root.arguments[1] == "--oneline");
    CHECK(work.tabs[1].root.arguments[2] == "--graph");
}

TEST_CASE("Config: a layout tab with recursive splits parses", "[config][layout]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
layouts:
    dev:
        tabs:
            - title: "servers"
              split:
                orientation: vertical
                panes:
                    - command: "npm run dev"
                      ratio: 0.6
                    - split:
                        orientation: horizontal
                        panes:
                            - command: "htop"
                            - command: "journalctl -f"
)"sv);

    auto const& tab = config.layouts.value().at("dev").tabs.at(0);
    auto const& root = tab.root;
    REQUIRE_FALSE(root.isLeaf());
    CHECK(root.orientation == vtmux::SplitState::Vertical);
    REQUIRE(root.children.size() == 2);

    CHECK(root.children[0].isLeaf());
    // `command` is shell-split into the program and its arguments.
    CHECK(*root.children[0].command == "npm");
    REQUIRE(root.children[0].arguments.size() == 2);
    CHECK(root.children[0].arguments[0] == "run");
    CHECK(root.children[0].arguments[1] == "dev");
    REQUIRE(root.children[0].ratio.has_value());
    CHECK(*root.children[0].ratio == Catch::Approx(0.6));
    // The unspecified sibling stays unset (it shares the remaining space at realization time).
    CHECK_FALSE(root.children[1].ratio.has_value());

    auto const& nested = root.children[1];
    REQUIRE_FALSE(nested.isLeaf());
    CHECK(nested.orientation == vtmux::SplitState::Horizontal);
    REQUIRE(nested.children.size() == 2);
    CHECK(*nested.children[0].command == "htop");
    CHECK(nested.children[0].arguments.empty());
    CHECK(*nested.children[1].command == "journalctl");
    REQUIRE(nested.children[1].arguments.size() == 1);
    CHECK(nested.children[1].arguments[0] == "-f");
}

TEST_CASE("Config: a split with a single child collapses into a plain leaf", "[config][layout]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
layouts:
    solo:
        tabs:
            - split:
                orientation: vertical
                panes:
                    - command: "solo"
)"sv);

    auto const& tab = config.layouts.value().at("solo").tabs.at(0);
    auto const& root = tab.root;
    // A single-child split has nothing to split against: it must behave as a plain leaf, not
    // spawn a bogus second (uncommanded) pane.
    REQUIRE(root.isLeaf());
    REQUIRE(root.command.has_value());
    CHECK(*root.command == "solo");
    CHECK(root.children.empty());
}

TEST_CASE("Config: default_layout scalar loads", "[config][layout]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_layout: work
layouts:
    work:
        tabs:
            - command: "bash"
)"sv);
    CHECK(config.defaultLayoutName.value() == "work");
}

TEST_CASE("Config: a sibling layouts.yml merges and overrides inline layouts", "[config][layout]")
{
    QTemporaryDir dir;
    auto const configPath = writeConfig(dir, R"(
layouts:
    work:
        tabs:
            - command: "inline-shell"
)"sv);
    {
        auto const siblingPath = std::filesystem::path(dir.path().toStdString()) / "layouts.yml";
        std::ofstream(siblingPath) << R"(
layouts:
    work:
        tabs:
            - command: "file-shell"
    extra:
        tabs:
            - command: "zsh"
)";
    }

    auto config = contour::config::Config {};
    contour::config::loadConfigFromFile(config, configPath);

    auto const& layouts = config.layouts.value();
    REQUIRE(layouts.contains("work"));
    REQUIRE(layouts.contains("extra"));
    // Sibling file wins the collision.
    CHECK(*layouts.at("work").tabs.at(0).root.command == "file-shell");
    CHECK(*layouts.at("extra").tabs.at(0).root.command == "zsh");
}

TEST_CASE("Config: a malformed layout field is skipped, not fatal to the rest of the config",
          "[config][layout]")
{
    // A single typo in a layout must not unwind the whole config load: entries parsed AFTER
    // `layouts` (most importantly input_mapping) and sibling layouts must still land.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
layouts:
    broken:
        tabs:
            - command: "top"
              ratio: half
    fine:
        tabs:
            - command: "bash"
input_mapping:
    - { mods: [Control], key: F5, action: LaunchLayout, name: fine }
)"sv);

    auto const& layouts = config.layouts.value();
    // The broken layout still parses; only its malformed ratio is dropped.
    REQUIRE(layouts.contains("broken"));
    REQUIRE(layouts.at("broken").tabs.size() == 1);
    CHECK(*layouts.at("broken").tabs.at(0).root.command == "top");
    CHECK_FALSE(layouts.at("broken").tabs.at(0).root.ratio.has_value());
    REQUIRE(layouts.contains("fine"));

    // input_mapping (loaded after layouts) must not have been skipped: the custom binding is
    // present in the key mappings.
    auto const& bindings = config.inputMappings.value().keyMappings;
    auto const hasLaunchLayoutBinding = std::ranges::any_of(bindings, [](auto const& mapping) {
        auto const* launch = std::get_if<contour::actions::LaunchLayout>(&mapping.binding.at(0));
        return launch != nullptr && launch->name == "fine";
    });
    CHECK(hasLaunchLayoutBinding);
}

TEST_CASE("Config: OpenConfiguration in_editor parameter parses", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
input_mapping:
    - { mods: [Control], key: F1, action: OpenConfiguration }
    - { mods: [Control], key: F2, action: OpenConfiguration, in_editor: true }
)"sv);

    // The defaults bind OpenConfiguration as CHAR mappings, so the two custom KEY mappings are ours.
    auto flags = std::vector<bool> {};
    for (auto const& mapping: config.inputMappings.value().keyMappings)
        if (auto const* oc = std::get_if<contour::actions::OpenConfiguration>(&mapping.binding.at(0)))
            flags.push_back(oc->inEditor);

    REQUIRE(flags.size() == 2);
    CHECK(std::ranges::count(flags, false) == 1); // default -> in-app settings dialog
    CHECK(std::ranges::count(flags, true) == 1);  // in_editor: true -> external editor
}

TEST_CASE("Config: an out-of-range or non-numeric ratio is ignored", "[config][layout]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
layouts:
    work:
        tabs:
            - split:
                orientation: vertical
                panes:
                    - { command: "a", ratio: 1.5 }
                    - { command: "b", ratio: 0.4 }
)"sv);
    auto const& root = config.layouts.value().at("work").tabs.at(0).root;
    REQUIRE(root.children.size() == 2);
    CHECK_FALSE(root.children[0].ratio.has_value()); // 1.5 is not a fraction of the split
    REQUIRE(root.children[1].ratio.has_value());
    CHECK(*root.children[1].ratio == Catch::Approx(0.4));
}

TEST_CASE("Config: duplicate layout names resolve to the later definition", "[config][layout]")
{
    // yaml-cpp yields duplicate map keys once each; a naive by-name re-lookup would parse the
    // FIRST definition twice, silently doubling its tabs.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
layouts:
    work:
        tabs:
            - command: "first"
            - command: "second"
    work:
        tabs:
            - command: "winner"
)"sv);
    auto const& layouts = config.layouts.value();
    REQUIRE(layouts.contains("work"));
    REQUIRE(layouts.at("work").tabs.size() == 1);
    CHECK(*layouts.at("work").tabs.at(0).root.command == "winner");
}

TEST_CASE("Config: split orientation parses case-insensitively", "[config][layout]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
layouts:
    work:
        tabs:
            - split:
                orientation: Horizontal
                panes:
                    - command: "a"
                    - command: "b"
            - split:
                orientation: bogus
                panes:
                    - command: "c"
                    - command: "d"
)"sv);
    auto const& tabs = config.layouts.value().at("work").tabs;
    REQUIRE(tabs.size() == 2);
    // Natural capitalization must work like every other enum-ish config value.
    CHECK(tabs.at(0).root.orientation == vtmux::SplitState::Horizontal);
    // An unknown value falls back to the vertical default (with a log line, not silently).
    CHECK(tabs.at(1).root.orientation == vtmux::SplitState::Vertical);
}

TEST_CASE("Config: an invalid tab color is ignored instead of turning black", "[config][layout]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
layouts:
    work:
        tabs:
            - title: "named"
              color: "green"
              command: "a"
            - title: "hex"
              color: "#00ff00"
              command: "b"
)"sv);
    auto const& tabs = config.layouts.value().at("work").tabs;
    REQUIRE(tabs.size() == 2);
    // RGBColor's string form only understands '#RRGGBB'; "green" would silently parse as black.
    CHECK_FALSE(tabs.at(0).color.has_value());
    REQUIRE(tabs.at(1).color.has_value());
    CHECK(*tabs.at(1).color == vtbackend::RGBColor { "#00ff00" });
}

TEST_CASE("Config: $${ escapes environment-variable expansion in layout commands", "[config][layout]")
{
    // A saved command containing literal ${...} text (sed or template tooling) is emitted
    // as $${...} by SaveLayout and must parse back to the literal text, not expand.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
layouts:
    work:
        tabs:
            - command: "sed"
              arguments: ["s/$${VERSION}/1.0/"]
)"sv);
    auto const& pane = config.layouts.value().at("work").tabs.at(0).root;
    REQUIRE(pane.arguments.size() == 1);
    CHECK(pane.arguments[0] == "s/${VERSION}/1.0/");
}

TEST_CASE("Config: a ratio on a nested split node parses", "[config][layout]")
{
    // The emitter writes `ratio:` on split children too (a nested split is itself a child of its
    // parent split); the parser must read it back rather than silently dropping it.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
layouts:
    work:
        tabs:
            - split:
                orientation: vertical
                panes:
                    - { command: "left", ratio: 0.7 }
                    - ratio: 0.3
                      split:
                        orientation: horizontal
                        panes:
                            - command: "top"
                            - command: "bottom"
)"sv);
    auto const& root = config.layouts.value().at("work").tabs.at(0).root;
    REQUIRE(root.children.size() == 2);
    REQUIRE(root.children[0].ratio.has_value());
    CHECK(*root.children[0].ratio == Catch::Approx(0.7));
    REQUIRE_FALSE(root.children[1].isLeaf());
    REQUIRE(root.children[1].ratio.has_value());
    CHECK(*root.children[1].ratio == Catch::Approx(0.3));
}

TEST_CASE("Config: color scheme with background image loads from YAML", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        colors: coverage
color_schemes:
    coverage:
        default:
            background: '#102030'
            foreground: '#D0D0D0'
        background_image:
            path: '/tmp/does-not-need-to-exist.png'
            opacity: 0.5
            blur: true
)"sv);

    // Color schemes load LAZILY: the profile's `colors:` reference resolves its named scheme out
    // of doc["color_schemes"] into the profile's own ColorConfig (Config.cpp documents this); the
    // global colorschemes map is not bulk-populated.
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    REQUIRE(std::holds_alternative<contour::config::SimpleColorConfig>(profile->colors.value()));
    auto const& simple = std::get<contour::config::SimpleColorConfig>(profile->colors.value());
    CHECK(simple.colorScheme == "coverage");
    auto const& scheme = simple.colors;
    CHECK(scheme.defaultBackground == vtbackend::RGBColor(0x10, 0x20, 0x30));
    CHECK(scheme.defaultForeground == vtbackend::RGBColor(0xD0, 0xD0, 0xD0));
    REQUIRE(scheme.backgroundImage != nullptr);
    // location is a variant<path, shared_ptr<const ImageData>>; the YAML loader always stores a path.
    REQUIRE(std::holds_alternative<std::filesystem::path>(scheme.backgroundImage->location));
    CHECK(std::get<std::filesystem::path>(scheme.backgroundImage->location)
          == std::filesystem::path("/tmp/does-not-need-to-exist.png"));
    CHECK(scheme.backgroundImage->opacity == 0.5f);
    CHECK(scheme.backgroundImage->blur == true);
}

TEST_CASE("Config: findProfile reports a missing profile instead of asserting", "[config]")
{
    // findProfile() is the fallible lookup for runtime input (a keybinding naming a removed
    // profile). Unlike profile(), a miss must return nullptr, not abort — the ChangeProfile action
    // relies on this to reject an unknown name gracefully.
    QTemporaryDir dir;
    auto config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
)"sv);

    CHECK(config.findProfile("main") != nullptr);
    CHECK(config.findProfile("no-such-profile") == nullptr);
    CHECK(std::as_const(config).findProfile("no-such-profile") == nullptr);
}

TEST_CASE("Config: the default-profile accessor resolves the configured profile", "[config]")
{
    // NB: profile(name) has an assert-on-missing PRECONDITION (looking up unknown names is a
    // programmer error by contract), so only the resolving path is exercised here.
    QTemporaryDir dir;
    auto config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
)"sv);

    CHECK(config.profile().shell.value().program == "/bin/sh");
}

TEST_CASE("Config: scrollbar, status line, history and permissions load from YAML", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        scrollbar:
            position: Right
            hide_in_alt_screen: false
        status_line:
            display: indicator
            position: Top
            sync_to_window_title: true
            indicator:
                left: "left {Clock}"
                middle: "mid"
                right: "right"
        history:
            limit: 200
            scroll_multiplier: 5
            auto_scroll_on_update: false
        permissions:
            change_font: allow
            capture_buffer: deny
            display_host_writable_statusline: ask
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);

    CHECK(profile->scrollbar.value().position == contour::config::ScrollBarPosition::Right);
    CHECK(profile->scrollbar.value().hideScrollbarInAltScreen == false);

    CHECK(profile->statusLine.value().initialType == vtbackend::StatusDisplayType::Indicator);
    CHECK(profile->statusLine.value().position == vtbackend::StatusDisplayPosition::Top);
    CHECK(profile->statusLine.value().syncWindowTitleWithHostWritableStatusDisplay == true);
    CHECK(profile->statusLine.value().indicator.left == "left {Clock}");

    // maxHistoryLineCount is a variant<LineCount, Infinite>: `limit: 200` selects the finite arm.
    REQUIRE(std::holds_alternative<vtbackend::LineCount>(profile->history.value().maxHistoryLineCount));
    CHECK(std::get<vtbackend::LineCount>(profile->history.value().maxHistoryLineCount)
          == vtbackend::LineCount(200));
    CHECK(profile->history.value().historyScrollMultiplier == vtbackend::LineCount(5));
    CHECK(profile->history.value().autoScrollOnUpdate == false);

    CHECK(profile->permissions.value().changeFont == contour::config::Permission::Allow);
    CHECK(profile->permissions.value().captureBuffer == contour::config::Permission::Deny);
    CHECK(profile->permissions.value().displayHostWritableStatusLine == contour::config::Permission::Ask);
}

TEST_CASE("Config: cursor, bell, mouse and modes load from YAML", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        mouse:
            hide_while_typing: false
        bell:
            sound: "off"
            alert: false
            volume: 0.5
        cursor:
            shape: bar
            blinking: false
        normal_mode:
            cursor:
                shape: underscore
                blinking: true
                blinking_interval: 400
        terminal_id: VT340
        draw_bold_text_with_bright_colors: true
        highlight_word_and_matches_on_double_click: false
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(profile->mouse.value().hideWhileTyping == false);
    CHECK(profile->bell.value().sound == "off");
    CHECK(profile->bell.value().alert == false);
    CHECK(profile->bell.value().volume == 0.5f);
    CHECK(profile->modeNormal.value().cursor.cursorShape == vtbackend::CursorShape::Underscore);
    CHECK(profile->modeNormal.value().cursor.cursorDisplay == vtbackend::CursorDisplay::Blink);
    CHECK(profile->modeInsert.value().cursor.cursorShape == vtbackend::CursorShape::Bar);
    CHECK(profile->modeInsert.value().cursor.cursorDisplay == vtbackend::CursorDisplay::Steady);
    CHECK(profile->terminalId.value() == vtbackend::VTType::VT340);
    CHECK(profile->drawBoldTextWithBrightColors.value() == true);
    CHECK(profile->highlightDoubleClickedWord.value() == false);
}

TEST_CASE("Config: font description loads family, size and text-shaping from YAML", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        font:
            size: 13
            locator: mock
            text_shaping:
                engine: complex
            builtin_box_drawing: false
            regular:
                family: "Fira Code"
                weight: bold
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(profile->fonts.value().size.pt == 13.0);
    CHECK(profile->fonts.value().regular.familyName == "Fira Code");
    CHECK(profile->fonts.value().builtinBoxDrawing == false);
}

TEST_CASE("Config: font features, fallback list, and a below-minimum size clamp load from YAML", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        font:
            size: 3
            regular:
                family: "Fira Code"
                features: ["+liga", "+calt", "-dlig"]
                fallback:
                    - "Noto Sans Mono"
                    - "DejaVu Sans Mono"
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    // A size below the 8pt minimum is clamped up (not rejected).
    CHECK(profile->fonts.value().size.pt >= 8.0);
    // The three 4-letter feature codes are parsed into the feature vector.
    CHECK(profile->fonts.value().regular.features.size() == 3);
    // The fallback sequence populates a font_fallback_list.
    REQUIRE(std::holds_alternative<text::font_fallback_list>(profile->fonts.value().regular.fontFallback));
    auto const& list = std::get<text::font_fallback_list>(profile->fonts.value().regular.fontFallback);
    CHECK(list.fallbackFonts.size() == 2);
}

TEST_CASE("Config: font fallback 'none' disables fallback", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        font:
            regular:
                family: "Fira Code"
                fallback: none
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(std::holds_alternative<text::font_fallback_none>(profile->fonts.value().regular.fontFallback));
}

TEST_CASE("Config: text-shaping engine names map to their engines", "[config]")
{
    // Regression guard: the engine lives under the nested `text_shaping:` map (as documented and
    // generated). A prior flat "text_shaping.engine" key lookup never matched, silently ignoring the
    // setting; these cases would all resolve to the default engine if that regressed.
    auto load = [](QTemporaryDir& dir, std::string_view engine) {
        return loadFromYaml(dir,
                            std::format(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        font:
            text_shaping:
                engine: {}
)",
                                        engine));
    };

    for (auto const& [name, expected]:
         std::vector<std::pair<std::string_view, vtrasterizer::TextShapingEngine>> {
             { "openshaper", vtrasterizer::TextShapingEngine::OpenShaper },
             { "dwrite", vtrasterizer::TextShapingEngine::DWrite },
             { "coretext", vtrasterizer::TextShapingEngine::CoreText },
         })
    {
        QTemporaryDir dir;
        auto const config = load(dir, name);
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        CHECK(profile->fonts.value().textShapingEngine == expected);
    }
}

TEST_CASE("Config: environment variables in values are expanded (defined and undefined)", "[config]")
{
    // Path/arg values run through the variable replacer: a defined ${VAR} expands to its value; an
    // undefined one expands to empty (and is logged). Set a known var so the defined branch is
    // deterministic; use a clearly-undefined name for the other branch. qputenv/qunsetenv are Qt's
    // portable env wrappers (POSIX ::setenv is unavailable on MSVC).
    qputenv("CONTOUR_CFG_TEST_VAR", QByteArray("expanded-value"));
    qunsetenv("CONTOUR_CFG_UNDEFINED_VAR");

    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        arguments: ["${CONTOUR_CFG_TEST_VAR}", "${CONTOUR_CFG_UNDEFINED_VAR}", "plain"]
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const& args = profile->shell.value().arguments;
    REQUIRE(args.size() == 3);
    CHECK(args[0] == "expanded-value"); // defined var expanded
    CHECK(args[1].empty());             // undefined var -> empty
    CHECK(args[2] == "plain");          // literal unchanged

    qunsetenv("CONTOUR_CFG_TEST_VAR");
}

TEST_CASE("Config: a malformed profile falls back to defaults without throwing", "[config]")
{
    // A structurally-invalid value for a typed field must be tolerated (logged + defaulted), never
    // abort config loading — a single bad key should not make the whole terminal unstartable.
    QTemporaryDir dir;
    CHECK_NOTHROW(loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        terminal_size:
            columns: not-a-number
            lines: 25
)"sv));
}

TEST_CASE("Config: an empty document yields a usable default config", "[config]")
{
    // Loading an empty file must still leave a working config (the built-in "main" profile), so a
    // brand-new user with an empty contour.yml gets a functioning terminal.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, "\n"sv);
    // The no-arg accessor returns the default profile by reference; it must resolve to a usable shell.
    CHECK_FALSE(config.profile().shell.value().program.empty());
}

TEST_CASE("Config: input mappings load key, char and mouse bindings with action arguments", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control, Shift], key: 'T', action: CreateNewTab }
    - { mods: [Control, Shift], key: 'W', action: CloseTab, mode: 'Insert' }
    - { mods: [Alt], key: Enter, action: SendChars, chars: "hello\r" }
    - { mods: [Control], key: 'C', action: CopySelection, format: HTML }
    - { mods: [Control], key: 'B', action: CopySelection, format: bogus }
    - { mods: [Control], mouse: Left, action: FollowHyperlink }
    - { mods: [], key: F5, action: ScreenshotVT }
)"sv);

    auto const& mappings = config.inputMappings.value();

    auto const hasKeyAction = [&](auto actionTag) {
        return std::ranges::any_of(mappings.keyMappings, [&](auto const& m) {
            return std::holds_alternative<decltype(actionTag)>(m.binding.at(0));
        });
    };
    auto const hasCharAction = [&](auto actionTag) {
        return std::ranges::any_of(mappings.charMappings, [&](auto const& m) {
            return std::holds_alternative<decltype(actionTag)>(m.binding.at(0));
        });
    };

    CHECK((hasKeyAction(contour::actions::CreateNewTab {})
           || hasCharAction(contour::actions::CreateNewTab {})));
    CHECK((hasKeyAction(contour::actions::CloseTab {}) || hasCharAction(contour::actions::CloseTab {})));
    CHECK(hasKeyAction(contour::actions::ScreenshotVT {}));

    // SendChars carries its unescaped chars payload.
    auto const sendChars = std::ranges::find_if(mappings.charMappings, [](auto const& m) {
        return std::holds_alternative<contour::actions::SendChars>(m.binding.at(0));
    });
    bool foundSendChars = sendChars != mappings.charMappings.end();
    if (foundSendChars)
        CHECK(std::get<contour::actions::SendChars>(sendChars->binding.at(0)).chars == "hello\r");
    else
    {
        auto const sendCharsKey = std::ranges::find_if(mappings.keyMappings, [](auto const& m) {
            return std::holds_alternative<contour::actions::SendChars>(m.binding.at(0));
        });
        REQUIRE(sendCharsKey != mappings.keyMappings.end());
        CHECK(std::get<contour::actions::SendChars>(sendCharsKey->binding.at(0)).chars == "hello\r");
    }

    // CopySelection format parsing: HTML parses; an invalid format falls back to Text.
    auto copyFormats = std::vector<contour::actions::CopyFormat> {};
    for (auto const& m: mappings.charMappings)
        if (std::holds_alternative<contour::actions::CopySelection>(m.binding.at(0)))
            copyFormats.push_back(std::get<contour::actions::CopySelection>(m.binding.at(0)).format);
    for (auto const& m: mappings.keyMappings)
        if (std::holds_alternative<contour::actions::CopySelection>(m.binding.at(0)))
            copyFormats.push_back(std::get<contour::actions::CopySelection>(m.binding.at(0)).format);
    CHECK(std::ranges::count(copyFormats, contour::actions::CopyFormat::HTML) == 1);
    CHECK(std::ranges::count(copyFormats, contour::actions::CopyFormat::Text) == 1);

    // The mouse binding landed in the mouse mapping table.
    CHECK(std::ranges::any_of(mappings.mouseMappings, [](auto const& m) {
        return std::holds_alternative<contour::actions::FollowHyperlink>(m.binding.at(0));
    }));
}

TEST_CASE("Config: SetTabColor's color is optional, and a bad one is not fatal", "[config]")
{
    // SetTabColor is the one action whose argument is optional AND meaningful when absent: no color
    // means "open the tab's color picker". Both spellings must therefore bind, which is also what keeps
    // the action out of ParameterizedActionConcept and thus IN the command palette.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control], key: F1, action: SetTabColor }
    - { mods: [Control], key: F2, action: SetTabColor, color: '#ff0000' }
    - { mods: [Control], key: F3, action: SetTabColor, color: '#0f0' }
    - { mods: [Control], key: F4, action: SetTabColor, color: 'rgb:00/80/FF' }
    - { mods: [Control], key: F5, action: SetTabColor, color: 'not-a-color' }
    - { mods: [Control], key: F6, action: ResetTabColor }
)"sv);

    auto const& mappings = config.inputMappings.value();

    auto const colorFor = [&](vtbackend::Key key) -> std::optional<vtbackend::RGBColor> {
        auto const found = std::ranges::find_if(mappings.keyMappings, [&](auto const& m) {
            return m.input == key && std::holds_alternative<contour::actions::SetTabColor>(m.binding.at(0));
        });
        REQUIRE(found != mappings.keyMappings.end());
        return std::get<contour::actions::SetTabColor>(found->binding.at(0)).color;
    };

    // No color at all: bound, and colorless. This is the picker.
    CHECK_FALSE(colorFor(vtbackend::Key::F1).has_value());

    // Every spelling vtbackend::parseColor() already understands is reused verbatim.
    CHECK(colorFor(vtbackend::Key::F2) == vtbackend::RGBColor { 0xFF, 0x00, 0x00 });
    CHECK(colorFor(vtbackend::Key::F3) == vtbackend::RGBColor { 0x00, 0xF0, 0x00 });
    CHECK(colorFor(vtbackend::Key::F4) == vtbackend::RGBColor { 0x00, 0x80, 0xFF });

    // A color that does not parse is NOT fatal: the binding survives, colorless, and opens the picker.
    // Deliberate (it mirrors CopySelection's bad `format`), so pin it — otherwise someone "fixes" it
    // into a dropped binding and a user's typo silently costs them their key.
    CHECK_FALSE(colorFor(vtbackend::Key::F5).has_value());

    CHECK(std::ranges::any_of(mappings.keyMappings, [](auto const& m) {
        return std::holds_alternative<contour::actions::ResetTabColor>(m.binding.at(0));
    }));
}

TEST_CASE("Config: an unquoted SetTabColor color is a YAML comment, and does not take the binding with it",
          "[config]")
{
    // The spelling a user reaches for first, in the block style the generated contour.yml is written in:
    //
    //     color: #ff0000
    //
    // YAML eats `#ff0000` as a COMMENT (the `#` follows a space), so the key is defined but its value is
    // null — not a scalar at all. That must still leave a usable binding (the picker), and it is warned
    // about in the log, since the value is gone by the time the parser sees it and nothing else would
    // point the user at the quoting. Pinned because the guard here is a NON-scalar test: an `IsScalar()`
    // check alone cannot tell "the user wrote no color" from "the user wrote one and YAML ate it".
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - mods: [Control]
      key: F2
      action: SetTabColor
      color: #ff0000
    - mods: [Control]
      key: F3
      action: SetTabColor
      color: '#ff0000'
)"sv);

    auto const& mappings = config.inputMappings.value();

    auto const colorFor = [&](vtbackend::Key key) -> std::optional<vtbackend::RGBColor> {
        auto const found = std::ranges::find_if(mappings.keyMappings, [&](auto const& m) {
            return m.input == key && std::holds_alternative<contour::actions::SetTabColor>(m.binding.at(0));
        });
        REQUIRE(found != mappings.keyMappings.end());
        return std::get<contour::actions::SetTabColor>(found->binding.at(0)).color;
    };

    // The binding survives, colorless: the key opens the picker rather than being dropped on the floor.
    CHECK_FALSE(colorFor(vtbackend::Key::F2).has_value());
    // ... and the quoted spelling right next to it, which is what the user should have written, colors.
    CHECK(colorFor(vtbackend::Key::F3) == vtbackend::RGBColor { 0xFF, 0x00, 0x00 });
}

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

    CHECK(profile->mouse.value().hideWhileTyping == false);
}

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
    CHECK(profile->tabBarPosition.value() == defaults.profile().tabBarPosition.value());
    CHECK(profile->tabBarVisibility.value() == defaults.profile().tabBarVisibility.value());
    CHECK(rendered.find("pixel_reporting:") != std::string::npos);
    CHECK(rendered.find("tab_bar_position:") != std::string::npos);
    CHECK(rendered.find("tab_bar_visibility:") != std::string::npos);
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

    // The loader appends to the built-in defaults, so assert growth and containment, not totals.
    auto const& mappings = config.inputMappings.value();
    CHECK_FALSE(mappings.keyMappings.empty());
    CHECK_FALSE(mappings.charMappings.empty());
    CHECK_FALSE(mappings.mouseMappings.empty());
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
    // Unknown enum literals leave the compiled-in defaults in place.
    CHECK(profile->scrollbar.value().position == contour::config::ScrollBarPosition::Hidden);
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
    CHECK(cwd.find('~') == std::string::npos);
    CHECK_FALSE(cwd.empty());
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

    SECTION("default is Logical, i.e. what every other terminal reports")
    {
        QTemporaryDir dir;
        auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
)"sv);
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        CHECK(profile->pixelReporting.value() == PixelReporting::Logical);
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
    CHECK(profile->pixelReporting.value() == PixelReporting::Logical);
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
profiles:
    main:
        shell: /bin/sh
        tab_bar_position: Bottom
        tab_bar_visibility: Never
)"sv);
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        CHECK(profile->tabBarPosition.value() == TabBarPosition::Bottom);
        CHECK(profile->tabBarVisibility.value() == TabBarVisibility::Never);
    }

    SECTION("top + multiple, lower-case tokens prove case-insensitivity")
    {
        QTemporaryDir dir;
        auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        tab_bar_position: top
        tab_bar_visibility: multiple
)"sv);
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        CHECK(profile->tabBarPosition.value() == TabBarPosition::Top);
        CHECK(profile->tabBarVisibility.value() == TabBarVisibility::Multiple);
    }

    SECTION("always (the default) still parses explicitly")
    {
        QTemporaryDir dir;
        auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        tab_bar_visibility: Always
)"sv);
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        CHECK(profile->tabBarVisibility.value() == TabBarVisibility::Always);
    }
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
profiles:
    main:
        shell: /bin/sh
        tab_bar_position: Sideways
        tab_bar_visibility: Sometimes
)"sv);
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(profile->tabBarPosition.value() == TabBarPosition::Top);
    CHECK(profile->tabBarVisibility.value() == TabBarVisibility::Always);
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
    QTemporaryDir dir;
    auto const path = std::filesystem::path(dir.path().toStdString()) / "nested" / "sub" / "contour.yml";

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
    QTemporaryDir dir;
    auto const path = std::filesystem::path(dir.path().toStdString()) / "created" / "contour.yml";
    REQUIRE_FALSE(std::filesystem::exists(path));

    auto const config = contour::config::loadConfigFromFile(path);
    CHECK(std::filesystem::exists(path));
    CHECK(config.profile(config.defaultProfileName.value()) != nullptr);
}

TEST_CASE("Config: defaultConfigFilePath and the documentation generators produce content", "[config]")
{
    // These are the pure string-producing entry points used by the `contour generate`/`documentation`
    // subcommands; exercising them covers the no-argument overloads and their default-Config path.
    CHECK_FALSE(contour::config::defaultConfigFilePath().empty());
    CHECK(contour::config::defaultConfigFilePath().find("contour.yml") != std::string::npos);

    CHECK_FALSE(contour::config::documentationGlobalConfig().empty());
    CHECK_FALSE(contour::config::documentationProfileConfig().empty());
    CHECK_FALSE(contour::config::defaultConfigString().empty());
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
    CHECK(features.count("feature_c") == 0); // disabled entries are not inserted
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
    CHECK(frozen.count(static_cast<vtbackend::DECMode>(999999)) == 0);
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

TEST_CASE("Config: SetTabTitle action binds, old SetTabName name is rejected", "[config]")
{
    // The tab-rename action was renamed SetTabName -> SetTabTitle. The new name must resolve to a
    // binding; the retired name must NOT (fromString() returns nullopt, so it is dropped).
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control, Shift], key: 'R', action: SetTabTitle }
    - { mods: [Control, Shift], key: 'N', action: SetTabName }
)"sv);

    auto const& mappings = config.inputMappings.value();
    auto const bindsTo = [&](auto actionTag) {
        auto const inKey = std::ranges::any_of(mappings.keyMappings, [&](auto const& m) {
            return std::holds_alternative<decltype(actionTag)>(m.binding.at(0));
        });
        auto const inChar = std::ranges::any_of(mappings.charMappings, [&](auto const& m) {
            return std::holds_alternative<decltype(actionTag)>(m.binding.at(0));
        });
        return inKey || inChar;
    };

    CHECK(bindsTo(contour::actions::SetTabTitle {}));
    // The retired "SetTabName" string does not resolve, so it is dropped: exactly ONE SetTabTitle
    // binding survives (the 'R' line), while the 'N'/SetTabName line adds nothing. Count across both
    // key and char mapping lists (an uppercase letter key may land in either).
    auto const countIn = [](auto const& list) {
        return std::ranges::count_if(list, [](auto const& m) {
            return std::holds_alternative<contour::actions::SetTabTitle>(m.binding.at(0));
        });
    };
    CHECK(countIn(mappings.keyMappings) + countIn(mappings.charMappings) == 1);
}

TEST_CASE("Config: the command palette is bound to Ctrl+Shift+P by default", "[config][palette]")
{
    // Ctrl+Shift+P arrives as a CHARACTER, not a named key: Qt::Key_P is not in helper.cpp's
    // KeyMappings table, so any Ctrl+printable is routed through sendCharEvent. A KeyInputMapping here
    // would therefore never match, and the chord would do nothing.
    auto const& mappings = contour::config::defaultInputMappings;

    auto const bound = std::ranges::find_if(mappings.charMappings, [](auto const& mapping) {
        return !mapping.binding.empty()
               && std::holds_alternative<contour::actions::OpenCommandPalette>(mapping.binding.at(0));
    });

    REQUIRE(bound != mappings.charMappings.end());
    CHECK(bound->input == U'P');
    CHECK(bound->modifiers
          == vtbackend::Modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Shift });
}

TEST_CASE("Config: command_palette_recent_count", "[config][palette]")
{
    auto tempDir = QTemporaryDir {};
    REQUIRE(tempDir.isValid());

    SECTION("defaults to 5 when unset")
    {
        auto const config = loadFromYaml(tempDir, "profiles:\n  main:\n    shell: \"/bin/bash\"\n");
        CHECK(config.commandPaletteRecentCount.value() == 5);
    }

    SECTION("is read from the config file")
    {
        auto const config = loadFromYaml(tempDir,
                                         "command_palette_recent_count: 12\n"
                                         "profiles:\n  main:\n    shell: \"/bin/bash\"\n");
        CHECK(config.commandPaletteRecentCount.value() == 12);
    }

    SECTION("zero is honored — it turns the recently-used section off")
    {
        auto const config = loadFromYaml(tempDir,
                                         "command_palette_recent_count: 0\n"
                                         "profiles:\n  main:\n    shell: \"/bin/bash\"\n");
        CHECK(config.commandPaletteRecentCount.value() == 0);
    }
}

TEST_CASE("config.builtinFallbackMouseMappings", "[config]")
{
    auto const& fallbacks = contour::config::builtinFallbackMouseMappings();

    SECTION("the right button opens the context menu")
    {
        REQUIRE(fallbacks.size() == 1);
        auto const& mapping = fallbacks.front();

        CHECK(mapping.input == vtbackend::MouseButton::Right);
        CHECK(mapping.modifiers == vtbackend::Modifiers { vtbackend::Modifier::None });
        REQUIRE(mapping.binding.size() == 1);
        CHECK(std::holds_alternative<contour::actions::OpenContextMenu>(mapping.binding.front()));
    }

    SECTION("it is a FALLBACK, not a default")
    {
        // This is the whole point of the table, so it is pinned rather than left to a comment: the right
        // button must NOT be in the default mouseMappings. Loading an `input_mapping:` section replaces
        // those wholesale, and the contour.yml Contour generates on first run writes every one of them
        // out — so a right-click binding placed there would be shadowed away by the config file of every
        // user who already has one, forever.
        auto const defaults = contour::config::Config {}.inputMappings.value().mouseMappings;
        auto const bindsRight = std::ranges::any_of(
            defaults, [](auto const& mapping) { return mapping.input == vtbackend::MouseButton::Right; });
        CHECK_FALSE(bindsRight);
    }
}

// {{{ GUI-managed side files (profiles/*.yml, colorschemes/*.yml, settings.yml) and the lock key.

TEST_CASE("Config: gui_config_locked loads and defaults to false", "[config][gui]")
{
    QTemporaryDir dir;
    CHECK_FALSE(loadFromYaml(dir, "default_profile: main\n").guiConfigLocked.value());

    QTemporaryDir dir2;
    CHECK(loadFromYaml(dir2, "gui_config_locked: true\n").guiConfigLocked.value());
}

TEST_CASE("Config: a profiles/<name>.yml side file merges as an editable GUI profile", "[config][gui]")
{
    QTemporaryDir dir;
    // The side file overrides only show_title_bar; every other field must inherit the default profile.
    writeSideFile(dir, "profiles/work.yml", "show_title_bar: false\n");
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        show_title_bar: true
        wm_class: inherited-value
)");

    auto const* work = config.findProfile("work");
    REQUIRE(work != nullptr);
    CHECK(work->showTitleBar.value() == false);        // overridden by the side file
    CHECK(work->wmClass.value() == "inherited-value"); // inherited from the default profile

    // Provenance gates GUI editability: inline entries are read-only, side-file entries are editable.
    CHECK(config.profileOrigins.at("main") == contour::config::SettingsOrigin::MainConfig);
    CHECK(config.profileOrigins.at("work") == contour::config::SettingsOrigin::SideFile);
}

TEST_CASE("Config: a profiles/<name>.yml with CRLF line endings still loads", "[config][gui]")
{
    // Regression guard for a Windows-only defect: the side-file reader sizes its read by
    // fs::file_size(), so it must open in binary. A text-mode read would collapse CRLF->LF, fall
    // short of the byte count, and leave a trailing NUL that breaks the YAML parse. Editors on
    // Windows routinely save CRLF, so this must round-trip. We write the bytes explicitly (binary)
    // to exercise the CRLF path on every platform, not just Windows.
    QTemporaryDir dir;
    {
        auto const path = std::filesystem::path(dir.path().toStdString()) / "profiles" / "crlf.yml";
        std::filesystem::create_directories(path.parent_path());
        auto out = std::ofstream(path, std::ios::binary);
        // A bool and a string value, both terminated by CRLF. The string is the sharp test: a stray
        // '\r' kept on the scalar would make wm_class compare unequal to "contour-crlf".
        out << "show_title_bar: false\r\n"
               "wm_class: contour-crlf\r\n";
    }
    auto const config = loadFromYaml(dir, "default_profile: main\n");

    auto const* crlf = config.findProfile("crlf");
    REQUIRE(crlf != nullptr);
    CHECK(crlf->showTitleBar.value() == false);
    CHECK(crlf->wmClass.value() == "contour-crlf");
}

TEST_CASE("Config: an in-place reload drops a removed side-file profile and its stale origin",
          "[config][gui]")
{
    // Production reloads config in place (reloadAllSessions reuses the live Config object), unlike a fresh
    // load. A profiles map that only ever accreted entries — and a provenance map that only ever emplaced
    // — would keep a GUI profile alive after its side file was deleted, so the settings page would still
    // offer it and a later Save would shadow contour.yml. The loader must reconcile both on every load.
    QTemporaryDir dir;
    writeSideFile(dir, "profiles/work.yml", "show_title_bar: false\n");
    auto const configPath = writeConfig(dir, R"(
default_profile: main
profiles:
    main:
        show_title_bar: true
)");

    contour::config::Config config;
    contour::config::loadConfigFromFile(config, configPath);
    REQUIRE(config.findProfile("work") != nullptr);
    REQUIRE(config.profileOrigins.at("work") == contour::config::SettingsOrigin::SideFile);

    // Delete the side file and reload INTO THE SAME Config object.
    std::filesystem::remove(std::filesystem::path(dir.path().toStdString()) / "profiles" / "work.yml");
    contour::config::loadConfigFromFile(config, configPath);

    CHECK(config.findProfile("work") == nullptr);    // no longer in the profiles map
    CHECK(config.profileOrigins.count("work") == 0); // and no stale SideFile provenance left behind
    REQUIRE(config.findProfile("main") != nullptr);  // the contour.yml profile is intact
    CHECK(config.profileOrigins.at("main") == contour::config::SettingsOrigin::MainConfig);
}

TEST_CASE("Config: a settings.yml with CRLF line endings still applies", "[config][gui]")
{
    // settings.yml is read through the same CRLF-normalizing reader as the profile side files, so a file
    // saved with Windows line endings must round-trip on every platform: a stray '\r' left on a scalar
    // would make default_profile name an unknown "work\r" and fail the typed int global override. Written
    // as explicit CRLF bytes in binary to exercise the path regardless of host line-ending conventions.
    QTemporaryDir dir;
    writeSideFile(dir, "profiles/work.yml", "show_title_bar: false\n");
    {
        auto const path = std::filesystem::path(dir.path().toStdString()) / "settings.yml";
        auto out = std::ofstream(path, std::ios::binary);
        out << "default_profile: work\r\n"
               "read_buffer_size: 32768\r\n";
    }
    auto const config = loadFromYaml(dir, R"(
default_profile: main
read_buffer_size: 16384
profiles:
    main:
        show_title_bar: true
)");

    CHECK(config.defaultProfileName.value() == "work"); // CRLF did not corrupt the profile name
    CHECK(config.ptyReadBufferSize.value() == 32768);   // the int global override applied through CRLF
}

TEST_CASE("Config: settings.yml default_profile overrides contour.yml's", "[config][gui]")
{
    QTemporaryDir dir;
    writeSideFile(dir, "profiles/work.yml", "show_title_bar: false\n");
    writeSideFile(dir, "settings.yml", "default_profile: work\n");
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        show_title_bar: true
)");

    CHECK(config.defaultProfileName.value() == "work");
    REQUIRE(config.guiManagedSettings.defaultProfile.has_value());
    CHECK(config.guiManagedSettings.defaultProfile.value() == "work");
}

TEST_CASE("Config: settings.yml default_profile naming a missing profile is ignored", "[config][gui]")
{
    QTemporaryDir dir;
    writeSideFile(dir, "settings.yml", "default_profile: ghost\n");
    auto const config = loadFromYaml(dir, "default_profile: main\n");
    CHECK(config.defaultProfileName.value() == "main");
}

TEST_CASE("Config: a malformed profiles/<name>.yml is skipped, not fatal to the rest", "[config][gui]")
{
    QTemporaryDir dir;
    writeSideFile(dir, "profiles/broken.yml", "colors: [1, 2\n"); // unterminated flow sequence
    writeSideFile(dir, "profiles/ok.yml", "show_title_bar: false\n");
    auto const config = loadFromYaml(dir, "default_profile: main\n");

    CHECK(config.findProfile("ok") != nullptr);     // the valid side file still loaded
    CHECK(config.findProfile("broken") == nullptr); // the broken one was skipped
    CHECK(config.findProfile("main") != nullptr);   // the rest of the config is intact
}

TEST_CASE("Config: a profile round-trips through emitProfileYaml and the side-file loader", "[config][gui]")
{
    QTemporaryDir dir;
    auto const source = loadFromYaml(dir, "default_profile: main\n");
    auto profile = *source.findProfile("main");
    profile.showTitleBar = false;
    profile.dimUnfocused = true;

    writeSideFile(dir, "profiles/roundtrip.yml", contour::config::emitProfileYaml(profile));

    auto const reloaded = loadFromYaml(dir, "default_profile: main\n");
    auto const* rt = reloaded.findProfile("roundtrip");
    REQUIRE(rt != nullptr);
    CHECK(rt->showTitleBar.value() == false);
    CHECK(rt->dimUnfocused.value() == true);
}

TEST_CASE("Config: a colorschemes/<name>.yml written via emitColorSchemeYaml is resolved by a profile",
          "[config][gui]")
{
    QTemporaryDir dir;
    auto palette = vtbackend::ColorPalette {};
    palette.defaultBackground = vtbackend::RGBColor { 0x12, 0x34, 0x56 };
    writeSideFile(dir, "colorschemes/mono.yml", contour::config::emitColorSchemeYaml(palette));

    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        colors: mono
)");

    auto const* main = config.findProfile("main");
    REQUIRE(main != nullptr);
    auto const* simple = std::get_if<contour::config::SimpleColorConfig>(&main->colors.value());
    REQUIRE(simple != nullptr);
    CHECK(simple->colors.defaultBackground == vtbackend::RGBColor { 0x12, 0x34, 0x56 });
}

TEST_CASE("Config: GUI settings round-trip through emitGuiSettingsYaml / loadGuiSettingsFile",
          "[config][gui]")
{
    QTemporaryDir dir;
    auto const path = std::filesystem::path(dir.path().toStdString()) / "settings.yml";
    {
        auto out = std::ofstream(path);
        out << contour::config::emitGuiSettingsYaml({ .defaultProfile = "work", .globalOverrides = {} });
    }

    auto const loaded = contour::config::loadGuiSettingsFile(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->defaultProfile.has_value());
    CHECK(loaded->defaultProfile.value() == "work");

    // A missing file is default (all-unset), not an error.
    auto const missing = contour::config::loadGuiSettingsFile(std::filesystem::path(dir.path().toStdString())
                                                              / "does-not-exist.yml");
    REQUIRE(missing.has_value());
    CHECK_FALSE(missing->defaultProfile.has_value());
}

TEST_CASE("Config: FileGuiConfigStore writes and removes side files the loader picks up", "[config][gui]")
{
    QTemporaryDir dir;
    auto const configDir = std::filesystem::path(dir.path().toStdString());

    auto const base = loadFromYaml(dir, "default_profile: main\n");
    auto profile = *base.findProfile("main");
    profile.showTitleBar = false;

    auto store = contour::FileGuiConfigStore(configDir);
    REQUIRE(store.saveProfile("saved", profile).has_value());
    REQUIRE(store
                .saveGuiSettings(
                    contour::config::GuiManagedSettings { .defaultProfile = "saved", .globalOverrides = {} })
                .has_value());

    auto const afterSave = loadFromYaml(dir, "default_profile: main\n");
    auto const* saved = afterSave.findProfile("saved");
    REQUIRE(saved != nullptr);
    CHECK(saved->showTitleBar.value() == false);
    CHECK(afterSave.defaultProfileName.value() == "saved");
    CHECK(afterSave.profileOrigins.at("saved") == contour::config::SettingsOrigin::SideFile);

    REQUIRE(store.deleteProfile("saved").has_value());
    auto const afterDelete = loadFromYaml(dir, "default_profile: main\n");
    CHECK(afterDelete.findProfile("saved") == nullptr);
}

// }}}
