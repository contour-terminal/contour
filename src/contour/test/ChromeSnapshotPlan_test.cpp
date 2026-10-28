// SPDX-License-Identifier: Apache-2.0
#include <contour/command/BlockMenu.hpp>
#include <contour/command/Shortcut.hpp>
#include <contour/session/EndReason.hpp>
#include <contour/test/ChromeSnapshotPlan.hpp>
#include <contour/test/TestEnvironment.hpp>

#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <string_view>
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
    // The solid band: single-tab under Reduce Transparency, and the only row built so.
    CHECK(scriptRow(SnapshotScript::BandSolid).name == "band-solid");
    CHECK(scriptRow(SnapshotScript::BandSolid).transparency
          == contour::config::TransparencyPreference::Reduced);
    CHECK(scriptRow(SnapshotScript::BandSolid).tabCount == scriptRow(SnapshotScript::SingleTab).tabCount);
    CHECK(scriptRow(SnapshotScript::BandSolid).frame == SnapshotFrame::ClientBand);
    CHECK(scriptRow(SnapshotScript::BandSolid).mode == SnapshotWindowMode::Windowed);
    // The macOS host: three-tabs with every row on macOS, and the only row that moves a row off its host.
    CHECK(scriptRow(SnapshotScript::MacOSHost).name == "macos-host");
    CHECK(scriptRow(SnapshotScript::MacOSHost).tabCount == scriptRow(SnapshotScript::ThreeTabs).tabCount);
    CHECK(scriptRow(SnapshotScript::MacOSHost).frame == SnapshotFrame::ClientBand);
    CHECK(scriptRow(SnapshotScript::MacOSHost).mode == SnapshotWindowMode::Windowed);
    for (auto const& row: SnapshotScripts)
    {
        CAPTURE(row.name);
        CHECK((row.host == SnapshotHost::MacOS) == (row.script == SnapshotScript::MacOSHost));
    }
    // Each of the four axes is set by its own row only: every other row is a client band, windowed,
    // with no drift.
    for (auto const& row: SnapshotScripts)
    {
        CAPTURE(row.name);
        CHECK((row.drift == SnapshotDrift::Shown) == (row.script == SnapshotScript::DaemonDrift));
        CHECK((row.frame == SnapshotFrame::NativeFrame) == (row.script == SnapshotScript::NativeTitleBar));
        CHECK((row.mode == SnapshotWindowMode::FullScreenRevealed)
              == (row.script == SnapshotScript::FullScreenReveal));
        CHECK((row.transparency == contour::config::TransparencyPreference::Reduced)
              == (row.script == SnapshotScript::BandSolid));
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

TEST_CASE("snapshot.scriptTable.densityAndPaneEnd", "[snapshot]")
{
    using contour::config::ChromeDensity;
    using namespace contour::test::snapshot;
    // The pane cards, Minimal, the header on hover and an ended pane, appended after the macOS host.
    CHECK(static_cast<int>(SnapshotScript::SplitMinimal) == static_cast<int>(SnapshotScript::MacOSHost) + 1);
    CHECK(scriptRow(SnapshotScript::SplitMinimal).name == "split-minimal");
    CHECK(scriptRow(SnapshotScript::SplitMinimal).panes == PaneShape::SplitThree);
    CHECK(scriptRow(SnapshotScript::SplitMinimal).density == ChromeDensity::Minimal);
    CHECK(scriptRow(SnapshotScript::SplitMinimal).action == ScriptAction::RevealHeaders);
    CHECK(scriptRow(SnapshotScript::SplitHeaderHover).name == "split-header-hover");
    CHECK(scriptRow(SnapshotScript::SplitHeaderHover).panes == PaneShape::SplitThree);
    CHECK(scriptRow(SnapshotScript::SplitHeaderHover).action == ScriptAction::HoverPaneHeader);
    CHECK(scriptRow(SnapshotScript::SplitHeaderHover).density == ChromeDensity::Comfortable);
    CHECK(scriptRow(SnapshotScript::PaneExited).name == "pane-exited");
    CHECK(scriptRow(SnapshotScript::PaneExited).panes == PaneShape::SplitThree);
    CHECK(scriptRow(SnapshotScript::PaneExited).paneEnd == SnapshotPaneEnd::Exited);
    CHECK(SnapshotExitMessage
          == std::format(
              contour::session::PaneFooterRows[static_cast<size_t>(contour::session::EndReason::Exited)]
                  .messageWithDetail,
              1));
    CHECK(scriptRow(SnapshotScript::PaneInterrupted).name == "pane-interrupted");
    CHECK(scriptRow(SnapshotScript::PaneInterrupted).panes == PaneShape::SplitThree);
    CHECK(scriptRow(SnapshotScript::PaneInterrupted).paneEnd == SnapshotPaneEnd::Interrupted);
    CHECK(scriptRow(SnapshotScript::SingleTabMinimal).name == "single-tab-minimal");
    CHECK(scriptRow(SnapshotScript::SingleTabMinimal).tabCount == 1);
    CHECK(scriptRow(SnapshotScript::SingleTabMinimal).panes == PaneShape::SingleLeaf);
    CHECK(scriptRow(SnapshotScript::SingleTabMinimal).density == ChromeDensity::Minimal);
    // The palette's grouped, filtered and drilled-in states, appended after it.
    CHECK(static_cast<int>(SnapshotScript::PaletteFiltered)
          == static_cast<int>(SnapshotScript::SingleTabMinimal) + 1);
    CHECK(scriptRow(SnapshotScript::PaletteFiltered).name == "palette-filtered");
    CHECK(scriptRow(SnapshotScript::PaletteFiltered).action == ScriptAction::FilterPalette);
    CHECK(scriptRow(SnapshotScript::PaletteDrilledIn).name == "palette-drill-in");
    CHECK(scriptRow(SnapshotScript::PaletteDrilledIn).action == ScriptAction::DrillInPalette);
    CHECK(static_cast<int>(SnapshotScript::FindBar)
          == static_cast<int>(SnapshotScript::PaletteDrilledIn) + 1);
    // Step 3c's rows come after 3b's find rows, appended.
    CHECK(static_cast<int>(SnapshotScript::PermissionPaste)
          == static_cast<int>(SnapshotScript::FindBarInvalid) + 1);
    // Each pane of a split is named, as the canvas's split names them.
    CHECK(paneCount(PaneShape::SingleLeaf) == 1);
    CHECK(paneCount(PaneShape::SplitThree) == 3);
    CHECK(SnapshotPaneLabels[0].process == "nvim");
    CHECK(SnapshotPaneLabels[2].process == "cmake");
    for (auto const& row: SnapshotScripts)
    {
        CAPTURE(row.name);
        // Every row names a density, and only the two Minimal rows leave Comfortable.
        CHECK(static_cast<std::size_t>(row.density) < contour::config::ChromeDensityCount);
        CHECK((row.density == ChromeDensity::Minimal)
              == (row.script == SnapshotScript::SplitMinimal
                  || row.script == SnapshotScript::SingleTabMinimal));
        // Only pane-exited ends a pane and only pane-interrupted restores one over a stopped command; either
        // is one of several.
        CHECK((row.paneEnd == SnapshotPaneEnd::Exited) == (row.script == SnapshotScript::PaneExited));
        CHECK((row.paneEnd == SnapshotPaneEnd::Interrupted)
              == (row.script == SnapshotScript::PaneInterrupted));
        if (row.paneEnd != SnapshotPaneEnd::None)
            CHECK(row.panes != PaneShape::SingleLeaf);
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

TEST_CASE("snapshot.variantHost", "[snapshot]")
{
    using contour::config::ChromeStyle;
    using contour::config::HostPlatform;
    using namespace contour::test::snapshot;
    for (auto const row:
         { ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux, ChromeStyle::Terminal })
    {
        CAPTURE(contour::config::chromeStyleName(row));
        // A script's own host overrides the row's; every other script keeps it.
        CHECK(variantHost({ .script = SnapshotScript::MacOSHost, .row = row }) == HostPlatform::MacOS);
        CHECK(variantHost({ .script = SnapshotScript::ThreeTabs, .row = row }) == rowHost(row).host);
    }
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

TEST_CASE("snapshot.scriptTable.findBar", "[snapshot]")
{
    using namespace contour::test::snapshot;
    // The find bar's three rows: on a pane, under a split's header, and on a refused pattern.
    CHECK(static_cast<int>(SnapshotScript::FindBarInvalid) == static_cast<int>(SnapshotScript::FindBar) + 2);
    CHECK(scriptRow(SnapshotScript::FindBar).name == "find-bar");
    CHECK(scriptRow(SnapshotScript::FindBar).panes == PaneShape::SingleLeaf);
    CHECK(scriptRow(SnapshotScript::FindBar).find == SnapshotFind::Matches);
    CHECK(scriptRow(SnapshotScript::FindBarSplit).name == "find-bar-split");
    CHECK(scriptRow(SnapshotScript::FindBarSplit).panes == PaneShape::SplitThree);
    CHECK(scriptRow(SnapshotScript::FindBarSplit).find == SnapshotFind::Matches);
    CHECK(scriptRow(SnapshotScript::FindBarInvalid).name == "find-bar-invalid");
    CHECK(scriptRow(SnapshotScript::FindBarInvalid).panes == PaneShape::SingleLeaf);
    CHECK(scriptRow(SnapshotScript::FindBarInvalid).find == SnapshotFind::Invalid);
    auto matches = std::size_t { 0 };
    auto invalid = std::size_t { 0 };
    for (auto const& row: SnapshotScripts)
    {
        CAPTURE(row.name);
        auto const isFindRow = row.script == SnapshotScript::FindBar
                               || row.script == SnapshotScript::FindBarSplit
                               || row.script == SnapshotScript::FindBarInvalid;
        // Only the find rows search and open the bar.
        CHECK((row.find != SnapshotFind::None) == isFindRow);
        CHECK((row.action == ScriptAction::OpenFindBar) == isFindRow);
        matches += row.find == SnapshotFind::Matches ? 1 : 0;
        invalid += row.find == SnapshotFind::Invalid ? 1 : 0;
    }
    // Each search the scene can give is pictured: matched on a pane and under a split's header, and refused.
    CHECK(matches == 2);
    CHECK(invalid == 1);
}

TEST_CASE("snapshot.scriptTable.permissionAndSaveLayout", "[snapshot]")
{
    using namespace contour::test::snapshot;
    CHECK(scriptRow(SnapshotScript::PermissionPaste).permission == SnapshotPermission::Paste);
    CHECK(scriptRow(SnapshotScript::PermissionPaste).action == ScriptAction::OpenPermission);
    CHECK(scriptRow(SnapshotScript::PermissionConsent).permission == SnapshotPermission::Consent);
    CHECK(scriptRow(SnapshotScript::SaveLayout).layoutName == SnapshotLayoutName::Available);
    CHECK(scriptRow(SnapshotScript::SaveLayout).action == ScriptAction::OpenSaveLayout);
    CHECK(scriptRow(SnapshotScript::SaveLayout).tabCount == 3);
    CHECK(scriptRow(SnapshotScript::SaveLayoutTaken).layoutName == SnapshotLayoutName::Taken);
    CHECK(scriptRow(SnapshotScript::ConfigReloadBanner).banner == SnapshotBanner::ConfigReload);
    CHECK(scriptRow(SnapshotScript::ConfigReloadBanner).name == "config-reload-banner");
    // Step 3c's rows run together, the banner last of them; Step 3e's overlays follow (scriptTable.overlays).
    CHECK(static_cast<int>(SnapshotScript::ConfigReloadBanner)
          == static_cast<int>(SnapshotScript::SaveLayoutTaken) + 1);
    // Every other row asks for neither.
    auto const asking = std::ranges::count_if(SnapshotScripts, [](SnapshotScriptRow const& row) {
        return row.permission != SnapshotPermission::None || row.layoutName != SnapshotLayoutName::None;
    });
    CHECK(asking == 4);
}

TEST_CASE("snapshot.scriptTable.overlays", "[snapshot]")
{
    using namespace contour::test::snapshot;
    // The size indicator, the link pill on a pane and in a header, the overlay scrollbar and the restore
    // summary, appended after Step 3c's rows.
    CHECK(static_cast<int>(SnapshotScript::SizeIndicator)
          == static_cast<int>(SnapshotScript::ConfigReloadBanner) + 1);
    CHECK(static_cast<int>(SnapshotScript::ScrollbarOverlay)
          == static_cast<int>(SnapshotScript::LinkPillSplit) + 1);
    // Step 6.3's pane-interrupted follows the keyboard-focus row.
    CHECK(static_cast<int>(SnapshotScript::PaneInterrupted)
          == static_cast<int>(SnapshotScript::KeyboardFocus) + 1);
    CHECK(SnapshotScripts[static_cast<std::size_t>(SnapshotScript::PaneInterrupted)].script
          == SnapshotScript::PaneInterrupted);
    CHECK(SnapshotScripts.back().script == SnapshotScript::RestoreSummary);
    struct Expected
    {
        SnapshotScript script;
        std::string_view name;
        PaneShape panes;
        ScriptAction action;
    };
    for (auto const& expected: std::array {
             Expected { SnapshotScript::SizeIndicator,
                        "size-indicator",
                        PaneShape::SplitThree,
                        ScriptAction::ShowSizeIndicator },
             Expected { SnapshotScript::LinkPill,
                        "link-pill",
                        PaneShape::SingleLeaf,
                        ScriptAction::HoldLinkModifier },
             Expected { SnapshotScript::LinkPillSplit,
                        "link-pill-split",
                        PaneShape::SplitThree,
                        ScriptAction::HoldLinkModifier },
             Expected { SnapshotScript::ScrollbarOverlay,
                        "scrollbar-overlay",
                        PaneShape::SingleLeaf,
                        ScriptAction::ScrollBack },
             Expected { SnapshotScript::RestoreSummary,
                        "restore-summary",
                        PaneShape::SingleLeaf,
                        ScriptAction::ShowRestoreSummary },
         })
    {
        CAPTURE(expected.name);
        auto const& row = scriptRow(expected.script);
        CHECK(row.name == expected.name);
        CHECK(row.panes == expected.panes);
        CHECK(row.action == expected.action);
        // Every other field at its default: a plain window, the overlay alone on it.
        CHECK(row.tabCount == 1);
        CHECK(row.banner == SnapshotBanner::None);
        CHECK(row.density == contour::config::ChromeDensity::Comfortable);
        CHECK(row.find == SnapshotFind::None);
    }
    // Only these rows perform these actions.
    for (auto const& row: SnapshotScripts)
    {
        CAPTURE(row.name);
        CHECK((row.action == ScriptAction::ShowSizeIndicator)
              == (row.script == SnapshotScript::SizeIndicator));
        CHECK((row.action == ScriptAction::HoldLinkModifier)
              == (row.script == SnapshotScript::LinkPill || row.script == SnapshotScript::LinkPillSplit));
        CHECK((row.action == ScriptAction::ScrollBack) == (row.script == SnapshotScript::ScrollbarOverlay));
        CHECK((row.action == ScriptAction::ShowRestoreSummary)
              == (row.script == SnapshotScript::RestoreSummary));
    }
    // The matrix grows by the rows on every row, edition and appearance.
    auto const rows =
        std::array { contour::config::ChromeStyle::MacOS, contour::config::ChromeStyle::Terminal };
    CHECK(snapshotMatrix(rows).size()
          == SnapshotScripts.size() * rows.size() * SnapshotEditions.size() * SnapshotAppearances.size());
}

TEST_CASE("snapshot.scriptTable.secureInput", "[snapshot][secureinput]")
{
    using namespace contour::test::snapshot;
    // Step 6.6's two rows, after Step 3e's: the lock in a split's header and on two tabs, and in the band.
    CHECK(static_cast<int>(SnapshotScript::SecureInput)
          == static_cast<int>(SnapshotScript::ScrollbarOverlay) + 1);
    CHECK(static_cast<int>(SnapshotScript::KeyboardFocus) + 1
          == static_cast<int>(SnapshotScript::PaneInterrupted)); // 6.3's row follows

    auto const& split = scriptRow(SnapshotScript::SecureInput);
    CHECK(split.name == "secure-input");
    CHECK(split.tabCount == 2);
    CHECK(split.panes == PaneShape::SplitThree);
    CHECK(split.activeTab == 0);
    CHECK(split.secure == SecureDecor::Locked);

    auto const& band = scriptRow(SnapshotScript::SecureInputBand);
    CHECK(band.name == "secure-input-band");
    CHECK(band.tabCount == 1);
    CHECK(band.panes == PaneShape::SingleLeaf);
    CHECK(band.secure == SecureDecor::Locked);

    // Only these two lock anything; every other field as the plain window has it.
    for (auto const& row: SnapshotScripts)
    {
        CAPTURE(row.name);
        CHECK(
            (row.secure == SecureDecor::Locked)
            == (row.script == SnapshotScript::SecureInput || row.script == SnapshotScript::SecureInputBand));
        if (row.secure == SecureDecor::Locked)
        {
            CHECK(row.action == ScriptAction::None);
            CHECK(row.banner == SnapshotBanner::None);
            CHECK(row.density == contour::config::ChromeDensity::Comfortable);
        }
    }
}

TEST_CASE("snapshot.scriptTable.keyboardFocus", "[snapshot]")
{
    using namespace contour::test::snapshot;
    // Three tabs, keyboard focus put on the second by the Ctrl+F6 action, the first still active.
    CHECK(static_cast<int>(SnapshotScript::KeyboardFocus)
          == static_cast<int>(SnapshotScript::SecureInputBand) + 1);
    CHECK(SnapshotScripts[static_cast<std::size_t>(SnapshotScript::KeyboardFocus)].script
          == SnapshotScript::KeyboardFocus);
    auto const& row = scriptRow(SnapshotScript::KeyboardFocus);
    CHECK(row.name == "keyboard-focus");
    CHECK(row.tabCount == 3);
    CHECK(row.panes == PaneShape::SingleLeaf);
    CHECK(row.action == ScriptAction::FocusTabWell);
    CHECK(row.activeTab == 0);
    // The focused tab is a tab of the window, and not the active one: the ring is not hidden by its fill.
    CHECK(SnapshotFocusedTab < row.tabCount);
    CHECK(SnapshotFocusedTab != row.activeTab);
    // Only this row moves focus into the chrome.
    for (auto const& other: SnapshotScripts)
    {
        CAPTURE(other.name);
        CHECK((other.action == ScriptAction::FocusTabWell)
              == (other.script == SnapshotScript::KeyboardFocus));
    }
}

TEST_CASE("snapshot.scriptTable.blocks", "[snapshot][blocks]")
{
    using namespace contour::test::snapshot;
    // The four rows after pane-interrupted: the block menu, the ⋯ button, and the sticky header in a pane
    // header and in the title band. Each is a picture of a sample block, and no other row shows one.
    struct Expected
    {
        SnapshotScript script;
        std::string_view name;
        int tabCount;
        PaneShape panes;
        ScriptAction action;
    };
    auto const expected = std::array {
        Expected {
            SnapshotScript::BlockMenu, "block-menu", 1, PaneShape::SingleLeaf, ScriptAction::OpenBlockMenu },
        Expected { SnapshotScript::BlockActions,
                   "block-actions",
                   1,
                   PaneShape::SingleLeaf,
                   ScriptAction::HoverBlock },
        Expected { SnapshotScript::StickyPaneHeader,
                   "sticky-pane-header",
                   1,
                   PaneShape::SplitThree,
                   ScriptAction::None },
        Expected { SnapshotScript::StickyTitleBand,
                   "sticky-title-band",
                   1,
                   PaneShape::SingleLeaf,
                   ScriptAction::None },
    };
    CHECK(static_cast<int>(expected.front().script) == static_cast<int>(SnapshotScript::PaneInterrupted) + 1);
    // Step 6.2's restore summary is the only row after them.
    CHECK(static_cast<int>(SnapshotScript::RestoreSummary)
          == static_cast<int>(SnapshotScript::StickyTitleBand) + 1);
    for (auto const& each: expected)
    {
        CAPTURE(each.name);
        auto const& row = scriptRow(each.script);
        CHECK(row.name == each.name);
        CHECK(row.tabCount == each.tabCount);
        CHECK(row.panes == each.panes);
        CHECK(row.action == each.action);
        CHECK(row.blocks == SnapshotBlocks::Sample);
    }
    for (auto const& row: SnapshotScripts)
    {
        CAPTURE(row.name);
        auto const listed =
            std::ranges::any_of(expected, [&](Expected const& e) { return e.script == row.script; });
        CHECK((row.blocks == SnapshotBlocks::Sample) == listed);
        CHECK((row.action == ScriptAction::OpenBlockMenu || row.action == ScriptAction::HoverBlock)
              == (row.script == SnapshotScript::BlockMenu || row.script == SnapshotScript::BlockActions));
    }
    // The menu's header is the four facts the block menu leads with, and its rows follow.
    CHECK(SnapshotBlockMenuHeader.size() == 4);
    CHECK(contour::command::BlockMenuRowCount == 5);
}
