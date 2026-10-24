// SPDX-License-Identifier: Apache-2.0
#include <contour/command/Shortcut.hpp>
#include <contour/test/ChromeSnapshotPlan.hpp>
#include <contour/test/TestEnvironment.hpp>

#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <format>
#include <vector>

using vtbackend::operator""_rgb;

TEST_CASE("snapshot.scriptTable", "[snapshot]")
{
    using namespace contour::test::snapshot;
    CHECK(SnapshotScripts.size() == static_cast<std::size_t>(SnapshotScript::Toast) + 1);
    CHECK(scriptRow(SnapshotScript::ThreeTabs).tabCount == 3);
    CHECK(scriptRow(SnapshotScript::RemoteReconnecting).connection == HostConnection::Reconnecting);
    CHECK(scriptRow(SnapshotScript::Settings).page == ContentPage::Settings);
}

TEST_CASE("snapshot.rowHosts", "[snapshot]")
{
    using contour::config::ChromeStyle;
    using contour::config::HostPlatform;
    using namespace contour::test::snapshot;
    using contour::command::ShortcutStyle;
    using contour::command::shortcutStyleFor;
    CHECK(rowHost(ChromeStyle::MacOS).host == HostPlatform::MacOS);
    CHECK(rowHost(ChromeStyle::Fluent).host == HostPlatform::Windows);
    CHECK(rowHost(ChromeStyle::ContourLinux).host == HostPlatform::Other);
    CHECK(rowHost(ChromeStyle::Terminal).host == HostPlatform::Other);
    CHECK(shortcutStyleFor(rowHost(ChromeStyle::MacOS).host) == ShortcutStyle::MacGlyphs);
    CHECK(shortcutStyleFor(rowHost(ChromeStyle::Fluent).host) == ShortcutStyle::Text);
    CHECK(shortcutStyleFor(rowHost(ChromeStyle::ContourLinux).host) == ShortcutStyle::Text);
    CHECK(shortcutStyleFor(rowHost(ChromeStyle::Terminal).host) == ShortcutStyle::Text);
}

TEST_CASE("snapshot.rowsRenderableUnder", "[snapshot]")
{
    using contour::config::ChromeStyle;
    using namespace contour::test::snapshot;
    CHECK(rowsRenderableUnder("Fusion")
          == std::vector { ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux });
    CHECK(rowsRenderableUnder("ContourTui") == std::vector { ChromeStyle::Terminal });
    CHECK(rowsRenderableUnder("ContourTUI").empty()); // a typo renders nothing
}

TEST_CASE("snapshot.matrixAndNames", "[snapshot]")
{
    using contour::config::ChromeStyle;
    using namespace contour::test::snapshot;
    auto const all = std::array {
        ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux, ChromeStyle::Terminal
    };
    auto const matrix = snapshotMatrix(all);
    CHECK(matrix.size()
          == SnapshotScripts.size() * all.size() * SnapshotEditions.size() * SnapshotAppearances.size());
    CHECK(snapshotFileName(matrix.front()) == "single-tab.macos.classic.dark.png");
    CHECK(snapshotFileName(matrix.back())
          == std::format("{}.terminal.native2026.light.png", SnapshotScripts.back().name));
    CHECK(snapshotMatrix(rowsRenderableUnder("ContourTui")).size()
          == SnapshotScripts.size() * SnapshotEditions.size() * SnapshotAppearances.size());
}

TEST_CASE("snapshot.lookPerEdition", "[snapshot]")
{
    using contour::config::DefaultsEdition;
    using contour::config::TabBarVisibility;
    using vtbackend::ColorPreference;
    using namespace contour::test::snapshot;
    CHECK(snapshotLook(DefaultsEdition::Native2026, ColorPreference::Dark).terminalBackground
          == 0x1C1E24_rgb);
    CHECK(snapshotLook(DefaultsEdition::Native2026, ColorPreference::Light).terminalBackground
          == 0xFAFAF8_rgb);
    CHECK(snapshotLook(DefaultsEdition::Native2026, ColorPreference::Dark).dimUnfocused == 0.15);
    CHECK(snapshotLook(DefaultsEdition::Native2026, ColorPreference::Dark).tabBarVisibility
          == TabBarVisibility::Multiple);
    // Classic names one scheme, so both appearances share its background.
    CHECK(snapshotLook(DefaultsEdition::Classic, ColorPreference::Dark).terminalBackground
          == snapshotLook(DefaultsEdition::Classic, ColorPreference::Light).terminalBackground);
    CHECK(snapshotLook(DefaultsEdition::Classic, ColorPreference::Dark).tabBarVisibility
          == TabBarVisibility::Always);
}

TEST_CASE("snapshot.environment", "[snapshot]")
{
    using contour::test::controlsStyleFor;
    using namespace contour::test::snapshot;
    auto const none = core::testing::FakeEnvironment {};
    CHECK(controlsStyleFor(none) == "Fusion");
    CHECK_FALSE(snapshotDirectory(none).has_value());
    auto const set = core::testing::FakeEnvironment { { { "CONTOUR_TEST_CONTROLS_STYLE", "ContourTui" },
                                                        { "CONTOUR_SNAPSHOT_DIR", "/tmp/shots" } } };
    CHECK(controlsStyleFor(set) == "ContourTui");
    CHECK(snapshotDirectory(set) == std::filesystem::path("/tmp/shots"));
    auto const empty = core::testing::FakeEnvironment { { { "CONTOUR_TEST_CONTROLS_STYLE", "" },
                                                          { "CONTOUR_SNAPSHOT_DIR", "" } } };
    CHECK(controlsStyleFor(empty) == "Fusion");
    CHECK_FALSE(snapshotDirectory(empty).has_value());
}

TEST_CASE("snapshot.manifestLine", "[snapshot]")
{
    using namespace contour::test::snapshot;
    auto const v = SnapshotVariant { .script = SnapshotScript::Palette,
                                     .row = contour::config::ChromeStyle::Terminal,
                                     .edition = contour::config::DefaultsEdition::Native2026,
                                     .appearance = vtbackend::ColorPreference::Dark };
    CHECK(
        manifestLine(v,
                     SnapshotFacts { .platform = "wayland",
                                     .devicePixelRatio = 1.0,
                                     .chromeFontFamily = "JetBrains Mono",
                                     .controlsStyle = "ContourTui",
                                     .qtVersion = "6.11.1" })
        == "palette.terminal.native2026.dark.png\tpalette\tterminal\tnative2026\tdark\twayland\t1\tJetBrains "
           "Mono\tContourTui\t6.11.1\n");
}
