// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the YAML configuration loader (Config.cpp) — the largest previously untested
// surface of the GUI layer. Each case writes an inline YAML document to a temp file, loads it
// through the production loadConfigFromFile(), and asserts the parsed ConfigEntry values, so a
// renamed/retyped YAML key or a broken loadFromEntry overload fails here instead of silently
// falling back to defaults at runtime.

#include <contour/config/Actions.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/ConfigEnum.hpp>
#include <contour/config/GpuSelector.hpp>
#include <contour/config/GuiConfigStore.hpp>
#include <contour/config/ModifierNames.hpp>
#include <contour/config/SingleQuoted.hpp>
#include <contour/platform/GuiTheme.hpp>
#include <contour/test/CoreFixtures.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/core/CommandBlockOutcome.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/input/InputGenerator.hpp>
#include <vtbackend/screen/GutterSettings.hpp>
#include <vtbackend/shell/ScrollbarMarks.hpp>
#include <vtbackend/shell/StickyHeader.hpp>

#include <vtpty/test/NonAsciiSample.hpp>

#include <vtrasterizer/Decorator.hpp>

#include <text_shaper/Font.hpp>

#include <crispy/StrongHash.hpp>

#include <core/Environment.hpp>
#include <core/Utf8Path.hpp>
#include <core/log/LogSink.hpp>
#include <core/log/LogStore.hpp>
#include <core/testing/EnvHelper.hpp>
#include <core/testing/ScopedTempDir.hpp>
#include <core/testing/ScopedWorkingDirectory.hpp>

#include <yaml-cpp/yaml.h>

#include <QtCore/QTemporaryDir>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

using namespace std::string_view_literals;

namespace
{

/// Writes @p yaml to a fresh file inside @p dir and returns its path.
[[nodiscard]] std::filesystem::path writeConfig(QTemporaryDir& dir, std::string_view yaml)
{
    auto const path = std::filesystem::path(dir.path().toStdU16String()) / "contour.yml";
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

/// The main profile's color scheme, which @p config must give as a single scheme rather than a dark/light
/// pair.
[[nodiscard]] vtbackend::ColorPalette schemeOf(contour::config::Config const& config)
{
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    REQUIRE(std::holds_alternative<contour::config::SimpleColorConfig>(profile->colors.value()));
    return std::get<contour::config::SimpleColorConfig>(profile->colors.value()).colors;
}

/// Writes @p content to a GUI side file at @p relativePath under @p dir (the config directory),
/// creating intermediate directories (e.g. `profiles/`, `colorschemes/`) as needed.
void writeSideFile(QTemporaryDir& dir, std::filesystem::path const& relativePath, std::string_view content)
{
    auto const path = std::filesystem::path(dir.path().toStdU16String()) / relativePath;
    std::filesystem::create_directories(path.parent_path());
    auto out = std::ofstream(path);
    out << content;
}

/// A Classic configuration, for the tests that edit the "default" colour scheme: a Classic profile
/// names that scheme, whereas a Native2026 one names contour-dark/-light.
[[nodiscard]] contour::config::Config classicConfig()
{
    return contour::config::Config { .defaultsEdition = contour::config::DefaultsEdition::Classic };
}

} // namespace

TEST_CASE("Config: multiline command copying is opt-in per profile", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
    single_line:
        shell: /bin/sh
        copy_command_preserve_multiline: false
    multiline:
        shell: /bin/sh
        copy_command_preserve_multiline: true
)"sv);
    CHECK_FALSE(config.profile("main")->copyCommandPreserveMultiline.value());
    CHECK_FALSE(config.profile("single_line")->copyCommandPreserveMultiline.value());
    CHECK(config.profile("multiline")->copyCommandPreserveMultiline.value());

    writeSideFile(
        dir, "profiles/roundtrip.yml", contour::config::emitProfileYaml(*config.profile("multiline")));
    auto const reloaded = loadFromYaml(dir, "default_profile: main\n");
    auto const* roundTripped = reloaded.findProfile("roundtrip");
    REQUIRE(roundTripped != nullptr);
    CHECK(roundTripped->copyCommandPreserveMultiline.value());
}

TEST_CASE("Config: global scalars load from YAML", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
early_exit_threshold: 11
live_config: true
reflow_on_resize: false
notification_close_timeout: 2500
profiles:
    main:
        shell: /bin/sh
)"sv);

    CHECK(config.defaultProfileName.value() == "main");
    CHECK(config.earlyExitThreshold.value() == 11);
    CHECK(config.live.value() == true);
    CHECK(config.reflowOnResize.value() == false);
    CHECK(config.notificationCloseTimeout.value() == std::chrono::milliseconds { 2500 });
}

TEST_CASE("Config: renderer.gpu loads, and a bad value falls back to auto", "[config][gpu]")
{
    QTemporaryDir dir;
    auto const cfg = loadFromYaml(dir, R"(
renderer:
    gpu: 10de:2820
profiles:
    main:
        shell: /bin/sh
)"sv);
    CHECK(cfg.renderer.value().gpu
          == contour::config::GpuSelector {
              .preference = contour::config::GpuPreference::Specific,
              .id = contour::config::PciId { .vendor = 0x10de, .device = 0x2820 } });

    QTemporaryDir badDir;
    auto const bad =
        loadFromYaml(badDir, "renderer:\n    gpu: banana\nprofiles:\n    main:\n        shell: /bin/sh\n"sv);
    CHECK(bad.renderer.value().gpu == contour::config::GpuSelector {});
}

TEST_CASE("Config: the generated config documents renderer.gpu with a value", "[config][gpu]")
{
    // A comment-only template is never serialized; the value line proves it round-trips.
    CHECK(contour::config::defaultConfigString().contains("    gpu: auto\n"));
}

TEST_CASE("Config: the desktop-notification close timeout defaults to ten seconds", "[config]")
{
    // Only consulted where a close cannot be observed -- today the portal backend a sandboxed
    // Contour must use. Comfortably past a typical desktop's own expiry, so the assumed close lands
    // after the popup is realistically gone rather than while it is still on screen.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
profiles:
    main:
        shell: /bin/sh
)"sv);

    CHECK(config.notificationCloseTimeout.value() == std::chrono::milliseconds { 10000 });
}

TEST_CASE("Config: grapheme clustering defaults on and can be turned off", "[config]")
{
    // Mode 2027 is set at power-on by default; the setting only chooses what an application finds,
    // and an application may still toggle the mode itself afterwards.
    QTemporaryDir dir;
    CHECK(loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
)"sv)
              .graphemeClustering.value());

    QTemporaryDir dir2;
    CHECK_FALSE(loadFromYaml(dir2, R"(
default_profile: main
grapheme_clustering: false
profiles:
    main:
        shell: /bin/sh
)"sv)
                    .graphemeClustering.value());
}

TEST_CASE("Config: text scaling method loads from YAML", "[config]")
{
    // Asserted with `stretch` on purpose: it is the NON-default, so this fails if the key is ignored
    // and the default silently answers instead.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
text_scaling_method: stretch
profiles:
    main:
        shell: /bin/sh
)"sv);
    CHECK(config.textScalingMethod.value() == vtrasterizer::GlyphScalingMethod::Stretch);
}

TEST_CASE("Config: text scaling method defaults to rerasterize", "[config]")
{
    // Re-rasterizing is the default because scaled text is text an application asked to be LARGE, and
    // a magnified bitmap is visibly soft at those sizes. Stretch remains the opt-in for the case it
    // exists for: large text scrolling past the viewport, where the per-(glyph, scale) rasterization
    // is paid repeatedly.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
)"sv);
    CHECK(config.textScalingMethod.value() == vtrasterizer::GlyphScalingMethod::Rerasterize);
}

TEST_CASE("Config: an invalid text scaling method keeps the default", "[config]")
{
    // A typo in a visible rendering setting must not silently select something else.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
text_scaling_method: crispy-please
profiles:
    main:
        shell: /bin/sh
)"sv);
    CHECK(config.textScalingMethod.value() == vtrasterizer::GlyphScalingMethod::Rerasterize);
}

TEST_CASE("Config: the shell environment identifies the terminal", "[config]")
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
    auto const& env = profile->shell.value().env;

    // TERM only names a terminfo capability set, which several terminals share; TERM_PROGRAM is how
    // an application learns WHICH terminal it is talking to. Python wcwidth's wcstwidth() selects
    // its per-terminal width correction table by exactly this variable, so an absent or stale value
    // silently costs correctness in every application that uses it.
    REQUIRE(env.contains("TERM_PROGRAM"));
    CHECK(env.at("TERM_PROGRAM") == "contour");

    REQUIRE(env.contains("TERM_PROGRAM_VERSION"));
    CHECK(env.at("TERM_PROGRAM_VERSION") == CONTOUR_VERSION_STRING);
    CHECK_FALSE(env.at("TERM_PROGRAM_VERSION").empty());
}

TEST_CASE("Config: emulationSettings carries the profile's VT semantics", "[config]")
{
    // The half of a profile's terminal settings that decides what the terminal IS, shared by the
    // GUI session factory and `contour daemon`. It exists so the two cannot disagree: a daemon
    // hosting sessions with a different scrollback depth than the client rendering them cannot
    // name the rows that client is trying to mirror, and the user sees blank scrollback.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
reflow_on_resize: false
grapheme_clustering: false
images:
    sixel_register_count: 512
profiles:
    main:
        terminal_id: VT340
        terminal_size:
            columns: 132
            lines: 40
        history:
            limit: 4242
)"sv);

    auto const* const profile = config.findProfile("main");
    REQUIRE(profile != nullptr);
    auto const settings = contour::config::emulationSettings(config, *profile);

    REQUIRE(std::holds_alternative<vtbackend::LineCount>(settings.historyLimits.capacity));
    CHECK(std::get<vtbackend::LineCount>(settings.historyLimits.capacity) == vtbackend::LineCount(4242));
    CHECK(settings.pageSize.columns == vtbackend::ColumnCount(132));
    CHECK(settings.pageSize.lines == vtbackend::LineCount(40));
    CHECK(settings.terminalId == vtbackend::VTType::VT340);
    CHECK(settings.maxImageRegisterCount == 512);
    CHECK(settings.primaryScreen.allowReflowOnResize == false);
    CHECK(settings.graphemeClustering == false);
}

TEST_CASE("Config: emulationSettings carries an unlimited history", "[config]")
{
    // maxHistoryLineCount is a variant; the daemon must inherit the Infinite arm too, or an
    // "unlimited scrollback" profile silently caps its hosted sessions.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        history:
            limit: -1
)"sv);

    auto const* const profile = config.findProfile("main");
    REQUIRE(profile != nullptr);
    CHECK(std::holds_alternative<vtbackend::Infinite>(
        contour::config::emulationSettings(config, *profile).historyLimits.capacity));
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
    CHECK(*t0.root.directory == "/tmp");

    auto const& t1 = work.tabs[1];
    CHECK(t1.title == "claude");
    CHECK(t1.root.isLeaf());
    CHECK(*t1.root.command == "claude");
    CHECK_FALSE(t1.root.directory.has_value());
}

TEST_CASE("Config: a layout directory with non-ASCII characters loads losslessly", "[config][layout]")
{
    // Regression: the layout directory is stored as a lossless std::filesystem::path, not narrowed to
    // a std::string at parse time. On Windows the old .string() narrowing threw std::system_error for a
    // path outside the active code page (e.g. a Unicode profile directory), failing the ENTIRE config
    // load; now such a path is preserved and only its own pane's launch would be affected.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
layouts:
    work:
        tabs:
            - title: "editor"
              directory: "/tmp/naïve project"
              command: "nvim"
)"sv);

    // The config loaded (no parse-time throw) and the directory was preserved intact.
    auto const& layouts = config.layouts.value();
    REQUIRE(layouts.contains("work"));
    auto const& work = layouts.at("work");
    REQUIRE(work.tabs.size() == 1);
    REQUIRE(work.tabs[0].root.directory.has_value());
    // UTF-8, as the file is: a narrow path would read it in the ANSI code page on Windows.
    CHECK(*work.tabs[0].root.directory == std::filesystem::path { u8"/tmp/naïve project" });
}

TEST_CASE("Config: a layout command is kept as written, its arguments after it", "[config][layout]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
layouts:
    work:
        tabs:
            - command: "/usr/bin/emacs -nw"
            - command: "git log --oneline"
              arguments: ["--graph"]
            - command: 'C:\tools\vim.exe -u NONE'
            - command: " \t"
)"sv);
    auto const& work = config.layouts.value().at("work");
    REQUIRE(work.tabs.size() == 4);

    // The spawn reads it, as it reads a profile's `shell:`, by this platform's rules.
    REQUIRE(work.tabs[0].root.command.has_value());
    CHECK(*work.tabs[0].root.command == "/usr/bin/emacs -nw");
    CHECK(work.tabs[0].root.arguments.empty());

    // Explicit `arguments:` entries go after the words of `command`.
    CHECK(*work.tabs[1].root.command == "git log --oneline");
    CHECK(work.tabs[1].root.arguments == std::vector<std::string> { "--graph" });

    // On Windows a backslash separates directories, where a POSIX shell's reading at load took it for an
    // escape and ran "C:toolsvim.exe".
    CHECK(work.tabs[2].root.command == std::optional<std::string> { R"(C:\tools\vim.exe -u NONE)" });

    // A blank one names nothing: the pane runs the shell.
    CHECK_FALSE(work.tabs[3].root.command.has_value());
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
    CHECK(root.orientation == vtworkspace::SplitState::Vertical);
    REQUIRE(root.children.size() == 2);

    CHECK(root.children[0].isLeaf());
    // `command` is kept as written, for the spawn to read.
    CHECK(*root.children[0].command == "npm run dev");
    CHECK(root.children[0].arguments.empty());
    REQUIRE(root.children[0].ratio.has_value());
    CHECK(*root.children[0].ratio == Catch::Approx(0.6));
    // The unspecified sibling stays unset (it shares the remaining space at realization time).
    CHECK_FALSE(root.children[1].ratio.has_value());

    auto const& nested = root.children[1];
    REQUIRE_FALSE(nested.isLeaf());
    CHECK(nested.orientation == vtworkspace::SplitState::Horizontal);
    REQUIRE(nested.children.size() == 2);
    CHECK(*nested.children[0].command == "htop");
    CHECK(nested.children[0].arguments.empty());
    CHECK(*nested.children[1].command == "journalctl -f");
    CHECK(nested.children[1].arguments.empty());
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
        auto const siblingPath = std::filesystem::path(dir.path().toStdU16String()) / "layouts.yml";
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

// {{{ hosts
TEST_CASE("Config: saved hosts parse", "[config][hosts]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
hosts:
    home:
        url: "ssh://me@homepc"
        remote_command: "/opt/contour/bin/contour daemon --stdio"
        profile: "remote"
    studio:
        url: "tls://studio.tailnet.ts.net:7297"
        token_file: "~/.config/contour/studio.token"
        tls_ca: "~/.config/contour/studio-ca.pem"
)"sv);

    auto const& hosts = config.hosts.value();
    REQUIRE(hosts.size() == 2);
    auto const& home = hosts.at("home");
    CHECK(home.url.scheme == contour::config::HostScheme::Ssh);
    CHECK(home.url.user == "me");
    CHECK(home.url.host == "homepc");
    CHECK(home.remoteCommand == "/opt/contour/bin/contour daemon --stdio");
    CHECK(home.profile == "remote");
    CHECK(home.tokenFile.empty());

    auto const& studio = hosts.at("studio");
    CHECK(studio.url.scheme == contour::config::HostScheme::Tls);
    CHECK(studio.url.port == 7297);
    CHECK(studio.tokenFile == vtpty::Process::homeDirectory() / ".config/contour/studio.token");
    CHECK(studio.tlsCa == vtpty::Process::homeDirectory() / ".config/contour/studio-ca.pem");
}

TEST_CASE("Config: a host with a malformed url is skipped, the others load", "[config][hosts]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
hosts:
    bad:
        url: "homepc"
    home:
        url: "ssh://homepc"
)"sv);

    auto const& hosts = config.hosts.value();
    CHECK(hosts.size() == 1);
    CHECK(hosts.contains("home"));
}

TEST_CASE("Config: a host without url is skipped", "[config][hosts]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
hosts:
    nowhere:
        profile: "remote"
    home:
        url: "ssh://homepc"
)"sv);

    auto const& hosts = config.hosts.value();
    CHECK(hosts.size() == 1);
    CHECK(hosts.contains("home"));
}

TEST_CASE("Config: tls host without token_file is skipped", "[config][hosts]")
{
    // A TCP daemon always requires a token, so a saved TLS host without one could never attach.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
hosts:
    studio:
        url: "tls://studio:7297"
)"sv);

    CHECK(config.hosts.value().empty());
}

TEST_CASE("Config: tls host without tls_ca loads, unverified", "[config][hosts]")
{
    // Warned about at load rather than skipped: the guide explains when an unpinned daemon is acceptable.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
hosts:
    studio:
        url: "tls://studio:7297"
        token_file: "/tmp/studio.token"
)"sv);

    REQUIRE(config.hosts.value().contains("studio"));
    CHECK(config.hosts.value().at("studio").tlsCa.empty());
}

TEST_CASE("Config: resolveHost finds saved hosts and parses ad-hoc URLs", "[config][hosts]")
{
    QTemporaryDir dir;
    auto const path = writeConfig(dir, R"(
hosts:
    home:
        url: "ssh://me@homepc"
    studio:
        url: "tls://studio:7297"
        token_file: "/tmp/studio.token"
)"sv);

    SECTION("a saved name")
    {
        auto const host = contour::config::resolveHost(path.string(), "home");
        REQUIRE(host.has_value());
        CHECK(host->url.user == "me");
    }

    SECTION("an unknown name lists the saved ones")
    {
        auto const host = contour::config::resolveHost(path.string(), "office");
        REQUIRE_FALSE(host.has_value());
        CHECK(host.error().contains("no host named 'office'"));
        CHECK(host.error().contains("home"));
        CHECK(host.error().contains("studio"));
    }

    SECTION("an ad-hoc ssh URL needs no configuration")
    {
        auto const host = contour::config::resolveHost(path.string(), "ssh://other");
        REQUIRE(host.has_value());
        CHECK(host->url.host == "other");
    }

    SECTION("an ad-hoc tls URL is refused")
    {
        auto const host = contour::config::resolveHost(path.string(), "tls://studio:7297");
        REQUIRE_FALSE(host.has_value());
        CHECK(host.error() == "a tls:// host needs a token; save it under hosts: with token_file");
    }

    SECTION("a malformed ad-hoc URL says why")
    {
        auto const host = contour::config::resolveHost(path.string(), "ssh://-oProxyCommand=x");
        REQUIRE_FALSE(host.has_value());
        CHECK(host.error().contains("a host may not begin with '-'"));
    }
}

TEST_CASE("Config: resolveHost says (none) when nothing is saved", "[config][hosts]")
{
    QTemporaryDir dir;
    auto const path = writeConfig(dir, "default_profile: main\n");
    auto const host = contour::config::resolveHost(path.string(), "home");
    REQUIRE_FALSE(host.has_value());
    CHECK(host.error().contains("(saved hosts: (none))"));
}
// }}}

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
    using contour::actions::ConfigurationTarget;
    auto targets = std::vector<ConfigurationTarget> {};
    for (auto const& mapping: config.inputMappings.value().keyMappings)
        if (auto const* oc = std::get_if<contour::actions::OpenConfiguration>(&mapping.binding.at(0)))
            targets.push_back(oc->target);

    REQUIRE(targets.size() == 2);
    // default -> Settings window
    CHECK(std::ranges::count(targets, ConfigurationTarget::SettingsWindow) == 1);
    // in_editor: true -> external editor
    CHECK(std::ranges::count(targets, ConfigurationTarget::Editor) == 1);
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
    CHECK(tabs.at(0).root.orientation == vtworkspace::SplitState::Horizontal);
    // An unknown value falls back to the vertical default (with a log line, not silently).
    CHECK(tabs.at(1).root.orientation == vtworkspace::SplitState::Vertical);
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

TEST_CASE("Config: the fold column's colors load and are written back", "[config]")
{
    QTemporaryDir dir;

    SECTION("unset by default, so the column derives from the scheme")
    {
        // Deliberately optional: a scheme that says nothing about folding must not have to, and gets
        // the column derived from the palette's own foreground/background -- the glyph faded at rest
        // and at full strength under the pointer.
        auto const scheme = schemeOf(loadFromYaml(dir, R"(
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
)"sv));
        CHECK_FALSE(scheme.foldMarker.has_value());
        CHECK_FALSE(scheme.foldMarkerHover.has_value());
    }

    SECTION("both pairs load")
    {
        // Deliberately NOT the colours the derivation produces, and not each other's inverse: this
        // section has to tell "read the configured value" apart from "fell back to the palette".

        auto const scheme = schemeOf(loadFromYaml(dir, R"(
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
        fold_marker:
            default:
                foreground: '#909090'
                background: '#101010'
            hover:
                foreground: '#FFD080'
                background: '#303030'
)"sv));
        REQUIRE(scheme.foldMarker.has_value());
        CHECK(scheme.foldMarker->foreground == vtbackend::RGBColor(0x90, 0x90, 0x90));
        CHECK(scheme.foldMarker->background == vtbackend::RGBColor(0x10, 0x10, 0x10));
        REQUIRE(scheme.foldMarkerHover.has_value());
        CHECK(scheme.foldMarkerHover->foreground == vtbackend::RGBColor(0xFF, 0xD0, 0x80));
        CHECK(scheme.foldMarkerHover->background == vtbackend::RGBColor(0x30, 0x30, 0x30));
    }

    SECTION("one pair alone loads, and the other stays unset")
    {
        auto const scheme = schemeOf(loadFromYaml(dir, R"(
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
        fold_marker:
            hover:
                foreground: '#010203'
                background: '#040506'
)"sv));
        CHECK_FALSE(scheme.foldMarker.has_value());
        REQUIRE(scheme.foldMarkerHover.has_value());
        CHECK(scheme.foldMarkerHover->foreground == vtbackend::RGBColor(0x01, 0x02, 0x03));
    }

    SECTION("the writer emits what was set, and only that")
    {
        // Every other palette entry hands the writer its live values; this one used to emit a fully
        // commented example whatever the scheme said, so a configured fold column did not survive a
        // write. Emitting the full block instead would be the opposite error -- it would PIN colours
        // an unset pair deliberately derives.
        auto config = classicConfig();
        auto& scheme = config.colorschemes.value()["default"];
        scheme.foldMarker = vtbackend::RGBColorPair { .foreground = vtbackend::RGBColor(0x90, 0x90, 0x90),
                                                      .background = vtbackend::RGBColor(0x1A, 0x1A, 0x1A) };

        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
        REQUIRE(written.contains("fold_marker:"));

        auto const reloaded = schemeOf(loadFromYaml(dir, written));
        REQUIRE(reloaded.foldMarker.has_value());
        CHECK(reloaded.foldMarker->foreground == vtbackend::RGBColor(0x90, 0x90, 0x90));
        CHECK(reloaded.foldMarker->background == vtbackend::RGBColor(0x1A, 0x1A, 0x1A));
        CHECK_FALSE(reloaded.foldMarkerHover.has_value());
    }
}

TEST_CASE("Config: a scheme's context tints are written back", "[config]")
{
    // A tint row carries two values, the context type and its colour. The writer used to take a first value
    // that was a std::string_view for the name of a configuration entry and drop it, so the row reached
    // std::vformat one argument short and threw: no scheme that set a tint could be written at all.
    QTemporaryDir dir;
    auto const elevate = static_cast<size_t>(vtbackend::ContextType::Elevate);
    auto const tint = vtbackend::RGBColor(0x2A, 0x1A, 0x1A);

    auto const setCount = [](auto const& tints) {
        return std::ranges::count_if(tints, [](auto const& color) { return color.has_value(); });
    };

    SECTION("in the configuration file")
    {
        auto config = classicConfig();
        config.colorschemes.value()["default"].contextTints[elevate] = tint;

        auto const tints =
            schemeOf(
                loadFromYaml(dir, contour::config::createString<contour::config::YAMLConfigWriter>(config)))
                .contextTints;
        CHECK(tints[elevate] == tint);
        CHECK(setCount(tints) == 1);
    }

    SECTION("in a scheme's own file")
    {
        auto palette = vtbackend::ColorPalette {};
        palette.contextTints[elevate] = tint;
        writeSideFile(dir, "colorschemes/tinted.yml", contour::config::emitColorSchemeYaml(palette));

        auto const tints = schemeOf(loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        colors: tinted
)"))
                               .contextTints;
        CHECK(tints[elevate] == tint);
        CHECK(setCount(tints) == 1);
    }
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

TEST_CASE("Config: scrollbar.marks loads a list of mark sources", "[config][scrollbar-marks]")
{
    using vtbackend::ScrollbarMarkSource;
    using vtbackend::ScrollbarMarkSources;
    QTemporaryDir dir;

    // The profile's scrollbar.marks after loading a scrollbar block whose marks line is @p marksLine.
    auto const marksAfter = [&](std::string_view marksLine) {
        auto const config = loadFromYaml(dir,
                                         std::format(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        scrollbar:
            position: Right
            {}
)",
                                                     marksLine));
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        return profile->scrollbar.value().marks;
    };

    SECTION("absent: every source, the default")
    {
        CHECK(marksAfter("") == vtbackend::AllScrollbarMarkSources);
    }

    SECTION("a list names exactly the sources it lists, in any case")
    {
        CHECK(marksAfter("marks: [failures, user_marks]")
              == ScrollbarMarkSources { ScrollbarMarkSource::Failures, ScrollbarMarkSource::UserMarks });
        CHECK(marksAfter("marks: [Commands]") == ScrollbarMarkSources { ScrollbarMarkSource::Commands });
    }

    SECTION("[] switches every tick off")
    {
        CHECK(marksAfter("marks: []").none());
    }

    SECTION("an unknown token is reported by its key path and skipped; the rest still load")
    {
        auto capture = core::log::ScopedCapture { "error" };
        CHECK(marksAfter("marks: [failures, sparkles]")
              == ScrollbarMarkSources { ScrollbarMarkSource::Failures });
        INFO("captured error log:\n" << capture.text());
        CHECK(capture.contains("sparkles"));
        CHECK(capture.contains("profiles.main.scrollbar.marks"));
    }

    SECTION("an item other than a word is reported and skipped; the rest still load")
    {
        auto capture = core::log::ScopedCapture { "error" };
        CHECK(marksAfter("marks: [[commands], user_marks]")
              == ScrollbarMarkSources { ScrollbarMarkSource::UserMarks });
        INFO("captured error log:\n" << capture.text());
        CHECK(capture.contains("profiles.main.scrollbar.marks"));
    }

    SECTION("a scalar is not a list: reported, and the setting is left alone")
    {
        auto capture = core::log::ScopedCapture { "error" };
        CHECK(marksAfter("marks: failures") == vtbackend::AllScrollbarMarkSources);
        INFO("captured error log:\n" << capture.text());
        CHECK(capture.contains("profiles.main.scrollbar.marks"));
    }
}

TEST_CASE("Config: scrollbar.marks round-trips through the writer", "[config][scrollbar-marks]")
{
    using vtbackend::ScrollbarMarkSource;
    using vtbackend::ScrollbarMarkSources;

    // `contour generate config` spells the default out, so a user editing it sees every token.
    CHECK(contour::config::defaultConfigString().contains("marks: [failures, commands, user_marks]"));

    auto const cases = std::array {
        ScrollbarMarkSources {},
        ScrollbarMarkSources { ScrollbarMarkSource::Failures },
        ScrollbarMarkSources { ScrollbarMarkSource::Commands, ScrollbarMarkSource::UserMarks },
        vtbackend::AllScrollbarMarkSources,
    };
    for (auto const marks: cases)
    {
        INFO("marks bits: " << static_cast<int>(marks.value()));
        QTemporaryDir dir;
        auto const source = loadFromYaml(dir, "default_profile: main\n");
        auto profile = *source.findProfile("main");
        profile.scrollbar.value().marks = marks;
        writeSideFile(dir, "profiles/roundtrip.yml", contour::config::emitProfileYaml(profile));

        auto const reloaded = loadFromYaml(dir, "default_profile: main\n");
        auto const* roundTripped = reloaded.findProfile("roundtrip");
        REQUIRE(roundTripped != nullptr);
        CHECK(roundTripped->scrollbar.value().marks == marks);
    }
}

TEST_CASE("Config: the scrollbar is shown on the right by default", "[config][scrollbar-marks]")
{
    // The scrollbar -- and with it the command marks -- is visible out of the box. `position: hidden` in a
    // profile, or the settings page, turns it off.
    using contour::config::ScrollBarPosition;

    CHECK(contour::config::ScrollBarConfig {}.position == ScrollBarPosition::Right);
    CHECK(contour::config::TerminalProfile {}.scrollbar.value().position == ScrollBarPosition::Right);

    // A profile that never mentions the scrollbar gets it ...
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
)"sv);
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(profile->scrollbar.value().position == ScrollBarPosition::Right);

    // ... `contour generate config` writes it out, so a user editing the file sees where it is ...
    auto const defaults = contour::config::Config {};
    auto const generated = YAML::Load(contour::config::defaultConfigString());
    auto const position = generated["profiles"][defaults.defaultProfileName.value()]["scrollbar"]["position"];
    REQUIRE(position.IsScalar());
    // The writer's spelling (std::formatter<ScrollBarPosition>); the reader ignores case.
    CHECK(position.as<std::string>() == "Right");

    // ... and `hidden` still turns it off.
    auto const hidden = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        scrollbar:
            position: hidden
)"sv);
    auto const* hiddenProfile = hidden.profile("main");
    REQUIRE(hiddenProfile != nullptr);
    CHECK(hiddenProfile->scrollbar.value().position == ScrollBarPosition::Hidden);
}

TEST_CASE("Config: folding loads from YAML and reaches the emulation settings", "[config]")
{
    QTemporaryDir dir;
    // Folding is a GLOBAL section, not a per-profile one: a fold is a property of how the user reads
    // output, not of which shell produced it, so it sits beside `images` rather than inside a profile.
    auto const config = loadFromYaml(dir, R"(
default_profile: main
folding:
    enabled: true
    show_markers: false
    auto_collapse_on_new_command: true
    on_jump_into_fold: skip
profiles:
    main:
        shell: /bin/sh
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);

    CHECK(config.folding.value().enabled == true);
    CHECK(config.folding.value().showMarkers == false);
    CHECK(config.folding.value().autoCollapseOnNewCommand == true);
    CHECK(config.folding.value().onJumpIntoFold == vtbackend::FoldJumpBehavior::Skip);

    // auto_collapse is what vtbackend actually needs to know about, and it is conjoined with `enabled`
    // at the boundary so the emulation never has to consult two flags.
    auto const settings = contour::config::emulationSettings(config, *profile);
    CHECK(settings.autoCollapseFoldOnNewCommand == true);
    CHECK(settings.foldJumpBehavior == vtbackend::FoldJumpBehavior::Skip);
}

TEST_CASE("Config: osc_context loads from YAML and reaches the emulation settings", "[config]")
{
    QTemporaryDir dir;
    // A GLOBAL section, not per-profile: this says how the terminal READS a protocol the shell speaks,
    // not how a pane looks. The two knobs that are presentation live in the colour scheme and the
    // indicator template instead.
    auto const config = loadFromYaml(dir, R"(
default_profile: main
osc_context:
    enabled: true
    max_depth: 8
    max_retained_contexts: 64
    derive_markers: never
    tinting: all
profiles:
    main:
        shell: /bin/sh
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);

    CHECK(config.oscContext.value().enabled == true);
    CHECK(config.oscContext.value().maxDepth == 8);
    CHECK(config.oscContext.value().maxRetained == 64);
    CHECK(config.oscContext.value().deriveMarkers == vtbackend::ContextMarkPolicy::Never);
    CHECK(config.oscContext.value().tinting == vtbackend::ContextTintScope::All);

    auto const settings = contour::config::emulationSettings(config, *profile);
    CHECK(settings.contextLimits.maxDepth == 8);
    CHECK(settings.contextLimits.maxRetained == 64);
    CHECK(settings.contextMarkPolicy == vtbackend::ContextMarkPolicy::Never);
    CHECK(settings.contextTintScope == vtbackend::ContextTintScope::All);
}

TEST_CASE("Config: osc_context defaults, and disabling it silences marks and tints", "[config]")
{
    QTemporaryDir dir;
    auto const defaults = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
)"sv);
    CHECK(defaults.oscContext.value().enabled == true);
    CHECK(defaults.oscContext.value().maxDepth == 16);
    CHECK(defaults.oscContext.value().maxRetained == 256);
    CHECK(defaults.oscContext.value().deriveMarkers == vtbackend::ContextMarkPolicy::WhenAlone);
    CHECK(defaults.oscContext.value().tinting == vtbackend::ContextTintScope::Boundaries);

    QTemporaryDir off;
    // The single kill switch. It travels as itself rather than being folded into the two policies
    // below, because it has to gate the ANCESTRY: a `false` that only forced Never/Off would still
    // leave the stack tracking, the breadcrumb drawing and the daemon replicating -- which is what the
    // documented "nothing is tracked, nothing is derived and nothing is tinted" promises it does not.
    auto const disabled = loadFromYaml(off, R"(
default_profile: main
osc_context:
    enabled: false
    derive_markers: when_alone
    tinting: all
profiles:
    main:
        shell: /bin/sh
)"sv);
    auto const* profile = disabled.profile("main");
    REQUIRE(profile != nullptr);
    auto const settings = contour::config::emulationSettings(disabled, *profile);
    CHECK(settings.contextSignalling == vtbackend::ContextSignalling::Disabled);
    // The two policies pass through untouched: with the sequence refused outright there is no ancestry
    // for either to describe, so silencing them a second time would say nothing extra.
    CHECK(settings.contextMarkPolicy == vtbackend::ContextMarkPolicy::WhenAlone);
    CHECK(settings.contextTintScope == vtbackend::ContextTintScope::All);

    // That the engine then HONOURS this is asserted where the engine lives:
    // Screen_context_test.cpp, "osc3008 is not read at all when signalling is disabled".
}

