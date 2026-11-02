// SPDX-License-Identifier: Apache-2.0
//
// Config loader tests, continued: the defaults edition, the GUI-managed side files, the input_mapping
// vocabulary, history limits, the gutter and session resolution.

#include <contour/config/Actions.hpp>
#include <contour/config/BindingCollisions.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/ConfigReader.hpp>
#include <contour/config/ConfigTestSupport.hpp>
#include <contour/config/GpuSelector.hpp>
#include <contour/config/GuiConfigStore.hpp>
#include <contour/config/ModifierNames.hpp>
#include <contour/test/CoreFixtures.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/core/CommandBlockOutcome.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/input/InputGenerator.hpp>
#include <vtbackend/screen/GutterSettings.hpp>
#include <vtbackend/shell/ScrollbarMarks.hpp>

#include <vtpty/test/NonAsciiSample.hpp>

#include <core/Utf8Path.hpp>
#include <core/log/LogSink.hpp>
#include <core/platform/NativeFileSystem.hpp>
#include <core/testing/EnvHelper.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <QtCore/QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

using namespace contour::test::configSupport;

namespace
{

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

/// The colour @p palette's scheme set for @p outcome's blocks; unset when it set none.
[[nodiscard]] std::optional<vtbackend::RGBColor> blockStatusIn(vtbackend::ColorPalette const& palette,
                                                               vtbackend::CommandBlockOutcome outcome)
{
    return palette.blockStatus[static_cast<size_t>(outcome)];
}

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
    auto const& fallbacks =
        contour::config::builtinFallbackMouseMappings(contour::config::BuildKeyboardPlatformKind);

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

    auto const fileSystem = core::platform::NativeFileSystem {};
    auto store = contour::config::FileGuiConfigStore(fileSystem, configDir);
    REQUIRE(store.write(contour::config::profileFile("saved"), contour::config::emitProfileYaml(profile))
                .has_value());
    REQUIRE(store
                .write(contour::config::guiSettingsFile(),
                       contour::config::emitGuiSettingsYaml(contour::config::GuiManagedSettings {
                           .defaultProfile = "saved", .globalOverrides = {} }))
                .has_value());

    auto const afterSave = loadFromYaml(dir, "default_profile: main\n");
    auto const* saved = afterSave.findProfile("saved");
    REQUIRE(saved != nullptr);
    CHECK(saved->showTitleBar.value() == false);
    CHECK(afterSave.defaultProfileName.value() == "saved");
    CHECK(afterSave.profileOrigins.at("saved") == contour::config::SettingsOrigin::SideFile);

    REQUIRE(store.remove(contour::config::profileFile("saved")).has_value());
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
    auto const fileSystem = core::platform::NativeFileSystem {};
    auto store = contour::config::FileGuiConfigStore(fileSystem, expected.parent_path());
    REQUIRE(
        store
            .write(contour::config::profileFile(name),
                   contour::config::emitProfileYaml(*loaded.findProfile(loaded.defaultProfileName.value())))
            .has_value());
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
    CHECK(contour::config::gutterSettingsFor(config, vtbackend::BlockColumnForm::Glyphs).hoverStroke
          == vtbackend::FoldHoverStroke::Unchanged);
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
    CHECK(contour::config::gutterSettingsFor(config, vtbackend::BlockColumnForm::Glyphs).hoverStroke
          == vtbackend::FoldHoverStroke::Bolder);
    CHECK(gutter.lineNumbers == vtbackend::LineNumberMode::Off);
    CHECK(gutter.lineNumberWidth == 6);
    CHECK_FALSE(gutter.timestamps);
    CHECK(gutter.timestampFormat == "%H:%M:%S");

    // Exit status and user marks share the fold column, which takes two cells: its controls and marks are
    // drawn across both, at twice the size, and a click anywhere in it lands.
    CHECK(vtbackend::gutterLayoutFor(
              contour::config::gutterSettingsFor(config, vtbackend::BlockColumnForm::Glyphs))
              .totalColumns
          == 2);
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
        CHECK(contour::config::gutterSettingsFor(config, vtbackend::BlockColumnForm::Glyphs).blockColumnWidth
              == testCase.expected);
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

