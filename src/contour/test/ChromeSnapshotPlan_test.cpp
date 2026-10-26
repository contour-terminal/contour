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
    CHECK(SnapshotScripts.size() == static_cast<std::size_t>(SnapshotScripts.back().script) + 1);
    CHECK(scriptRow(SnapshotScript::ThreeTabs).tabCount == 3);
    CHECK(scriptRow(SnapshotScript::TabMenu).tabCount == 3);
    CHECK(scriptRow(SnapshotScript::TabMenu).action == ScriptAction::OpenTabMenu);
    CHECK(scriptRow(SnapshotScript::About).action == ScriptAction::OpenAbout);
    CHECK(scriptRow(SnapshotScript::RemoteReconnecting).connection == HostConnection::Reconnecting);
    CHECK(scriptRow(SnapshotScript::Settings).surface == SnapshotSurface::SettingsWindow);
    CHECK(scriptRow(SnapshotScript::HostingBanner).banner == SnapshotBanner::Hosting);
    CHECK(scriptRow(SnapshotScript::NewDefaultsBanner).banner == SnapshotBanner::NewDefaults);
    CHECK(scriptRow(SnapshotScript::NewDefaultsBanner).name == "new-defaults-banner");
    CHECK(scriptRow(SnapshotScript::SingleTab).banner == SnapshotBanner::None);
    CHECK(scriptRow(SnapshotScript::TabBadges).decor == TabDecor::EveryBadge);
    CHECK(scriptRow(SnapshotScript::TabOverflow).tabCount == 20);
    CHECK(scriptRow(SnapshotScript::TabOverflow).activeTab
          == scriptRow(SnapshotScript::TabOverflow).tabCount - 1);
    CHECK(scriptRow(SnapshotScript::TabColorFlyout).action == ScriptAction::OpenTabColorFlyout);
    CHECK(scriptRow(SnapshotScript::TabColorFlyout).tabCount >= 2); // it opens the second tab's flyout
    // The scripts that came before the decor keep plain tabs, the first one active.
    for (auto const script: { SnapshotScript::SingleTab, SnapshotScript::ThreeTabs, SnapshotScript::TabMenu })
    {
        CHECK(scriptRow(script).decor == TabDecor::Plain);
        CHECK(scriptRow(script).activeTab == 0);
    }
    // The title band's rows.
    CHECK(scriptRow(SnapshotScript::RemoteConnected).connection == HostConnection::Connected);
    CHECK(scriptRow(SnapshotScript::RemoteConnected).hostLabel == "build-01");
    CHECK(scriptRow(SnapshotScript::RemoteConnected).name == "remote-connected");
    CHECK(scriptRow(SnapshotScript::RemoteDisconnected).connection == HostConnection::Disconnected);
    CHECK(scriptRow(SnapshotScript::RemoteDisconnected).hostLabel == "build-01");
    CHECK(scriptRow(SnapshotScript::RemoteDisconnected).name == "remote-disconnected");
    CHECK(scriptRow(SnapshotScript::DaemonDrift).drift == SnapshotDrift::Shown);
    CHECK(scriptRow(SnapshotScript::DaemonDrift).name == "daemon-drift");
    CHECK(scriptRow(SnapshotScript::NativeTitleBar).frame == SnapshotFrame::NativeFrame);
    CHECK(scriptRow(SnapshotScript::NativeTitleBar).tabCount == 3);
    CHECK(scriptRow(SnapshotScript::NativeTitleBar).name == "native-title-bar");
    CHECK(scriptRow(SnapshotScript::FullScreenReveal).mode == SnapshotWindowMode::FullScreenRevealed);
    CHECK(scriptRow(SnapshotScript::FullScreenReveal).name == "full-screen-reveal");
    // Each of the three axes is set by its own row only: every other row is a client band, windowed,
    // with no drift.
    for (auto const& row: SnapshotScripts)
    {
        CAPTURE(row.name);
        CHECK((row.drift == SnapshotDrift::Shown) == (row.script == SnapshotScript::DaemonDrift));
        CHECK((row.frame == SnapshotFrame::NativeFrame) == (row.script == SnapshotScript::NativeTitleBar));
        CHECK((row.mode == SnapshotWindowMode::FullScreenRevealed)
              == (row.script == SnapshotScript::FullScreenReveal));
        // A host chip needs a host to name, and a host is never local.
        CHECK(row.hostLabel.empty() == (row.connection == HostConnection::Local));
    }
    // Every row's active tab is one of its tabs.
    for (auto const& row: SnapshotScripts)
    {
        CAPTURE(row.name);
        CHECK(row.activeTab >= 0);
        CHECK(row.activeTab < row.tabCount);
    }
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

TEST_CASE("the offscreen platform on Windows is given the system's fonts", "[snapshot]")
{
    using contour::test::offscreenFontDirectory;
    CHECK(offscreenFontDirectory(core::testing::FakeEnvironment { { { "WINDIR", "C:\\Windows" } } })
          == "C:\\Windows/Fonts");
    // A directory the run was given is kept.
    CHECK_FALSE(
        offscreenFontDirectory(core::testing::FakeEnvironment {
                                   { { "WINDIR", "C:\\Windows" }, { "QT_QPA_FONTDIR", "D:/fonts" } } })
            .has_value());
    // Not Windows: nothing to point at.
    CHECK_FALSE(offscreenFontDirectory(core::testing::FakeEnvironment {}).has_value());
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