TEST_CASE("Config: osc_context limits are clamped to a usable range", "[config]")
{
    QTemporaryDir dir;
    // A retained bound below the depth bound would let a context that is still an ancestor be evicted,
    // leaving the chain holding a record nothing can resolve.
    auto const config = loadFromYaml(dir, R"(
default_profile: main
osc_context:
    max_depth: 0
    max_retained_contexts: 1
profiles:
    main:
        shell: /bin/sh
)"sv);
    CHECK(config.oscContext.value().maxDepth >= 1);
    CHECK(config.oscContext.value().maxRetained >= config.oscContext.value().maxDepth);

    QTemporaryDir huge;
    auto const big = loadFromYaml(huge, R"(
default_profile: main
osc_context:
    max_depth: 60000
profiles:
    main:
        shell: /bin/sh
)"sv);
    CHECK(big.oscContext.value().maxDepth <= 256);
}

TEST_CASE("Config: an unknown osc_context enum value keeps the default", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
osc_context:
    derive_markers: nonsense
    tinting: alsononsense
profiles:
    main:
        shell: /bin/sh
)"sv);
    CHECK(config.oscContext.value().deriveMarkers == vtbackend::ContextMarkPolicy::WhenAlone);
    CHECK(config.oscContext.value().tinting == vtbackend::ContextTintScope::Boundaries);
}

TEST_CASE("Config: command_blocks loads from YAML and reaches the emulation settings", "[config]")
{
    QTemporaryDir dir;
    // GLOBAL, like folding: it says how the terminal reads the protocol a shell speaks.
    auto const config = loadFromYaml(dir, R"(
default_profile: main
command_blocks:
    max_records: 250
profiles:
    main:
        shell: /bin/sh
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(config.commandBlocks.value().maxRecords == 250);

    auto const settings = contour::config::emulationSettings(config, *profile);
    CHECK(settings.commandBlockLimits.maxRecords == 250);
}

TEST_CASE("Config: command_blocks defaults to a thousand records and is written back", "[config]")
{
    auto const defaults = contour::config::Config {};
    CHECK(defaults.commandBlocks.value().maxRecords == 1000);

    auto const& profile = defaults.profiles.value().at(defaults.defaultProfileName.value());
    CHECK(contour::config::emulationSettings(defaults, profile).commandBlockLimits.maxRecords == 1000);

    auto const generated = contour::config::defaultConfigString();
    CHECK(generated.contains("command_blocks:"));
    CHECK(generated.contains("max_records: 1000"));
}

TEST_CASE("Config: command_blocks.max_records is clamped to a usable range", "[config]")
{
    auto const load = [](std::string_view value) {
        QTemporaryDir dir;
        auto const yaml = contour::test::profileConfigYaml(
            "main", std::format("command_blocks:\n    max_records: {}\n", value));
        return loadFromYaml(dir, yaml).commandBlocks.value().maxRecords;
    };

    // The current block must always resolve, so a store of none is a store of one...
    CHECK(load("0") == 1);
    // ...a negative count is not a reason to throw or wrap to "unbounded"...
    CHECK(load("-5") == 1);
    // ...and a flood of OSC 133 must not be able to cost unbounded memory through the configuration.
    CHECK(load("99999999999") == static_cast<size_t>(contour::config::MaxCommandBlockRecords));
}

TEST_CASE("Config: folding defaults, and auto-collapse travels beside its switch", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
folding:
    enabled: false
    auto_collapse_on_new_command: true
profiles:
    main:
        shell: /bin/sh
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);

    // Unspecified keys keep their defaults.
    CHECK(config.folding.value().showMarkers == true);
    CHECK(config.folding.value().onJumpIntoFold == vtbackend::FoldJumpBehavior::Expand);

    // `auto_collapse_on_new_command` travels as itself, beside the switch: the terminal refuses every
    // collapse while folding is off, the automatic one included, so `enabled: false` still wins.
    auto const settings = contour::config::emulationSettings(config, *profile);
    CHECK(settings.autoCollapseFoldOnNewCommand);
    CHECK(settings.foldingAvailability == vtbackend::FoldingAvailability::Disabled);
}

TEST_CASE("Config: folding decides the block column's fold controls, not whether the column is there",
          "[config]")
{
    // The fold switch reaches the terminal as its gutter's fold controls. The block column itself stays
    // for as long as exit status or user marks still draw in it, and goes only once nothing does.
    struct Case
    {
        std::string_view folding;
        bool foldMarkers;
        int gutterColumns;
    };
    auto const cases = std::array {
        Case { .folding = "", .foldMarkers = true, .gutterColumns = 2 },
        Case { .folding = "folding:\n    show_markers: false\n", .foldMarkers = false, .gutterColumns = 2 },
        Case { .folding = "folding:\n    enabled: false\n", .foldMarkers = false, .gutterColumns = 2 },
        Case { .folding = "folding:\n    show_markers: false\n"
                          "gutter:\n    exit_status: false\n    user_marks: false\n",
               .foldMarkers = false,
               .gutterColumns = 0 },
    };
    for (auto const& testCase: cases)
    {
        CAPTURE(testCase.folding);
        QTemporaryDir dir;
        auto const config = loadFromYaml(dir, contour::test::profileConfigYaml("main", testCase.folding));
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);

        auto const gutter = contour::config::emulationSettings(config, *profile).gutter;
        CHECK(gutter.foldMarkers == testCase.foldMarkers);
        CHECK(gutter.foldMarkers == config.folding.value().markersVisible());
        CHECK(vtbackend::gutterLayoutFor(gutter).totalColumns == testCase.gutterColumns);
    }
}

TEST_CASE("Config: an unknown on_jump_into_fold value keeps the default", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
folding:
    on_jump_into_fold: sideways
profiles:
    main:
        shell: /bin/sh
)"sv);

    CHECK(config.folding.value().onJumpIntoFold == vtbackend::FoldJumpBehavior::Expand);
}

TEST_CASE("Config: folding.enabled reaches the engine as its availability", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
folding:
    enabled: false
profiles:
    main:
        shell: /bin/sh
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);

    CHECK(contour::config::FoldingConfig {}.availability() == vtbackend::FoldingAvailability::Enabled);
    CHECK(config.folding.value().availability() == vtbackend::FoldingAvailability::Disabled);
    // The vi `z` keys reach the engine without passing the session's gate, so the engine is told.
    CHECK(contour::config::emulationSettings(config, *profile).foldingAvailability
          == vtbackend::FoldingAvailability::Disabled);
}

TEST_CASE("Config: images.sixel_scrolling reaches the emulation settings", "[config]")
{
    // Through the emulation settings, so a daemon-hosted session -- which gets nothing else -- starts with it
    // and returns to it on RIS, as a local one does.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
images:
    sixel_scrolling: false
profiles:
    main:
        shell: /bin/sh
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(contour::config::emulationSettings(config, *profile).sixelScrolling
          == vtbackend::SixelScrolling::Disabled);
    CHECK(contour::config::emulationSettings(contour::config::Config {}, *profile).sixelScrolling
          == vtbackend::SixelScrolling::Enabled);
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
    // The weight was fed but never asserted, so the reader could have dropped it unnoticed --
    // which is exactly what it did for every spelling but this one.
    CHECK(profile->fonts.value().regular.weight == text::FontWeight::Bold);
}

TEST_CASE("Config: the font render mode loads from its documented key", "[config]")
{
    // The key is `render_mode`: it is what every released version reads, what metainfo.xml
    // announces, and the only spelling a user has ever had reason to write. An identifier sweep
    // briefly renamed the string literal along with the C++ type, which changed no code but
    // invalidated every existing profile in silence.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        font:
            render_mode: lcd
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(profile->fonts.value().renderMode == text::RenderMode::LCD);
}

TEST_CASE("Config: font weight and slant accept the spellings Contour itself writes", "[config]")
{
    // `contour generate config` formats these through std::formatter, which emits CamelCase, while
    // the documentation advertises the underscore forms. Both must load, or the config Contour
    // generated does not survive its own reader.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        font:
            regular:
                family: "Fira Code"
                weight: ExtraBold
                slant: Italic
            bold:
                family: "Fira Code"
                weight: extra_light
            italic:
                family: "Fira Code"
                slant: oblique
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const& fonts = profile->fonts.value();
    CHECK(fonts.regular.weight == text::FontWeight::ExtraBold);
    CHECK(fonts.regular.slant == text::FontSlant::Italic);
    CHECK(fonts.bold.weight == text::FontWeight::ExtraLight);
    CHECK(fonts.italic.slant == text::FontSlant::Oblique);
}

TEST_CASE("Config: an unparsable font weight or slant is reported, not swallowed", "[config]")
{
    // Silence here is worse than elsewhere: the italic and bold-italic faces are pre-seeded with
    // FontSlant::Italic before the file is read, so a dropped value leaves the inherited Italic
    // rather than the default -- the user sees a slant they did not ask for and no diagnostic.
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };

    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        font:
            regular:
                family: "Fira Code"
                weight: heavyish
                slant: itallic
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(profile->fonts.value().regular.weight == text::FontWeight::Normal);
    CHECK(profile->fonts.value().regular.slant == text::FontSlant::Normal);

    auto const& log = capture.text();
    INFO("captured error log:\n" << log);
    CHECK(log.contains("heavyish"));
    CHECK(log.contains("itallic"));
}

TEST_CASE("Config: the deprecated slant 'thin' still loads, and says so", "[config]")
{
    // `thin` is a weight; it reached the slant table as a copy-paste and resolved to Normal for
    // years. Removing it outright would silently flip an existing profile's italic face from
    // upright to slanted, so it is honoured and announced instead.
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };

    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        font:
            italic:
                family: "Fira Code"
                slant: thin
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(profile->fonts.value().italic.slant == text::FontSlant::Normal);

    auto const& log = capture.text();
    INFO("captured error log:\n" << log);
    CHECK(log.contains("thin"));
    CHECK(log.contains("deprecated"));
}

TEST_CASE("Config: the generated default config loads back into the defaults", "[config]")
{
    // `contour generate config` writes this file for the user to edit, so every key it emits and
    // every value it formats must be something the reader accepts. The two sides were written
    // independently: the writer emitted `weight: Regular` and `slant: Normal`, neither of which
    // the parser knew, and the reader looked up a key the writer never wrote. Both failures were
    // silent, because an unmatched key and an unparsed value both just leave the default in place.
    //
    // The empty error log is the load-bearing assertion, not the comparison. Because the generated
    // file holds exactly the defaults, a silently dropped value leaves the field AT the default and
    // the comparison still passes -- which is precisely how these bugs survived. What cannot pass
    // is a reader reporting that it did not understand something Contour itself wrote.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());

    auto capture = core::log::ScopedCapture { "error" };

    auto const generated = contour::config::defaultConfigString();
    auto const reloaded = loadFromYaml(dir, generated);
    auto const defaults = contour::config::Config {};

    INFO("generated config:\n" << generated);
    CHECK(capture.text().empty());

    auto const* profile = reloaded.profile(defaults.defaultProfileName.value());
    REQUIRE(profile != nullptr);
    auto const& expected = defaults.profiles.value().at(defaults.defaultProfileName.value()).fonts.value();

    // Whole-struct, so a field added to FontDescriptions is covered without editing this test.
    CHECK(profile->fonts.value() == expected);
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
    // The fallback sequence populates a FontFallbackList.
    REQUIRE(std::holds_alternative<text::FontFallbackList>(profile->fonts.value().regular.fontFallback));
    auto const& list = std::get<text::FontFallbackList>(profile->fonts.value().regular.fontFallback);
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
    CHECK(std::holds_alternative<text::FontFallbackNone>(profile->fonts.value().regular.fontFallback));
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
             // `harfbuzz` is what the writer emits for OpenShaper, and it is the default engine on
             // every platform -- so until the reader learned this spelling, the engine line in a
             // generated config was dropped on load.
             { "harfbuzz", vtrasterizer::TextShapingEngine::OpenShaper },
             { "DirectWrite", vtrasterizer::TextShapingEngine::DWrite },
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
    // portable env wrappers (POSIX ::setenv is unavailable on MSVC) -- Qt is fine here because this
    // suite only ever builds with the GUI frontend, and writing through it is in fact the stronger
    // assertion: the replacer reads a core::LiveEnvironment, so this also proves that reader sees
    // a variable written after the process started, which is the whole reason it is not the cached
    // one. (A test that needs to control what the replacer reads injects its own environment into
    // YAMLConfigReader instead; loadFromYaml goes through the production entry point on purpose.)
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
    - { mods: [Control], key: 'D', action: CopySelection, format: png }
    - { mods: [Control], mouse: Left, action: FollowHyperlink }
    - { mods: [], key: F5, action: ScreenshotVT }
)"sv);

    auto const& mappings = config.inputMappings.value();

    auto const hasKeyAction = [&](auto const& actionTag) {
        return std::ranges::any_of(mappings.keyMappings, [&](auto const& m) {
            return std::holds_alternative<std::remove_cvref_t<decltype(actionTag)>>(m.binding.at(0));
        });
    };
    auto const hasCharAction = [&](auto const& actionTag) {
        return std::ranges::any_of(mappings.charMappings, [&](auto const& m) {
            return std::holds_alternative<std::remove_cvref_t<decltype(actionTag)>>(m.binding.at(0));
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
    bool const foundSendChars = sendChars != mappings.charMappings.end();
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

    // CopySelection format parsing: HTML parses, png ignoring its case; an invalid format falls back to Text.
    auto copyFormats = std::vector<contour::actions::CopyFormat> {};
    for (auto const& m: mappings.charMappings)
        if (std::holds_alternative<contour::actions::CopySelection>(m.binding.at(0)))
            copyFormats.push_back(std::get<contour::actions::CopySelection>(m.binding.at(0)).format);
    for (auto const& m: mappings.keyMappings)
        if (std::holds_alternative<contour::actions::CopySelection>(m.binding.at(0)))
            copyFormats.push_back(std::get<contour::actions::CopySelection>(m.binding.at(0)).format);
    CHECK(std::ranges::count(copyFormats, contour::actions::CopyFormat::HTML) == 1);
    CHECK(std::ranges::count(copyFormats, contour::actions::CopyFormat::Text) == 1);
    CHECK(std::ranges::count(copyFormats, contour::actions::CopyFormat::PNG) == 1);

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

// {{{ extensions: and status_line.extension
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
} // namespace

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

namespace
{

/// Every enumerator of @p Enum has a row in its table, and no other row exists.
///
/// The table is the single source the YAML reader, the config writer, the settings page and any menu
/// offering these values all read from. An enumerator missing a row could therefore not be written,
/// read back, or shown -- so pin that every one of them has a row with a label, and that a row's token
/// survives the round trip the config file depends on. Walking the enumerators rather than the rows
/// names the enumerator that has none.
/// @param enumerators Every enumerator of @p Enum.
template <typename Enum, size_t Count>
void checkEnumerators(std::array<Enum, Count> const& enumerators)
{
    using namespace contour::config;
    CHECK(configEnumValues<Enum>().size() == Count);
    for (auto const value: enumerators)
    {
        auto const token = configEnumToken(value);
        INFO("enumerator: " << static_cast<int>(value) << ", token: " << token);
        CHECK(!token.empty());
        CHECK(!configEnumLabel(value).empty());
        CHECK(configEnumFromToken<Enum>(token) == value);
    }
}

} // namespace

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
                                  WindowControlStyle::Plasma });
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

namespace
{

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

} // namespace

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

    checkEnumerators(std::array {
        ScrollbarMarkSource::Failures, ScrollbarMarkSource::Commands, ScrollbarMarkSource::UserMarks });
    // The spellings the documentation promises.
    CHECK(configEnumToken(ScrollbarMarkSource::Failures) == "failures");
    CHECK(configEnumToken(ScrollbarMarkSource::Commands) == "commands");
    CHECK(configEnumToken(ScrollbarMarkSource::UserMarks) == "user_marks");

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
    CHECK(config.tabBarVisibility.value() == TabBarVisibility::Multiple); // the Native2026 default
}