    auto const settings = contour::config::gutterSettingsFor(config, vtbackend::BlockColumnForm::Glyphs);
    CHECK(settings.foldMarkers); // folding.enabled and folding.show_markers, both on by default
    CHECK(settings.exitStatus);
    CHECK(settings.userMarks);
    CHECK(settings.lineNumbers == vtbackend::LineNumberMode::Relative);
    CHECK(settings.lineNumberWidth == 4);
    CHECK(settings.timestamps);
    CHECK(settings.timestampFormat == "%H:%M");

    config.folding.value().enabled = false;
    CHECK_FALSE(contour::config::gutterSettingsFor(config, vtbackend::BlockColumnForm::Glyphs).foldMarkers);
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
    CHECK(settings.gutter == contour::config::gutterSettingsFor(config, vtbackend::BlockColumnForm::Glyphs));
    CHECK(settings.gutter.lineNumbers == vtbackend::LineNumberMode::Absolute);
    CHECK_FALSE(settings.gutter.foldMarkers);
    CHECK(settings.gutter.exitStatus); // the block column outlives the fold markers

    // A presenting session draws the block column in its chrome's form; the daemon's hosted half in glyphs.
    for (auto const form: { vtbackend::BlockColumnForm::Glyphs, vtbackend::BlockColumnForm::Rail })
    {
        CAPTURE(static_cast<int>(form));
        auto const session =
            contour::config::sessionSettings(config, *profile, vtbackend::ColorPreference::Dark, form);
        CHECK(session.gutter == contour::config::gutterSettingsFor(config, form));
    }
    CHECK(
        contour::config::resolvedSessionConfig(config, *profile, "main", vtbackend::ColorPreference::Dark, {})
            .settings.gutter.blockColumnForm
        == vtbackend::BlockColumnForm::Glyphs);
}

// ---- Primary -----------------------------------------------------------------------------------

namespace
{
using contour::config::KeyboardPlatformKind;
using vtbackend::Modifier;
using vtbackend::Modifiers;

constexpr auto PlatformKinds =
    std::array { KeyboardPlatformKind::Xkb, KeyboardPlatformKind::Win32, KeyboardPlatformKind::Mac };

/// The `input_mapping:` section a document with one row stating @p mods and nothing else.
[[nodiscard]] std::string oneRowConfig(std::string_view mods)
{
    return std::format(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - {{ mods: [{}], key: 'T', action: CreateNewTab }}
)",
                       mods);
}
} // namespace

TEST_CASE("Primary resolves per platform", "[config][primary]")
{
    using contour::config::primaryModifiers;
    CHECK(primaryModifiers(KeyboardPlatformKind::Mac) == Modifiers { Modifier::Super });
    CHECK(primaryModifiers(KeyboardPlatformKind::Xkb) == Modifiers { Modifier::Control, Modifier::Shift });
    CHECK(primaryModifiers(KeyboardPlatformKind::Win32) == Modifiers { Modifier::Control, Modifier::Shift });
}

TEST_CASE("mods: [Primary, Alt] parses to Primary's set plus Alt", "[config][primary]")
{
    auto dir = QTemporaryDir {};
    REQUIRE(dir.isValid());

    for (auto const kind: PlatformKinds)
    {
        CAPTURE(static_cast<int>(kind));
        auto const config = loadFromYaml(dir, oneRowConfig("Primary, Alt"), kind);
        REQUIRE(config.keyboardPlatformKind == kind);
        REQUIRE(config.inputMappings.value().charMappings.size() == 1);
        CHECK(config.inputMappings.value().charMappings.front().modifiers
              == contour::config::primaryModifiers(kind).with(Modifier::Alt));
    }

    SECTION("the spelling is case-insensitive, like every other")
    {
        auto const config = loadFromYaml(dir, oneRowConfig("primary"), KeyboardPlatformKind::Mac);
        REQUIRE(config.inputMappings.value().charMappings.size() == 1);
        CHECK(config.inputMappings.value().charMappings.front().modifiers == Modifiers { Modifier::Super });
    }
}

