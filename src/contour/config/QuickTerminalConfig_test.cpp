// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Config.hpp>
#include <contour/config/ConfigReader.hpp>
#include <contour/platform/HotkeyKeys.hpp>
#include <contour/test/CoreFixtures.hpp>

#include <yaml-cpp/yaml.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <fstream>
#include <set>
#include <string>

using contour::config::Config;
using contour::config::DefaultsEdition;
using contour::config::HotkeyChord;
using contour::config::HotkeyChordError;
using contour::config::QuickTerminalFocusLoss;
using contour::config::validateHotkeyChord;
using contour::config::YAMLConfigWriter;
using contour::test::loadConfigFromYaml;
using vtbackend::Key;
using vtbackend::Modifier;

TEST_CASE("quickTerminal.bothEditionsStartUnboundAndHideOnFocusLoss", "[config][quickterminal]")
{
    for (auto const edition: { DefaultsEdition::Classic, DefaultsEdition::Native2026 })
    {
        auto const config = Config { .defaultsEdition = edition };
        CHECK_FALSE(config.quickTerminal.value().hotkey.has_value());
        CHECK(config.quickTerminal.value().focusLoss == QuickTerminalFocusLoss::Hide);
    }
}

TEST_CASE("quickTerminal.hotkeyParsesAsAnInputMappingSpellsIt", "[config][quickterminal]")
{
    auto const config = loadConfigFromYaml("quick_terminal:\n  hotkey: { mods: [Control], key: '`' }\n");
    auto const& hotkey = config.quickTerminal.value().hotkey;
    REQUIRE(hotkey.has_value());
    CHECK(*hotkey == HotkeyChord { .modifiers = Modifier::Control, .key = U'`' });
}

TEST_CASE("quickTerminal.aNamedKeyParses", "[config][quickterminal]")
{
    auto const config = loadConfigFromYaml("quick_terminal:\n  hotkey: { mods: [Control, Alt], key: F12 }\n");
    auto const& hotkey = config.quickTerminal.value().hotkey;
    REQUIRE(hotkey.has_value());
    CHECK(*hotkey
          == HotkeyChord { .modifiers = vtbackend::Modifiers { Modifier::Control, Modifier::Alt },
                           .key = Key::F12 });
}

TEST_CASE("quickTerminal.aChordWithoutAHeldModifierStaysUnbound", "[config][quickterminal]")
{
    CHECK_FALSE(loadConfigFromYaml("quick_terminal:\n  hotkey: { mods: [], key: F12 }\n")
                    .quickTerminal.value()
                    .hotkey.has_value());
    CHECK_FALSE(loadConfigFromYaml("quick_terminal:\n  hotkey: { mods: [Shift], key: F12 }\n")
                    .quickTerminal.value()
                    .hotkey.has_value());
}

TEST_CASE("quickTerminal.aHotkeyWithoutAKeyStaysUnbound", "[config][quickterminal]")
{
    CHECK_FALSE(loadConfigFromYaml("quick_terminal:\n  hotkey: { mods: [Control] }\n")
                    .quickTerminal.value()
                    .hotkey.has_value());
}

TEST_CASE("quickTerminal.hideOnFocusLossFalseMeansStay", "[config][quickterminal]")
{
    auto const config = loadConfigFromYaml("quick_terminal:\n  hide_on_focus_loss: false\n");
    CHECK(config.quickTerminal.value().focusLoss == QuickTerminalFocusLoss::Stay);
    CHECK_FALSE(config.quickTerminal.value().hotkey.has_value());
}

TEST_CASE("quickTerminal.validateHotkeyChord", "[config][quickterminal]")
{
    auto const accepted = std::array { Modifier::Control, Modifier::Alt, Modifier::Super };
    for (auto const modifier: accepted)
        CHECK(validateHotkeyChord(HotkeyChord { .modifiers = modifier, .key = Key::F1 }).has_value());

    auto const refused = validateHotkeyChord(HotkeyChord { .modifiers = Modifier::Shift, .key = Key::F1 });
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == HotkeyChordError::NoModifier);
}

TEST_CASE("quickTerminal.theWrittenConfigReadsBackTheSame", "[config][quickterminal]")
{
    auto original = Config {};
    original.quickTerminal = contour::config::QuickTerminalConfig {
        .hotkey = HotkeyChord { .modifiers = vtbackend::Modifiers { Modifier::Control, Modifier::Alt },
                                .key = U'`' },
        .focusLoss = QuickTerminalFocusLoss::Stay,
    };
    auto const reloaded = loadConfigFromYaml(contour::config::createString<YAMLConfigWriter>(original));
    CHECK(reloaded.quickTerminal.value() == original.quickTerminal.value());

    original.quickTerminal = contour::config::QuickTerminalConfig {
        .hotkey = HotkeyChord { .modifiers = Modifier::Super, .key = Key::F12 },
    };
    auto const reloadedNamed = loadConfigFromYaml(contour::config::createString<YAMLConfigWriter>(original));
    CHECK(reloadedNamed.quickTerminal.value() == original.quickTerminal.value());
}