TEST_CASE("Config: invalid tab_bar_* values fall back to the defaults", "[config]")
{
    using contour::config::TabBarPosition;
    using contour::config::TabBarVisibility;

    QTemporaryDir dir;
    // Unrecognized tokens must leave the fields at their ConfigEntry defaults (Top / Multiple), not throw
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
    CHECK(config.tabBarVisibility.value() == TabBarVisibility::Multiple); // the Native2026 default
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

namespace
{

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
};

/// Every first-launch value of the 2026 redesign.
constexpr auto Native2026Expectation = ExpectedEditionDefaults {
    .edition = contour::config::DefaultsEdition::Native2026,
#if defined(_WIN32)
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
    .statusLineLeft = " {InputMode:Bold}{Context:Left=  }",
    .statusLineMiddle = "{Title}",
    .statusLineRight = "{HistoryLineCount:Faint}  {Clock:Faint} ",
    .linkDecoration = vtbackend::LinkDecorationMode::Modifier,
    .dimUnfocused = 0.15,
    .bellSound = "off",
    .scrollback = 10000,
    .insertCursorShape = vtbackend::CursorShape::Block,
    .screenTransitionDuration = std::chrono::milliseconds { 150 },
    .darkColorScheme = "contour-dark",
    .lightColorScheme = "contour-light",
    .tabBarVisibility = contour::config::TabBarVisibility::Multiple,
#ifdef __APPLE__
    .followHyperlinkModifier = vtbackend::Modifier::Super, // Command: Qt's Control/Meta swap is off
#else
    .followHyperlinkModifier = vtbackend::Modifier::Control,
#endif
    .theme = contour::config::GuiTheme::Terminal,
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
    CHECK(profile.tabLabel.value() == "{WindowTitle}");

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
    auto const& mappings = contour::config::defaultInputMappings();

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

TEST_CASE("config.tab_switch_on_horizontal_wheel", "[config]")
{
    QTemporaryDir dir;

    SECTION("defaults to true")
    {
        CHECK(contour::config::Config {}.tabSwitchOnHorizontalWheel.value());
    }

    SECTION("loads false")
    {
        auto const config = loadFromYaml(dir,
                                         "tab_switch_on_horizontal_wheel: false\n"
                                         "profiles:\n  main:\n    shell: \"/bin/bash\"\n");
        CHECK_FALSE(config.tabSwitchOnHorizontalWheel.value());
    }

    SECTION("round-trips through the generated config")
    {
        // The writer is the reflection walk in createForGlobal(), so this pins that the new entry is
        // actually emitted -- a field it skipped would silently reset on every GUI-driven save.
        auto config = contour::config::Config {};
        config.tabSwitchOnHorizontalWheel = false;
        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
        REQUIRE(written.contains("tab_switch_on_horizontal_wheel"));

        auto const reloaded = loadFromYaml(dir, written);
        CHECK_FALSE(reloaded.tabSwitchOnHorizontalWheel.value());
    }
}

TEST_CASE("config.search_case_sensitivity", "[config]")
{
    using vtbackend::SearchCaseSensitivity;

    QTemporaryDir dir;
    auto const profileWith = [](std::string_view entry) {
        return std::string("profiles:\n  main:\n    shell: \"/bin/bash\"\n    ") + std::string(entry) + "\n";
    };
    auto const modeOf = [&](std::string_view entry) {
        auto const config = loadFromYaml(dir, profileWith(entry));
        return config.profile("main")->searchCaseSensitivity.value();
    };

    SECTION("defaults to smart, which is what the terminal has always done")
    {
        // The regression gate: an existing configuration must keep the behaviour it had.
        CHECK(contour::config::TerminalProfile {}.searchCaseSensitivity.value()
              == SearchCaseSensitivity::Smart);
    }

    SECTION("loads every value, ignoring case")
    {
        CHECK(modeOf("search_case_sensitivity: sensitive") == SearchCaseSensitivity::Sensitive);
        CHECK(modeOf("search_case_sensitivity: INSENSITIVE") == SearchCaseSensitivity::Insensitive);
        CHECK(modeOf("search_case_sensitivity: SmArT") == SearchCaseSensitivity::Smart);
    }

    SECTION("keeps the default when the value is not a known token")
    {
        CHECK(modeOf("search_case_sensitivity: whatever") == SearchCaseSensitivity::Smart);
    }

    SECTION("the retired search_mode_switch still loads, and does not disturb this one")
    {
        // The promise made when search_mode_switch was deprecated: a configuration carrying it keeps
        // loading. The reader warns on its presence and parses nothing, so the rest of the profile --
        // this key included, which sits directly after it -- must come through untouched.
        auto const config = loadFromYaml(dir,
                                         "profiles:\n  main:\n    shell: \"/bin/bash\"\n"
                                         "    search_mode_switch: true\n"
                                         "    search_case_sensitivity: sensitive\n");
        auto const profile = config.profile("main");
        REQUIRE(profile != nullptr);
        CHECK(profile->searchCaseSensitivity.value() == SearchCaseSensitivity::Sensitive);
    }

    SECTION("round-trips through the generated config")
    {
        // Pins the reflection walk AND the formatter: a field the writer skipped would silently reset
        // on every GUI-driven save, and a formatter emitting the enumerator rather than the token
        // would write a file that no longer re-parses.
        auto config = contour::config::Config {};
        config.profile("main")->searchCaseSensitivity = SearchCaseSensitivity::Sensitive;

        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
        REQUIRE(written.contains("search_case_sensitivity: sensitive"));

        auto const reloaded = loadFromYaml(dir, written);
        CHECK(reloaded.profile("main")->searchCaseSensitivity.value() == SearchCaseSensitivity::Sensitive);
    }
}

TEST_CASE("config.ui_style", "[config]")
{
    using contour::config::UiStyle;

    QTemporaryDir dir;

    SECTION("defaults to the native chrome")
    {
        // The default is the regression gate for the whole feature: an existing configuration must
        // keep the appearance it had, so nothing here may change without an explicit opt-in.
        CHECK(contour::config::Config {}.uiStyle.value() == UiStyle::Native);
        CHECK(contour::config::Config {}.uiFontFamily.value().empty());
        CHECK(contour::config::Config {}.uiFontSize.value() == 0.0);
    }

    SECTION("loads every value, ignoring case")
    {
        auto const profiles = "profiles:\n  main:\n    shell: \"/bin/bash\"\n";
        CHECK(loadFromYaml(dir, std::string("ui_style: terminal\n") + profiles).uiStyle.value()
              == UiStyle::Terminal);
        CHECK(loadFromYaml(dir, std::string("ui_style: TERMINAL\n") + profiles).uiStyle.value()
              == UiStyle::Terminal);
        CHECK(loadFromYaml(dir, std::string("ui_style: native\n") + profiles).uiStyle.value()
              == UiStyle::Native);
    }

    SECTION("keeps the default when the value is not a known token")
    {
        auto const config = loadFromYaml(dir,
                                         "ui_style: ascii-art\n"
                                         "profiles:\n  main:\n    shell: \"/bin/bash\"\n");
        CHECK(config.uiStyle.value() == UiStyle::Native);
    }

    SECTION("loads the chrome font overrides")
    {
        auto const config = loadFromYaml(dir,
                                         "ui_font_family: \"JetBrains Mono\"\n"
                                         "ui_font_size: 11.5\n"
                                         "profiles:\n  main:\n    shell: \"/bin/bash\"\n");
        CHECK(config.uiFontFamily.value() == "JetBrains Mono");
        CHECK(config.uiFontSize.value() == 11.5);
    }

    SECTION("round-trips through the generated config")
    {
        // The writer is the reflection walk in createForGlobal(), so this pins that the new entries
        // are actually emitted -- a field it skipped would silently reset on every GUI-driven save.
        auto config = contour::config::Config {};
        config.uiStyle = UiStyle::Terminal;
        config.uiFontFamily = "JetBrains Mono";
        config.uiFontSize = 11.0;
        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
        REQUIRE(written.contains("ui_style"));
        REQUIRE(written.contains("ui_font_family"));
        REQUIRE(written.contains("ui_font_size"));

        // The formatter must emit the token the reader accepts, not the enumerator name -- that is
        // what makes the round trip work at all.
        REQUIRE(written.contains("ui_style: terminal"));

        auto const reloaded = loadFromYaml(dir, written);
        CHECK(reloaded.uiStyle.value() == UiStyle::Terminal);
        CHECK(reloaded.uiFontFamily.value() == "JetBrains Mono");
        CHECK(reloaded.uiFontSize.value() == 11.0);
    }

    SECTION("round-trips the EMPTY chrome font, which is the line every installation gets")
    {
        // createDefaultConfig() writes from a default-constructed Config, so an unquoted template
        // emits a bare `ui_font_family:` -- a YAML null, which the string reader cannot convert and
        // which therefore logs a conversion failure on every single start, for a key nobody set.
        auto const written =
            contour::config::createString<contour::config::YAMLConfigWriter>(contour::config::Config {});
        REQUIRE(written.contains("ui_font_family: \"\""));

        auto const reloaded = loadFromYaml(dir, written);
        CHECK(reloaded.uiFontFamily.value().empty());
    }

    SECTION("round-trips a family name YAML would otherwise reinterpret")
    {
        // "@MS Gothic" is a genuine Windows family, and a leading @ is a reserved YAML indicator --
        // unquoted it is not a string the parser will hand back. The quotes are what make the
        // writer's own `"`/`\` escaping mean anything, too.
        auto config = contour::config::Config {};
        config.uiFontFamily = "@MS Gothic";
        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
        CHECK(loadFromYaml(dir, written).uiFontFamily.value() == "@MS Gothic");
    }
}

TEST_CASE("config.parseMouseButton accepts the horizontal wheel", "[config]")
{
    QTemporaryDir dir;

    SECTION("WheelLeft and WheelRight are bindable, case-insensitively")
    {
        auto const config = loadFromYaml(dir,
                                         "input_mapping:\n"
                                         "    - { mods: [], mouse: WheelRight, action: SwitchToTabLeft }\n"
                                         "    - { mods: [], mouse: wheelleft,  action: SwitchToTabRight }\n"
                                         "profiles:\n  main:\n    shell: \"/bin/bash\"\n");

        auto const& mouseMappings = config.inputMappings.value().mouseMappings;
        auto const bound = [&](vtbackend::MouseButton button) {
            return std::ranges::any_of(mouseMappings, [button](auto const& m) { return m.input == button; });
        };
        CHECK(bound(vtbackend::MouseButton::WheelRight));
        CHECK(bound(vtbackend::MouseButton::WheelLeft));
    }
}

TEST_CASE("config.builtinFallbackMouseMappings", "[config]")
{
    auto const& fallbacks = contour::config::builtinFallbackMouseMappings();

    auto const rowFor = [&fallbacks](vtbackend::MouseButton button) {
        auto const it = std::ranges::find_if(
            fallbacks, [button](auto const& row) { return row.mapping.input == button; });
        REQUIRE(it != fallbacks.end());
        return it;
    };

    SECTION("the right button opens the context menu")
    {
        auto const& row = *rowFor(vtbackend::MouseButton::Right);

        CHECK(row.mapping.modifiers == vtbackend::Modifiers { vtbackend::Modifier::None });
        REQUIRE(row.mapping.binding.size() == 1);
        CHECK(std::holds_alternative<contour::actions::OpenContextMenu>(row.mapping.binding.front()));
        CHECK(row.enabled(contour::config::Config {}));
    }

    SECTION("the horizontal wheel switches tabs")
    {
        auto const& left = *rowFor(vtbackend::MouseButton::WheelLeft);
        auto const& right = *rowFor(vtbackend::MouseButton::WheelRight);

        REQUIRE(left.mapping.binding.size() == 1);
        CHECK(std::holds_alternative<contour::actions::SwitchToTabLeft>(left.mapping.binding.front()));
        REQUIRE(right.mapping.binding.size() == 1);
        CHECK(std::holds_alternative<contour::actions::SwitchToTabRight>(right.mapping.binding.front()));

        // Modifiers{None} is load-bearing, not incidental: sendWheelEvent() transposes the wheel axes
        // while Alt is held, so Alt+vertical wheel arrives on the HORIZONTAL axis. Carrying Alt, it must
        // not match these rows, or Alt+scroll would switch tabs instead of changing opacity.
        CHECK(left.mapping.modifiers == vtbackend::Modifiers { vtbackend::Modifier::None });
        CHECK(right.mapping.modifiers == vtbackend::Modifiers { vtbackend::Modifier::None });
    }

    SECTION("the horizontal-wheel rows follow tab_switch_on_horizontal_wheel")
    {
        auto enabledConfig = contour::config::Config {};
        enabledConfig.tabSwitchOnHorizontalWheel = true;
        auto disabledConfig = contour::config::Config {};
        disabledConfig.tabSwitchOnHorizontalWheel = false;

        for (auto const button: { vtbackend::MouseButton::WheelLeft, vtbackend::MouseButton::WheelRight })
        {
            auto const& row = *rowFor(button);
            CHECK(row.enabled(enabledConfig));
            CHECK_FALSE(row.enabled(disabledConfig));
        }

        // ... and the context menu stays reachable either way.
        CHECK(rowFor(vtbackend::MouseButton::Right)->enabled(disabledConfig));
    }

    SECTION("it is a FALLBACK, not a default")
    {
        // This is the whole point of the table, so it is pinned rather than left to a comment: these
        // buttons must NOT be in the default mouseMappings. Loading an `input_mapping:` section replaces
        // those wholesale, and the contour.yml Contour generates on first run writes every one of them
        // out — so a binding placed there would be shadowed away by the config file of every user who
        // already has one, forever.
        auto const defaults = contour::config::Config {}.inputMappings.value().mouseMappings;
        for (auto const button: { vtbackend::MouseButton::Right,
                                  vtbackend::MouseButton::WheelLeft,
                                  vtbackend::MouseButton::WheelRight })
        {
            auto const bound = std::ranges::any_of(
                defaults, [button](auto const& mapping) { return mapping.input == button; });
            CHECK_FALSE(bound);
        }
    }
}

TEST_CASE("config.builtinFallbackKeyMappings", "[config]")
{
    using contour::config::Config;
    using vtbackend::Key;
    using vtbackend::Modifier;
    using vtbackend::Modifiers;

    auto const modeFlags = uint8_t { 0 };
    auto const ctrl = Modifiers { Modifier::Control };
    auto const ctrlShift = Modifiers { Modifier::Control, Modifier::Shift };

    SECTION("the browser chords switch tabs")
    {
        auto const config = Config {};

        auto const resolves = [&](Key key, Modifiers modifiers) {
            return contour::config::applyBuiltinFallback(config, key, modifiers, modeFlags);
        };

        auto const* pageUp = resolves(Key::PageUp, ctrl);
        REQUIRE(pageUp != nullptr);
        REQUIRE(pageUp->size() == 1);
        CHECK(std::holds_alternative<contour::actions::SwitchToTabLeft>(pageUp->front()));

        auto const* pageDown = resolves(Key::PageDown, ctrl);
        REQUIRE(pageDown != nullptr);
        REQUIRE(pageDown->size() == 1);
        CHECK(std::holds_alternative<contour::actions::SwitchToTabRight>(pageDown->front()));

        auto const* tab = resolves(Key::Tab, ctrl);
        REQUIRE(tab != nullptr);
        REQUIRE(tab->size() == 1);
        CHECK(std::holds_alternative<contour::actions::SwitchToTabRight>(tab->front()));

        // Ctrl+Shift+Tab reaches the binding layer as Key::Tab with Shift re-added (Qt reports the
        // shifted press as Key_Backtab; helper.cpp rewrites it).
        auto const* backTab = resolves(Key::Tab, ctrlShift);
        REQUIRE(backTab != nullptr);
        REQUIRE(backTab->size() == 1);
        CHECK(std::holds_alternative<contour::actions::SwitchToTabLeft>(backTab->front()));
    }

    SECTION("Ctrl+Shift+Up and Down go from prompt to prompt, on the primary screen only")
    {
        auto const config = Config {};

        auto const* up = contour::config::applyBuiltinFallback(config, Key::UpArrow, ctrlShift, modeFlags);
        REQUIRE(up != nullptr);
        REQUIRE(up->size() == 1);
        CHECK(std::holds_alternative<contour::actions::ScrollMarkUp>(up->front()));

        auto const* down =
            contour::config::applyBuiltinFallback(config, Key::DownArrow, ctrlShift, modeFlags);
        REQUIRE(down != nullptr);
        REQUIRE(down->size() == 1);
        CHECK(std::holds_alternative<contour::actions::ScrollMarkDown>(down->front()));

        // A full-screen application keeps the chord.
        auto const alternateScreen = uint8_t { vtbackend::MatchModes::AlternateScreen };
        CHECK(contour::config::applyBuiltinFallback(config, Key::UpArrow, ctrlShift, alternateScreen)
              == nullptr);

        // And the plain defaults leave it free, so a user's own binding of it is found first.
        CHECK(std::ranges::none_of(config.inputMappings.value().keyMappings, [&](auto const& mapping) {
            return (mapping.input == Key::UpArrow || mapping.input == Key::DownArrow)
                   && mapping.modifiers == ctrlShift;
        }));
    }

    SECTION("matching is on the exact chord")
    {
        auto const config = Config {};

        // apply() requires modifier EQUALITY, so a chord carrying anything extra must not match -- an
        // application binding Ctrl+Alt+Tab keeps it.
        CHECK(contour::config::applyBuiltinFallback(
                  config, Key::Tab, Modifiers { Modifier::Control, Modifier::Alt }, modeFlags)
              == nullptr);
        CHECK(contour::config::applyBuiltinFallback(config, Key::Tab, Modifiers { Modifier::None }, modeFlags)
              == nullptr);
        CHECK(contour::config::applyBuiltinFallback(
                  config, Key::PageUp, Modifiers { Modifier::Shift }, modeFlags)
              == nullptr);
    }

    SECTION("a user's own input_mapping cannot shadow them away")
    {
        // THE reason this table exists, so it is pinned rather than left to a comment. Loading ANY
        // `input_mapping:` section replaces the built-in key mappings wholesale, and the contour.yml
        // Contour generates on first run writes every default into that section -- so a binding that
        // lived only in the defaults would be shadowed, forever, by the config file of every user who
        // already had one. Here a config supplies one unrelated binding, which wipes the defaults; the
        // fallback must still resolve.
        QTemporaryDir dir;
        auto const config = loadFromYaml(dir,
                                         "input_mapping:\n"
                                         "    - { mods: [Control], key: 'F9', action: ToggleFullscreen }\n"
                                         "profiles:\n  main:\n    shell: \"/bin/bash\"\n");

        auto const& keyMappings = config.inputMappings.value().keyMappings;
        auto const boundInUserTable = [&](Key key) {
            return std::ranges::any_of(keyMappings, [key](auto const& m) { return m.input == key; });
        };
        // The wipe really happened: the defaults are gone from the user's table.
        CHECK_FALSE(boundInUserTable(Key::PageUp));
        CHECK_FALSE(boundInUserTable(Key::Tab));

        // ...and the fallback still carries them.
        auto const* tab = contour::config::applyBuiltinFallback(config, Key::Tab, ctrl, modeFlags);
        REQUIRE(tab != nullptr);
        CHECK(std::holds_alternative<contour::actions::SwitchToTabRight>(tab->front()));

        auto const* pageUp = contour::config::applyBuiltinFallback(config, Key::PageUp, ctrl, modeFlags);
        REQUIRE(pageUp != nullptr);
        CHECK(std::holds_alternative<contour::actions::SwitchToTabLeft>(pageUp->front()));
    }

    SECTION("a fresh config carries them in its own table too, so they can be seen and rebound")
    {
        auto const defaults = Config {}.inputMappings.value().keyMappings;
        auto const boundTo = [&](Key key, Modifiers modifiers) {
            return std::ranges::any_of(
                defaults, [&](auto const& m) { return m.input == key && m.modifiers == modifiers; });
        };
        CHECK(boundTo(Key::PageUp, ctrl));
        CHECK(boundTo(Key::PageDown, ctrl));
        CHECK(boundTo(Key::Tab, ctrl));
        CHECK(boundTo(Key::Tab, ctrlShift));
    }
}

TEST_CASE("config.applyBuiltinFallback", "[config]")
{
    auto const modeFlags = uint8_t { 0 };
    auto const noModifiers = vtbackend::Modifiers { vtbackend::Modifier::None };

    SECTION("a horizontal wheel notch resolves to a tab switch")
    {
        auto const config = contour::config::Config {};

        auto const* left = contour::config::applyBuiltinFallback(
            config, vtbackend::MouseButton::WheelLeft, noModifiers, modeFlags);
        REQUIRE(left != nullptr);
        REQUIRE(left->size() == 1);
        CHECK(std::holds_alternative<contour::actions::SwitchToTabLeft>(left->front()));

        auto const* right = contour::config::applyBuiltinFallback(
            config, vtbackend::MouseButton::WheelRight, noModifiers, modeFlags);
        REQUIRE(right != nullptr);
        CHECK(std::holds_alternative<contour::actions::SwitchToTabRight>(right->front()));
    }

    SECTION("a disabled row matches nothing")
    {
        auto config = contour::config::Config {};
        config.tabSwitchOnHorizontalWheel = false;

        CHECK(contour::config::applyBuiltinFallback(
                  config, vtbackend::MouseButton::WheelLeft, noModifiers, modeFlags)
              == nullptr);
        CHECK(contour::config::applyBuiltinFallback(
                  config, vtbackend::MouseButton::WheelRight, noModifiers, modeFlags)
              == nullptr);

        // The gate is per-row: the ungated context menu is unaffected.
        CHECK(contour::config::applyBuiltinFallback(
                  config, vtbackend::MouseButton::Right, noModifiers, modeFlags)
              != nullptr);
    }

    SECTION("a modifier that is not None never matches")
    {
        auto const config = contour::config::Config {};
        auto const alt = vtbackend::Modifiers { vtbackend::Modifier::Alt };

        CHECK(contour::config::applyBuiltinFallback(config, vtbackend::MouseButton::WheelLeft, alt, modeFlags)
              == nullptr);
    }

    SECTION("an unbound button matches nothing")
    {
        auto const config = contour::config::Config {};

        CHECK(contour::config::applyBuiltinFallback(
                  config, vtbackend::MouseButton::Middle, noModifiers, modeFlags)
              == nullptr);
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
        auto const path = std::filesystem::path(dir.path().toStdU16String()) / "profiles" / "crlf.yml";
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
    // A caller may reload into a Config it already loaded, unlike a fresh load. A profiles map that only ever
    // accreted entries — and a provenance map that only ever emplaced — would keep a GUI profile alive after
    // its side file was deleted, so the settings page would still offer it and a later Save would shadow
    // contour.yml. The loader must reconcile both on every load.
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
    std::filesystem::remove(std::filesystem::path(dir.path().toStdU16String()) / "profiles" / "work.yml");
    contour::config::loadConfigFromFile(config, configPath);

    CHECK(config.findProfile("work") == nullptr);   // no longer in the profiles map
    CHECK(!config.profileOrigins.contains("work")); // and no stale SideFile provenance left behind
    REQUIRE(config.findProfile("main") != nullptr); // the contour.yml profile is intact
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
        auto const path = std::filesystem::path(dir.path().toStdU16String()) / "settings.yml";
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
    profile.dimUnfocused = 1.0;

    writeSideFile(dir, "profiles/roundtrip.yml", contour::config::emitProfileYaml(profile));

    auto const reloaded = loadFromYaml(dir, "default_profile: main\n");
    auto const* rt = reloaded.findProfile("roundtrip");
    REQUIRE(rt != nullptr);
    CHECK(rt->showTitleBar.value() == false);
    CHECK(rt->dimUnfocused.value() == 1.0);
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
    QTemporaryDir const dir;
    auto const path = std::filesystem::path(dir.path().toStdU16String()) / "settings.yml";
    {
        auto out = std::ofstream(path);
        out << contour::config::emitGuiSettingsYaml({ .defaultProfile = "work", .globalOverrides = {} });
    }

    auto const loaded = contour::config::loadGuiSettingsFile(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->defaultProfile.has_value());
    CHECK(loaded->defaultProfile.value() == "work");

    // A missing file is default (all-unset), not an error.
    auto const missing = contour::config::loadGuiSettingsFile(
        std::filesystem::path(dir.path().toStdU16String()) / "does-not-exist.yml");
    REQUIRE(missing.has_value());
    CHECK_FALSE(missing->defaultProfile.has_value());
}

TEST_CASE("Config: GUI settings round-trip dotted keys as nested YAML", "[config][gui][gpu]")
{
    QTemporaryDir const dir;
    auto const path = std::filesystem::path(dir.path().toStdU16String()) / "settings.yml";
    auto const yaml = contour::config::emitGuiSettingsYaml(
        { .defaultProfile = std::nullopt,
          .globalOverrides = { { "renderer.gpu", "10de:2820" }, { "theme", "dark" } } });
    CHECK(yaml.contains("renderer:"));
    CHECK_FALSE(yaml.contains("renderer.gpu"));
    {
        auto out = std::ofstream(path);
        out << yaml;
    }

    auto const loaded = contour::config::loadGuiSettingsFile(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->globalOverrides.at("renderer.gpu") == "10de:2820");
    CHECK(loaded->globalOverrides.at("theme") == "dark");
}

TEST_CASE("Config: settings.yml renderer.gpu overrides contour.yml; junk nesting is ignored",
          "[config][gui][gpu]")
{
    QTemporaryDir dir;
    writeSideFile(
        dir, "settings.yml", "renderer:\n    gpu: discrete\n    nested:\n        deeper: 1\ntheme: dark\n");
    auto const cfg = loadFromYaml(dir, R"(
renderer:
    backend: Vulkan
    gpu: integrated
profiles:
    main:
        shell: /bin/sh
)"sv);
    CHECK(cfg.renderer.value().gpu.preference == contour::config::GpuPreference::Discrete);
    CHECK(cfg.renderer.value().renderingBackend == contour::config::RenderingBackend::Vulkan);
    CHECK(cfg.theme.value() == contour::config::GuiTheme::Dark);
}

TEST_CASE("Config: a scalar renderer in settings.yml is ignored, not fatal", "[config][gui][gpu]")
{
    QTemporaryDir dir;
    writeSideFile(dir, "settings.yml", "renderer: foo\ntheme: dark\n");
    auto const yaml = "renderer:\n    gpu: integrated\nprofiles:\n    main:\n        shell: /bin/sh\n"sv;
    REQUIRE_NOTHROW(loadFromYaml(dir, yaml));
    auto const cfg = loadFromYaml(dir, yaml);
    CHECK(cfg.theme.value() == contour::config::GuiTheme::Dark);
    CHECK(cfg.renderer.value().gpu.preference == contour::config::GpuPreference::Integrated);
}

TEST_CASE("Config: a GPU set over a scalar renderer in settings.yml replaces it and applies",
          "[config][gui][gpu]")
{
    // The settings page re-saves what it loaded plus its edit, so a hand-edited `renderer: foo` comes back
    // as a flat override beside the new nested renderer.gpu. Written as two `renderer` keys, the scalar
    // would shadow the section on the next load and the GPU would never apply.
    QTemporaryDir dir;
    writeSideFile(dir, "settings.yml", "renderer: foo\ntheme: dark\n");
    auto const yaml = "profiles:\n    main:\n        shell: /bin/sh\n"sv;
    auto settings = loadFromYaml(dir, yaml).guiManagedSettings;
    settings.globalOverrides["renderer.gpu"] = "discrete";
    writeSideFile(dir, "settings.yml", contour::config::emitGuiSettingsYaml(settings));

    auto const cfg = loadFromYaml(dir, yaml);
    CHECK(cfg.renderer.value().gpu.preference == contour::config::GpuPreference::Discrete);
    CHECK(cfg.theme.value() == contour::config::GuiTheme::Dark);
    CHECK_FALSE(cfg.guiManagedSettings.globalOverrides.contains("renderer"));
}

TEST_CASE("Config: a non-scalar key in settings.yml is skipped, its siblings still apply",
          "[config][gui][gpu]")
{
    QTemporaryDir dir;
    writeSideFile(dir, "settings.yml", "? [a]\n: 2\nrenderer:\n    ? [a]\n    : 1\n    gpu: discrete\n");
    REQUIRE_NOTHROW(loadFromYaml(dir, "profiles:\n    main:\n        shell: /bin/sh\n"sv));
    auto const cfg = loadFromYaml(dir, "profiles:\n    main:\n        shell: /bin/sh\n"sv);
    CHECK(cfg.renderer.value().gpu.preference == contour::config::GpuPreference::Discrete);
}

TEST_CASE("Config: a non-scalar renderer.gpu falls back to auto", "[config][gpu]")
{
    QTemporaryDir dir;
    auto const yaml = "renderer:\n    gpu: [x]\nprofiles:\n    main:\n        shell: /bin/sh\n"sv;
    REQUIRE_NOTHROW(loadFromYaml(dir, yaml));
    CHECK(loadFromYaml(dir, yaml).renderer.value().gpu == contour::config::GpuSelector {});
}

TEST_CASE("Config: a non-scalar renderer.gpu in settings.yml never aborts the load", "[config][gui][gpu]")
{
    QTemporaryDir dir;
    writeSideFile(dir, "settings.yml", "renderer:\n    gpu: [x]\n");
    auto const yaml = "profiles:\n    main:\n        shell: /bin/sh\n"sv;
    REQUIRE_NOTHROW(loadFromYaml(dir, yaml));
    CHECK(loadFromYaml(dir, yaml).renderer.value().gpu.preference == contour::config::GpuPreference::Auto);
}

TEST_CASE("Config: a malformed renderer section in settings.yml is skipped, its siblings still apply",
          "[config][gui][gpu]")
{
    // A scalar renderer key makes the section loader run; the other keys are junk it would choke on.
    QTemporaryDir dir;
    writeSideFile(dir,
                  "settings.yml",
                  "renderer:\n    gpu: [x]\n    backend: [y]\n    tile_hashtable_slots: 8192\ntheme: dark\n");
    auto const yaml = "profiles:\n    main:\n        shell: /bin/sh\n"sv;
    REQUIRE_NOTHROW(loadFromYaml(dir, yaml));
    CHECK(loadFromYaml(dir, yaml).theme.value() == contour::config::GuiTheme::Dark);
}

TEST_CASE("Config: a flat settings.yml written by an older Contour still loads", "[config][gui]")
{
    QTemporaryDir dir;
    writeSideFile(dir, "settings.yml", "reflow_on_resize: false\n");
    auto const cfg = loadFromYaml(dir, "profiles:\n    main:\n        shell: /bin/sh\n"sv);
    CHECK(cfg.reflowOnResize.value() == false);
}

TEST_CASE("Config: FileGuiConfigStore writes and removes side files the loader picks up", "[config][gui]")
{
    QTemporaryDir dir;
    auto const configDir = std::filesystem::path(dir.path().toStdU16String());

    auto const base = loadFromYaml(dir, "default_profile: main\n");
    auto profile = *base.findProfile("main");
    profile.showTitleBar = false;

    auto store = contour::config::FileGuiConfigStore(configDir);
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

TEST_CASE("Config: the configuration is found and kept under a directory beyond ASCII", "[config]")
{
    // A profile directory such as C:\Users\李 on Windows.
    auto const dir = core::testing::ScopedTempDir { "contour-config-home" };
    auto const home = dir.path() / vtpty::testing::NonAsciiName;
    std::filesystem::create_directories(home);
#ifdef _WIN32
    auto const variable = core::testing::ScopedEnv { "LOCALAPPDATA", core::pathToUtf8(home) };
#else
    auto const variable = core::testing::ScopedEnv { "XDG_CONFIG_HOME", core::pathToUtf8(home) };
#endif

    // Found where the environment says, and created there when it is not yet.
    auto const expected = home / "contour" / "contour.yml";
    CHECK(contour::config::configHome() == expected.parent_path());
    CHECK(core::pathFromUtf8(contour::config::defaultConfigFilePath()) == expected);
    auto const loaded = contour::config::loadConfig();
    CHECK(loaded.configFile == expected);
    CHECK(std::filesystem::exists(expected));

    // A side file is named for its profile, and read back under that name.
    auto const name = std::string { "\xe6\x9d\x8e" }; // U+674E
    auto store = contour::config::FileGuiConfigStore(expected.parent_path());
    REQUIRE(store.saveProfile(name, *loaded.findProfile(loaded.defaultProfileName.value())).has_value());
    CHECK(
        std::filesystem::exists(expected.parent_path() / "profiles" / std::u8string_view { u8"\u674e.yml" }));
    CHECK(contour::config::loadConfig().findProfile(name) != nullptr);
}

TEST_CASE("Config: a nested settings.yml override replaces one leaf and keeps its siblings", "[config][gui]")
{
    // The settings page writes only the leaves the user changed. Each section loader writes only the
    // children present into a struct contour.yml already filled (Config.hpp's ConfigEntry overload loads
    // into the live value), so what the GUI did not touch keeps contour.yml's value -- not the default.
    QTemporaryDir dir;
    auto const settings = contour::config::emitGuiSettingsYaml(contour::config::GuiManagedSettings {
        .defaultProfile = std::nullopt,
        .globalOverrides = { { "folding.enabled", "false" },
                             { "gutter.line_numbers", "relative" },
                             { "command_blocks.pager", "most" } },
    });
    INFO("settings.yml:\n" << settings);
    // Written the way the settings page writes it: nested, as contour.yml spells it. A literal
    // `folding.enabled:` key is one no section loader ever looks up.
    REQUIRE(YAML::Load(settings)["folding"].IsMap());
    writeSideFile(dir, "settings.yml", settings);
    auto const config = loadFromYaml(dir, R"(
default_profile: main
folding:
    show_markers: false
    auto_collapse_on_new_command: true
gutter:
    timestamps: true
    timestamp_format: "%H:%M"
    line_number_width: 8
command_blocks:
    max_records: 250
)");

    // What the GUI set wins ...
    CHECK(config.folding.value().enabled == false);
    CHECK(config.gutter.value().lineNumbers == vtbackend::LineNumberMode::Relative);
    CHECK(config.commandBlocks.value().pager == "most");

    // ... and every sibling it did not set keeps what contour.yml said.
    CHECK(config.folding.value().showMarkers == false);
    CHECK(config.folding.value().autoCollapseOnNewCommand == true);
    CHECK(config.gutter.value().timestamps == true);
    CHECK(config.gutter.value().timestampFormat == "%H:%M");
    CHECK(config.gutter.value().lineNumberWidth == 8);
    CHECK(config.commandBlocks.value().maxRecords == size_t { 250 });
}

TEST_CASE("Config: a malformed folding or command_blocks entry in settings.yml is skipped, not fatal",
          "[config][gui]")
{
    // settings.yml is hand-editable. A scalar where a section belongs overrides no leaf of it, and a leaf of
    // the wrong shape keeps the value it had: neither costs the load. Each malformed section keeps one
    // well-formed leaf, and that leaf applies, so the merge did run the section's loader.
    QTemporaryDir dir;
    writeSideFile(dir,
                  "settings.yml",
                  "folding:\n    enabled: [x]\n    show_markers: false\n"
                  "gutter: off\n"
                  "command_blocks:\n    max_records: [y]\n    pager: most\n"
                  "theme: dark\n");
    auto const config = loadFromYaml(dir, "profiles:\n    main:\n        shell: /bin/sh\n"sv);

    CHECK(config.folding.value().enabled == true);
    CHECK(config.folding.value().showMarkers == false);
    CHECK(config.gutter.value() == contour::config::GutterConfig {});
    CHECK(config.commandBlocks.value().maxRecords == size_t { 1000 });
    CHECK(config.commandBlocks.value().pager == "most");
    CHECK(config.theme.value() == contour::config::GuiTheme::Dark);
}

TEST_CASE("Config: a malformed settings.yml entry is reported by the file and its key path there",
          "[config][gui]")
{
    // settings.yml spells its keys as contour.yml does, so the key path alone would send the user looking
    // in contour.yml, which holds no such value. A section and a top-level key both name the file.
    QTemporaryDir dir;
    writeSideFile(
        dir, "settings.yml", "folding:\n    enabled: [x]\n    show_markers: false\nreflow_on_resize: [y]\n");
    auto capture = core::log::ScopedCapture { "error" };
    std::ignore = loadFromYaml(dir, "profiles:\n    main:\n        shell: /bin/sh\n"sv);

    INFO("captured error log:\n" << capture.text());
    CHECK(capture.text().contains("settings.yml: folding"));
    CHECK(capture.text().contains("line 2, column 14"));
    CHECK(capture.text().contains("settings.yml: reflow_on_resize"));
    CHECK(capture.text().contains("line 4, column 19"));
}

TEST_CASE("Config: a malformed entry of a profile side file is reported by the file and its key path there",
          "[config][gui]")
{
    // A profile side file's root is the profile's body, so its key path alone is a bare `cursor`: no file, no
    // profile, and a line number in a file the message does not name. The file is named for its profile.
    QTemporaryDir dir;
    writeSideFile(dir,
                  "profiles/work.yml",
                  "cursor: block\nnotify_on_command_finish:\n    when: [always]\n    outcome: failure\n");
    auto capture = core::log::ScopedCapture { "error" };
    auto const config =
        loadFromYaml(dir, "default_profile: main\nprofiles:\n    main:\n        shell: /bin/sh\n"sv);

    INFO("captured error log:\n" << capture.text());
    CHECK(capture.text().contains("work.yml: cursor"));
    CHECK(capture.text().contains("line 1, column 9"));
    CHECK(capture.text().contains("work.yml: notify_on_command_finish.when"));
    CHECK(capture.text().contains("line 3, column 11"));
    // The bad values cost only themselves: the profile still loads, with the key read after them.
    auto const* work = config.findProfile("work");
    REQUIRE(work != nullptr);
    CHECK(work->notifyOnCommandFinish.value().outcome == contour::config::NotifyOutcome::Failure);
}

TEST_CASE("Config: a scalar where a settings.yml section belongs is ignored, not read as the section",
          "[config][gui]")
{
    // A section's loader runs only when settings.yml overrides one of its leaves, a `<section>.<leaf>` key.
    // A bare scalar under a section's name overrides none -- the flat reader keeps it as a key no row
    // writes -- so it is skipped quietly rather than subscripted into a logged error.
    QTemporaryDir dir;
    writeSideFile(dir, "settings.yml", "gutter: off\nfolding: true\ntheme: dark\n");
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir, "profiles:\n    main:\n        shell: /bin/sh\n"sv);

    INFO("captured error log:\n" << capture.text());
    CHECK_FALSE(capture.text().contains("gutter"));
    CHECK_FALSE(capture.text().contains("folding"));
    CHECK(config.gutter.value() == contour::config::GutterConfig {});
    CHECK(config.theme.value() == contour::config::GuiTheme::Dark);
}

TEST_CASE("Config: a wrong-typed leaf of a section costs only that leaf", "[config][gui]")
{
    // `timestamps: maybe` is no boolean. Read under its own guard, it keeps the value it had, and the line
    // numbers set beside it still apply: the settings page marks both rows overridden, and the one that is
    // well formed must be in effect. The same holds where contour.yml sets the section.
    auto const sections = "gutter:\n    line_numbers: relative\n    timestamps: maybe\n"
                          "folding:\n    show_markers: false\n    enabled: [x]\n"
                          "command_blocks:\n    max_records: [y]\n    pager: most\n"
                          "renderer:\n    tile_cache_count: lots\n    gpu: discrete\n"sv;
    auto const profiles = "profiles:\n    main:\n        shell: /bin/sh\n"sv;
    QTemporaryDir dir;

    SECTION("in settings.yml, over what contour.yml sets")
    {
        writeSideFile(dir, "settings.yml", sections);
        auto const config = loadFromYaml(
            dir,
            std::format("gutter:\n    timestamps: true\ncommand_blocks:\n    max_records: 250\n{}",
                        profiles));

        CHECK(config.gutter.value().lineNumbers == vtbackend::LineNumberMode::Relative);
        CHECK(config.gutter.value().timestamps == true);
        CHECK(config.folding.value().showMarkers == false);
        CHECK(config.folding.value().enabled == true);
        CHECK(config.commandBlocks.value().pager == "most");
        CHECK(config.commandBlocks.value().maxRecords == size_t { 250 });
        CHECK(config.renderer.value().gpu.preference == contour::config::GpuPreference::Discrete);
    }

    SECTION("in contour.yml")
    {
        auto const config = loadFromYaml(dir, std::format("{}{}", sections, profiles));

        CHECK(config.gutter.value().lineNumbers == vtbackend::LineNumberMode::Relative);
        CHECK(config.gutter.value().timestamps == false);
        CHECK(config.folding.value().showMarkers == false);
        CHECK(config.folding.value().enabled == true);
        CHECK(config.commandBlocks.value().pager == "most");
        CHECK(config.commandBlocks.value().maxRecords == size_t { 1000 });
        CHECK(config.renderer.value().gpu.preference == contour::config::GpuPreference::Discrete);
    }
}

TEST_CASE("Config: a side file whose document is no map is skipped, not fatal", "[config][gui]")
{
    // A side file holds a map of settings. A bare word parses as a document too, and the readers subscript
    // it: that threw out of the whole load for settings.yml and a profile's file, and was blamed on the
    // profile's `colors` entry for a colour scheme's file.
    auto const yaml = "read_buffer_size: 16384\nprofiles:\n    main:\n        colors: word\n"sv;
    QTemporaryDir dir;

    SECTION("settings.yml")
    {
        writeSideFile(dir, "settings.yml", "dark\n");
        auto const config = loadFromYaml(dir, yaml);
        CHECK(config.ptyReadBufferSize.value() == 16384);
        CHECK(config.guiManagedSettings.globalOverrides.empty());
    }

    SECTION("a profile's file")
    {
        writeSideFile(dir, "profiles/listed.yml", "- dark\n");
        writeSideFile(dir, "profiles/word.yml", "dark\n");
        writeSideFile(dir, "profiles/empty.yml", "# nothing set yet\n");
        auto const config = loadFromYaml(dir, yaml);
        CHECK(config.findProfile("listed") == nullptr);
        CHECK(config.findProfile("word") == nullptr);
        CHECK(config.findProfile("empty") != nullptr); // an empty document is a profile of defaults
        CHECK(config.findProfile("main") != nullptr);
    }

    SECTION("a colour scheme's file")
    {
        writeSideFile(dir, "colorschemes/word.yml", "dark\n");
        auto capture = core::log::ScopedCapture { "error" };
        std::ignore = loadFromYaml(dir, yaml);
        INFO("captured error log:\n" << capture.text());
        CHECK(capture.text().contains("word.yml"));
        CHECK_FALSE(capture.text().contains("profiles.main.colors"));
    }
}

TEST_CASE("Config: profile tab colors round trip and can be cleared", "[config][tab-color]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        tab_color: '#2E3440'
    plain:
        tab_color: null
    invalid:
        tab_color: '#GGGGGG'
)"sv);
    auto const nord = vtbackend::RGBColor { 0x2E, 0x34, 0x40 };
    CHECK(config.profile("main")->tabColor.value() == nord);
    CHECK_FALSE(config.profile("plain")->tabColor.value().has_value());
    CHECK(config.profile("invalid")->tabColor.value() == nord);
    CHECK_FALSE(contour::config::TerminalProfile {}.tabColor.value().has_value());
    auto const emitted = contour::config::emitProfileYaml(*config.profile("main"));
    CHECK(YAML::Load(emitted)["tab_color"].as<std::string>() == "#2E3440");
    CHECK(YAML::Load(contour::config::emitProfileYaml(*config.profile("plain")))["tab_color"].IsNull());
}

// }}}

// {{{ input_mapping modifier vocabulary and key case folding
//
// These pin the two independent defects behind issue #1987, both of which presented as "my binding
// silently does nothing": a modifier spelling the UI renders but the parser refused (`Ctrl`), and a
// single-character `key:` whose case could never match what the input routes deliver.

namespace
{

/// The mapping bound to @p key in @p config, or nullptr.
[[nodiscard]] contour::config::KeyInputMapping const* keyMappingFor(contour::config::Config const& config,
                                                                    vtbackend::Key key)
{
    auto const& mappings = config.inputMappings.value().keyMappings;
    auto const i = std::ranges::find(mappings, key, &contour::config::KeyInputMapping::input);
    return i != mappings.end() ? &*i : nullptr;
}

/// A one-row `input_mapping` document binding @p mods + @p key to a harmless action.
///
/// ScreenshotVT is used as the payload throughout: it takes no argument and, unlike Quit, cannot
/// end the test run if a binding unexpectedly fires.
[[nodiscard]] std::string oneMappingYaml(std::string_view mods, std::string_view key)
{
    return std::format("default_profile: main\n"
                       "profiles:\n"
                       "    main:\n"
                       "        shell: /bin/sh\n"
                       "input_mapping:\n"
                       "    - {{ mods: {}, key: {}, action: ScreenshotVT }}\n",
                       mods,
                       key);
}

} // namespace

TEST_CASE("Config: 'Ctrl' is accepted as a spelling of Control (issue #1987)", "[config][input-mapping]")
{
    auto dir = QTemporaryDir {};
    REQUIRE(dir.isValid());

    // The reproduction from the issue, verbatim. Before the fix parseModifierKey rejected "Ctrl",
    // parseModifier bailed on it, and the row was dropped without a single line of output.
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Shift,Alt,Ctrl], key: 'Q', action: ClearHistoryAndReset }
)"sv);

    auto const& mappings = config.inputMappings.value();

    // An `input_mapping:` section replaces the built-in defaults, so this row is the only one left.
    REQUIRE(mappings.charMappings.size() == 1);
    CHECK(mappings.keyMappings.empty());
    CHECK(mappings.mouseMappings.empty());

    auto const& bound = mappings.charMappings.front();
    CHECK(bound.input == U'Q');
    // Exact equality, not .test(): config::apply matches modifiers with ==, so a partially-parsed
    // chord would still fail at runtime even though every individual bit looked right here.
    CHECK(bound.modifiers
          == vtbackend::Modifiers {
              vtbackend::Modifier::Shift, vtbackend::Modifier::Alt, vtbackend::Modifier::Control });
    REQUIRE(bound.binding.size() == 1);
    CHECK(std::holds_alternative<contour::actions::ClearHistoryAndReset>(bound.binding.at(0)));
}