TEST_CASE("the writer spells a row containing the Primary set as Primary", "[config][primary]")
{
    using contour::config::canonicalModifierSpellings;
    using Spellings = std::vector<std::string_view>;

    CHECK(canonicalModifierSpellings(Modifiers { Modifier::Control, Modifier::Shift },
                                     KeyboardPlatformKind::Xkb)
          == Spellings { "Primary" });
    CHECK(canonicalModifierSpellings(Modifiers { Modifier::Control }, KeyboardPlatformKind::Xkb)
          == Spellings { "Control" });
    CHECK(canonicalModifierSpellings(Modifiers { Modifier::Control, Modifier::Shift, Modifier::Alt },
                                     KeyboardPlatformKind::Win32)
          == Spellings { "Primary", "Alt" });
    CHECK(
        canonicalModifierSpellings(Modifiers { Modifier::Super, Modifier::Shift }, KeyboardPlatformKind::Mac)
        == Spellings { "Primary", "Shift" });
    // The same chord is not Primary on the other platform: Super is just Super on the desktop.
    CHECK(canonicalModifierSpellings(Modifiers { Modifier::Super }, KeyboardPlatformKind::Xkb)
          == Spellings { "Super" });
    CHECK(canonicalModifierSpellings(Modifiers {}, KeyboardPlatformKind::Mac).empty());

    SECTION("a written configuration says Primary where the host's set is")
    {
        auto config = contour::config::Config { .keyboardPlatformKind = KeyboardPlatformKind::Xkb };
        config.inputMappings.value() = {};
        config.inputMappings.value().charMappings.push_back(
            contour::config::CharInputMapping { .modes = {},
                                                .modifiers = Modifiers { Modifier::Control, Modifier::Shift },
                                                .input = U'T',
                                                .binding = { contour::actions::CreateNewTab {} } });
        CHECK(contour::config::createString<contour::config::YAMLConfigWriter>(config).contains(
            "mods: [Primary]"));
    }
}

TEST_CASE("a written config round-trips on its own platform", "[config][primary]")
{
    auto dir = QTemporaryDir {};
    REQUIRE(dir.isValid());

    for (auto const kind: PlatformKinds)
    {
        CAPTURE(static_cast<int>(kind));
        auto original = contour::config::Config { .keyboardPlatformKind = kind };
        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(original);
        auto const reloaded = loadFromYaml(dir, written, kind);

        // The chords are what is compared: a row's action list is written as its first action only.
        auto const chords = [](auto const& rows) {
            return std::ranges::to<std::vector>(
                rows | std::views::transform([](auto const& row) {
                    return std::tuple { row.input, row.modifiers.value(), row.modes, row.clickCount };
                }));
        };
        CHECK(chords(reloaded.inputMappings.value().keyMappings)
              == chords(original.inputMappings.value().keyMappings));
        CHECK(chords(reloaded.inputMappings.value().charMappings)
              == chords(original.inputMappings.value().charMappings));
        CHECK(chords(reloaded.inputMappings.value().mouseMappings)
              == chords(original.inputMappings.value().mouseMappings));
    }
}

TEST_CASE("Primary+Shift and Primary collide on the desktop", "[config][primary]")
{
    auto dir = QTemporaryDir {};
    REQUIRE(dir.isValid());

    // Two rows for one chord: the loader folds them into ONE binding that runs both actions (it always has,
    // for any two rows spelling the same chord), so a collision shows as a merged row rather than as two
    // rows findBindingCollisions() could pair up.
    constexpr auto Document = R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Primary, Shift], key: 'P', action: CreateNewTab }
    - { mods: [Primary], key: 'P', action: OpenCommandPalette }
)"sv;

    SECTION("on the desktop both spell Ctrl+Shift")
    {
        auto const xkb = loadFromYaml(dir, Document, KeyboardPlatformKind::Xkb);
        REQUIRE(xkb.inputMappings.value().charMappings.size() == 1);
        CHECK(xkb.inputMappings.value().charMappings.front().binding.size() == 2);
        CHECK(contour::config::findBindingCollisions(xkb.inputMappings.value()).empty());
    }

    SECTION("on macOS they are two chords, Command+Shift and Command")
    {
        auto const mac = loadFromYaml(dir, Document, KeyboardPlatformKind::Mac);
        REQUIRE(mac.inputMappings.value().charMappings.size() == 2);
        CHECK(mac.inputMappings.value().charMappings[0].modifiers
              == Modifiers { Modifier::Super, Modifier::Shift });
        CHECK(mac.inputMappings.value().charMappings[1].modifiers == Modifiers { Modifier::Super });
        CHECK(contour::config::findBindingCollisions(mac.inputMappings.value()).empty());
    }
}

