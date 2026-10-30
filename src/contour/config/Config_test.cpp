// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the YAML configuration loader (Config.cpp) — the largest previously untested
// surface of the GUI layer. Each case writes an inline YAML document to a temp file, loads it
// through the production loadConfigFromFile(), and asserts the parsed ConfigEntry values, so a
// renamed/retyped YAML key or a broken loadFromEntry overload fails here instead of silently
// falling back to defaults at runtime.
//
// Config_test.cpp holds the loader basics: global scalars, profile knobs, layouts, hosts, colour
// schemes and the scrollbar. The remaining areas live in ConfigExtensions_test.cpp,
// ConfigSideFiles_test.cpp and ConfigBlocksAndLinks_test.cpp, split so that no single translation unit
// compiles the whole suite.

#include <contour/config/Actions.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/ConfigReader.hpp>
#include <contour/config/ConfigTestSupport.hpp>
#include <contour/config/GpuSelector.hpp>
#include <contour/test/CoreFixtures.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/input/InputGenerator.hpp>
#include <vtbackend/screen/GutterSettings.hpp>
#include <vtbackend/shell/ScrollbarMarks.hpp>

#include <text_shaper/Font.hpp>

#include <core/log/LogSink.hpp>

#include <QtCore/QTemporaryDir>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace contour::test::configSupport;

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

TEST_CASE("Config: the untouched default scheme pairs with the edition's dark and light schemes",
          "[config][colors]")
{
    using contour::config::DefaultsEdition;
    using contour::config::DualColorConfig;
    using contour::config::SimpleColorConfig;
    QTemporaryDir dir;

    // The profile's colors after loading a file of edition @p edition whose profile says @p colorsLine.
    auto const colorsIn =
        [&](std::string_view edition, std::string_view colorsLine, std::string_view extra = {}) {
            auto const config = contour::config::loadConfigFromFile(writeConfig(dir,
                                                                                std::format(R"(
defaults_edition: {}
default_profile: main
{}
profiles:
    main:
        shell: /bin/sh
        {}
)",
                                                                                            edition,
                                                                                            extra,
                                                                                            colorsLine)));
            auto const* profile = config.profile("main");
            REQUIRE(profile != nullptr);
            return profile->colors.value();
        };

    SECTION("native2026: `colors: default` and an absent key are the contour pair")
    {
        for (auto const* line: { "colors: default", "" })
        {
            INFO("line: '" << line << "'");
            auto const colors = colorsIn("native2026", line);
            auto const* dual = std::get_if<DualColorConfig>(&colors);
            REQUIRE(dual != nullptr);
            CHECK(dual->colorSchemeDark == "contour-dark");
            CHECK(dual->colorSchemeLight == "contour-light");
            CHECK(dual->darkMode.defaultBackground != dual->lightMode.defaultBackground);
        }
    }

    SECTION("native2026: another named scheme stays fixed")
    {
        auto const colors = colorsIn("native2026", "colors: solarized-dark");
        auto const* simple = std::get_if<SimpleColorConfig>(&colors);
        REQUIRE(simple != nullptr);
        CHECK(simple->colorScheme == "solarized-dark");
    }

    SECTION("native2026: a color_schemes entry named default is a customization and stays fixed")
    {
        auto const colors =
            colorsIn("native2026",
                     "colors: default",
                     "color_schemes:\n  default:\n    default:\n      background: '#123456'\n");
        auto const* simple = std::get_if<SimpleColorConfig>(&colors);
        REQUIRE(simple != nullptr);
        CHECK(simple->colors.defaultBackground == vtbackend::RGBColor { 0x123456 });
    }

    SECTION("native2026: a generated file that spells the stock scheme out is still untouched")
    {
        // As an older `contour config` wrote it: the normal colors spelled out, which makes the loader derive
        // the dim ones, while the built-in scheme carries its own.
        auto const colors = colorsIn("native2026",
                                     "colors: default",
                                     "color_schemes:\n"
                                     "  default:\n"
                                     "    default:\n"
                                     "      background: '#1A1716'\n"
                                     "      foreground: '#D0D0D0'\n"
                                     "    normal:\n"
                                     "      black:   '#000000'\n"
                                     "      red:     '#C63939'\n"
                                     "      green:   '#00A000'\n"
                                     "      yellow:  '#A0A000'\n"
                                     "      blue:    '#4D79FF'\n"
                                     "      magenta: '#FF66FF'\n"
                                     "      cyan:    '#00A0A0'\n"
                                     "      white:   '#C0C0C0'\n");
        CHECK(std::holds_alternative<DualColorConfig>(colors));
    }

    SECTION("native2026: a recolored ANSI color in the default scheme is a customization")
    {
        auto const colors = colorsIn("native2026",
                                     "colors: default",
                                     "color_schemes:\n  default:\n    normal:\n      red: '#123456'\n");
        CHECK(std::holds_alternative<SimpleColorConfig>(colors));
    }

    SECTION("classic: `colors: default` stays the one fixed scheme")
    {
        auto const colors = colorsIn("classic", "colors: default");
        CHECK(std::holds_alternative<SimpleColorConfig>(colors));
    }
}