TEST_CASE("Config: every modifier the UI can render parses back from config", "[config][input-mapping]")
{
    auto dir = QTemporaryDir {};
    REQUIRE(dir.isValid());

    // The anti-recurrence test. Contour renders chords using ShortcutModifierTable and the config
    // writer emits vtbackend::ChordModifierTable's names verbatim; if the reader accepts neither,
    // Contour teaches the user a spelling it then refuses -- which is exactly issue #1987.
    //
    // Sweeping the whole table also covers Hyper, which renders in the shortcut column but had no
    // config spelling at all before this change: Contour could write a binding it could not read back.
    for (auto const& row: vtbackend::ChordModifierTable)
    {
        CAPTURE(row.name);
        auto const config = loadFromYaml(dir, oneMappingYaml(std::format("[{}]", row.name), "F1"));

        auto const* bound = keyMappingFor(config, vtbackend::Key::F1);
        REQUIRE(bound != nullptr);

        // "Meta" is the one deliberate exception: it is spelled for Super, so it parses but not to
        // itself. Everything else must round-trip exactly. @see contour::ConfigModifierTable
        if (row.modifier == contour::config::ModifierWithoutConfigSpelling)
            CHECK(bound->modifiers == vtbackend::Modifiers { vtbackend::Modifier::Super });
        else
            CHECK(bound->modifiers == vtbackend::Modifiers { row.modifier });
    }
}

TEST_CASE("Config: modifier spellings are case-insensitive", "[config][input-mapping]")
{
    auto dir = QTemporaryDir {};
    REQUIRE(dir.isValid());

    SECTION("in a sequence")
    {
        auto const config = loadFromYaml(dir, oneMappingYaml("[ctrl, ALT, ShIfT]", "F1"));
        auto const* bound = keyMappingFor(config, vtbackend::Key::F1);
        REQUIRE(bound != nullptr);
        CHECK(bound->modifiers
              == vtbackend::Modifiers {
                  vtbackend::Modifier::Control, vtbackend::Modifier::Alt, vtbackend::Modifier::Shift });
    }

    SECTION("as a bare scalar")
    {
        // parseModifier handles a scalar `mods:` through a different branch than a sequence, so a
        // table-driven rewrite could easily fix one and leave the other behind.
        auto const config = loadFromYaml(dir, oneMappingYaml("Ctrl", "F1"));
        auto const* bound = keyMappingFor(config, vtbackend::Key::F1);
        REQUIRE(bound != nullptr);
        CHECK(bound->modifiers == vtbackend::Modifiers { vtbackend::Modifier::Control });
    }
}

TEST_CASE("Config: 'Meta' keeps its legacy mapping to Super", "[config][input-mapping]")
{
    auto dir = QTemporaryDir {};
    REQUIRE(dir.isValid());

    // contour.yml has spelled the Windows/Command key "Meta" since before Modifier::Meta existed.
    // Re-pointing it at Modifier::Meta would silently relocate every existing `mods: [Meta]` binding
    // onto a key most keyboards do not have, and nothing else in the tree would notice.
    auto const config = loadFromYaml(dir, oneMappingYaml("[Meta]", "F1"));

    auto const* bound = keyMappingFor(config, vtbackend::Key::F1);
    REQUIRE(bound != nullptr);
    CHECK(bound->modifiers == vtbackend::Modifiers { vtbackend::Modifier::Super });
    CHECK_FALSE(bound->modifiers.test(vtbackend::Modifier::Meta));
}

TEST_CASE("Config: an unknown modifier drops the whole row, not just the modifier", "[config][input-mapping]")
{
    auto dir = QTemporaryDir {};
    REQUIRE(dir.isValid());

    // The all-or-nothing contract. Rewriting the old if-chain into a table lookup makes it very
    // natural to skip past an unrecognized name and bind a PARTIAL chord -- which would be worse
    // than dropping the row, because Ctrl+F1 would then silently start firing on its own.
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Ctrl, Bogus], key: F1, action: ScreenshotVT }
    - { mods: [Ctrl],        key: F2, action: ScreenshotVT }
)"sv);

    auto const& mappings = config.inputMappings.value();
    REQUIRE(mappings.keyMappings.size() == 1);
    CHECK(mappings.keyMappings.front().input == vtbackend::Key::F2);
}

TEST_CASE("Config: the mouse modifier settings accept the same vocabulary", "[config][input-mapping]")
{
    auto dir = QTemporaryDir {};
    REQUIRE(dir.isValid());

    // These two settings share parseModifierKey with `input_mapping`, so they were silently ignoring
    // `Ctrl` for the same root cause. Widening the vocabulary fixes them for free; pin that.
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
bypass_mouse_protocol_modifier: Ctrl
mouse_block_selection_modifier: Hyper
)"sv);

    CHECK(config.bypassMouseProtocolModifiers.value()
          == vtbackend::Modifiers { vtbackend::Modifier::Control });
    CHECK(config.mouseBlockSelectionModifiers.value() == vtbackend::Modifiers { vtbackend::Modifier::Hyper });
}

TEST_CASE("Config: an input_mapping section replaces the built-in defaults", "[config][input-mapping]")
{
    auto dir = QTemporaryDir {};
    REQUIRE(dir.isValid());

    // Intended, supported behaviour: writing an `input_mapping:` section declares that you are
    // defining the full set of bindings. Documented in docs/configuration/key-mapping.md.
    auto const hasCommandPaletteDefault = [](contour::config::Config const& config) {
        auto const& charMappings = config.inputMappings.value().charMappings;
        return std::ranges::any_of(charMappings, [](auto const& mapping) {
            return mapping.input == U'P'
                   && mapping.modifiers
                          == vtbackend::Modifiers { vtbackend::Modifier::Control,
                                                    vtbackend::Modifier::Shift };
        });
    };

    SECTION("with no section at all, the defaults are intact")
    {
        auto const config = loadFromYaml(dir, "default_profile: main\n");
        CHECK(hasCommandPaletteDefault(config));
    }

    SECTION("a section discards every default it does not restate")
    {
        auto const config = loadFromYaml(dir, oneMappingYaml("[Control]", "F5"));
        auto const& mappings = config.inputMappings.value();

        CHECK(mappings.keyMappings.size() == 1);
        CHECK(mappings.charMappings.empty());
        CHECK(mappings.mouseMappings.empty());
        CHECK_FALSE(hasCommandPaletteDefault(config));
    }
}

TEST_CASE("Config: a single-character key: is stored case-folded", "[config][input-mapping]")
{
    auto dir = QTemporaryDir {};
    REQUIRE(dir.isValid());

    auto const soleCharMapping = [](contour::config::Config const& config) {
        auto const& charMappings = config.inputMappings.value().charMappings;
        REQUIRE(charMappings.size() == 1);
        return charMappings.front().input;
    };

    SECTION("a lowercase letter folds")
    {
        // Before folding this parsed fine and produced a binding that could never fire, because the
        // Ctrl branch of sendKeyEvent always reports the uppercase key label.
        CHECK(soleCharMapping(loadFromYaml(dir, oneMappingYaml("[Control]", "'q'"))) == U'Q');
    }

    SECTION("an uppercase letter is unchanged")
    {
        CHECK(soleCharMapping(loadFromYaml(dir, oneMappingYaml("[Control]", "'Q'"))) == U'Q');
    }

    SECTION("a non-ASCII letter is NOT folded")
    {
        // Deliberate: no input route can deliver a non-ASCII codepoint in two different cases, so
        // folding above 0x7F would only merge bindings that are distinct today. This is the guard
        // against someone swapping in unicode::simple_uppercase.
        CHECK(soleCharMapping(loadFromYaml(dir, oneMappingYaml("[Control]", "'ä'"))) == U'ä');
    }

    SECTION("punctuation is unchanged")
    {
        CHECK(soleCharMapping(loadFromYaml(dir, oneMappingYaml("[Control]", "','"))) == U',');
    }
}

TEST_CASE("Config: case-differing rows under the same chord merge", "[config][input-mapping]")
{
    auto dir = QTemporaryDir {};
    REQUIRE(dir.isValid());

    // A consequence of folding, pinned deliberately rather than discovered later: the two rows now
    // describe the same chord, so appendOrCreateBinding collapses them into one binding that runs
    // both actions in order. Previously the lowercase row was simply a silent no-op.
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control], key: 'a', action: ScreenshotVT }
    - { mods: [Control], key: 'A', action: CopySelection }
)"sv);

    auto const& charMappings = config.inputMappings.value().charMappings;
    REQUIRE(charMappings.size() == 1);
    CHECK(charMappings.front().input == U'A');
    REQUIRE(charMappings.front().binding.size() == 2);
    CHECK(std::holds_alternative<contour::actions::ScreenshotVT>(charMappings.front().binding.at(0)));
    CHECK(std::holds_alternative<contour::actions::CopySelection>(charMappings.front().binding.at(1)));
}

TEST_CASE("Config: every built-in char binding is stored folded", "[config][input-mapping]")
{
    // defaultInputMappings is a static C++ aggregate that never passes through parseKeyOrChar, so
    // its folded-ness is an invariant held only by inspection. A lowercase row added tomorrow would
    // produce a default binding that can never fire; fail here instead of silently at runtime.
    //
    // A runtime CHECK rather than a static_assert: defaultInputMappings holds std::vector and is
    // therefore not usable in a constant expression.
    for (auto const& mapping: contour::config::defaultInputMappings().charMappings)
    {
        CAPTURE(static_cast<uint32_t>(mapping.input));
        CHECK(mapping.input == contour::config::foldedBindingCodepoint(mapping.input));
    }
}

TEST_CASE("Config: a dropped input_mapping entry is reported", "[config][input-mapping]")
{
    auto dir = QTemporaryDir {};
    REQUIRE(dir.isValid());

    // The silence was the whole reason issue #1987 was hard to diagnose: a row vanished and nothing
    // anywhere said so. These assertions stay deliberately weak -- that the offending row and field
    // are NAMED -- rather than pinning the sentence, which would break on any rewording.
    auto capture = core::log::ScopedCapture { "error" };

    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Shift, Crtl], key: 'Q', action: ClearHistoryAndReset }
    - { mods: [Control], key: 'K', action: NoSuchActionName }
    - { mods: [Control], key: 'J', action: ScreenshotVT }
)"sv);

    // The good row still binds: a bad neighbour must not take the whole section down.
    CHECK(config.inputMappings.value().charMappings.size() == 1);

    auto const& log = capture.text();
    INFO("captured error log:\n" << log);

    // The misspelled modifier is named, and so is the row it killed.
    CHECK(log.contains("Crtl"));
    CHECK(log.contains("ClearHistoryAndReset"));
    // ...and the unknown action, with its own row.
    CHECK(log.contains("NoSuchActionName"));
    // The accepted spellings are offered, so the user can see what to write instead.
    CHECK(log.contains("Ctrl"));
}

TEST_CASE("Config: an input_mapping that is not a list is reported, not silently obeyed",
          "[config][input-mapping]")
{
    auto dir = QTemporaryDir {};
    REQUIRE(dir.isValid());

    // The defaults are discarded as soon as the section is PRESENT, before its shape is checked. So a
    // malformed section leaves the user with nothing bound at all -- the worst version of the silent
    // loss behind issue #1987, and now the most likely one, since the docs tell people the section
    // replaces the defaults.
    auto capture = core::log::ScopedCapture { "error" };

    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    mods: [Control]
    key: 'Q'
    action: ScreenshotVT
)"sv);

    auto const& mappings = config.inputMappings.value();
    CHECK(mappings.keyMappings.empty());
    CHECK(mappings.charMappings.empty());
    CHECK(mappings.mouseMappings.empty());

    INFO("captured error log:\n" << capture.text());
    CHECK(capture.text().contains("input_mapping"));
}

// }}}