TEST_CASE("quickTerminal.everyHotkeyKeyRowSurvivesTheYamlSpelling", "[config][quickterminal]")
{
    // The Settings window stages a chord as hotkeyYaml() spells it and reads it back with parseHotkeyYaml().
    for (auto const& row: contour::platform::HotkeyKeyRows)
    {
        auto const chord =
            HotkeyChord { .modifiers = vtbackend::Modifiers { Modifier::Control, Modifier::Alt },
                          .key = row.key };
        INFO(contour::config::hotkeyYaml(chord));
        CHECK(contour::config::parseHotkeyYaml(contour::config::hotkeyYaml(chord)) == chord);
    }
    CHECK(contour::config::hotkeyYaml({ .modifiers = Modifier::Control, .key = U'`' })
          == "{ mods: [Control], key: '`' }");
    CHECK_FALSE(contour::config::parseHotkeyYaml("{ mods: [Shift], key: F12 }").has_value());
    CHECK_FALSE(contour::config::parseHotkeyYaml("not a mapping: [").has_value());
}

TEST_CASE("quickTerminal.aSettingsYmlOverrideOfTheHotkeyIsNestedAndReadsBack", "[config][quickterminal]")
{
    // A leaf that is itself a mapping must be written as one, and read back as the same text.
    auto const chord = HotkeyChord { .modifiers = Modifier::Control, .key = U'`' };
    auto const yaml = contour::config::emitGuiSettingsYaml(
        { .defaultProfile = std::nullopt,
          .globalOverrides = { { "quick_terminal.hotkey", contour::config::hotkeyYaml(chord) },
                               { "quick_terminal.hide_on_focus_loss", "false" } },
          .collectionOverrides = { "quick_terminal.hotkey" } });
    INFO(yaml);
    auto const document = YAML::Load(yaml);
    REQUIRE(document["quick_terminal"]["hotkey"].IsMap());
    CHECK(document["quick_terminal"]["hotkey"]["mods"][0].as<std::string>() == "Control");
    CHECK(document["quick_terminal"]["hide_on_focus_loss"].as<bool>() == false);

    // The merge of settings.yml over contour.yml reads it through the loader contour.yml uses.
    contour::test::TempDir const dir;
    REQUIRE(dir.isValid());
    {
        auto out = std::ofstream(dir.path() / "contour.yml");
        out << "quick_terminal:\n  hide_on_focus_loss: true\n";
    }
    {
        auto out = std::ofstream(dir.path() / "settings.yml");
        out << yaml;
    }
    auto config = Config {};
    contour::config::loadConfigFromFile(config, dir.path() / "contour.yml");
    CHECK(config.quickTerminal.value().hotkey == chord);
    CHECK(config.quickTerminal.value().focusLoss == QuickTerminalFocusLoss::Stay);
}

TEST_CASE("quickTerminal.aScalarThatLooksLikeACollectionStaysAString", "[config][settings]")
{
    // The row decides what an override is written as, not the look of its text: these are strings.
    auto const yaml = contour::config::emitGuiSettingsYaml(
        { .defaultProfile = std::nullopt,
          .globalOverrides = { { "command_blocks.pager", "{ less -R; }" },
                               { "command_blocks.pager_format", "[plain]" },
                               { "gutter.line_numbers", "{x}" } } });
    INFO(yaml);
    auto const document = YAML::Load(yaml);
    REQUIRE(document["command_blocks"]["pager"].IsScalar());
    CHECK(document["command_blocks"]["pager"].as<std::string>() == "{ less -R; }");
    REQUIRE(document["command_blocks"]["pager_format"].IsScalar());
    CHECK(document["command_blocks"]["pager_format"].as<std::string>() == "[plain]");
    REQUIRE(document["gutter"]["line_numbers"].IsScalar());
}

TEST_CASE("quickTerminal.aSettingsYmlSurvivesALoadAndASaveWhateverItsLeavesLookLike", "[config][settings]")
{
    contour::test::TempDir const dir;
    REQUIRE(dir.isValid());
    auto const path = dir.path() / "settings.yml";
    {
        auto out = std::ofstream(path);
        out << "command_blocks:\n"
               "  pager: \"{ less -R; }\"\n"
               "  pager_format: \"[plain]\"\n"
               "gutter:\n"
               "  hand_written: { nested: { depth: 2 }, list: [a, b] }\n"
               "quick_terminal:\n"
               "  hotkey: { mods: [Control], key: '`' }\n";
    }
    auto const loaded = contour::config::loadGuiSettingsFile(path);
    REQUIRE(loaded.has_value());
    // The marks come from what the nodes were, not from their text.
    CHECK(loaded->collectionOverrides
          == std::set<std::string> { "gutter.hand_written", "quick_terminal.hotkey" });

    auto const saved = YAML::Load(contour::config::emitGuiSettingsYaml(*loaded));
    REQUIRE(saved["command_blocks"]["pager"].IsScalar());
    CHECK(saved["command_blocks"]["pager"].as<std::string>() == "{ less -R; }");
    REQUIRE(saved["command_blocks"]["pager_format"].IsScalar());
    CHECK(saved["command_blocks"]["pager_format"].as<std::string>() == "[plain]");
    REQUIRE(saved["gutter"]["hand_written"].IsMap());
    CHECK(saved["gutter"]["hand_written"]["nested"]["depth"].as<int>() == 2);
    CHECK(saved["gutter"]["hand_written"]["list"].size() == 2);
    REQUIRE(saved["quick_terminal"]["hotkey"].IsMap());
}