TEST_CASE("Config: scrollbar.marks loads a list of mark sources", "[config][scrollbar-marks]")
{
    using vtbackend::ScrollbarMarkSource;
    using vtbackend::ScrollbarMarkSources;
    QTemporaryDir dir;

    // The profile's scrollbar.marks after loading a scrollbar block whose marks line is @p marksLine, in a
    // file headed by @p header.
    auto const marksAfterIn = [&](std::string_view header, std::string_view marksLine) {
        // The path-taking loader, which picks the edition from the file the way startup does.
        auto const config = contour::config::loadConfigFromFile(writeConfig(dir,
                                                                            std::format(R"(
{}
default_profile: main
profiles:
    main:
        shell: /bin/sh
        scrollbar:
            position: Right
            {}
)",
                                                                                        header,
                                                                                        marksLine)));
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        return profile->scrollbar.value().marks;
    };
    auto const marksAfter = [&](std::string_view marksLine) {
        return marksAfterIn("", marksLine);
    };

    SECTION("absent: the edition's default")
    {
        CHECK(marksAfterIn("defaults_edition: classic", "")
              == ScrollbarMarkSources { ScrollbarMarkSource::Failures,
                                        ScrollbarMarkSource::Commands,
                                        ScrollbarMarkSource::Running,
                                        ScrollbarMarkSource::UserMarks });
        CHECK(marksAfter("") == marksAfterIn("defaults_edition: classic", ""));
        CHECK(marksAfterIn("defaults_edition: native2026", "")
              == ScrollbarMarkSources { ScrollbarMarkSource::Failures,
                                        ScrollbarMarkSource::Running,
                                        ScrollbarMarkSource::SearchMatches });
        for (auto const edition:
             { contour::config::DefaultsEdition::Classic, contour::config::DefaultsEdition::Native2026 })
            CHECK(contour::config::TerminalProfile { .defaultsEdition = edition }.scrollbar.value().marks
                  == contour::config::editionDefaults(edition).scrollBarMarks);
    }

    SECTION("a list names exactly the sources it lists, in any case")
    {
        CHECK(marksAfter("marks: [failures, user_marks]")
              == ScrollbarMarkSources { ScrollbarMarkSource::Failures, ScrollbarMarkSource::UserMarks });
        CHECK(marksAfter("marks: [running]") == ScrollbarMarkSources { ScrollbarMarkSource::Running });
        CHECK(marksAfter("marks: [Search_Matches]")
              == ScrollbarMarkSources { ScrollbarMarkSource::SearchMatches });
    }

    SECTION("commands and running are two sources: leaving running out keeps it off, through a save")
    {
        CHECK(marksAfter("marks: [Commands]") == ScrollbarMarkSources { ScrollbarMarkSource::Commands });
        auto const kept = ScrollbarMarkSources { ScrollbarMarkSource::Failures,
                                                 ScrollbarMarkSource::Commands,
                                                 ScrollbarMarkSource::UserMarks };
        CHECK(marksAfter(std::format("marks: {}", contour::config::YAMLConfigWriter::format(kept))) == kept);
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
        CHECK(marksAfter("marks: failures") == marksAfter(""));
        INFO("captured error log:\n" << capture.text());
        CHECK(capture.contains("profiles.main.scrollbar.marks"));
    }
}