TEST_CASE("Config: tryLoadConfigFromFile loads a file or says why it could not", "[config]")
{
    QTemporaryDir dir;

    SECTION("a file that loads")
    {
        auto const loaded = contour::config::tryLoadConfigFromFile(writeConfig(dir, R"(
default_profile: main
profiles:
    main:
        terminal_id: VT340
)"sv));
        REQUIRE(loaded.has_value());
        CHECK(loaded->profile("main")->terminalId.value() == vtbackend::VTType::VT340);
    }

    SECTION("a file that does not parse")
    {
        auto const loaded = contour::config::tryLoadConfigFromFile(writeConfig(dir, R"(
profiles:
    main:
  bad_indentation: [unclosed
)"sv));
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error().starts_with("cannot load config: "));
    }
}

TEST_CASE("Config: emulationSettingsFor resolves a profile of a configuration in memory", "[config]")
{
    // What `contour client` states for each session it creates: the configuration it holds, as a reload
    // left it, resolved and refused like a profile named on the command line.
    QTemporaryDir dir;
    auto const path = writeConfig(dir, R"(
default_profile: main
profiles:
    main:
        terminal_id: VT340
    work:
        terminal_id: VT220
)"sv);
    auto const config = contour::config::loadConfigFromFile(path);

    SECTION("no name is the configuration's default profile")
    {
        auto const settings = contour::config::emulationSettingsFor(config, "");
        REQUIRE(settings.has_value());
        CHECK(settings->terminalId == vtbackend::VTType::VT340);
    }

    SECTION("a profile by name")
    {
        auto const settings = contour::config::emulationSettingsFor(config, "work");
        REQUIRE(settings.has_value());
        CHECK(settings->terminalId == vtbackend::VTType::VT220);
    }

    SECTION("an unknown profile is refused, naming both the profile and the file")
    {
        auto const settings = contour::config::emulationSettingsFor(config, "nope");
        REQUIRE_FALSE(settings.has_value());
        CHECK(settings.error().contains("nope"));
        CHECK(settings.error().contains(path.string()));
    }
}

TEST_CASE("Config: resolveSessionConfig carries a profile's shell, sandbox policy and presentation "
          "fields",
          "[config]")
{
    // The regression this guards: `contour daemon` (and therefore `contour client`) used to resolve
    // only emulationSettings() and hardcode the OS login shell plus escapeSandbox=true, silently
    // dropping everything below for every session it hosts.
    QTemporaryDir dir;
    auto const path = writeConfig(dir, R"(
default_profile: main
profiles:
    main:
        shell: /usr/bin/fish
        arguments: ["-l"]
        escape_sandbox: false
        colors: coverage
        status_line:
            display: indicator
        cursor:
            shape: underscore
color_schemes:
    coverage:
        default:
            background: '#102030'
            foreground: '#D0D0D0'
)"sv);

    auto const resolved = contour::config::resolveSessionConfig(path.string(), "main");
    REQUIRE(resolved.has_value());

    SECTION("shell")
    {
        CHECK(resolved->shell.program == "/usr/bin/fish");
        REQUIRE(resolved->shell.arguments.size() == 1);
        CHECK(resolved->shell.arguments.front() == "-l");
    }

    SECTION("CONTOUR_PROFILE is set, matching a local session's environment")
    {
        REQUIRE(resolved->shell.env.contains("CONTOUR_PROFILE"));
        CHECK(resolved->shell.env.at("CONTOUR_PROFILE") == "main");
    }

    SECTION("escape_sandbox: false is honored, not hardcoded true")
    {
        CHECK_FALSE(resolved->escapeSandbox);
    }

    SECTION("the profile's color scheme reaches Settings.colorPalette")
    {
        CHECK(resolved->settings.colorPalette.defaultBackground == vtbackend::RGBColor(0x10, 0x20, 0x30));
        CHECK(resolved->settings.colorPalette.defaultForeground == vtbackend::RGBColor(0xD0, 0xD0, 0xD0));
    }

    SECTION("presentation fields (status line type, cursor shape) reach Settings")
    {
        CHECK(resolved->settings.statusDisplayType == vtbackend::StatusDisplayType::Indicator);
        CHECK(resolved->settings.cursorShape == vtbackend::CursorShape::Underscore);
    }
}

TEST_CASE("Config: resolveSessionConfig defaults match a profile that sets nothing explicitly", "[config]")
{
    // escape_sandbox defaults to true (DaemonConfig's own prior default), so a profile that says
    // nothing about it must not regress to false.
    QTemporaryDir dir;
    auto const path = writeConfig(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
)"sv);

    auto const resolved = contour::config::resolveSessionConfig(path.string(), "main");
    REQUIRE(resolved.has_value());
    CHECK(resolved->escapeSandbox);
    CHECK(resolved->shell.program == "/bin/sh");
    REQUIRE(resolved->shell.env.contains("CONTOUR_PROFILE"));
    CHECK(resolved->shell.env.at("CONTOUR_PROFILE") == "main");
}

TEST_CASE("Config: resolveSessionConfig falls back to the home directory when "
          "initial_working_directory is unset",
          "[config]")
{
    // The regression this guards: a profile that never sets shell.initial_working_directory used
    // to resolve to an EMPTY workingDirectory (YAMLConfigReader::loadFromEntry's homeResolvedPath
    // only substitutes `~`, not "absent"), and vtpty::Process then leaves an empty cwd as "inherit
    // the hosting process's cwd" -- fine for a locally-launched GUI, wrong for `contour daemon`,
    // whose own cwd is whatever started it (e.g. systemd's WorkingDirectory=). Every hosted shell
    // used to get vtpty::Process::homeDirectory() unconditionally (the old ContourApp::daemonAction);
    // resolvedSessionConfig must still land there when the profile leaves the directory unset.
    QTemporaryDir dir;
    auto const path = writeConfig(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
)"sv);

    auto const resolved = contour::config::resolveSessionConfig(path.string(), "main");
    REQUIRE(resolved.has_value());
    CHECK(resolved->shell.workingDirectory == vtpty::Process::homeDirectory());
}

TEST_CASE("Config: resolveSessionConfig honors an explicit initial_working_directory", "[config]")
{
    QTemporaryDir dir;
    auto const path = writeConfig(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        initial_working_directory: /var/tmp
)"sv);

    auto const resolved = contour::config::resolveSessionConfig(path.string(), "main");
    REQUIRE(resolved.has_value());
    CHECK(resolved->shell.workingDirectory == std::filesystem::path("/var/tmp"));
}

TEST_CASE("Config: resolveSessionConfig refuses an unknown profile", "[config]")
{
    QTemporaryDir dir;
    auto const path = writeConfig(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
)"sv);

    auto const resolved = contour::config::resolveSessionConfig(path.string(), "nope");
    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.error().contains("nope"));
}

TEST_CASE("Config: resolveSessionConfig carries the profile's default_layout into startupLayout", "[config]")
{
    // The regression this guards: a freshly spawned daemon always started with exactly one
    // window/tab/pane, silently ignoring default_layout/layouts: even after shell/presentation
    // resolution was fixed (#2092). contour client mirrors whatever tab/pane tree the daemon
    // has, so this is what made a configured multi-pane layout invisible on first attach.
    QTemporaryDir dir;
    auto const path = writeConfig(dir, R"(
default_profile: main
default_layout: work
profiles:
    main:
        shell: /bin/sh
layouts:
    work:
        tabs:
            - title: "editor"
              command: "nvim"
            - title: "servers"
              split:
                orientation: vertical
                panes:
                    - command: "npm run dev"
                      ratio: 0.6
                    - command: "htop"
)"sv);

    auto const resolved = contour::config::resolveSessionConfig(path.string(), "main");
    REQUIRE(resolved.has_value());

    REQUIRE(resolved->startupLayout.tabs.size() == 2);

    auto const& editorTab = resolved->startupLayout.tabs[0];
    CHECK(editorTab.title == "editor");
    CHECK(editorTab.root.isLeaf());
    REQUIRE(editorTab.root.command.has_value());
    CHECK(*editorTab.root.command == "nvim");

    auto const& serversTab = resolved->startupLayout.tabs[1];
    CHECK(serversTab.title == "servers");
    REQUIRE_FALSE(serversTab.root.isLeaf());
    CHECK(serversTab.root.orientation == vtworkspace::SplitState::Vertical);
    REQUIRE(serversTab.root.children.size() == 2);
    CHECK(*serversTab.root.children[0].command == "npm run dev");
    CHECK(*serversTab.root.children[1].command == "htop");
}

TEST_CASE("Config: resolveSessionConfig leaves startupLayout empty when default_layout is unset", "[config]")
{
    QTemporaryDir dir;
    auto const path = writeConfig(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
layouts:
    work:
        tabs:
            - command: "nvim"
)"sv);

    auto const resolved = contour::config::resolveSessionConfig(path.string(), "main");
    REQUIRE(resolved.has_value());
    CHECK(resolved->startupLayout.tabs.empty());
}

TEST_CASE("Config: resolveSessionConfig leaves startupLayout empty for an unknown default_layout "
          "name, not fatal to the rest of resolution",
          "[config]")
{
    // A typo'd default_layout must not fail the whole daemon startup: it degrades to "no startup
    // layout" (a single default tab), matching TerminalSessionManager::findLayout's own
    // "no layout named X" + fall back behavior on the local-GUI path.
    QTemporaryDir dir;
    auto const path = writeConfig(dir, R"(
default_profile: main
default_layout: nonexistent
profiles:
    main:
        shell: /bin/sh
layouts:
    work:
        tabs:
            - command: "nvim"
)"sv);

    auto const resolved = contour::config::resolveSessionConfig(path.string(), "main");
    REQUIRE(resolved.has_value());
    CHECK(resolved->startupLayout.tabs.empty());
    // The rest of resolution is unaffected by the bad layout name.
    CHECK(resolved->shell.program == "/bin/sh");
}

// {{{ history.hard_limit (issue #836)
namespace
{
/// A minimal profile whose `history:` block is @p historyBody.
[[nodiscard]] std::string historyConfig(std::string_view historyBody)
{
    return std::format("platform_plugin: auto\n"
                       "default_profile: main\n"
                       "profiles:\n"
                       "    main:\n"
                       "        shell: \"/bin/bash\"\n"
                       "        history:\n"
                       "{}",
                       historyBody);
}

/// The reconciled scrollback bounds a profile with this `history:` block ends up with.
[[nodiscard]] vtbackend::HistoryLimits limitsFrom(QTemporaryDir& dir, std::string_view historyBody)
{
    auto const config = loadFromYaml(dir, historyConfig(historyBody));
    auto const* const profile = config.profile("main");
    REQUIRE(profile != nullptr);
    return profile->history.value().limits();
}
} // namespace

TEST_CASE("Config: history.hard_limit defaults to history.limit", "[config][history]")
{
    auto dir = QTemporaryDir {};

    // A configuration naming only `limit` must keep the behaviour it always had: no headroom, so no
    // block-atomic eviction and no ring larger than what was asked for.
    auto const limits = limitsFrom(dir, "            limit: 4242\n");
    CHECK(limits.guaranteed == vtbackend::MaxHistoryLineCount { vtbackend::LineCount(4242) });
    CHECK(limits.capacity == limits.guaranteed);
    CHECK(!limits.hasHeadroom());
}

TEST_CASE("Config: history.hard_limit above the limit opens the headroom", "[config][history]")
{
    auto dir = QTemporaryDir {};
    auto const config =
        loadFromYaml(dir, historyConfig("            limit: 1000\n            hard_limit: 2500\n"));
    auto const* const profile = config.profile("main");
    REQUIRE(profile != nullptr);

    auto const limits = profile->history.value().limits();
    CHECK(limits.guaranteed == vtbackend::MaxHistoryLineCount { vtbackend::LineCount(1000) });
    CHECK(limits.capacity == vtbackend::MaxHistoryLineCount { vtbackend::LineCount(2500) });
    CHECK(limits.hasHeadroom());

    // And it reaches the terminal the daemon builds, not just the profile.
    CHECK(contour::config::emulationSettings(config, *profile).historyLimits == limits);
}

TEST_CASE("Config: history.hard_limit below the limit is raised to it", "[config][history]")
{
    auto dir = QTemporaryDir {};

    // Clamped rather than rejected, landing on "no headroom" -- a working configuration.
    auto const limits = limitsFrom(dir, "            limit: 5000\n            hard_limit: 200\n");
    CHECK(limits.capacity == vtbackend::MaxHistoryLineCount { vtbackend::LineCount(5000) });
    CHECK(!limits.hasHeadroom());
}

TEST_CASE("Config: an infinite history.limit makes the hard limit infinite too", "[config][history]")
{
    auto dir = QTemporaryDir {};

    // Nothing is ever evicted, so a ceiling under it would describe a bound that is never reached.
    auto const limits = limitsFrom(dir, "            limit: -1\n            hard_limit: 2000\n");
    CHECK(std::holds_alternative<vtbackend::Infinite>(limits.guaranteed));
    CHECK(std::holds_alternative<vtbackend::Infinite>(limits.capacity));
    CHECK(!limits.hasHeadroom());
}

TEST_CASE("Config: history.hard_limit survives a write/read round-trip", "[config][history]")
{
    auto dir = QTemporaryDir {};
    auto const config =
        loadFromYaml(dir, historyConfig("            limit: 1000\n            hard_limit: 2500\n"));
    auto const* const profile = config.profile("main");
    REQUIRE(profile != nullptr);

    auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
    auto secondDir = QTemporaryDir {};
    auto const reloaded = loadFromYaml(secondDir, written);
    auto const* const reloadedProfile = reloaded.profile("main");
    REQUIRE(reloadedProfile != nullptr);

    CHECK(reloadedProfile->history.value().limits() == profile->history.value().limits());
}
// }}}

namespace
{
/// The colour @p palette's scheme set for @p outcome's blocks; unset when it set none.
[[nodiscard]] std::optional<vtbackend::RGBColor> blockStatusIn(vtbackend::ColorPalette const& palette,
                                                               vtbackend::CommandBlockOutcome outcome)
{
    return palette.blockStatus[static_cast<size_t>(outcome)];
}
} // namespace

TEST_CASE("Config: the gutter's status and text colors load and are written back", "[config]")
{
    QTemporaryDir dir;

    SECTION("unset by default, so vtbackend derives them")
    {
        auto const scheme = schemeOf(loadFromYaml(dir, R"(
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
)"sv));
        CHECK_FALSE(blockStatusIn(scheme, vtbackend::CommandBlockOutcome::Success).has_value());
        CHECK_FALSE(blockStatusIn(scheme, vtbackend::CommandBlockOutcome::Failure).has_value());
        CHECK_FALSE(blockStatusIn(scheme, vtbackend::CommandBlockOutcome::Running).has_value());
        CHECK_FALSE(scheme.gutterText.has_value());
    }

    SECTION("every key loads")
    {
        auto const scheme = schemeOf(loadFromYaml(dir, R"(
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
        block_status:
            success: '#20A020'
            failure: '#E02020'
            running: '#E0E020'
        block_status_hover:
            success: '#40C040'
            failure: '#FF4040'
            running: '#FFFF40'
        gutter_text: '#606060'
)"sv));
        CHECK(scheme.blockStatusHover[static_cast<size_t>(vtbackend::CommandBlockOutcome::Success)]
              == vtbackend::RGBColor(0x40, 0xC0, 0x40));
        CHECK(scheme.blockStatusHover[static_cast<size_t>(vtbackend::CommandBlockOutcome::Failure)]
              == vtbackend::RGBColor(0xFF, 0x40, 0x40));
        CHECK(scheme.blockStatusHover[static_cast<size_t>(vtbackend::CommandBlockOutcome::Running)]
              == vtbackend::RGBColor(0xFF, 0xFF, 0x40));
        CHECK(blockStatusIn(scheme, vtbackend::CommandBlockOutcome::Success)
              == vtbackend::RGBColor(0x20, 0xA0, 0x20));
        CHECK(blockStatusIn(scheme, vtbackend::CommandBlockOutcome::Failure)
              == vtbackend::RGBColor(0xE0, 0x20, 0x20));
        CHECK(blockStatusIn(scheme, vtbackend::CommandBlockOutcome::Running)
              == vtbackend::RGBColor(0xE0, 0xE0, 0x20));
        CHECK(scheme.gutterText == vtbackend::RGBColor(0x60, 0x60, 0x60));
    }

    SECTION("one key alone loads, and the others stay unset")
    {
        auto const scheme = schemeOf(loadFromYaml(dir, R"(
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
        block_status:
            failure: '#E02020'
)"sv));
        CHECK_FALSE(blockStatusIn(scheme, vtbackend::CommandBlockOutcome::Success).has_value());
        CHECK(blockStatusIn(scheme, vtbackend::CommandBlockOutcome::Failure)
              == vtbackend::RGBColor(0xE0, 0x20, 0x20));
        CHECK_FALSE(blockStatusIn(scheme, vtbackend::CommandBlockOutcome::Running).has_value());
        CHECK_FALSE(scheme.gutterText.has_value());
    }

    SECTION("the writer emits what was set, and only that")
    {
        auto config = classicConfig();
        auto& scheme = config.colorschemes.value()["default"];
        scheme.blockStatus[static_cast<size_t>(vtbackend::CommandBlockOutcome::Failure)] =
            vtbackend::RGBColor(0xE0, 0x20, 0x20);
        scheme.gutterText = vtbackend::RGBColor(0x60, 0x60, 0x60);

        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
        REQUIRE(written.contains("block_status:"));

        auto const reloaded = schemeOf(loadFromYaml(dir, written));
        CHECK(blockStatusIn(reloaded, vtbackend::CommandBlockOutcome::Failure)
              == vtbackend::RGBColor(0xE0, 0x20, 0x20));
        CHECK(reloaded.gutterText == vtbackend::RGBColor(0x60, 0x60, 0x60));
        // Writing back a derived colour would pin what the scheme deliberately left to the palette.
        CHECK_FALSE(blockStatusIn(reloaded, vtbackend::CommandBlockOutcome::Success).has_value());
        CHECK_FALSE(blockStatusIn(reloaded, vtbackend::CommandBlockOutcome::Running).has_value());
    }

    SECTION("the generated default config carries the commented examples")
    {
        auto const generated = contour::config::defaultConfigString();
        CHECK(generated.contains("# block_status:"));
        CHECK(generated.contains("# block_status_hover:"));
        CHECK(generated.contains("# gutter_text:"));
    }
}

namespace
{
/// The palette's colour for @p Outcome, as a slot a table of colour keys can name.
template <vtbackend::CommandBlockOutcome Outcome>
std::optional<vtbackend::RGBColor>& blockStatusOf(vtbackend::ColorPalette& palette)
{
    return palette.blockStatus[static_cast<size_t>(Outcome)];
}

/// The palette's hover colour for @p Outcome, as a slot a table of colour keys can name.
template <vtbackend::CommandBlockOutcome Outcome>
std::optional<vtbackend::RGBColor>& blockStatusHoverOf(vtbackend::ColorPalette& palette)
{
    return palette.blockStatusHover[static_cast<size_t>(Outcome)];
}
} // namespace

TEST_CASE("Config: each gutter color a scheme sets is written back on its own", "[config]")
{
    // One key at a time, so a key the writer skips, or spells as another, shows up as exactly that key.
    using Slot = std::optional<vtbackend::RGBColor>& (*) (vtbackend::ColorPalette&);
    auto constexpr Slots = std::array {
        std::pair { "block_status.success"sv,
                    Slot { &blockStatusOf<vtbackend::CommandBlockOutcome::Success> } },
        std::pair { "block_status.failure"sv,
                    Slot { &blockStatusOf<vtbackend::CommandBlockOutcome::Failure> } },
        std::pair { "block_status.running"sv,
                    Slot { &blockStatusOf<vtbackend::CommandBlockOutcome::Running> } },
        std::pair { "block_status_hover.success"sv,
                    Slot { &blockStatusHoverOf<vtbackend::CommandBlockOutcome::Success> } },
        std::pair { "block_status_hover.failure"sv,
                    Slot { &blockStatusHoverOf<vtbackend::CommandBlockOutcome::Failure> } },
        std::pair { "block_status_hover.running"sv,
                    Slot { &blockStatusHoverOf<vtbackend::CommandBlockOutcome::Running> } },
        std::pair { "gutter_text"sv,
                    Slot { [](vtbackend::ColorPalette& palette) -> std::optional<vtbackend::RGBColor>& {
                        return palette.gutterText;
                    } } },
    };
    auto const color = vtbackend::RGBColor(0x12, 0x34, 0x56);
    QTemporaryDir dir;

    for (auto const& [name, slot]: Slots)
    {
        INFO("set: " << name);
        auto config = classicConfig();
        slot(config.colorschemes.value()["default"]) = color;

        auto const reloaded =
            loadFromYaml(dir, contour::config::createString<contour::config::YAMLConfigWriter>(config));
        auto scheme = schemeOf(reloaded);

        for (auto const& [readName, readSlot]: Slots)
        {
            INFO("read: " << readName);
            auto const expected =
                readSlot == slot ? std::optional { color } : std::optional<vtbackend::RGBColor> {};
            CHECK(readSlot(scheme) == expected);
        }
    }
}

TEST_CASE("Config: the gutter colors the generated config quotes are what the default scheme derives",
          "[config]")
{
    // The commented examples say so, so they are held to it here: retuning a derivation in ColorPalette
    // fails this rather than leaving the generated config quoting the old color.
    auto const generated = contour::config::defaultConfigString();
    auto const palette = vtbackend::ColorPalette {};
    using vtbackend::CommandBlockOutcome;

    CHECK(generated.contains(
        std::format("#     success: {}\n", palette.blockStatusColor(CommandBlockOutcome::Success))));
    CHECK(generated.contains(
        std::format("#     failure: {}\n", palette.blockStatusColor(CommandBlockOutcome::Failure))));
    CHECK(generated.contains(
        std::format("#     running: {}\n", palette.blockStatusColor(CommandBlockOutcome::Running))));
    for (auto const& [key, outcome]: { std::pair { "success"sv, CommandBlockOutcome::Success },
                                       std::pair { "failure"sv, CommandBlockOutcome::Failure },
                                       std::pair { "running"sv, CommandBlockOutcome::Running } })
    {
        INFO("block_status_hover." << key);
        CHECK(generated.contains(std::format("#     {}: {}\n", key, palette.blockStatusHoverColor(outcome))));
    }
    CHECK(generated.contains(std::format("# gutter_text: {}\n", palette.gutterTextColor())));
}

TEST_CASE("Config: the gutter section loads from YAML", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
gutter:
    exit_status: false
    user_marks: false
    bold_on_hover: false
    line_numbers: hybrid
    line_number_width: 8
    timestamps: true
    timestamp_format: '%H:%M'
profiles:
    main:
        shell: /bin/sh
)"sv);

    auto const& gutter = config.gutter.value();
    CHECK(gutter.exitStatus == false);
    CHECK(gutter.userMarks == false);
    CHECK(gutter.boldOnHover == false);
    CHECK(contour::config::gutterSettingsFor(config).hoverStroke == vtbackend::FoldHoverStroke::Unchanged);
    CHECK(gutter.lineNumbers == vtbackend::LineNumberMode::Hybrid);
    CHECK(gutter.lineNumberWidth == 8);
    CHECK(gutter.timestamps == true);
    CHECK(gutter.timestampFormat == "%H:%M");
}

TEST_CASE("Config: gutter defaults draw the block column two cells wide", "[config]")
{
    auto const config = contour::config::Config {};
    auto const& gutter = config.gutter.value();
    CHECK(gutter.exitStatus);
    CHECK(gutter.userMarks);
    CHECK(gutter.blockColumnWidth == 2);
    CHECK(gutter.boldOnHover);
    CHECK(contour::config::gutterSettingsFor(config).hoverStroke == vtbackend::FoldHoverStroke::Bolder);
    CHECK(gutter.lineNumbers == vtbackend::LineNumberMode::Off);
    CHECK(gutter.lineNumberWidth == 6);
    CHECK_FALSE(gutter.timestamps);
    CHECK(gutter.timestampFormat == "%H:%M:%S");

    // Exit status and user marks share the fold column, which takes two cells: its controls and marks are
    // drawn across both, at twice the size, and a click anywhere in it lands.
    CHECK(vtbackend::gutterLayoutFor(contour::config::gutterSettingsFor(config)).totalColumns == 2);
}

TEST_CASE("Config: gutter.block_column_width takes 1 or 2 cells", "[config]")
{
    struct Case
    {
        std::string_view width;
        int expected;
    };
    auto const cases = std::array {
        Case { .width = "1", .expected = 1 },  Case { .width = "2", .expected = 2 },
        Case { .width = "0", .expected = 2 },  Case { .width = "3", .expected = 2 },
        Case { .width = "-1", .expected = 2 },
    };
    for (auto const& testCase: cases)
    {
        CAPTURE(testCase.width);
        QTemporaryDir dir;
        auto capture = core::log::ScopedCapture { "error" };
        auto const config =
            loadFromYaml(dir,
                         contour::test::profileConfigYaml(
                             "main", std::format("gutter:\n    block_column_width: {}\n", testCase.width)));
        CHECK(config.gutter.value().blockColumnWidth == testCase.expected);
        CHECK(contour::config::gutterSettingsFor(config).blockColumnWidth == testCase.expected);
        // Refused, and said so, rather than taken: a width out of range is a typo.
        CHECK(capture.text().contains("block_column_width")
              == (testCase.expected != std::stoi(std::string { testCase.width })));
    }
}

TEST_CASE("Config: an unusable gutter width or timestamp format keeps the default and says why", "[config]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir, R"(
default_profile: main
gutter:
    line_numbers: absolute
    line_number_width: 40
    timestamp_format: '%K'
profiles:
    main:
        shell: /bin/sh
)"sv);

    auto const& gutter = config.gutter.value();
    CHECK(gutter.lineNumbers == vtbackend::LineNumberMode::Absolute); // the siblings still load
    CHECK(gutter.lineNumberWidth == 6);
    CHECK(gutter.timestampFormat == "%H:%M:%S");
    CHECK(capture.text().contains("line_number_width"));
    CHECK(capture.text().contains("timestamp_format"));
}

TEST_CASE("Config: gutter.line_number_width takes 3 to 10 columns", "[config]")
{
    struct Case
    {
        std::string_view width;
        int expected;
    };
    auto const cases = std::array {
        Case { .width = "3", .expected = 3 },  Case { .width = "10", .expected = 10 },
        Case { .width = "2", .expected = 6 },  Case { .width = "11", .expected = 6 },
        Case { .width = "-4", .expected = 6 },
    };
    for (auto const& testCase: cases)
    {
        CAPTURE(testCase.width);
        QTemporaryDir dir;
        auto const config =
            loadFromYaml(dir,
                         contour::test::profileConfigYaml(
                             "main", std::format("gutter:\n    line_number_width: {}\n", testCase.width)));
        CHECK(config.gutter.value().lineNumberWidth == testCase.expected);
    }
}

TEST_CASE("Config: a gutter timestamp format must begin with a conversion and spell one column a character",
          "[config]")
{
    // Refused by the gutter's own measure (measureTimestampFormat), so the file is held to exactly what the
    // gutter can draw, and the log says why in the gutter's words.
    struct Case
    {
        std::string_view format;
        vtbackend::TimestampFormatError error;
    };
    auto const cases = std::array {
        // A leading literal.
        Case { .format = "[%H:%M]", .error = vtbackend::TimestampFormatError::NoLeadingConversion },
        // A leading width and a leading alignment, which std::format would take as padding.
        Case { .format = "8%H", .error = vtbackend::TimestampFormatError::NoLeadingConversion },
        Case { .format = ">%H:%M", .error = vtbackend::TimestampFormatError::NoLeadingConversion },
        // U+6642 takes two columns.
        Case { .format = "%H\xE6\x99\x82%M",
               .error = vtbackend::TimestampFormatError::WideOrZeroWidthCharacter },
        // U+0301 COMBINING ACUTE ACCENT takes none.
        Case { .format = "%H:%M\xCC\x81",
               .error = vtbackend::TimestampFormatError::WideOrZeroWidthCharacter },
    };
    for (auto const& testCase: cases)
    {
        CAPTURE(testCase.format);
        QTemporaryDir dir;
        auto capture = core::log::ScopedCapture { "error" };
        auto const config =
            loadFromYaml(dir,
                         contour::test::profileConfigYaml(
                             "main", std::format("gutter:\n    timestamp_format: '{}'\n", testCase.format)));

        CHECK(config.gutter.value().timestampFormat == vtbackend::DefaultTimestampFormat);
        INFO("captured error log:\n" << capture.text());
        CHECK(capture.text().contains("timestamp_format"));
        CHECK(capture.text().contains(vtbackend::timestampFormatErrorText(testCase.error)));
    }
}

TEST_CASE("Config: an unknown line_numbers value keeps the default", "[config]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir, R"(
default_profile: main
gutter:
    line_numbers: sideways
profiles:
    main:
        shell: /bin/sh
)"sv);
    CHECK(config.gutter.value().lineNumbers == vtbackend::LineNumberMode::Off);
    CHECK(config.gutter.value() == contour::config::GutterConfig {}); // and every key it did not name
    CHECK(capture.text().contains("sideways"));
}

TEST_CASE("Config: gutterSettingsFor joins the gutter section and the fold switch", "[config]")
{
    auto config = contour::config::Config {};
    config.gutter.value().lineNumbers = vtbackend::LineNumberMode::Relative;
    config.gutter.value().lineNumberWidth = 4;
    config.gutter.value().timestamps = true;
    config.gutter.value().timestampFormat = "%H:%M";

    auto const settings = contour::config::gutterSettingsFor(config);
    CHECK(settings.foldMarkers); // folding.enabled and folding.show_markers, both on by default
    CHECK(settings.exitStatus);
    CHECK(settings.userMarks);
    CHECK(settings.lineNumbers == vtbackend::LineNumberMode::Relative);
    CHECK(settings.lineNumberWidth == 4);
    CHECK(settings.timestamps);
    CHECK(settings.timestampFormat == "%H:%M");

    config.folding.value().enabled = false;
    CHECK_FALSE(contour::config::gutterSettingsFor(config).foldMarkers);
}

TEST_CASE("Config: the gutter section survives a write and a read", "[config]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };

    SECTION("the generated defaults read back as the defaults")
    {
        auto const generated = contour::config::defaultConfigString();
        REQUIRE(generated.contains("\ngutter:\n"));
        CHECK(loadFromYaml(dir, generated).gutter.value() == contour::config::GutterConfig {});
    }

    SECTION("set values, a quote in the format among them, read back as set")
    {
        auto config = contour::config::Config {};
        config.gutter.value() =
            contour::config::GutterConfig { .exitStatus = false,
                                            .userMarks = true,
                                            .lineNumbers = vtbackend::LineNumberMode::Relative,
                                            .lineNumberWidth = 9,
                                            .timestamps = true,
                                            .timestampFormat = "%H:%M 'x'" };
        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
        CHECK(loadFromYaml(dir, written).gutter.value() == config.gutter.value());
    }

    CHECK(capture.text().empty());
}

TEST_CASE("Config: the terminal draws the gutter the window geometry reserves", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
folding:
    show_markers: false
gutter:
    line_numbers: absolute
profiles:
    main:
        shell: /bin/sh
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const settings = contour::config::emulationSettings(config, *profile);
    CHECK(settings.gutter == contour::config::gutterSettingsFor(config));
    CHECK(settings.gutter.lineNumbers == vtbackend::LineNumberMode::Absolute);
    CHECK_FALSE(settings.gutter.foldMarkers);
    CHECK(settings.gutter.exitStatus); // the block column outlives the fold markers
}

namespace
{
/// The action of type @p BoundAction bound to @p key.
/// @param config The loaded configuration.
/// @param key The key the binding is for.
/// @return The action, or nullopt when the key has no binding (the reader dropped it).
template <typename BoundAction>
[[nodiscard]] std::optional<BoundAction> boundAction(contour::config::Config const& config,
                                                     vtbackend::Key key)
{
    auto const* mapping = keyMappingFor(config, key);
    if (mapping == nullptr || mapping->binding.empty())
        return std::nullopt;
    return std::get<BoundAction>(mapping->binding.at(0));
}
} // namespace

TEST_CASE("Config: SelectCommandBlock reads its target, part and pinned block", "[config][semanticblocks]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control], key: 'F1', action: SelectCommandBlock }
    - { mods: [Control], key: 'F2', action: SelectCommandBlock, target: cursor, part: input }
    - { mods: [Control], key: 'F3', action: SelectCommandBlock, target: POINTER, part: all, block: 7 }
    - { mods: [Control], key: 'F4', action: SelectCommandBlock, part: bogus }
)"sv);

    auto const bound = [&](vtbackend::Key key) {
        return boundAction<contour::actions::SelectCommandBlock>(config, key);
    };

    auto const plain = bound(vtbackend::Key::F1);
    REQUIRE(plain.has_value());
    CHECK(plain->target == vtbackend::CommandBlockTarget::Last);
    CHECK(plain->part == vtbackend::CommandBlockPart::Output);
    CHECK(plain->block.value == 0);

    auto const atCursor = bound(vtbackend::Key::F2);
    REQUIRE(atCursor.has_value());
    CHECK(atCursor->target == vtbackend::CommandBlockTarget::Cursor);
    CHECK(atCursor->part == vtbackend::CommandBlockPart::Prompt);

    auto const pinned = bound(vtbackend::Key::F3);
    REQUIRE(pinned.has_value());
    CHECK(pinned->target == vtbackend::CommandBlockTarget::Pointer); // tokens ignore case
    CHECK(pinned->part == vtbackend::CommandBlockPart::PromptAndOutput);
    CHECK(pinned->block.value == 7);

    // A misspelt part drops the binding rather than silently selecting something else.
    CHECK_FALSE(bound(vtbackend::Key::F4).has_value());
}

TEST_CASE("Config: a block that names no block id drops the binding", "[config][semanticblocks]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control], key: 'F1', action: SelectCommandBlock, block: abc }
    - { mods: [Control], key: 'F2', action: SelectCommandBlock, block: -1 }
    - { mods: [Control], key: 'F3', action: SelectCommandBlock, block: 4294967296 }
    - { mods: [Control], key: 'F4', action: SelectCommandBlock, block: [7] }
    - { mods: [Control], key: 'F5', action: SelectCommandBlock, block: 0 }
    - { mods: [Control], key: 'F6', action: SelectCommandBlock, block: 4294967295 }
    - { mods: [Control], key: 'F7', action: SelectCommandBlock, block: 7 }
    - { mods: [Control], key: 'F8', action: SelectCommandBlock, part: bogus }
    - { mods: [Control], key: 'F9', action: SelectCommandBlock, block: 0x7 }
)"sv);

    auto const bound = [&](vtbackend::Key key) {
        return boundAction<contour::actions::SelectCommandBlock>(config, key);
    };

    // None of these is a block id written as Contour writes one. Read as some id anyway -- "not pinned", or
    // the digits before the junk -- each would quietly act on another block, so each drops the binding, as
    // a misspelt part does.
    CHECK_FALSE(bound(vtbackend::Key::F1).has_value()); // not a number
    CHECK_FALSE(bound(vtbackend::Key::F2).has_value()); // negative
    CHECK_FALSE(bound(vtbackend::Key::F3).has_value()); // past the id's range
    CHECK_FALSE(bound(vtbackend::Key::F4).has_value()); // not a scalar
    CHECK_FALSE(bound(vtbackend::Key::F9).has_value()); // not decimal: digits with more after them

    // Zero is "not pinned"; every other id the type holds pins.
    auto const unpinned = bound(vtbackend::Key::F5);
    REQUIRE(unpinned.has_value());
    CHECK(unpinned->block.value == 0);

    auto const largest = bound(vtbackend::Key::F6);
    REQUIRE(largest.has_value());
    CHECK(largest->block.value == 4294967295U);

    auto const pinned = bound(vtbackend::Key::F7);
    REQUIRE(pinned.has_value());
    CHECK(pinned->block.value == 7);

    // A dropped block says so in the words a dropped part uses.
    CHECK_FALSE(bound(vtbackend::Key::F8).has_value());
    auto const& log = capture.text();
    INFO("captured error log:\n" << log);
    CHECK(log.contains("Invalid part 'bogus' in action 'SelectCommandBlock'; ignoring the binding."));
    CHECK(log.contains("Invalid block 'abc' in action 'SelectCommandBlock'; ignoring the binding."));
    CHECK(log.contains("Invalid block '' in action 'SelectCommandBlock'; ignoring the binding.")); // [7]
}

TEST_CASE("Config: the block copy actions read back what the config writer writes",
          "[config][semanticblocks]")
{
    using namespace contour::actions;
    using vtbackend::CommandBlockId;
    using vtbackend::CommandBlockPart;
    using vtbackend::CommandBlockTarget;

    // The writer's own output, fed to the reader: what Contour writes is by construction what it reads.
    auto const copyPrompt = CopyCommandBlock { .target = CommandBlockTarget::Pointer,
                                               .part = CommandBlockPart::Prompt,
                                               .block = CommandBlockId(4) };
    auto const copyLine =
        CopyCommandLine { .target = CommandBlockTarget::Cursor, .block = CommandBlockId(2) };

    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir,
                                     std::format(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - {{ mods: [Control], key: 'F1', action: CopyCommandBlock }}
    - {{ mods: [Control], key: 'F2', action: {} }}
    - {{ mods: [Control], key: 'F3', action: CopyCommandLine }}
    - {{ mods: [Control], key: 'F4', action: {} }}
    - {{ mods: [Control], key: 'F5', action: CopyCommandBlock, part: bogus }}
    - {{ mods: [Control], key: 'F6', action: CopyCommandLine, target: nowhere }}
    - {{ mods: [Control], key: 'F7', action: CopyCommandLine, block: abc }}
)",
                                                 Action { copyPrompt },
                                                 Action { copyLine }));

    auto const plainBlock = boundAction<CopyCommandBlock>(config, vtbackend::Key::F1);
    REQUIRE(plainBlock.has_value());
    CHECK(plainBlock->target == CommandBlockTarget::Last);
    CHECK(plainBlock->part == CommandBlockPart::Output);
    CHECK(plainBlock->block.value == 0);

    auto const prompt = boundAction<CopyCommandBlock>(config, vtbackend::Key::F2);
    REQUIRE(prompt.has_value());
    CHECK(prompt->target == copyPrompt.target);
    CHECK(prompt->part == copyPrompt.part);
    CHECK(prompt->block == copyPrompt.block);

    auto const plainLine = boundAction<CopyCommandLine>(config, vtbackend::Key::F3);
    REQUIRE(plainLine.has_value());
    CHECK(plainLine->target == CommandBlockTarget::Last);
    CHECK(plainLine->block.value == 0);

    auto const line = boundAction<CopyCommandLine>(config, vtbackend::Key::F4);
    REQUIRE(line.has_value());
    CHECK(line->target == copyLine.target);
    CHECK(line->block == copyLine.block);

    // A misspelt argument drops the binding, in the words SelectCommandBlock's use.
    CHECK_FALSE(boundAction<CopyCommandBlock>(config, vtbackend::Key::F5).has_value());
    CHECK_FALSE(boundAction<CopyCommandLine>(config, vtbackend::Key::F6).has_value());
    CHECK_FALSE(boundAction<CopyCommandLine>(config, vtbackend::Key::F7).has_value());
    auto const& log = capture.text();
    INFO("captured error log:\n" << log);
    CHECK(log.contains("Invalid part 'bogus' in action 'CopyCommandBlock'; ignoring the binding."));
    CHECK(log.contains("Invalid target 'nowhere' in action 'CopyCommandLine'; ignoring the binding."));
    CHECK(log.contains("Invalid block 'abc' in action 'CopyCommandLine'; ignoring the binding."));
}