TEST_CASE("rows written differently that resolve to one chord warn when they fold", "[config][primary]")
{
    auto dir = QTemporaryDir {};
    REQUIRE(dir.isValid());

    auto const document = [](std::string_view first, std::string_view second) {
        return std::format(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - {{ mods: [{}], key: 'P', action: CreateNewTab }}
    - {{ mods: [{}], key: 'P', action: OpenCommandPalette }}
)",
                           first,
                           second);
    };

    SECTION("on the desktop Primary+Shift and Primary are one chord: the pair is named")
    {
        auto capture = core::log::ScopedCapture { "error" };
        (void) loadFromYaml(dir, document("Primary, Shift", "Primary"), KeyboardPlatformKind::Xkb);
        CHECK(capture.text().contains("resolve to the same chord"));
        CHECK(capture.text().contains("CreateNewTab"));
        CHECK(capture.text().contains("OpenCommandPalette"));
    }

    SECTION("on macOS they are distinct chords: no warning")
    {
        auto capture = core::log::ScopedCapture { "error" };
        (void) loadFromYaml(dir, document("Primary, Shift", "Primary"), KeyboardPlatformKind::Mac);
        CHECK_FALSE(capture.text().contains("resolve to the same chord"));
    }

    SECTION("rows written identically keep the quiet merge")
    {
        auto capture = core::log::ScopedCapture { "error" };
        auto const config = loadFromYaml(dir, document("Primary", "Primary"), KeyboardPlatformKind::Xkb);
        CHECK_FALSE(capture.text().contains("resolve to the same chord"));
        REQUIRE(config.inputMappings.value().charMappings.size() == 1);
        CHECK(config.inputMappings.value().charMappings.front().binding.size() == 2);
    }

    SECTION("synonym spellings of one modifier are not a difference")
    {
        auto capture = core::log::ScopedCapture { "error" };
        (void) loadFromYaml(dir, document("Ctrl", "Control"), KeyboardPlatformKind::Xkb);
        CHECK_FALSE(capture.text().contains("resolve to the same chord"));
    }
}

TEST_CASE("an Alt character binding warns on macOS when Option always composes", "[config][primary]")
{
    auto dir = QTemporaryDir {};
    REQUIRE(dir.isValid());

    auto const document = [](std::string_view optionAsAlt) {
        return std::format(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        option_as_alt: {}
input_mapping:
    - {{ mods: [Alt], key: 'E', action: CreateNewTab }}
    - {{ mods: [Alt], key: Left, action: SwitchToTabLeft }}
    - {{ mods: [Alt], key: 'F', action: CreateNewTab }}
    - {{ mods: [Alt, Control], key: 'G', action: CreateNewTab }}
    - {{ mods: [Alt, Super], key: 'H', action: CreateNewTab }}
)",
                           optionAsAlt);
    };

    SECTION("Option composes on every profile: the character binding is named, the key binding is not")
    {
        auto capture = core::log::ScopedCapture { "error" };
        (void) loadFromYaml(dir, document("off"), KeyboardPlatformKind::Mac);
        CHECK(capture.text().contains("Alt+E, Alt+F cannot fire on macOS while option_as_alt is off"));
        CHECK_FALSE(capture.text().contains("Alt+Left"));
        CHECK_FALSE(capture.text().contains("Alt+L"));
        // Control and Super keep the chord firing: they are not listed.
        CHECK_FALSE(capture.text().contains("Alt+G"));
        CHECK_FALSE(capture.text().contains("Alt+H"));
        // One summary line, not one error per binding.
        auto const& text = capture.text();
        CHECK(text.contains("cannot fire"));
        CHECK(text.find("cannot fire", text.find("cannot fire") + 1) == std::string::npos);
    }

    SECTION("a profile that turns Option into Alt keeps the binding firing, so no warning")
    {
        auto capture = core::log::ScopedCapture { "error" };
        (void) loadFromYaml(dir, document("left"), KeyboardPlatformKind::Mac);
        CHECK_FALSE(capture.text().contains("cannot fire"));
    }

    SECTION("other platforms have no Option key to compose with")
    {
        auto capture = core::log::ScopedCapture { "error" };
        (void) loadFromYaml(dir, document("off"), KeyboardPlatformKind::Xkb);
        CHECK_FALSE(capture.text().contains("cannot fire"));
    }
}

TEST_CASE("the \"expected one of\" diagnostic lists Primary", "[config][primary]")
{
    auto dir = QTemporaryDir {};
    REQUIRE(dir.isValid());
    auto capture = core::log::ScopedCapture { "error" };

    (void) loadFromYaml(dir, oneRowConfig("Bogus"));
    CHECK(capture.text().contains("Primary"));
}