TEST_CASE("Config: scrollbar.marks round-trips through the writer", "[config][scrollbar-marks]")
{
    using vtbackend::ScrollbarMarkSource;
    using vtbackend::ScrollbarMarkSources;

    // `contour generate config` spells the default out, so a user editing it sees every token.
    CHECK(contour::config::defaultConfigString().contains("marks: [failures, running, search_matches]"));

    auto const cases = std::array {
        ScrollbarMarkSources {},
        ScrollbarMarkSources { ScrollbarMarkSource::Failures },
        ScrollbarMarkSources { ScrollbarMarkSource::Commands, ScrollbarMarkSource::Running },
        ScrollbarMarkSources { ScrollbarMarkSource::SearchMatches, ScrollbarMarkSource::UserMarks },
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

TEST_CASE("Config: a generated file documents hosts: in comments only", "[config][hosts]")
{
    auto const generated = contour::config::defaultConfigString();
    // The documentation and its example, every line commented out ...
    CHECK(generated.contains("# Saved remote hosts that \"Attach to Host\" lists"));
    CHECK(generated.contains("# hosts:\n"));
    CHECK(generated.contains("#         url: \"ssh://me@homepc\"\n"));
    // ... so the file names no host, and loads back with none.
    CHECK_FALSE(YAML::Load(generated)["hosts"]);
    QTemporaryDir dir;
    CHECK(loadFromYaml(dir, generated).hosts.value().empty());
}

TEST_CASE("Config: a scheme's hint_label_typed loads, stays empty where unset, and round-trips", "[config]")
{
    QTemporaryDir dir;
    constexpr auto WithTyped = R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        colors: typed
color_schemes:
    typed:
        default:
            background: '#102030'
            foreground: '#D0D0D0'
        hint_label_typed:
            foreground: '#D9DCE3'
            foreground_alpha: 1.0
            background: '#3A4A6B'
            background_alpha: 1.0
)"sv;
    auto const scheme = schemeOf(loadFromYaml(dir, WithTyped));
    REQUIRE(scheme.hintLabelTyped.has_value());
    CHECK(scheme.hintLabelTyped->foreground
          == vtbackend::CellRGBColor { vtbackend::RGBColor(0xD9, 0xDC, 0xE3) });
    CHECK(scheme.hintLabelTyped->background
          == vtbackend::CellRGBColor { vtbackend::RGBColor(0x3A, 0x4A, 0x6B) });

    SECTION("unset stays empty, and is not written")
    {
        auto config = classicConfig();
        REQUIRE_FALSE(config.colorschemes.value()["default"].hintLabelTyped.has_value());
        CHECK_FALSE(contour::config::createString<contour::config::YAMLConfigWriter>(config).contains(
            "hint_label_typed"));
    }

    SECTION("the writer round-trips it")
    {
        auto config = classicConfig();
        config.colorschemes.value()["default"].hintLabelTyped = scheme.hintLabelTyped;
        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
        REQUIRE(written.contains("hint_label_typed:"));
        auto const reloaded = schemeOf(loadFromYaml(dir, written));
        REQUIRE(reloaded.hintLabelTyped.has_value());
        CHECK(*reloaded.hintLabelTyped == *scheme.hintLabelTyped);
    }
}

TEST_CASE("Config: scrollbar.mode reads system, overlay and always, and keeps the default on anything else",
          "[config]")
{
    using contour::config::ScrollBarModePreference;

    // The profile's scrollbar.mode after loading a contour.yml that opens with @p header and whose scrollbar
    // block carries @p modeLine. Read the way the application reads it, so the file's edition applies.
    auto const modeAfter = [](std::string_view header, std::string_view modeLine) {
        QTemporaryDir dir;
        auto const config = contour::config::loadConfigFromFile(writeConfig(dir,
                                                                            std::format(R"({}
default_profile: main
profiles:
    main:
        shell: /bin/sh
        scrollbar:
            position: Right
            {}
)",
                                                                                        header,
                                                                                        modeLine)));
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        return profile->scrollbar.value().mode;
    };

    SECTION("every token loads, in any case")
    {
        for (auto const& info: contour::config::configEnumValues<ScrollBarModePreference>())
        {
            INFO("token " << info.token);
            CHECK(modeAfter("", std::format("mode: {}", info.token)) == info.value);
            CHECK(modeAfter("defaults_edition: native2026", std::format("mode: {}", info.token))
                  == info.value);
        }
        CHECK(modeAfter("", "mode: Overlay") == ScrollBarModePreference::Overlay);
    }

    SECTION("an unknown word keeps the edition's default, and is reported by its key path")
    {
        auto const editions = std::array {
            std::pair { "", ScrollBarModePreference::Always },
            std::pair { "defaults_edition: native2026", ScrollBarModePreference::System },
        };
        for (auto const& [header, expected]: editions)
        {
            auto capture = core::log::ScopedCapture { "error" };
            INFO("header '" << header << "'");
            CHECK(modeAfter(header, "mode: sideways") == expected);
            CHECK(modeAfter(header, "") == expected);
            INFO("captured error log:\n" << capture.text());
            CHECK(capture.contains("sideways"));
            CHECK(capture.contains("profiles.main.scrollbar.mode"));
        }
    }

    SECTION("the generated config spells the mode out, and every mode round-trips through the writer")
    {
        auto const generated = YAML::Load(contour::config::defaultConfigString());
        auto const defaults = contour::config::Config {};
        auto const mode = generated["profiles"][defaults.defaultProfileName.value()]["scrollbar"]["mode"];
        REQUIRE(mode.IsScalar());
        CHECK(contour::config::configEnumFromToken<ScrollBarModePreference>(mode.as<std::string>())
              == defaults.profile().scrollbar.value().mode);

        for (auto const& info: contour::config::configEnumValues<ScrollBarModePreference>())
        {
            INFO("mode " << info.token);
            QTemporaryDir dir;
            auto const source = loadFromYaml(dir, "default_profile: main\n");
            auto profile = *source.findProfile("main");
            profile.scrollbar.value().mode = info.value;
            writeSideFile(dir, "profiles/roundtrip.yml", contour::config::emitProfileYaml(profile));

            auto const reloaded = loadFromYaml(dir, "default_profile: main\n");
            auto const* roundTripped = reloaded.findProfile("roundtrip");
            REQUIRE(roundTripped != nullptr);
            CHECK(roundTripped->scrollbar.value().mode == info.value);
        }
    }
}