TEST_CASE("Config: CopySelection reads its fallback", "[config][semanticblocks]")
{
    using contour::actions::CopyFallback;
    using contour::actions::CopyFormat;
    using contour::actions::CopySelection;

    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(
        dir,
        std::format(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - {{ mods: [Control], key: 'F1', action: CopySelection, fallback: last_command_output }}
    - {{ mods: [Control], key: 'F2', action: CopySelection, format: HTML }}
    - {{ mods: [Control], key: 'F3', action: CopySelection, fallback: everything }}
    - {{ mods: [Control], key: 'F4', action: {} }}
)",
                    contour::actions::Action { CopySelection {
                        .format = CopyFormat::HTML, .fallback = CopyFallback::LastCommandOutput } }));

    auto const fallback = boundAction<CopySelection>(config, vtbackend::Key::F1);
    REQUIRE(fallback.has_value());
    CHECK(fallback->fallback == CopyFallback::LastCommandOutput);
    CHECK(fallback->format == CopyFormat::Text);

    auto const html = boundAction<CopySelection>(config, vtbackend::Key::F2);
    REQUIRE(html.has_value());
    CHECK(html->format == CopyFormat::HTML);
    CHECK(html->fallback == CopyFallback::None);

    // A misspelt fallback drops the binding: guessing would either empty the clipboard or fill it with a
    // build log the user never asked for.
    CHECK_FALSE(boundAction<CopySelection>(config, vtbackend::Key::F3).has_value());
    auto const& log = capture.text();
    INFO("captured error log:\n" << log);
    CHECK(log.contains("Invalid fallback 'everything' in action 'CopySelection'; ignoring the binding."));

    // What the config writer writes reads back.
    auto const both = boundAction<CopySelection>(config, vtbackend::Key::F4);
    REQUIRE(both.has_value());
    CHECK(both->format == CopyFormat::HTML);
    CHECK(both->fallback == CopyFallback::LastCommandOutput);
}

TEST_CASE("Config: a mouse binding may name a click count", "[config][semanticblocks]")
{
    using vtbackend::Modifier;
    using vtbackend::Modifiers;
    using vtbackend::MouseButton;

    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control], mouse: Left, action: FollowHyperlink }
    - { mods: [Control], mouse: Left, clicks: 3, action: SelectCommandBlock, target: pointer }
    - { mods: [Control], mouse: Left, clicks: 9, action: SelectAll }
    - { mods: [Control], key: 'F5', clicks: 2, action: SelectAll }
)"sv);

    auto const& mappings = config.inputMappings.value();

    // The click-count row is a binding of its own, not a second action on the plain Ctrl+Left one; the
    // out-of-range count and the count on a key are dropped.
    REQUIRE(mappings.mouseMappings.size() == 2);
    CHECK(mappings.keyMappings.empty());

    auto const ctrl = Modifiers { Modifier::Control };
    auto const* plain =
        contour::config::apply(mappings.mouseMappings, MouseButton::Left, ctrl, uint8_t { 0 });
    REQUIRE(plain != nullptr);
    CHECK(std::holds_alternative<contour::actions::FollowHyperlink>(plain->at(0)));

    auto const* third =
        contour::config::apply(mappings.mouseMappings, MouseButton::Left, ctrl, uint8_t { 0 }, uint8_t { 3 });
    REQUIRE(third != nullptr);
    REQUIRE(std::holds_alternative<contour::actions::SelectCommandBlock>(third->at(0)));
    CHECK(std::get<contour::actions::SelectCommandBlock>(third->at(0)).target
          == vtbackend::CommandBlockTarget::Pointer);

    CHECK(
        contour::config::apply(mappings.mouseMappings, MouseButton::Left, ctrl, uint8_t { 0 }, uint8_t { 2 })
        == nullptr);
}

TEST_CASE("Config: a malformed click count drops the binding", "[config][semanticblocks]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control], mouse: Left, clicks: 0, action: SelectAll }
    - { mods: [Control], mouse: Left, clicks: 5, action: SelectAll }
    - { mods: [Control], mouse: Left, clicks: -1, action: SelectAll }
    - { mods: [Control], mouse: Left, clicks: three, action: SelectAll }
    - { mods: [Control], mouse: Left, clicks: 0x3, action: SelectAll }
    - { mods: [Control], mouse: Left, clicks: 256, action: SelectAll }
    - { mods: [Control], mouse: Left, clicks: [3], action: SelectAll }
    - { mods: [Control], key: 'F5', clicks: 2, action: SelectAll }
    - { mods: [Control], key: 'F6', mouse: Left, clicks: 2, action: SelectAll }
    - { mods: [Control], mouse: WheelUp, clicks: 1, action: SelectAll }
    - { mods: [Control], mouse: Middle, clicks: 2, action: SelectCommandBlock, part: bogus }
    - { mods: [Control], mouse: Right, clicks: 4, action: SelectCommandBlock, target: cursor }
)"sv);

    // Every count above but the last's is no count, or one no press carries. Read as some count anyway --
    // the digits before the junk, the count wrapped into range, "any count" -- each would quietly bind
    // another click, so each drops the binding, as a misspelt modifier does. A count on a key or a wheel
    // names a click that never comes. The action's own arguments are read as on any other row.
    auto const& mappings = config.inputMappings.value();
    CHECK(mappings.keyMappings.empty());
    REQUIRE(mappings.mouseMappings.size() == 1);
    auto const& kept = mappings.mouseMappings.front();
    CHECK(kept.input == vtbackend::MouseButton::Right);
    CHECK(kept.clickCount == uint8_t { 4 });
    REQUIRE(std::holds_alternative<contour::actions::SelectCommandBlock>(kept.binding.at(0)));
    CHECK(std::get<contour::actions::SelectCommandBlock>(kept.binding.at(0)).target
          == vtbackend::CommandBlockTarget::Cursor);

    auto const& log = capture.text();
    INFO("captured error log:\n" << log);
    for (auto const* count: { "0", "5", "-1", "three", "0x3", "256" })
    {
        INFO("clicks: " << count);
        CHECK(log.contains(
            std::format("Dropping input_mapping entry [action: SelectAll, mouse: Left, clicks: {}]: "
                        "could not parse its clicks (expected 1 to 4).",
                        count)));
    }
    CHECK(log.contains("Dropping input_mapping entry [action: SelectAll, mouse: Left]: could not parse its "
                       "clicks (expected 1 to 4).")); // [3]
    CHECK(
        log.contains("Dropping input_mapping entry [action: SelectAll, key: F5, clicks: 2]: 'clicks' applies "
                     "to the Left, Middle and Right mouse buttons only."));
    CHECK(log.contains("Dropping input_mapping entry [action: SelectAll, key: F6, mouse: Left, clicks: 2]: "
                       "'clicks' applies to the Left, Middle and Right mouse buttons only."));
    CHECK(
        log.contains("Dropping input_mapping entry [action: SelectAll, mouse: WheelUp, clicks: 1]: 'clicks' "
                     "applies to the Left, Middle and Right mouse buttons only."));
    CHECK(log.contains("Invalid part 'bogus' in action 'SelectCommandBlock'; ignoring the binding."));
}

TEST_CASE("Config: Ctrl+triple-click selects the output under the pointer by default",
          "[config][semanticblocks]")
{
    using vtbackend::Modifier;
    using vtbackend::Modifiers;
    using vtbackend::MouseButton;

    auto const config = contour::config::Config {};
    auto const ctrl = Modifiers { Modifier::Control };

    auto const* bound =
        contour::config::applyBuiltinFallback(config, MouseButton::Left, ctrl, uint8_t { 0 }, uint8_t { 3 });
    REQUIRE(bound != nullptr);
    REQUIRE(std::holds_alternative<contour::actions::SelectCommandBlock>(bound->at(0)));
    auto const& select = std::get<contour::actions::SelectCommandBlock>(bound->at(0));
    CHECK(select.target == vtbackend::CommandBlockTarget::Pointer);
    CHECK(select.part == vtbackend::CommandBlockPart::Output);

    // Only the third click: a plain Ctrl+click and a Ctrl+double-click are not claimed by it, nor is a
    // triple-click without Ctrl or of another button.
    CHECK(contour::config::applyBuiltinFallback(config, MouseButton::Left, ctrl, uint8_t { 0 }) == nullptr);
    CHECK(contour::config::applyBuiltinFallback(config, MouseButton::Left, ctrl, uint8_t { 0 }, uint8_t { 2 })
          == nullptr);
    CHECK(contour::config::applyBuiltinFallback(
              config, MouseButton::Left, Modifiers { Modifier::None }, uint8_t { 0 }, uint8_t { 3 })
          == nullptr);
    CHECK(
        contour::config::applyBuiltinFallback(config, MouseButton::Middle, ctrl, uint8_t { 0 }, uint8_t { 3 })
        == nullptr);

    // A fallback, not a default (see builtinFallbackMouseMappings): no default row names a click count.
    CHECK(std::ranges::none_of(config.inputMappings.value().mouseMappings,
                               [](auto const& mapping) { return mapping.clickCount.has_value(); }));
}

TEST_CASE("Config: Ctrl+triple-click leaves the alternate screen's extended word alone",
          "[config][semanticblocks]")
{
    using vtbackend::Modifier;
    using vtbackend::Modifiers;
    using vtbackend::MouseButton;

    // There are no blocks on the alternate screen, so the row would claim the click only to refuse it,
    // and the terminal's own extended-word selection would be lost.
    auto const config = contour::config::Config {};
    auto const ctrl = Modifiers { Modifier::Control };
    auto const alternateScreen = uint8_t { vtbackend::MatchModes::AlternateScreen };
    CHECK(
        contour::config::applyBuiltinFallback(config, MouseButton::Left, ctrl, alternateScreen, uint8_t { 3 })
        == nullptr);
    CHECK(contour::config::applyBuiltinFallback(config, MouseButton::Left, ctrl, uint8_t { 0 }, uint8_t { 3 })
          != nullptr);
}

TEST_CASE("Config: a mouse binding's mode survives the writer", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Alt], mouse: Middle, action: SelectAll, mode: '~Select' }
)");
    auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
    REQUIRE(written.contains("mode: '~Select'"));

    QTemporaryDir reloadDir;
    auto const reloaded = loadFromYaml(reloadDir, written);
    auto const& mappings = reloaded.inputMappings.value().mouseMappings;
    REQUIRE(mappings.size() == 1);
    CHECK(mappings.front().modes == vtbackend::MatchModes { 0, vtbackend::MatchModes::Select });
}

TEST_CASE("Config: a click count survives the writer", "[config][semanticblocks]")
{
    QTemporaryDir dir;
    auto config = contour::config::Config {};
    auto mappings = config.inputMappings.value();
    mappings.mouseMappings.push_back(
        contour::config::MouseInputMapping { .modes { vtbackend::MatchModes {} },
                                             .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt } },
                                             .input = vtbackend::MouseButton::Middle,
                                             .binding = { { contour::actions::SelectAll {} } },
                                             .clickCount = uint8_t { 2 } });
    config.inputMappings = mappings;

    auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
    REQUIRE(written.contains("clicks: 2"));

    auto const reloaded = loadFromYaml(dir, written);
    CHECK(std::ranges::any_of(reloaded.inputMappings.value().mouseMappings, [](auto const& mapping) {
        return mapping.input == vtbackend::MouseButton::Middle && mapping.clickCount == uint8_t { 2 };
    }));
}

TEST_CASE("Config: command_blocks.pager", "[config][semanticblocks]")
{
    QTemporaryDir dir;

    SECTION("defaults to the platform's pager")
    {
        auto const config = contour::config::Config {};
#ifdef _WIN32
        CHECK(config.commandBlocks.value().pager == "more");
#else
        CHECK(config.commandBlocks.value().pager == "less -R");
#endif
        CHECK(config.commandBlocks.value().pager == contour::config::defaultCommandOutputPager());
    }

    SECTION("pager_format defaults to sgr, which less -R and Windows more both render")
    {
        CHECK(contour::config::Config {}.commandBlocks.value().pagerFormat
              == contour::actions::OutputFormat::Sgr);
    }

    SECTION("pager_format is read, and survives the writer")
    {
        auto const config = loadFromYaml(dir,
                                         "command_blocks:\n"
                                         "    pager: 'pg'\n"
                                         "    pager_format: plain\n"
                                         "profiles:\n  main:\n    shell: \"/bin/bash\"\n");
        CHECK(config.commandBlocks.value().pagerFormat == contour::actions::OutputFormat::Plain);

        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
        REQUIRE(written.contains("pager_format: plain"));
        QTemporaryDir reloadDir;
        CHECK(loadFromYaml(reloadDir, written).commandBlocks.value().pagerFormat
              == contour::actions::OutputFormat::Plain);
    }

    SECTION("the format follows the program unless the binding names one")
    {
        using contour::actions::OpenCommandOutput;
        using contour::actions::OutputFormat;
        using contour::config::outputFormatFor;
        auto blocks = contour::config::CommandBlocksConfig {};
        blocks.pagerFormat = OutputFormat::Plain;

        // The configured pager's format; a named program is assumed to render SGR, as less -R does.
        CHECK(outputFormatFor(OpenCommandOutput {}, blocks) == OutputFormat::Plain);
        CHECK(outputFormatFor(OpenCommandOutput { .program = "less -R" }, blocks) == OutputFormat::Sgr);
        // The binding's own format wins either way.
        CHECK(outputFormatFor(OpenCommandOutput { .format = OutputFormat::Sgr }, blocks)
              == OutputFormat::Sgr);
        CHECK(outputFormatFor(OpenCommandOutput { .program = "pg", .format = OutputFormat::Plain }, blocks)
              == OutputFormat::Plain);
    }

    SECTION("is read from command_blocks")
    {
        auto const config = loadFromYaml(dir,
                                         "command_blocks:\n"
                                         "    pager: 'bat --paging=always'\n"
                                         "profiles:\n  main:\n    shell: \"/bin/bash\"\n");
        CHECK(config.commandBlocks.value().pager == "bat --paging=always");
    }

    SECTION("round-trips through the generated config")
    {
        auto config = contour::config::Config {};
        auto blocks = config.commandBlocks.value();
        blocks.pager = "most -s";
        config.commandBlocks = blocks;

        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
        REQUIRE(written.contains("pager: 'most -s'"));
        CHECK(loadFromYaml(dir, written).commandBlocks.value().pager == "most -s");
    }

    SECTION("round-trips a quote, a colon and a hash")
    {
        auto config = contour::config::Config {};
        auto blocks = config.commandBlocks.value();
        blocks.pager = "less --prompt='page: #%d'";
        config.commandBlocks = blocks;

        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
        CHECK(loadFromYaml(dir, written).commandBlocks.value().pager == "less --prompt='page: #%d'");
    }
}

TEST_CASE("Config: OpenCommandOutput reads its arguments", "[config][semanticblocks]")
{
    using contour::actions::OpenCommandOutput;
    using contour::actions::OutputFormat;
    using contour::actions::OutputPlacement;

    // The writer's own output, fed to the reader: what Contour writes is by construction what it reads.
    auto const written = OpenCommandOutput { .target = vtbackend::CommandBlockTarget::Pointer,
                                             .program = "less --prompt='x'",
                                             .placement = OutputPlacement::Tab,
                                             .format = OutputFormat::Plain,
                                             .block = vtbackend::CommandBlockId(4) };

    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir,
                                     std::format(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - {{ mods: [Control], key: 'F1', action: OpenCommandOutput }}
    - {{ mods: [Control], key: 'F2', action: OpenCommandOutput, program: 'wl-copy', placement: detached, format: plain }}
    - {{ mods: [Control], key: 'F3', action: {} }}
    - {{ mods: [Control], key: 'F4', action: OpenCommandOutput, placement: window }}
    - {{ mods: [Control], key: 'F5', action: OpenCommandOutput, format: html }}
    - {{ mods: [Control], key: 'F6', action: OpenCommandOutput, target: nowhere }}
    - {{ mods: [Control], key: 'F7', action: OpenCommandOutput, block: abc }}
    - {{ mods: [Control], key: 'F8', action: OpenCommandOutput, program: [less, -R] }}
)",
                                                 contour::actions::Action { written }));

    auto const plain = boundAction<OpenCommandOutput>(config, vtbackend::Key::F1);
    REQUIRE(plain.has_value());
    CHECK(plain->target == vtbackend::CommandBlockTarget::Last);
    CHECK(plain->program.empty());
    CHECK(plain->placement == OutputPlacement::Split);
    CHECK_FALSE(plain->format.has_value()); // the program's own format applies
    CHECK(plain->block.value == 0);

    auto const piped = boundAction<OpenCommandOutput>(config, vtbackend::Key::F2);
    REQUIRE(piped.has_value());
    CHECK(piped->program == "wl-copy");
    CHECK(piped->placement == OutputPlacement::Detached);
    CHECK(piped->format == OutputFormat::Plain);

    auto const readBack = boundAction<OpenCommandOutput>(config, vtbackend::Key::F3);
    REQUIRE(readBack.has_value());
    CHECK(readBack->target == written.target);
    CHECK(readBack->program == written.program);
    CHECK(readBack->placement == written.placement);
    CHECK(readBack->format == written.format);
    CHECK(readBack->block == written.block);

    // A misspelt argument drops the binding, in the words the other block actions use: guessing would turn
    // the binding into one that does something else. A program that is no scalar is no program either.
    CHECK_FALSE(boundAction<OpenCommandOutput>(config, vtbackend::Key::F4).has_value());
    CHECK_FALSE(boundAction<OpenCommandOutput>(config, vtbackend::Key::F5).has_value());
    CHECK_FALSE(boundAction<OpenCommandOutput>(config, vtbackend::Key::F6).has_value());
    CHECK_FALSE(boundAction<OpenCommandOutput>(config, vtbackend::Key::F7).has_value());
    CHECK_FALSE(boundAction<OpenCommandOutput>(config, vtbackend::Key::F8).has_value());
    auto const& log = capture.text();
    INFO("captured error log:\n" << log);
    CHECK(log.contains("Invalid placement 'window' in action 'OpenCommandOutput'; ignoring the binding."));
    CHECK(log.contains("Invalid format 'html' in action 'OpenCommandOutput'; ignoring the binding."));
    CHECK(log.contains("Invalid target 'nowhere' in action 'OpenCommandOutput'; ignoring the binding."));
    CHECK(log.contains("Invalid block 'abc' in action 'OpenCommandOutput'; ignoring the binding."));
    CHECK(log.contains("Invalid program '' in action 'OpenCommandOutput'; ignoring the binding."));
}

TEST_CASE("Config: Ctrl+Shift+G opens the last command's output by default", "[config][semanticblocks]")
{
    using vtbackend::Modifier;
    using vtbackend::Modifiers;

    auto const config = contour::config::Config {};
    auto const ctrlShift = Modifiers { Modifier::Control, Modifier::Shift };

    auto const* bound = contour::config::applyBuiltinFallback(config, U'G', ctrlShift, uint8_t { 0 });
    REQUIRE(bound != nullptr);
    REQUIRE(std::holds_alternative<contour::actions::OpenCommandOutput>(bound->at(0)));
    auto const& open = std::get<contour::actions::OpenCommandOutput>(bound->at(0));
    CHECK(open.target == vtbackend::CommandBlockTarget::Last);
    CHECK(open.program.empty()); // command_blocks.pager
    CHECK(open.placement == contour::actions::OutputPlacement::Split);

    // Exactly the chord: not Ctrl+G, which the shell owns.
    CHECK(contour::config::applyBuiltinFallback(config, U'G', Modifiers { Modifier::Control }, uint8_t { 0 })
          == nullptr);

    // A fallback, not a default -- it must reach users whose contour.yml lists the defaults.
    CHECK(std::ranges::none_of(config.inputMappings.value().charMappings,
                               [](auto const& mapping) { return mapping.input == U'G'; }));
}

// {{{ notify_on_command_finish

TEST_CASE("Config: notify_on_command_finish is on by default for unfocused commands of ten seconds",
          "[config][notification]")
{
    auto const policy = contour::config::TerminalProfile {}.notifyOnCommandFinish.value();
    CHECK(policy.when == contour::config::NotifyWhen::Unfocused);
    CHECK(policy.minDuration == std::chrono::seconds { 10 });
    CHECK(policy.outcome == contour::config::NotifyOutcome::Any);
    CHECK(policy.action == contour::config::NotifyAction::Notify);
    CHECK(policy.clearOn == contour::config::NotifyClearOn::Focus);
}

TEST_CASE("Config: notify_on_command_finish loads every key from a profile", "[config][notification]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        notify_on_command_finish:
            when: hidden
            min_duration: 42
            outcome: failure
            action: notify_bell
            clear_on: next
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const expected = contour::config::FinishNotificationConfig {
        .when = contour::config::NotifyWhen::Hidden,
        .minDuration = std::chrono::seconds { 42 },
        .outcome = contour::config::NotifyOutcome::Failure,
        .action = contour::config::NotifyAction::NotifyBell,
        .clearOn = contour::config::NotifyClearOn::Next,
    };
    CHECK(profile->notifyOnCommandFinish.value() == expected);
}

TEST_CASE("Config: an unknown notify_on_command_finish value keeps the default and is reported",
          "[config][notification]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        notify_on_command_finish:
            when: sometimes
            min_duration: -5
            action: shout
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const& policy = profile->notifyOnCommandFinish.value();
    CHECK(policy.when == contour::config::NotifyWhen::Unfocused);
    CHECK(policy.action == contour::config::NotifyAction::Notify);
    // A negative floor reads as "every command", which is what zero already says.
    CHECK(policy.minDuration == std::chrono::seconds { 0 });
    CHECK(capture.text().contains("sometimes"));
    CHECK(capture.text().contains("shout"));
}

TEST_CASE("Config: a min_duration that is no number keeps the default and is reported",
          "[config][notification]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        notify_on_command_finish:
            min_duration: soon
            clear_on: next
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const& policy = profile->notifyOnCommandFinish.value();
    CHECK(policy.minDuration == std::chrono::seconds { 10 });
    // One bad value costs only itself: the keys after it are still read.
    CHECK(policy.clearOn == contour::config::NotifyClearOn::Next);
    CHECK(capture.text().contains("soon"));
}

namespace
{

/// A configuration entry given a value of the wrong shape: a scalar where a map belongs, or a list where
/// one word belongs.
struct MalformedEntryCase
{
    std::string_view topLevel;  ///< Lines at the document's top level; empty for none.
    std::string_view inProfile; ///< Lines inside the main profile, indented by eight spaces; empty for none.
    std::string_view key;       ///< The key the diagnostic must name, spelled out from the document's root.
    /// Whether the entry still holds its default (or, for a nested value, what its siblings say).
    bool (*keptDefault)(contour::config::Config const& config);
};

/// The main profile of @p config, which every malformed-entry document defines.
[[nodiscard]] contour::config::TerminalProfile const& mainProfile(contour::config::Config const& config)
{
    auto const* profile = config.findProfile("main");
    REQUIRE(profile != nullptr);
    return *profile;
}

/// The palette of the `broken` scheme the main profile of @p config uses, once its sibling map after the
/// malformed one was read; nullptr when the profile lost the scheme, or the sibling was not read.
[[nodiscard]] vtbackend::ColorPalette const* brokenScheme(contour::config::Config const& config)
{
    auto const* colors = std::get_if<contour::config::SimpleColorConfig>(&mainProfile(config).colors.value());
    if (colors == nullptr || colors->colorScheme != "broken"
        || colors->colors.gutterText != vtbackend::RGBColor { 0x123456 })
        return nullptr;
    return &colors->colors;
}

constexpr auto MalformedEntryCases = std::array {
    // A map-valued key given a scalar.
    MalformedEntryCase { .topLevel = {},
                         .inProfile = "        notify_on_command_finish: never\n",
                         .key = "profiles.main.notify_on_command_finish",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 return mainProfile(config).notifyOnCommandFinish.value()
                                        == contour::config::FinishNotificationConfig {};
                             } },
    MalformedEntryCase { .topLevel = "command_blocks: true\n",
                         .inProfile = {},
                         .key = "command_blocks",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 return config.commandBlocks.value().maxRecords
                                        == contour::config::CommandBlocksConfig {}.maxRecords;
                             } },
    MalformedEntryCase { .topLevel = "osc_context: off\n",
                         .inProfile = {},
                         .key = "osc_context",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 return config.oscContext.value().enabled
                                        == contour::config::OscContextConfig {}.enabled;
                             } },
    MalformedEntryCase { .topLevel = {},
                         .inProfile = "        scrollbar: left\n",
                         .key = "profiles.main.scrollbar",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 return mainProfile(config).scrollbar.value().position
                                        == contour::config::ScrollBarConfig {}.position;
                             } },
    // A list-valued key given one word: it costs the list, not its siblings.
    MalformedEntryCase { .topLevel = {},
                         .inProfile = "        scrollbar:\n"
                                      "            position: left\n"
                                      "            marks: failures\n",
                         .key = "profiles.main.scrollbar.marks",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 auto const& scrollbar = mainProfile(config).scrollbar.value();
                                 return scrollbar.marks == vtbackend::AllScrollbarMarkSources
                                        && scrollbar.position == contour::config::ScrollBarPosition::Left;
                             } },
    MalformedEntryCase { .topLevel = {},
                         .inProfile = "        sticky_header: always\n",
                         .key = "profiles.main.sticky_header",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 return mainProfile(config).stickyHeader.value()
                                        == contour::config::StickyHeaderConfig {};
                             } },
    // A colour map of a scheme given a scalar: it costs that map, whose palette slot keeps its default, and
    // the profile keeps the scheme -- the map after it is still read.
    MalformedEntryCase { .topLevel = "color_schemes:\n"
                                     "    broken:\n"
                                     "        sticky_header: '#202830'\n"
                                     "        gutter_text: '#123456'\n",
                         .inProfile = "        colors: broken\n",
                         .key = "color_schemes.broken.sticky_header",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 auto const* scheme = brokenScheme(config);
                                 return scheme != nullptr && !scheme->stickyHeaderBackground
                                        && !scheme->stickyHeaderSeparator;
                             } },
    MalformedEntryCase { .topLevel = "color_schemes:\n"
                                     "    broken:\n"
                                     "        fold_marker: '#202830'\n"
                                     "        gutter_text: '#123456'\n",
                         .inProfile = "        colors: broken\n",
                         .key = "color_schemes.broken.fold_marker",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 auto const* scheme = brokenScheme(config);
                                 return scheme != nullptr && !scheme->foldMarker && !scheme->foldMarkerHover;
                             } },
    MalformedEntryCase { .topLevel = "color_schemes:\n"
                                     "    broken:\n"
                                     "        block_status: '#202830'\n"
                                     "        gutter_text: '#123456'\n",
                         .inProfile = "        colors: broken\n",
                         .key = "color_schemes.broken.block_status",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 auto const* scheme = brokenScheme(config);
                                 return scheme != nullptr
                                        && std::ranges::none_of(scheme->blockStatus,
                                                                [](auto const& c) { return c.has_value(); });
                             } },
    MalformedEntryCase { .topLevel = "color_schemes:\n"
                                     "    broken:\n"
                                     "        tint: '#202830'\n"
                                     "        gutter_text: '#123456'\n",
                         .inProfile = "        colors: broken\n",
                         .key = "color_schemes.broken.tint",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 auto const* scheme = brokenScheme(config);
                                 return scheme != nullptr && !scheme->hasContextTints();
                             } },
    // A map whose malformed key comes after a good one: the map is put back whole, not left half read.
    MalformedEntryCase { .topLevel = "color_schemes:\n"
                                     "    broken:\n"
                                     "        sticky_header:\n"
                                     "            background: '#202830'\n"
                                     "            separator: [1, 2]\n"
                                     "        gutter_text: '#123456'\n",
                         .inProfile = "        colors: broken\n",
                         .key = "color_schemes.broken.sticky_header",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 auto const* scheme = brokenScheme(config);
                                 return scheme != nullptr && !scheme->stickyHeaderBackground
                                        && !scheme->stickyHeaderSeparator;
                             } },
    // So are the palette's own colour maps.
    MalformedEntryCase { .topLevel = "color_schemes:\n"
                                     "    broken:\n"
                                     "        gutter_text: '#123456'\n"
                                     "        normal:\n"
                                     "            red: [1, 2]\n",
                         .inProfile = "        colors: broken\n",
                         .key = "color_schemes.broken.normal",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 auto const* scheme = brokenScheme(config);
                                 return scheme != nullptr
                                        && scheme->palette[1] == vtbackend::ColorPalette {}.palette[1];
                             } },
    // The scheme's older maps are guarded the same way.
    MalformedEntryCase { .topLevel = "color_schemes:\n"
                                     "    broken:\n"
                                     "        cursor: block\n"
                                     "        gutter_text: '#123456'\n",
                         .inProfile = "        colors: broken\n",
                         .key = "color_schemes.broken.cursor",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 auto const* scheme = brokenScheme(config);
                                 return scheme != nullptr
                                        && scheme->cursor.color == vtbackend::ColorPalette {}.cursor.color;
                             } },
    // A map whose malformed key comes after a good one: the entry is put back whole, not left half read.
    MalformedEntryCase {
        .topLevel = {},
        .inProfile = "        history:\n"
                     "            limit: 4242\n"
                     "            auto_scroll_on_update: [yes]\n",
        .key = "profiles.main.history",
        .keptDefault =
            [](contour::config::Config const& config) {
                return mainProfile(config).history.value().maxHistoryLineCount
                       == contour::config::TerminalProfile {}.history.value().maxHistoryLineCount;
            } },
    // An enum key given a list.
    MalformedEntryCase { .topLevel = "tab_bar_position: [bottom]\n",
                         .inProfile = {},
                         .key = "tab_bar_position",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 return config.tabBarPosition.value() == contour::config::TabBarPosition::Top;
                             } },
    MalformedEntryCase { .topLevel = "ui_style: [terminal]\n",
                         .inProfile = {},
                         .key = "ui_style",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 return config.uiStyle.value() == contour::config::UiStyle::Native;
                             } },
    MalformedEntryCase { .topLevel = {},
                         .inProfile = "        search_case_sensitivity: [sensitive]\n",
                         .key = "profiles.main.search_case_sensitivity",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 return mainProfile(config).searchCaseSensitivity.value()
                                        == vtbackend::SearchCaseSensitivity::Smart;
                             } },
    // Nested: the bad value costs only itself, so its sibling after it is still read.
    MalformedEntryCase { .topLevel = {},
                         .inProfile = "        notify_on_command_finish:\n"
                                      "            when: [always]\n"
                                      "            outcome: failure\n",
                         .key = "profiles.main.notify_on_command_finish.when",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 auto const& policy = mainProfile(config).notifyOnCommandFinish.value();
                                 return policy.when == contour::config::NotifyWhen::Unfocused
                                        && policy.outcome == contour::config::NotifyOutcome::Failure;
                             } },
    // Nested map-valued keys that are no config entries of their own: each costs itself, not its profile.
    MalformedEntryCase {
        .topLevel = {},
        // Not `block`: that is the insert cursor's default now, and a value equal to the default could
        // not tell a skipped entry from a misread one.
        .inProfile = "        cursor: underscore\n",
        .key = "profiles.main.cursor",
        .keptDefault =
            [](contour::config::Config const& config) {
                return mainProfile(config).modeInsert.value().cursor.cursorShape
                       == contour::config::TerminalProfile {}.modeInsert.value().cursor.cursorShape;
            } },
    MalformedEntryCase { .topLevel = {},
                         .inProfile = "        normal_mode: block\n",
                         .key = "profiles.main.normal_mode",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 return mainProfile(config).modeNormal.value().cursor.cursorShape
                                        == vtbackend::CursorShape::Block;
                             } },
    MalformedEntryCase { .topLevel = {},
                         .inProfile = "        visual_mode:\n"
                                      "            cursor: underscore\n",
                         .key = "profiles.main.visual_mode.cursor",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 return mainProfile(config).modeVisual.value().cursor.cursorShape
                                        == vtbackend::CursorShape::Block;
                             } },
};

} // namespace

TEST_CASE("Config: a value of the wrong shape costs only its own entry", "[config][notification]")
{
    // yaml-cpp throws when a value has another shape than its reader expects. Uncaught, that throw ended
    // the whole load, and every entry read after it silently kept its default.
    for (auto const& testCase: MalformedEntryCases)
    {
        INFO("key: " << testCase.key);
        QTemporaryDir dir;
        auto capture = core::log::ScopedCapture { "error" };
        auto const config = loadFromYaml(dir,
                                         std::format(R"(default_profile: main
{}profiles:
    main:
        shell: /bin/sh
{}        terminal_id: VT420
    second:
        shell: /bin/sh
layouts:
    work:
        tabs:
            - title: "editor"
input_mapping:
    - {{ mods: [Control], key: 'J', action: ScreenshotVT }}
)",
                                                     testCase.topLevel,
                                                     testCase.inProfile));

        CHECK(testCase.keptDefault(config));
        // Read after the malformed entry: in the same profile, in another profile, and at the top level.
        CHECK(mainProfile(config).terminalId.value() == vtbackend::VTType::VT420);
        CHECK(config.findProfile("second") != nullptr);
        CHECK(config.layouts.value().contains("work"));
        CHECK(config.inputMappings.value().charMappings.size() == 1);

        INFO("captured error log:\n" << capture.text());
        CHECK(capture.text().contains(testCase.key));
    }
}

TEST_CASE("Config: a malformed entry is reported by its key path and its place in the file", "[config]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir, R"(default_profile: main
profiles:
    main:
        cursor: block
        notify_on_command_finish:
            when: [always]
        search_case_sensitivity: [sensitive]
)");

    INFO("captured error log:\n" << capture.text());
    CHECK(capture.text().contains("profiles.main.cursor"));
    CHECK(capture.text().contains("line 4, column 17"));
    CHECK(capture.text().contains("profiles.main.notify_on_command_finish.when (line 6, column 19)"));
    CHECK(capture.text().contains("profiles.main.search_case_sensitivity (line 7, column 34)"));
}

TEST_CASE("Config: a malformed map of a colour scheme is reported where the scheme is written", "[config]")
{
    // The profile only names the scheme: the map is somewhere else, and that is where to look for it.
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };

    SECTION("in a scheme file: by the file and the key path in it")
    {
        writeSideFile(dir, "colorschemes/broken.yml", "gutter_text: '#123456'\nsticky_header: '#202830'\n");
        std::ignore = loadFromYaml(dir, R"(default_profile: main
profiles:
    main:
        shell: /bin/sh
        colors: broken
)");
        INFO("captured error log:\n" << capture.text());
        CHECK(capture.text().contains("broken.yml: sticky_header"));
        CHECK(capture.text().contains("line 2, column 16"));
        CHECK_FALSE(capture.text().contains("profiles.main.colors"));
    }

    SECTION("in the configuration: by the key path from its root")
    {
        std::ignore = loadFromYaml(dir, R"(default_profile: main
profiles:
    main:
        shell: /bin/sh
        colors: broken
color_schemes:
    broken:
        sticky_header: '#202830'
)");
        INFO("captured error log:\n" << capture.text());
        CHECK(capture.text().contains("color_schemes.broken.sticky_header"));
        CHECK(capture.text().contains("line 8, column 24"));
    }
}

TEST_CASE("Config: after a colour scheme's file, the profile's entries are reported by its key path again",
          "[config]")
{
    // The scheme's file is read with the key path rooted at that file, and the profile's own path is put
    // back afterwards: an entry the profile sets after `colors` is found in contour.yml, not in the file.
    QTemporaryDir dir;
    writeSideFile(dir, "colorschemes/broken.yml", "sticky_header: '#202830'\n");
    auto capture = core::log::ScopedCapture { "error" };
    std::ignore = loadFromYaml(dir, R"(default_profile: main
profiles:
    main:
        shell: /bin/sh
        colors: broken
        hint_scrollback_lines: [x]
)");

    INFO("captured error log:\n" << capture.text());
    CHECK(capture.text().contains("broken.yml: sticky_header"));
    CHECK(capture.text().contains("profiles.main.hint_scrollback_lines: yaml-cpp: error at line 6"));
}

TEST_CASE("Config: notify_on_command_finish round-trips through a profile side file",
          "[config][notification]")
{
    QTemporaryDir dir;
    auto const source = loadFromYaml(dir, "default_profile: main\n");
    auto const* base = source.findProfile("main");
    REQUIRE(base != nullptr);
    auto profile = *base;
    auto const policy = contour::config::FinishNotificationConfig {
        .when = contour::config::NotifyWhen::Always,
        .minDuration = std::chrono::seconds { 3 },
        .outcome = contour::config::NotifyOutcome::Failure,
        .action = contour::config::NotifyAction::Bell,
        .clearOn = contour::config::NotifyClearOn::Never,
    };
    profile.notifyOnCommandFinish = policy;

    writeSideFile(dir, "profiles/roundtrip.yml", contour::config::emitProfileYaml(profile));

    auto const reloaded = loadFromYaml(dir, "default_profile: main\n");
    auto const* roundTripped = reloaded.findProfile("roundtrip");
    REQUIRE(roundTripped != nullptr);
    CHECK(roundTripped->notifyOnCommandFinish.value() == policy);
}

TEST_CASE("Config: the generated config spells out notify_on_command_finish and reads it back",
          "[config][notification]")
{
    QTemporaryDir dir;
    auto const rendered = contour::config::defaultConfigString();
    CHECK(rendered.contains("notify_on_command_finish:"));
    CHECK(rendered.contains("when: unfocused"));
    CHECK(rendered.contains("min_duration: 10"));
    CHECK(rendered.contains("clear_on: focus"));

    auto const defaults = contour::config::Config {};
    auto const reloaded = loadFromYaml(dir, rendered);
    auto const* profile = reloaded.profile(defaults.defaultProfileName.value());
    REQUIRE(profile != nullptr);
    CHECK(profile->notifyOnCommandFinish.value() == contour::config::FinishNotificationConfig {});
}

TEST_CASE("Config: every notify_on_command_finish value has one token that reads back",
          "[config][notification]")
{
    using namespace contour::config;
    checkEnumerators(
        std::array { NotifyWhen::Never, NotifyWhen::Unfocused, NotifyWhen::Hidden, NotifyWhen::Always });
    checkEnumerators(std::array { NotifyOutcome::Any, NotifyOutcome::Failure });
    checkEnumerators(std::array { NotifyAction::Notify, NotifyAction::Bell, NotifyAction::NotifyBell });
    checkEnumerators(std::array { NotifyClearOn::Focus, NotifyClearOn::Next, NotifyClearOn::Never });
}

// }}}

// {{{ sticky_header

TEST_CASE("Config: every sticky header mode is described by its table", "[config][stickyheader]")
{
    using namespace contour::config;
    using vtbackend::StickyHeaderMode;

    checkEnumerators(
        std::array { StickyHeaderMode::Never, StickyHeaderMode::Scrolled, StickyHeaderMode::Always });
    // The spellings the documentation promises, and the reader's case-insensitivity.
    CHECK(configEnumToken(StickyHeaderMode::Never) == "never");
    CHECK(configEnumToken(StickyHeaderMode::Scrolled) == "scrolled");
    CHECK(configEnumToken(StickyHeaderMode::Always) == "always");
    CHECK(configEnumFromToken<StickyHeaderMode>("ALWAYS") == StickyHeaderMode::Always);
}

TEST_CASE("Config: sticky_header loads from YAML and reaches the session settings", "[config][stickyheader]")
{
    QTemporaryDir dir;

    SECTION("defaults: scrolled, with the evicted placeholder shown")
    {
        auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
)"sv);
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        CHECK(profile->stickyHeader.value().mode == vtbackend::StickyHeaderMode::Scrolled);
        CHECK(profile->stickyHeader.value().showEvicted);
        CHECK(
            contour::config::sessionSettings(config, *profile, vtbackend::ColorPreference::Dark).stickyHeader
            == vtbackend::StickyHeaderSettings {});
    }

    SECTION("explicit values reach vtbackend, the YAML bool as the engine's enum")
    {
        auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        sticky_header:
            mode: always
            show_evicted: false
)"sv);
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        CHECK(
            contour::config::sessionSettings(config, *profile, vtbackend::ColorPreference::Dark).stickyHeader
            == vtbackend::StickyHeaderSettings { .mode = vtbackend::StickyHeaderMode::Always,
                                                 .evictedPlaceholder =
                                                     vtbackend::EvictedPlaceholder::Hidden });
    }

    SECTION("an unknown mode keeps the default, and its sibling still loads")
    {
        auto capture = core::log::ScopedCapture { "error" };
        auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        sticky_header:
            mode: sometimes
            show_evicted: false
)"sv);
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        CHECK(profile->stickyHeader.value().mode == vtbackend::StickyHeaderMode::Scrolled);
        CHECK_FALSE(profile->stickyHeader.value().showEvicted);
        CHECK(capture.text().contains("sometimes"));
    }

    SECTION("a profile round-trips through the writer")
    {
        auto const source = loadFromYaml(dir, "default_profile: main\n");
        auto const* base = source.findProfile("main");
        REQUIRE(base != nullptr);
        auto profile = *base;
        auto const written = contour::config::StickyHeaderConfig { .mode = vtbackend::StickyHeaderMode::Never,
                                                                   .showEvicted = false };
        profile.stickyHeader = written;
        writeSideFile(dir, "profiles/roundtrip.yml", contour::config::emitProfileYaml(profile));

        auto const reloaded = loadFromYaml(dir, "default_profile: main\n");
        auto const* roundTripped = reloaded.findProfile("roundtrip");
        REQUIRE(roundTripped != nullptr);
        CHECK(roundTripped->stickyHeader.value() == written);
    }
}

TEST_CASE("Config: the generated config spells out sticky_header and reads it back", "[config][stickyheader]")
{
    QTemporaryDir dir;
    auto const rendered = contour::config::defaultConfigString();

    // The profile key's own lines -- not the colour scheme's commented example, which names the key too.
    auto const key = rendered.find("\n        sticky_header:\n");
    REQUIRE(key != std::string::npos);
    auto const section = std::string_view { rendered }.substr(key, rendered.find("\n\n", key + 1) + 1 - key);
    CHECK(section.contains("\n            mode: scrolled\n"));
    CHECK(section.contains("\n            show_evicted: true\n"));
    // What a header needs: the prompt's own mark. An integration that marks only the command gets none.
    CHECK(section.contains("Needs prompt marks"));
    CHECK(section.contains("(OSC 133;A)"));

    auto const defaults = contour::config::Config {};
    auto const reloaded = loadFromYaml(dir, rendered);
    auto const* profile = reloaded.profile(defaults.defaultProfileName.value());
    REQUIRE(profile != nullptr);
    CHECK(profile->stickyHeader.value() == contour::config::StickyHeaderConfig {});
}

namespace
{
/// A configuration whose one profile reads the colour scheme `coverage`: a background, a foreground, and then
/// @p stickyHeader.
/// @param stickyHeader The scheme's further keys, indented as its own -- its sticky_header map, if any.
/// @return The YAML.
[[nodiscard]] std::string schemeWithStickyHeader(std::string_view stickyHeader)
{
    return std::format(R"(
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
{})",
                       stickyHeader);
}
} // namespace

TEST_CASE("Config: the sticky header's colors load and are written back", "[config][stickyheader]")
{
    QTemporaryDir dir;

    SECTION("unset by default, so both derive from the scheme")
    {
        auto const scheme = schemeOf(loadFromYaml(dir, schemeWithStickyHeader("")));
        CHECK_FALSE(scheme.stickyHeaderBackground.has_value());
        CHECK_FALSE(scheme.stickyHeaderSeparator.has_value());
    }

    SECTION("both slots load")
    {
        auto const scheme =
            schemeOf(loadFromYaml(dir,
                                  schemeWithStickyHeader("        sticky_header:\n"
                                                         "            background: '#202830'\n"
                                                         "            separator: '#506070'\n")));
        CHECK(scheme.stickyHeaderBackground == vtbackend::RGBColor(0x20, 0x28, 0x30));
        CHECK(scheme.stickyHeaderSeparator == vtbackend::RGBColor(0x50, 0x60, 0x70));
    }

    SECTION("one slot alone loads, and the other stays unset")
    {
        auto const scheme =
            schemeOf(loadFromYaml(dir,
                                  schemeWithStickyHeader("        sticky_header:\n"
                                                         "            separator: '#506070'\n")));
        CHECK_FALSE(scheme.stickyHeaderBackground.has_value());
        CHECK(scheme.stickyHeaderSeparator == vtbackend::RGBColor(0x50, 0x60, 0x70));
    }

    SECTION("the writer emits what was set, and only that")
    {
        auto config = classicConfig();
        config.colorschemes.value()["default"].stickyHeaderSeparator = vtbackend::RGBColor(0x50, 0x60, 0x70);

        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
        // The commented example stands in for a map the scheme does not set, and only for that.
        CHECK_FALSE(written.contains("# sticky_header:"));

        auto const reloaded = schemeOf(loadFromYaml(dir, written));
        CHECK(reloaded.stickyHeaderSeparator == vtbackend::RGBColor(0x50, 0x60, 0x70));
        // Writing back a derived colour would pin what the scheme deliberately left to the palette.
        CHECK_FALSE(reloaded.stickyHeaderBackground.has_value());
    }

    SECTION("a colour scheme file carries both through the side-file loader")
    {
        auto palette = vtbackend::ColorPalette {};
        palette.stickyHeaderBackground = vtbackend::RGBColor(0x20, 0x28, 0x30);
        palette.stickyHeaderSeparator = vtbackend::RGBColor(0x50, 0x60, 0x70);
        auto const written = contour::config::emitColorSchemeYaml(palette);
        REQUIRE(written.contains("\nsticky_header:\n"));
        writeSideFile(dir, "colorschemes/banded.yml", written);

        auto const reloaded = schemeOf(loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        colors: banded
)"sv));
        CHECK(reloaded.stickyHeaderBackground == vtbackend::RGBColor(0x20, 0x28, 0x30));
        CHECK(reloaded.stickyHeaderSeparator == vtbackend::RGBColor(0x50, 0x60, 0x70));
    }
}

TEST_CASE("Config: the sticky header colors the generated config quotes are what the default scheme derives",
          "[config][stickyheader]")
{
    // The commented example says so, so it is held to it here: retuning a lift in ColorPalette fails this
    // rather than leaving the generated config quoting the old colour.
    auto const generated = contour::config::defaultConfigString();
    auto const palette = vtbackend::ColorPalette {};
    CHECK(generated.contains("# sticky_header:\n"));
    CHECK(generated.contains(std::format("#     background: {}\n", palette.stickyHeaderBackgroundColor())));
    CHECK(generated.contains(std::format("#     separator: {}\n", palette.stickyHeaderSeparatorColor())));
}

// }}}

// {{{ The generated config and the reference documentation

namespace
{

/// Whether @p node has a defined child at @p path, walked one segment at a time: yaml-cpp throws on
/// indexing below a missing node, so a chained lookup could not report a missing section.
[[nodiscard]] bool hasPath(YAML::Node const& node, std::span<std::string const> path)
{
    if (path.empty())
        return node.IsDefined();
    if (!node.IsDefined() || !node.IsMap())
        return false;
    return hasPath(node[path.front()], path.subspan(1));
}

/// The entry of @p section in reference page @p doc, from its heading to the next one; empty when it has
/// none. A leaf is looked for there, as its `==term==`: a bare word such as `marks` or `pager` turns up in
/// other entries' prose, and in its own entry's YAML example besides.
[[nodiscard]] std::string_view referenceEntry(std::string_view doc, std::string_view section)
{
    auto const heading = std::format("### `{}`\n", section);
    auto const begin = doc.find(heading);
    if (begin == std::string_view::npos)
        return {};
    // Without a next heading, the length is npos less a little, and substr() stops at the page's end.
    return doc.substr(begin, doc.find("### `", begin + heading.size()) - begin);
}

} // namespace

TEST_CASE("Config: the generated config and the reference docs carry every command-block key", "[config]")
{
    // A key that `contour generate config` does not write, or the website does not document, is a key
    // nobody discovers. Each section's leaves are listed so a dropped one names itself.
    auto const text = contour::config::defaultConfigString();
    auto const generated = YAML::Load(text);
    auto const defaults = contour::config::Config {};
    auto const profile = generated["profiles"][defaults.defaultProfileName.value()];
    auto const globalDoc = contour::config::documentationGlobalConfig();
    auto const profileDoc = contour::config::documentationProfileConfig();

    using Sections = std::vector<std::pair<std::string, std::vector<std::string>>>;
    auto const globalSections = Sections {
        { "gutter",
          { "exit_status",
            "user_marks",
            "block_column_width",
            "bold_on_hover",
            "line_numbers",
            "line_number_width",
            "timestamps",
            "timestamp_format" } },
        { "command_blocks", { "max_records", "pager", "pager_format" } },
        { "folding", { "enabled", "show_markers", "auto_collapse_on_new_command", "on_jump_into_fold" } },
    };
    auto const profileSections = Sections {
        { "scrollbar", { "marks" } },
        { "sticky_header", { "mode", "show_evicted" } },
        { "notify_on_command_finish", { "when", "min_duration", "outcome", "action", "clear_on" } },
    };
    auto const checkSections = [](std::string_view scope,
                                  YAML::Node const& written,
                                  std::string_view reference,
                                  Sections const& sections) {
        for (auto const& [section, leaves]: sections)
            for (auto const& leaf: leaves)
            {
                INFO(scope << ": " << section << "." << leaf);
                CHECK(hasPath(written, std::array { section, leaf }));
                CHECK(referenceEntry(reference, section).contains(std::format("=={}==", leaf)));
            }
    };
    checkSections("global", generated, globalDoc, globalSections);
    checkSections("profile", profile, profileDoc, profileSections);

    // The colour-scheme slots are optional, so the generated scheme carries them as commented examples, as
    // it does fold_marker: present in the text, absent from the parsed tree.
    auto const schemes = text.substr(text.find("\ncolor_schemes:"));
    for (auto const* key: { "block_status", "block_status_hover", "gutter_text", "sticky_header" })
    {
        INFO("generated colour scheme: " << key);
        CHECK(schemes.contains(key));
    }

    // The scrollbar is shown on the right by default: the generated config spells the default out, and the
    // profile reference -- docs/configuration/profiles.md -- shows and states it.
    CHECK(profile["scrollbar"]["position"].as<std::string>() == "Right");
    CHECK(profileDoc.contains("position: Right"));
    CHECK(profileDoc.contains("The default is Right"));
}

// }}}

// {{{ link_detection

namespace
{
/// Loads a main profile whose `link_detection` map is @p body (empty: the key is absent).
[[nodiscard]] contour::config::LinkDetectionConfig linkDetectionOf(QTemporaryDir& dir, std::string_view body)
{
    auto const config = loadFromYaml(dir,
                                     std::format(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
{}
)",
                                                 body));
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    return profile->linkDetection.value();
}
} // namespace