TEST_CASE("Config: option_as_alt names the Option keys that act as Alt", "[config][optionasalt]")
{
    using contour::config::OptionAsAlt;

    // The profile's option_as_alt after loading a contour.yml that opens with @p header and whose profile
    // carries @p line. Read the way the application reads it, so the file's edition applies.
    auto const optionAfter = [](std::string_view header, std::string_view line) {
        QTemporaryDir dir;
        auto const config = contour::config::loadConfigFromFile(writeConfig(dir,
                                                                            std::format(R"({}
default_profile: main
profiles:
    main:
        shell: /bin/sh
        {}
)",
                                                                                        header,
                                                                                        line)));
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        return profile->optionAsAlt.value();
    };

    SECTION("a token loads, and so does the boolean it replaces")
    {
        CHECK(optionAfter("", "option_as_alt: true") == OptionAsAlt::Both);
        CHECK(optionAfter("", "option_as_alt: false") == OptionAsAlt::Off);
        CHECK(optionAfter("", "option_as_alt: left") == OptionAsAlt::Left);
        CHECK(optionAfter("", "option_as_alt: Right") == OptionAsAlt::Right);
        CHECK(optionAfter("", "option_as_alt: both") == OptionAsAlt::Both);
        CHECK(optionAfter("", "option_as_alt: off") == OptionAsAlt::Off);
    }

    SECTION("an unknown word keeps the default, and is reported by its key path")
    {
        auto capture = core::log::ScopedCapture { "error" };
        CHECK(optionAfter("", "option_as_alt: sideways") == OptionAsAlt::Off);
        INFO("captured error log:\n" << capture.text());
        CHECK(capture.contains("sideways"));
        CHECK(capture.contains("profiles.main.option_as_alt"));
        CHECK(capture.contains("off, left, right, both"));
    }

    SECTION("a profile without the key is Off in every edition")
    {
        CHECK(optionAfter("", "") == OptionAsAlt::Off);
        CHECK(optionAfter("defaults_edition: native2026", "") == OptionAsAlt::Off);
        CHECK(optionAfter("defaults_edition: classic", "") == OptionAsAlt::Off);
    }

    SECTION("every setting round-trips through the writer, as its token")
    {
        for (auto const& info: contour::config::configEnumValues<OptionAsAlt>())
        {
            INFO("setting " << info.token);
            QTemporaryDir dir;
            auto const source = loadFromYaml(dir, "default_profile: main\n");
            auto profile = *source.findProfile("main");
            profile.optionAsAlt.value() = info.value;
            auto const written = contour::config::emitProfileYaml(profile);
            CHECK(written.contains(std::format("option_as_alt: {}\n", info.token)));
            writeSideFile(dir, "profiles/roundtrip.yml", written);

            auto const reloaded = loadFromYaml(dir, "default_profile: main\n");
            auto const* roundTripped = reloaded.findProfile("roundtrip");
            REQUIRE(roundTripped != nullptr);
            CHECK(roundTripped->optionAsAlt.value() == info.value);
        }
    }

    SECTION("the generated config documents the setting, and spells the default out")
    {
        auto const generated = contour::config::defaultConfigString();
        CHECK(generated.contains("option_as_alt: off\n"));
        CHECK(generated.contains("Which Option keys act as Alt (Meta) on macOS: off, left, right or both."));
        auto const parsed = YAML::Load(generated);
        auto const defaults = contour::config::Config {};
        auto const written = parsed["profiles"][defaults.defaultProfileName.value()]["option_as_alt"];
        REQUIRE(written.IsScalar());
        CHECK(contour::config::configEnumFromToken<OptionAsAlt>(written.as<std::string>())
              == OptionAsAlt::Off);
    }
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