TEST_CASE("Config: link_detection defaults to paths on the primary screen only", "[config][links]")
{
    QTemporaryDir dir;
    auto const value = linkDetectionOf(dir, "");
    CHECK(value == contour::config::LinkDetectionConfig {});
    CHECK(contour::config::linkDetectionPolicyOf(value) == vtbackend::LinkDetectionPolicy {});
}

TEST_CASE("Config: link_detection reads every switch", "[config][links]")
{
    QTemporaryDir dir;
    auto const value = linkDetectionOf(dir,
                                       "        link_detection:\n"
                                       "            primary_screen:\n"
                                       "                urls: false\n"
                                       "                file_paths: false\n"
                                       "            other_screens:\n"
                                       "                urls: false\n"
                                       "                file_paths: true\n");
    CHECK_FALSE(value.primaryScreen.urls);
    CHECK_FALSE(value.primaryScreen.filePaths);
    CHECK_FALSE(value.otherScreens.urls);
    CHECK(value.otherScreens.filePaths);
}

TEST_CASE("Config: a partial link_detection keeps the other defaults", "[config][links]")
{
    QTemporaryDir dir;
    auto const value = linkDetectionOf(dir,
                                       "        link_detection:\n"
                                       "            other_screens:\n"
                                       "                file_paths: true\n");
    CHECK(value.primaryScreen
          == contour::config::ScreenLinkDetectionConfig { .urls = true, .filePaths = true });
    CHECK(value.otherScreens.urls);
    CHECK(value.otherScreens.filePaths);
}

TEST_CASE("Config: link_detection round-trips through the writer", "[config][links]")
{
    auto const generated = contour::config::defaultConfigString();
    CHECK(generated.contains("link_detection:"));
    CHECK(generated.contains("other_screens:"));
    CHECK(generated.contains("file_paths: false"));

    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, generated);
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(profile->linkDetection.value() == contour::config::LinkDetectionConfig {});
}

TEST_CASE("Config: a non-default link_detection round-trips through the writer", "[config][links]")
{
    // Order-sensitive on purpose: the screens differ, and so do a screen's two switches, so a writer or a
    // reader that exchanges urls with file_paths, or primary_screen with other_screens, is caught.
    constexpr auto Chosen =
        contour::config::LinkDetectionConfig { .primaryScreen = { .urls = false, .filePaths = true },
                                               .otherScreens = { .urls = true, .filePaths = false } };
    QTemporaryDir dir;
    auto configured = loadFromYaml(dir, "default_profile: main\n");
    auto* const profile = configured.profile("main");
    REQUIRE(profile != nullptr);
    profile->linkDetection.value() = Chosen;
    REQUIRE(profile->linkDetection.value() != contour::config::LinkDetectionConfig {});

    auto const reloaded =
        loadFromYaml(dir, contour::config::createString<contour::config::YAMLConfigWriter>(configured));
    auto const* const reloadedProfile = reloaded.profile("main");
    REQUIRE(reloadedProfile != nullptr);
    CHECK(reloadedProfile->linkDetection.value() == Chosen);
}

TEST_CASE("Config: a profile inherits link_detection keys it leaves out", "[config][links]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        link_detection:
            primary_screen:
                urls: false
    alt:
        link_detection:
            other_screens:
                file_paths: true
)");
    auto const* alt = config.profile("alt");
    REQUIRE(alt != nullptr);
    auto const value = alt->linkDetection.value();
    CHECK_FALSE(value.primaryScreen.urls); // inherited from the default profile
    CHECK(value.primaryScreen.filePaths);  // the default
    CHECK(value.otherScreens.urls);        // the default
    CHECK(value.otherScreens.filePaths);   // its own
}

TEST_CASE("Config: sessionSettings carries the link detection policy", "[config][links]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir,
                                     R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        link_detection:
            primary_screen:
                urls: false
)");
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const settings =
        contour::config::sessionSettings(config, *profile, vtbackend::ColorPreference::Dark);
    CHECK(settings.linkDetection == contour::config::linkDetectionPolicyOf(profile->linkDetection.value()));
    CHECK_FALSE(settings.linkDetection.primaryScreen.urls == vtbackend::DetectLinks::Yes);
}

// }}}

// {{{ hyperlink_decoration

namespace
{
/// Loads a main profile whose `hyperlink_decoration` map is @p body and returns its decorators.
[[nodiscard]] contour::config::HyperlinkDecorationConfig hyperlinkDecorationOf(QTemporaryDir& dir,
                                                                               std::string_view body)
{
    auto const config = loadFromYaml(dir,
                                     std::format(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        hyperlink_decoration:
{}
)",
                                                 body));
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    return profile->hyperlinkDecoration.value();
}
} // namespace

TEST_CASE("Config: hyperlink_decoration accepts the spelling the documentation used",
          "[config][hyperlink-decoration]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };

    // `dotted` is what the documentation showed for years. The hover default is Underline, so the
    // value read back can only have come from the file.
    auto const decorators = hyperlinkDecorationOf(dir,
                                                  "            normal: curly-underline\n"
                                                  "            hover: dotted\n");
    CHECK(decorators.normal == vtrasterizer::Decorator::CurlyUnderline);
    CHECK(decorators.hover == vtrasterizer::Decorator::DottedUnderline);
    CHECK_FALSE(capture.contains("hyperlink_decoration"));
}

TEST_CASE("Config: every decorator name parses, whatever its case", "[config][hyperlink-decoration]")
{
    for (auto const& [token, decorator]: vtrasterizer::DecoratorNames)
    {
        QTemporaryDir dir;
        auto capture = core::log::ScopedCapture { "error" };

        auto upper = std::string { token };
        std::ranges::transform(
            upper, upper.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });

        INFO("token: " << token);
        CHECK(hyperlinkDecorationOf(dir, std::format("            hover: {}\n", token)).hover == decorator);
        CHECK(hyperlinkDecorationOf(dir, std::format("            hover: {}\n", upper)).hover == decorator);
        CHECK_FALSE(capture.contains("hyperlink_decoration"));
    }
}

TEST_CASE("Config: an unknown hyperlink decorator is reported and keeps its default",
          "[config][hyperlink-decoration]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };

    auto const decorators = hyperlinkDecorationOf(dir,
                                                  "            normal: wavy\n"
                                                  "            hover: framed\n");

    // The bad value costs only its own key: normal keeps the default, hover still loads.
    CHECK(decorators.normal == contour::config::HyperlinkDecorationConfig {}.normal);
    CHECK(decorators.hover == vtrasterizer::Decorator::Framed);

    INFO("captured error log:\n" << capture.text());
    CHECK(capture.contains("wavy"));
    CHECK(capture.contains("hyperlink_decoration.normal"));
    for (auto const& [token, decorator]: vtrasterizer::DecoratorNames)
        CHECK(capture.contains(token));
}

TEST_CASE("Config: the decorator names the documentation lists are the ones the parser accepts",
          "[config][hyperlink-decoration]")
{
    namespace doc = contour::config::documentation;
    auto const text = std::string_view { doc::HyperlinkDecorationWeb.value };

    constexpr auto Marker = "Possible values: "sv;
    auto const begin = text.find(Marker);
    REQUIRE(begin != std::string_view::npos);
    auto const start = begin + Marker.size();
    auto const line = text.substr(start, text.find('\n', start) - start);

    auto listed = std::vector<std::string_view> {};
    for (auto const word: std::views::split(line, ", "sv))
    {
        auto const name = std::string_view { word.begin(), word.end() };
        // The sentence ends in a remark ("encircle (if implemented)"): the name is the first word.
        listed.push_back(name.substr(0, name.find(' ')));
    }

    // Every listed name parses ...
    for (auto const name: listed)
    {
        INFO("documented name: " << name);
        CHECK(vtrasterizer::toDecorator(name).has_value());
    }
    // ... and every name the parser prints is listed.
    for (auto const& [token, decorator]: vtrasterizer::DecoratorNames)
    {
        INFO("table name: " << token);
        CHECK(std::ranges::find(listed, token) != listed.end());
    }

    // The example's own values must be among them.
    CHECK(text.contains("normal: dotted-underline"));
    CHECK(text.contains("hover: underline"));
}

TEST_CASE("Config: hyperlink_decoration.mode reads, writes and defaults", "[config][links]")
{
    QTemporaryDir dir;
    auto const config =
        loadFromYaml(dir, "profiles:\n  main:\n    hyperlink_decoration:\n      mode: modifier\n"sv);
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(profile->hyperlinkDecoration.value().mode == vtbackend::LinkDecorationMode::Modifier);
    CHECK(contour::config::TerminalProfile {}.hyperlinkDecoration.value().mode
          == vtbackend::LinkDecorationMode::Modifier);
    CHECK(contour::config::TerminalProfile { .defaultsEdition = contour::config::DefaultsEdition::Classic }
              .hyperlinkDecoration.value()
              .mode
          == vtbackend::LinkDecorationMode::Always);

    auto const generated = contour::config::defaultConfigString();
    CHECK(generated.contains("mode: modifier"));
    CHECK(generated.contains("normal: dotted-underline"));
}

TEST_CASE("Config: an unknown hyperlink_decoration.mode is reported and keeps the default", "[config][links]")
{
    auto const capture = core::log::ScopedCapture { "error" };
    QTemporaryDir dir;
    auto const config =
        loadFromYaml(dir, "profiles:\n  main:\n    hyperlink_decoration:\n      mode: sometimes\n"sv);
    INFO("captured error log:\n" << capture.text());
    CHECK(capture.contains("'sometimes'"));
    CHECK(capture.contains("hyperlink_decoration.mode"));
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(profile->hyperlinkDecoration.value().mode == vtbackend::LinkDecorationMode::Modifier); // Native2026
}

TEST_CASE("Config: the generated hyperlink_decoration comment lists the names the parser accepts",
          "[config][links]")
{
    namespace doc = contour::config::documentation;
    auto const text = std::string_view { doc::HyperlinkDecorationConfig.value };

    // The decorator names are the sentence after "under the pointer: ", up to its full stop, spread over
    // comment lines.
    constexpr auto Marker = "under the pointer: "sv;
    auto const begin = text.find(Marker);
    REQUIRE(begin != std::string_view::npos);
    auto const start = begin + Marker.size();
    auto const sentence = text.substr(start, text.find('.', start) - start);

    auto listed = std::vector<std::string> {};
    for (auto const line: std::views::split(sentence, '\n'))
    {
        auto words = std::string_view { line.begin(), line.end() };
        words.remove_prefix(words.starts_with("{comment} ") ? std::string_view { "{comment} " }.size() : 0);
        for (auto const word: std::views::split(words, ","sv))
        {
            auto name = std::string { word.begin(), word.end() };
            std::erase(name, ' ');
            if (!name.empty())
                listed.push_back(std::move(name));
        }
    }

    for (auto const& [token, decorator]: vtrasterizer::DecoratorNames)
    {
        INFO("table name: " << token);
        CHECK(std::ranges::find(listed, std::string { token }) != listed.end());
    }
    for (auto const& name: listed)
    {
        INFO("documented name: " << name);
        CHECK(vtrasterizer::toDecorator(name).has_value());
    }

    // Every row of the mode table has its line: "{comment}   <token> - ...".
    for (auto const& row: contour::config::configEnumValues<vtbackend::LinkDecorationMode>())
    {
        INFO("mode: " << row.token);
        CHECK(text.contains(std::format("{{comment}}   {} ", row.token)));
    }
}

TEST_CASE("Config: a profile without hint patterns shares the built-in link table", "[config][links]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, "profiles:\n  main:\n    shell: /bin/sh\n"sv);
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(contour::config::linkPatternsFor(*profile) == vtbackend::LinkPatterns::builtins());
}

TEST_CASE("Config: a broken hint pattern is reported once at load, naming it", "[config][links]")
{
    auto const capture = core::log::ScopedCapture { "error" };
    QTemporaryDir dir;
    auto const config = loadFromYaml(
        dir,
        "profiles:\n  main:\n    shell: /bin/sh\n    hint_patterns:\n      - name: bad\n        regex: '([a-'\n"sv);
    INFO("captured error log:\n" << capture.text());
    auto const& text = capture.text();
    auto const first = text.find("'bad'");
    REQUIRE(first != std::string::npos);
    CHECK(text.find("'bad'", first + 1) == std::string::npos);
    CHECK(text.contains("'main'"));

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(contour::config::linkPatternsFor(*profile)->find("bad") == nullptr);
}

TEST_CASE("Config: a profile inheriting a broken hint pattern does not report it again", "[config][links]")
{
    auto const capture = core::log::ScopedCapture { "error" };
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir,
                                     "default_profile: main\nprofiles:\n  main:\n    shell: /bin/sh\n"
                                     "    hint_patterns:\n      - name: bad\n        regex: '([a-'\n"
                                     "  other:\n    shell: /bin/sh\n"sv);
    auto const& text = capture.text();
    auto const first = text.find("'bad'");
    REQUIRE(first != std::string::npos);
    CHECK(text.find("'bad'", first + 1) == std::string::npos);
}

TEST_CASE("Config: sessionSettings hands the terminal the profile's compiled link table", "[config][links]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(
        dir,
        "profiles:\n  main:\n    shell: /bin/sh\n    hint_patterns:\n      - name: jira\n        regex: '\\b[A-Z]+-\\d+\\b'\n"sv);
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const settings =
        contour::config::sessionSettings(config, *profile, vtbackend::ColorPreference::Dark);
    REQUIRE(settings.linkPatterns != nullptr);
    CHECK(settings.linkPatterns->find("jira") != nullptr);
    CHECK(settings.linkPatterns != vtbackend::LinkPatterns::builtins());
}

// }}}

TEST_CASE("Config: the follow gesture's modifiers come from the mouse bindings", "[config][links]")
{
    auto const defaults = contour::config::Config {};
#ifdef __APPLE__
    CHECK(contour::config::followHyperlinkModifiers(defaults)
          == vtbackend::Modifiers { vtbackend::Modifier::Super }); // Command, under the Native2026 defaults
#else
    CHECK(contour::config::followHyperlinkModifiers(defaults)
          == vtbackend::Modifiers { vtbackend::Modifier::Control });
#endif
    auto const classic =
        contour::config::Config { .defaultsEdition = contour::config::DefaultsEdition::Classic };
    CHECK(contour::config::followHyperlinkModifiers(classic)
          == vtbackend::Modifiers { vtbackend::Modifier::Control });

    auto const alt = contour::test::loadConfigFromYaml(
        "input_mapping:\n  - { mods: [Alt], mouse: Left, action: FollowHyperlink }\n");
    CHECK(contour::config::followHyperlinkModifiers(alt)
          == vtbackend::Modifiers { vtbackend::Modifier::Alt });
}

TEST_CASE("Config: no mouse binding that follows links leaves no follow modifiers", "[config][links]")
{
    auto const none = contour::test::loadConfigFromYaml(
        "input_mapping:\n  - { mods: [Alt], mouse: Left, action: PasteClipboard }\n");
    CHECK(contour::config::followHyperlinkModifiers(none) == std::nullopt);
}

TEST_CASE("Config: sessionSettings carries the follow modifiers", "[config][links]")
{
    QTemporaryDir dir;
    auto const config =
        loadFromYaml(dir,
                     "profiles:\n  main:\n    shell: /bin/sh\n"
                     "input_mapping:\n  - { mods: [Alt], mouse: Left, action: FollowHyperlink }\n"sv);
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const settings =
        contour::config::sessionSettings(config, *profile, vtbackend::ColorPreference::Dark);
    CHECK(settings.linkFollowModifiers
          == std::optional { vtbackend::Modifiers { vtbackend::Modifier::Alt } });
}

TEST_CASE("Config: mode modifier without a follow binding is one diagnostic naming every such profile",
          "[config][links]")
{
    auto const capture = core::log::ScopedCapture { "error" };
    QTemporaryDir dir;
    // "other" sets no mode and inherits modifier from the default profile; "plain" says always.
    auto const config =
        loadFromYaml(dir,
                     "default_profile: main\n"
                     "profiles:\n"
                     "  main:\n    shell: /bin/sh\n    hyperlink_decoration:\n      mode: modifier\n"
                     "  other:\n    shell: /bin/sh\n"
                     "  plain:\n    shell: /bin/sh\n    hyperlink_decoration:\n      mode: always\n"
                     "input_mapping:\n  - { mods: [Alt], mouse: Left, action: PasteClipboard }\n"sv);
    INFO("captured error log:\n" << capture.text());
    auto const& text = capture.text();
    auto const first = text.find("hyperlink_decoration.mode is modifier");
    REQUIRE(first != std::string::npos);
    CHECK(text.find("hyperlink_decoration.mode is modifier", first + 1) == std::string::npos);
    CHECK(text.contains("in profiles main, other but no FollowHyperlink mouse binding exists"));
    CHECK_FALSE(text.contains("plain"));
}

TEST_CASE("Config: mode modifier with a follow binding is not reported", "[config][links]")
{
    auto const capture = core::log::ScopedCapture { "error" };
    QTemporaryDir dir;
    auto const config = loadFromYaml(
        dir,
        "profiles:\n  main:\n    shell: /bin/sh\n    hyperlink_decoration:\n      mode: modifier\n"
        "input_mapping:\n  - { mods: [Alt], mouse: Left, action: FollowHyperlink }\n"sv);
    INFO("captured error log:\n" << capture.text());
    CHECK_FALSE(capture.contains("hyperlink_decoration.mode is modifier"));
}

// {{{ file_opener

namespace
{
/// Loads a main profile whose `file_opener` entry is @p entry: its value, as it follows the key.
[[nodiscard]] contour::config::FileOpenerConfig fileOpenerOf(QTemporaryDir& dir, std::string_view entry)
{
    auto const config = loadFromYaml(dir,
                                     std::format("default_profile: main\n"
                                                 "profiles:\n"
                                                 "    main:\n"
                                                 "        shell: /bin/sh\n"
                                                 "        file_opener:{}\n",
                                                 entry));
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    return profile->fileOpener.value();
}
} // namespace

TEST_CASE("Config: file_opener reads a string or a map, and rejects an unknown placeholder",
          "[config][links]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };

    auto const asString = fileOpenerOf(dir, " 'code --goto {path}:{line}'");
    CHECK(asString.command == "code --goto {path}:{line}");
    CHECK(asString.placement == contour::actions::OutputPlacement::Detached);

    auto const asMap = fileOpenerOf(dir,
                                    "\n"
                                    "            command: 'nvim +{line} {path}'\n"
                                    "            placement: split\n");
    CHECK(asMap.command == "nvim +{line} {path}");
    CHECK(asMap.placement == contour::actions::OutputPlacement::Split);
    INFO("captured error log:\n" << capture.text());
    CHECK(capture.text().empty());

    // Reported, and kept as written: it is disabled where a file is opened, so that a save of the profile
    // writes back what the user wrote rather than nothing.
    auto const bad = fileOpenerOf(dir, " 'code {file}'");
    CHECK(bad.command == "code {file}");
    CHECK(capture.contains("file_opener 'code {file}': an unknown placeholder"));
}

TEST_CASE("Config: an invalid file_opener survives the writer and the reader as written", "[config][links]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir,
                                     "default_profile: main\n"
                                     "profiles:\n"
                                     "    main:\n"
                                     "        shell: /bin/sh\n"
                                     "        file_opener: 'code {file}'\n"sv);
    REQUIRE(config.profile("main") != nullptr);
    REQUIRE(config.profile("main")->fileOpener.value().command == "code {file}");

    QTemporaryDir again;
    auto const reloaded =
        loadFromYaml(again, contour::config::createString<contour::config::YAMLConfigWriter>(config));
    REQUIRE(reloaded.profile("main") != nullptr);
    CHECK(reloaded.profile("main")->fileOpener.value().command == "code {file}");
}

TEST_CASE("Config: file_opener's string form runs detached, whatever the profile inherited",
          "[config][links]")
{
    // A profile starts as a copy of the default one: the string form names its command alone, which runs
    // detached.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir,
                                     "default_profile: main\n"
                                     "profiles:\n"
                                     "    main:\n"
                                     "        shell: /bin/sh\n"
                                     "        file_opener:\n"
                                     "            command: 'nvim +{line} {path}'\n"
                                     "            placement: split\n"
                                     "    other:\n"
                                     "        shell: /bin/sh\n"
                                     "        file_opener: 'code --goto {path}'\n"sv);
    REQUIRE(config.profile("main") != nullptr);
    REQUIRE(config.profile("other") != nullptr);
    CHECK(config.profile("main")->fileOpener.value().placement == contour::actions::OutputPlacement::Split);
    CHECK(config.profile("other")->fileOpener.value().command == "code --goto {path}");
    CHECK(config.profile("other")->fileOpener.value().placement
          == contour::actions::OutputPlacement::Detached);
}

TEST_CASE("Config: file_opener defaults to the system's default application", "[config][links]")
{
    auto const defaults = contour::config::Config {};
    auto const* profile = defaults.profile(defaults.defaultProfileName.value());
    REQUIRE(profile != nullptr);
    CHECK(profile->fileOpener.value().command.empty());
    CHECK(profile->fileOpener.value().placement == contour::actions::OutputPlacement::Detached);
}

TEST_CASE("Config: the generated config spells out file_opener and reads it back", "[config][links]")
{
    auto const generated = contour::config::defaultConfigString();
    CHECK(generated.contains("file_opener:"));
    CHECK(generated.contains("placement: detached"));
    CHECK(generated.contains("{path}"));

    // A configured opener survives the writer and the reader.
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto config = contour::config::Config {};
    auto* profile = config.profile(config.defaultProfileName.value());
    REQUIRE(profile != nullptr);
    profile->fileOpener =
        contour::config::FileOpenerConfig { .command = "code --goto '{path}:{line}'",
                                            .placement = contour::actions::OutputPlacement::Tab };
    auto const reloaded =
        loadFromYaml(dir, contour::config::createString<contour::config::YAMLConfigWriter>(config));
    INFO("captured error log:\n" << capture.text());
    CHECK(capture.text().empty());
    auto const* back = reloaded.profile(config.defaultProfileName.value());
    REQUIRE(back != nullptr);
    CHECK(back->fileOpener.value().command == "code --goto '{path}:{line}'");
    CHECK(back->fileOpener.value().placement == contour::actions::OutputPlacement::Tab);
}

// }}}

// {{{ hint_patterns: clickable, url, name-only overrides

TEST_CASE("Config: hint_patterns take clickable and url, and a built-in's name alone", "[config][links]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir,
                                     "default_profile: main\n"
                                     "profiles:\n"
                                     "  main:\n"
                                     "    shell: /bin/sh\n"
                                     "    hint_patterns:\n"
                                     "      - name: url\n"
                                     "        clickable: false\n"
                                     "      - name: jira\n"
                                     "        regex: '\\b[A-Z]+-\\d+\\b'\n"
                                     "        clickable: true\n"
                                     "        url: 'https://jira.example.com/browse/{0}'\n"sv);
    INFO("captured error log:\n" << capture.text());
    CHECK(capture.text().empty());
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const& patterns = profile->hintPatterns.value();
    REQUIRE(patterns.size() == 2);
    CHECK(patterns[0].regex.empty());
    CHECK(patterns[0].clickable == false);
    CHECK_FALSE(patterns[0].url.has_value());
    CHECK(patterns[1].clickable == true);
    CHECK(patterns[1].url == "https://jira.example.com/browse/{0}");

    auto const specs = contour::config::linkPatternSpecs(*profile);
    REQUIRE(specs.size() == 2);
    CHECK(specs[0].regex == std::nullopt);
    CHECK(specs[0].activation == vtbackend::LinkActivation::HintOnly);
    CHECK(specs[1].activation == vtbackend::LinkActivation::Clickable);
    CHECK(specs[1].urlTemplate == "https://jira.example.com/browse/{0}");

    auto const table = contour::config::linkPatternsFor(*profile);
    REQUIRE(table->find("url") != nullptr);
    CHECK(table->find("url")->activation == vtbackend::LinkActivation::HintOnly);
    REQUIRE(table->find("jira") != nullptr);
    CHECK(table->find("jira")->activation == vtbackend::LinkActivation::Clickable);
    CHECK(table->find("jira")->urlTemplate.has_value());
}

TEST_CASE("Config: a name-only hint pattern that names no built-in is skipped and reported",
          "[config][links]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir,
                                     "default_profile: main\n"
                                     "profiles:\n"
                                     "  main:\n"
                                     "    shell: /bin/sh\n"
                                     "    hint_patterns:\n"
                                     "      - name: nobody\n"
                                     "        clickable: true\n"sv);
    REQUIRE(config.profile("main") != nullptr);
    CHECK(config.profile("main")->hintPatterns.value().empty());
    CHECK(capture.contains("nobody"));
    CHECK(capture.contains("filepath")); // it names the built-ins a name-only entry may override
}

TEST_CASE("Config: a hint pattern's url naming a group its regex lacks is reported at load",
          "[config][links]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir,
                                     "default_profile: main\n"
                                     "profiles:\n"
                                     "  main:\n"
                                     "    shell: /bin/sh\n"
                                     "    hint_patterns:\n"
                                     "      - name: jira\n"
                                     "        regex: '\\b[A-Z]+-\\d+\\b'\n"
                                     "        clickable: true\n"
                                     "        url: 'https://j/{1}'\n"sv);
    REQUIRE(config.profile("main") != nullptr);
    CHECK(capture.contains("jira"));
    CHECK(capture.contains("capture group"));
}

TEST_CASE("Config: hint_patterns' new keys survive a write and a read", "[config][links]")
{
    QTemporaryDir dir;
    auto config = contour::config::Config {};
    auto* const profile = config.profile(config.defaultProfileName.value());
    REQUIRE(profile != nullptr);
    profile->hintPatterns.value() = {
        contour::config::HintPatternConfig { .name = "url", .regex = {}, .clickable = false, .url = {} },
        contour::config::HintPatternConfig {
            .name = "jira", .regex = "[A-Z]+-[0-9]+", .clickable = true, .url = "https://j/{0}" },
    };
    auto const reloaded =
        loadFromYaml(dir, contour::config::createString<contour::config::YAMLConfigWriter>(config));
    auto const* back = reloaded.profile(config.defaultProfileName.value());
    REQUIRE(back != nullptr);
    CHECK(back->hintPatterns.value() == profile->hintPatterns.value());
}

TEST_CASE("Config: the generated config documents clickable patterns", "[config][links]")
{
    auto const generated = contour::config::defaultConfigString();
    CHECK(generated.contains("clickable:"));
    CHECK(generated.contains("url:"));
    CHECK(generated.contains("group 1"));
}

TEST_CASE("Config: a malformed key drops its hint pattern alone, and says which", "[config][links]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir,
                                     "default_profile: main\n"
                                     "profiles:\n"
                                     "  main:\n"
                                     "    shell: /bin/sh\n"
                                     "    hint_patterns:\n"
                                     "      - name: jira\n"
                                     "        regex: '[A-Z]+-[0-9]+'\n"
                                     "        clickable: maybe\n"
                                     "      - name: ticket\n"
                                     "        regex: 'T-[0-9]+'\n"
                                     "        url: [not, a, string]\n"
                                     "      - name: uuid\n"
                                     "        regex: '[0-9a-f]{8}'\n"sv);
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const& patterns = profile->hintPatterns.value();
    REQUIRE(patterns.size() == 1);
    CHECK(patterns[0].name == "uuid");
    CHECK(capture.contains("jira"));
    CHECK(capture.contains("ticket"));
}

// }}}

TEST_CASE("Config: motionAdjusted zeroes the motion durations under Reduce Motion and nothing else",
          "[config][motion]")
{
    using namespace std::chrono_literals;
    using contour::config::MotionPreference;

    auto settings = vtbackend::Settings {};
    settings.screenTransitionDuration = 150ms;
    settings.cursorMotionAnimationDuration = 80ms;
    settings.smoothScrolling = true;

    auto const reduced = contour::config::motionAdjusted(settings, MotionPreference::Reduced);
    CHECK(reduced.screenTransitionDuration == 0ms);
    CHECK(reduced.cursorMotionAnimationDuration == 0ms);
    CHECK(reduced.smoothScrolling);

    auto const full = contour::config::motionAdjusted(settings, MotionPreference::Full);
    for (auto const setting: contour::config::MotionSettings)
        CHECK(full.*setting == settings.*setting);
    CHECK(full.smoothScrolling);
}
