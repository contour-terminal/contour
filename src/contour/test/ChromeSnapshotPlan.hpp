// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/AccessibilityPreferences.hpp>
#include <contour/config/ChromeDensity.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/DefaultsEdition.hpp>
#include <contour/config/TabBarMode.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/config/WindowControlStyle.hpp>

#include <vtbackend/core/ColorPalette.hpp>

#include <core/Environment.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// The chrome snapshot driver's plan: which windows to render, as data with no Qt in it.
///
/// Everything here is a table or a pure function of one, so the plan is checkable headlessly and the
/// renderer that consumes it contains no decisions of its own.
namespace contour::test::snapshot
{

/// A named window state worth a picture.
enum class SnapshotScript : uint8_t
{
    SingleTab = 0,
    ThreeTabs,
    SplitThree,
    Palette,
    Settings,
    RemoteReconnecting,
    Toast,
    KitDialog,
    HostingBanner,
    NewDefaultsBanner,
    TabMenu,
    About,
    TabBadges,          //!< Every badge kind on one strip, beside a coloured active tab.
    TabOverflow,        //!< More tabs than the strip holds, the last one active.
    TabColorFlyout,     //!< The second tab's colour flyout.
    RemoteConnected,    //!< A window attached to a host, connected: the host chip in the band.
    RemoteDisconnected, //!< A window whose host connection is lost: the chip says so.
    DaemonDrift,        //!< The daemon runs another release: the drift chip in the band.
    NativeTitleBar,     //!< show_title_bar: the system frame draws the title, the strip sits below it.
    FullScreenReveal,   //!< Full screen, with the pointer at the top edge revealing the band.
    BandSolid,          //!< single-tab under Reduce Transparency: the band's opaque tint.
    MacOSHost,          //!< three-tabs with every row on a macOS host: the Terminal row's band grown
                        //!< to whole cells around the traffic lights.
    SplitMinimal,       //!< split-three under Minimal, the held modifier revealing the headers over it.
    SplitHeaderHover,   //!< split-three with the pointer over the active header, then the gap: the
                        //!< header's actions and the split handle's grabber.
    PaneExited,         //!< split-three with an ended pane: its footer below the cells.
    SingleTabMinimal,   //!< One tab under Minimal: the bare band, or none (the Terminal row).
    PaletteFiltered,    //!< The palette filtered by "split": Pane, then Layouts with a surfaced child.
    PaletteDrilledIn,   //!< The palette inside Change Profile's level.
};

/// How a script's active tab is divided into panes.
enum class PaneShape : uint8_t
{
    SingleLeaf = 0, //!< One pane.
    SplitThree,     //!< `[ a | split(b, c) ]`, with `a` active.
};

/// How many panes @p shape divides a tab into.
/// @param shape The pane shape.
/// @return Its leaf count.
[[nodiscard]] constexpr std::size_t paneCount(PaneShape shape) noexcept
{
    switch (shape)
    {
        case PaneShape::SingleLeaf: break;
        case PaneShape::SplitThree: return 3;
    }
    return 1;
}

/// Which window a script's picture is of.
enum class SnapshotSurface : uint8_t
{
    MainWindow = 0, //!< The terminal window, Main.qml.
    SettingsWindow, //!< The app's one Settings window, opened over the terminal window.
};

/// An interaction performed after the window is shown.
enum class ScriptAction : uint8_t
{
    None = 0,
    OpenPalette,
    ShowToast,
    OpenKitDialog,
    OpenTabMenu,        //!< The second tab's context menu, with its "Tab Color" sub-menu open.
    OpenAbout,          //!< The About dialog, raised by the ShowAbout action.
    OpenTabColorFlyout, //!< The second tab's colour flyout, as the SetTabColor action opens it.
    HoverPaneHeader,    //!< The pointer over the active pane's header, then on the gap beside it.
    RevealHeaders,      //!< Minimal's headers revealed over the panes, as the held modifier reveals them.
    FilterPalette,      //!< The palette open, its filter "split".
    DrillInPalette,     //!< The palette open inside the Change Profile level.
};

/// What a script's tabs carry beyond their titles.
enum class TabDecor : uint8_t
{
    Plain = 0,  //!< "Tab 1", "Tab 2", ...: no colour, no badge.
    EveryBadge, //!< Row 0 coloured, rows 1 to 5 one badge kind each (see MockMainController).
    LongTitle,  //!< Row 0's title is long enough to elide in any tab.
};

/// Whether the window is attached to a host, and in which state.
enum class HostConnection : uint8_t
{
    Local = 0,    //!< Not attached: no host chip.
    Connected,    //!< Attached, and the daemon is reachable.
    Reconnecting, //!< Attached, and the connection is being re-established.
    Disconnected, //!< Attached, and nothing is retried.
};

/// Whether the daemon a script's window talks to runs another release than the client.
enum class SnapshotDrift : uint8_t
{
    None = 0, //!< The releases match: no drift chip.
    Shown,    //!< They differ: the drift chip shows.
};

/// Who draws a script's window title area, as show_title_bar decides.
enum class SnapshotFrame : uint8_t
{
    ClientBand = 0, //!< show_title_bar false: the chrome draws its own band.
    NativeFrame,    //!< show_title_bar true: the system frame draws the title.
};

/// How a script's window is shown.
enum class SnapshotWindowMode : uint8_t
{
    Windowed = 0,       //!< An ordinary window.
    FullScreenRevealed, //!< Full screen, with the band revealed by the pointer at the top edge.
};

/// The banner a script shows above the content.
enum class SnapshotBanner : uint8_t
{
    None = 0,
    Hosting,     //!< The daemon-hosting notice.
    NewDefaults, //!< The offer of the newest defaults.
};

/// Whether one of a script's panes has ended.
enum class SnapshotPaneEnd : uint8_t
{
    None = 0, //!< Every pane runs.
    Exited,   //!< The last pane's program exited: its footer offers Restart and Close.
};

/// What the footer of a script's ended pane says (SnapshotPaneEnd::Exited): session::PaneFooterRows' Exited
/// row with exit code 1, which ChromeSnapshotPlan_test checks it against.
inline constexpr std::string_view SnapshotExitMessage = "Process exited with code 1.";

/// What one pane of a script's tab runs, as its header names it.
struct SnapshotPaneLabel
{
    std::string_view process {};          ///< The foreground process: the header's name.
    std::string_view workingDirectory {}; ///< The working directory, as the header shows it.
};

/// The panes' labels, leaf by leaf in tree order: the panes of the split the canvas boards draw
/// (mac-dark.dc.html, tui.dc.html). A pane shape of fewer leaves takes the first ones.
inline constexpr auto SnapshotPaneLabels = std::array {
    SnapshotPaneLabel { .process = "nvim", .workingDirectory = "~/repo/contour/src/contour/config" },
    SnapshotPaneLabel { .process = "zsh", .workingDirectory = "~/repo/contour" },
    SnapshotPaneLabel { .process = "cmake", .workingDirectory = "out/appleclang-debug" },
};

/// The text of the hosting notice a script with SnapshotBanner::Hosting shows.
inline constexpr std::string_view SnapshotHostingNotice =
    "Contour could not reach its daemon (refused). This window runs its shells itself; they end when it "
    "closes.";

/// Which host a script renders each chrome row on.
enum class SnapshotHost : uint8_t
{
    RowDefault = 0, //!< Each row on its own host, as SnapshotRowHosts lists it.
    MacOS,          //!< Every row on macOS: its traffic lights, its shortcut glyphs and its title bar.
};

/// One row of the script table.
struct SnapshotScriptRow
{
    SnapshotScript script = SnapshotScript::SingleTab;      ///< The script this row describes.
    std::string_view name {};                               ///< Its name in file names and the manifest.
    int tabCount = 1;                                       ///< How many tabs the window holds.
    PaneShape panes = PaneShape::SingleLeaf;                ///< The active tab's pane layout.
    SnapshotSurface surface = SnapshotSurface::MainWindow;  ///< The window the picture is of.
    ScriptAction action = ScriptAction::None;               ///< What is done after showing the window.
    HostConnection connection = HostConnection::Local;      ///< The attached host's state.
    std::string_view hostLabel {};                          ///< The attached host's label; empty when local.
    SnapshotBanner banner = SnapshotBanner::None;           ///< The banner shown above the content.
    TabDecor decor = TabDecor::Plain;                       ///< What the tabs carry.
    int activeTab = 0;                                      ///< The active tab's index.
    SnapshotDrift drift = SnapshotDrift::None;              ///< Whether the drift chip shows.
    SnapshotFrame frame = SnapshotFrame::ClientBand;        ///< Who draws the title area.
    SnapshotWindowMode mode = SnapshotWindowMode::Windowed; ///< How the window is shown.
    /// The Reduce Transparency setting the chrome is built under.
    config::TransparencyPreference transparency = config::TransparencyPreference::Allowed;
    SnapshotHost host = SnapshotHost::RowDefault; ///< The host each chrome row is rendered on.
    /// The chrome density the window is built under.
    config::ChromeDensity density = config::ChromeDensity::Comfortable;
    SnapshotPaneEnd paneEnd = SnapshotPaneEnd::None; ///< Whether a pane has ended.
};

/// The rows of the script table, in enumerator order.
inline constexpr auto SnapshotScripts = std::array {
    SnapshotScriptRow { .script = SnapshotScript::SingleTab,
                        .name = "single-tab",
                        .tabCount = 1,
                        .panes = PaneShape::SingleLeaf,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::None,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::ThreeTabs,
                        .name = "three-tabs",
                        .tabCount = 3,
                        .panes = PaneShape::SingleLeaf,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::None,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::SplitThree,
                        .name = "split-three",
                        .tabCount = 1,
                        .panes = PaneShape::SplitThree,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::None,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::Palette,
                        .name = "palette",
                        .tabCount = 1,
                        .panes = PaneShape::SingleLeaf,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::OpenPalette,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::Settings,
                        .name = "settings",
                        .tabCount = 1,
                        .panes = PaneShape::SingleLeaf,
                        .surface = SnapshotSurface::SettingsWindow,
                        .action = ScriptAction::None,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::RemoteReconnecting,
                        .name = "remote-reconnecting",
                        .tabCount = 1,
                        .panes = PaneShape::SingleLeaf,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::None,
                        .connection = HostConnection::Reconnecting,
                        .hostLabel = "build-01",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::Toast,
                        .name = "toast",
                        .tabCount = 1,
                        .panes = PaneShape::SingleLeaf,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::ShowToast,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::KitDialog,
                        .name = "kit-dialog",
                        .tabCount = 1,
                        .panes = PaneShape::SingleLeaf,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::OpenKitDialog,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::HostingBanner,
                        .name = "hosting-banner",
                        .tabCount = 1,
                        .panes = PaneShape::SingleLeaf,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::None,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::Hosting,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::NewDefaultsBanner,
                        .name = "new-defaults-banner",
                        .tabCount = 1,
                        .panes = PaneShape::SingleLeaf,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::None,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::NewDefaults,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::TabMenu,
                        .name = "tab-menu",
                        .tabCount = 3,
                        .panes = PaneShape::SingleLeaf,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::OpenTabMenu,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::About,
                        .name = "about",
                        .tabCount = 1,
                        .panes = PaneShape::SingleLeaf,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::OpenAbout,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::TabBadges,
                        .name = "tab-badges",
                        .tabCount = 6,
                        .panes = PaneShape::SingleLeaf,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::None,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::EveryBadge,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::TabOverflow,
                        .name = "tab-overflow",
                        .tabCount = 20,
                        .panes = PaneShape::SingleLeaf,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::None,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 19,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::TabColorFlyout,
                        .name = "tab-color-flyout",
                        .tabCount = 3,
                        .panes = PaneShape::SingleLeaf,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::OpenTabColorFlyout,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::RemoteConnected,
                        .name = "remote-connected",
                        .tabCount = 1,
                        .panes = PaneShape::SingleLeaf,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::None,
                        .connection = HostConnection::Connected,
                        .hostLabel = "build-01",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::RemoteDisconnected,
                        .name = "remote-disconnected",
                        .tabCount = 1,
                        .panes = PaneShape::SingleLeaf,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::None,
                        .connection = HostConnection::Disconnected,
                        .hostLabel = "build-01",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::DaemonDrift,
                        .name = "daemon-drift",
                        .tabCount = 3,
                        .panes = PaneShape::SingleLeaf,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::None,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::Shown,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::NativeTitleBar,
                        .name = "native-title-bar",
                        .tabCount = 3,
                        .panes = PaneShape::SingleLeaf,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::None,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::NativeFrame,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::FullScreenReveal,
                        .name = "full-screen-reveal",
                        .tabCount = 3,
                        .panes = PaneShape::SingleLeaf,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::None,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::FullScreenRevealed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::BandSolid,
                        .name = "band-solid",
                        .tabCount = 1,
                        .panes = PaneShape::SingleLeaf,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::None,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Reduced,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::MacOSHost,
                        .name = "macos-host",
                        .tabCount = 3,
                        .panes = PaneShape::SingleLeaf,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::None,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::MacOS,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::SplitMinimal,
                        .name = "split-minimal",
                        .tabCount = 1,
                        .panes = PaneShape::SplitThree,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::RevealHeaders,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Minimal,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::SplitHeaderHover,
                        .name = "split-header-hover",
                        .tabCount = 1,
                        .panes = PaneShape::SplitThree,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::HoverPaneHeader,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::PaneExited,
                        .name = "pane-exited",
                        .tabCount = 1,
                        .panes = PaneShape::SplitThree,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::None,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::Exited },
    SnapshotScriptRow { .script = SnapshotScript::SingleTabMinimal,
                        .name = "single-tab-minimal",
                        .tabCount = 1,
                        .panes = PaneShape::SingleLeaf,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::None,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Minimal,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::PaletteFiltered,
                        .name = "palette-filtered",
                        .tabCount = 1,
                        .panes = PaneShape::SingleLeaf,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::FilterPalette,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
    SnapshotScriptRow { .script = SnapshotScript::PaletteDrilledIn,
                        .name = "palette-drill-in",
                        .tabCount = 1,
                        .panes = PaneShape::SingleLeaf,
                        .surface = SnapshotSurface::MainWindow,
                        .action = ScriptAction::DrillInPalette,
                        .connection = HostConnection::Local,
                        .hostLabel = "",
                        .banner = SnapshotBanner::None,
                        .decor = TabDecor::Plain,
                        .activeTab = 0,
                        .drift = SnapshotDrift::None,
                        .frame = SnapshotFrame::ClientBand,
                        .mode = SnapshotWindowMode::Windowed,
                        .transparency = config::TransparencyPreference::Allowed,
                        .host = SnapshotHost::RowDefault,
                        .density = config::ChromeDensity::Comfortable,
                        .paneEnd = SnapshotPaneEnd::None },
};

static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, SnapshotScripts.size()),
                                  [](std::size_t i) {
                                      return static_cast<std::size_t>(SnapshotScripts[i].script) == i;
                                  }),
              "SnapshotScripts must list one row per SnapshotScript, in enumerator order");
static_assert(SnapshotScripts.size() == static_cast<std::size_t>(SnapshotScripts.back().script) + 1,
              "every SnapshotScript needs a row");

static_assert(std::ranges::all_of(SnapshotScripts,
                                  [](SnapshotScriptRow const& row) {
                                      return paneCount(row.panes) <= SnapshotPaneLabels.size();
                                  }),
              "every pane of every script needs a label");

/// The row describing @p script.
/// @param script The script to look up.
/// @return Its row of the script table.
[[nodiscard]] constexpr SnapshotScriptRow const& scriptRow(SnapshotScript script) noexcept
{
    return SnapshotScripts[static_cast<std::size_t>(script)];
}

/// The host a chrome row is rendered as, so its window controls and shortcut glyphs match.
struct SnapshotRowHost
{
    config::ChromeStyle row = config::ChromeStyle::Terminal; ///< The chrome row.
    config::HostPlatform host = config::HostPlatform::Other; ///< The platform whose window controls and
                                                             ///< shortcut spelling the row shows.
};

/// The rows of the row-host table, in @c config::ChromeStyle order.
inline constexpr auto SnapshotRowHosts = std::array {
    SnapshotRowHost { .row = config::ChromeStyle::MacOS, .host = config::HostPlatform::MacOS },
    SnapshotRowHost { .row = config::ChromeStyle::Fluent, .host = config::HostPlatform::Windows },
    SnapshotRowHost { .row = config::ChromeStyle::ContourLinux, .host = config::HostPlatform::Other },
    SnapshotRowHost { .row = config::ChromeStyle::Terminal, .host = config::HostPlatform::Other },
};

static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, SnapshotRowHosts.size()),
                                  [](std::size_t i) {
                                      return static_cast<std::size_t>(SnapshotRowHosts[i].row) == i;
                                  }),
              "SnapshotRowHosts must list one row per ChromeStyle, in enumerator order");
static_assert(SnapshotRowHosts.size() == config::detail::ChromeStyleCount, "every ChromeStyle needs a host");

/// The host @p row is rendered as.
/// @param row The chrome row.
/// @return Its row of the row-host table. The window controls are
///         `resolveWindowControlStyle(WindowControlStyle::Auto, rowHost(row).host)`, and the shortcuts
///         are spelled `command::shortcutStyleFor(rowHost(row).host)`.
[[nodiscard]] constexpr SnapshotRowHost const& rowHost(config::ChromeStyle row) noexcept
{
    return SnapshotRowHosts[static_cast<std::size_t>(row)];
}

/// The editions a snapshot is rendered under.
inline constexpr auto SnapshotEditions =
    std::array { config::DefaultsEdition::Classic, config::DefaultsEdition::Native2026 };

/// The appearances a snapshot is rendered under.
inline constexpr auto SnapshotAppearances =
    std::array { vtbackend::ColorPreference::Dark, vtbackend::ColorPreference::Light };

/// The name of @p appearance in file names and the manifest.
/// @param appearance The appearance.
/// @return "dark" or "light".
[[nodiscard]] constexpr std::string_view appearanceName(vtbackend::ColorPreference appearance) noexcept
{
    switch (appearance)
    {
        case vtbackend::ColorPreference::Dark: break;
        case vtbackend::ColorPreference::Light: return "light";
    }
    return "dark";
}

/// One picture in the matrix.
struct SnapshotVariant
{
    SnapshotScript script = SnapshotScript::SingleTab;                        ///< The window state.
    config::ChromeStyle row = config::ChromeStyle::Terminal;                  ///< The chrome row.
    config::DefaultsEdition edition = config::DefaultsEdition::Classic;       ///< The defaults edition.
    vtbackend::ColorPreference appearance = vtbackend::ColorPreference::Dark; ///< Dark or light.

    [[nodiscard]] bool operator==(SnapshotVariant const&) const = default;
};

/// The host @p variant is rendered on: its row's, unless its script names one for every row.
/// @param variant The picture.
/// @return The platform whose window controls, shortcut spelling and title bar the picture shows.
[[nodiscard]] constexpr config::HostPlatform variantHost(SnapshotVariant const& variant) noexcept
{
    switch (scriptRow(variant.script).host)
    {
        case SnapshotHost::RowDefault: break;
        case SnapshotHost::MacOS: return config::HostPlatform::MacOS;
    }
    return rowHost(variant.row).host;
}

/// The chrome rows whose Quick Controls style is @p controlsStyle.
/// @param controlsStyle The style in effect, as `uiStyleTokens(row).quickControlsStyle` spells it.
/// @return The matching rows, in enumerator order; empty when the name matches none.
[[nodiscard]] inline std::vector<config::ChromeStyle> rowsRenderableUnder(std::string_view controlsStyle)
{
    auto const matches = [&](config::ChromeStyle row) {
        return config::uiStyleTokens(row).quickControlsStyle == controlsStyle;
    };
    return std::ranges::to<std::vector>(SnapshotRowHosts | std::views::transform(&SnapshotRowHost::row)
                                        | std::views::filter(matches));
}

/// Every variant to render for @p rows.
/// @param rows The chrome rows to cover.
/// @return The variants, nested script, then row, then edition, then appearance (script outermost).
[[nodiscard]] inline std::vector<SnapshotVariant> snapshotMatrix(std::span<config::ChromeStyle const> rows)
{
    auto matrix = std::vector<SnapshotVariant>();
    matrix.reserve(SnapshotScripts.size() * rows.size() * SnapshotEditions.size()
                   * SnapshotAppearances.size());
    for (auto const& scriptRowEntry: SnapshotScripts)
        for (auto const row: rows)
            for (auto const edition: SnapshotEditions)
                for (auto const appearance: SnapshotAppearances)
                    matrix.push_back(SnapshotVariant { .script = scriptRowEntry.script,
                                                       .row = row,
                                                       .edition = edition,
                                                       .appearance = appearance });
    return matrix;
}

/// The file @p variant is written to.
/// @param variant The picture.
/// @return `<script>.<row>.<edition>.<appearance>.png`.
[[nodiscard]] inline std::string snapshotFileName(SnapshotVariant const& variant)
{
    return std::format("{}.{}.{}.{}.png",
                       scriptRow(variant.script).name,
                       config::chromeStyleName(variant.row),
                       config::configEnumToken(variant.edition),
                       appearanceName(variant.appearance));
}

/// The edition- and appearance-dependent look a window is configured with.
struct SnapshotLook
{
    vtbackend::RGBColor terminalBackground {}; ///< The terminal's default background.
    double dimUnfocused = 0.0; ///< How far unfocused panes are dimmed; Classic's value by default.
    /// When the tab strip is shown; Classic's value by default.
    config::TabBarVisibility tabBarVisibility = config::TabBarVisibility::Always;
};

/// The look @p edition ships under @p appearance.
/// @param edition The defaults edition.
/// @param appearance Dark or light.
/// @return The look, built from the edition's defaults and its color configuration.
[[nodiscard]] inline SnapshotLook snapshotLook(config::DefaultsEdition edition,
                                               vtbackend::ColorPreference appearance)
{
    auto const& defaults = config::editionDefaults(edition);
    return SnapshotLook {
        .terminalBackground =
            config::preferredColorPalette(config::defaultColors(edition), appearance)->defaultBackground,
        .dimUnfocused = defaults.dimUnfocused,
        .tabBarVisibility = defaults.tabBarVisibility,
    };
}

/// The variable that names the directory snapshots are written to.
inline constexpr std::string_view SnapshotDirectoryVariable = "CONTOUR_SNAPSHOT_DIR";

/// The directory snapshots are written to.
/// @param env The environment to read.
/// @return The variable's value, or std::nullopt when it is unset or empty: no directory, no snapshots.
[[nodiscard]] inline std::optional<std::filesystem::path> snapshotDirectory(core::Environment const& env)
{
    if (auto value = env.get(SnapshotDirectoryVariable); value && !value->empty())
        return std::filesystem::path(*value);
    return std::nullopt;
}

/// What the manifest records about the machine a snapshot was rendered on.
struct SnapshotFacts
{
    std::string platform {};         ///< The Qt platform plugin.
    double devicePixelRatio = 1.0;   ///< The window's device pixel ratio.
    std::string chromeFontFamily {}; ///< The font the chrome resolved.
    std::string controlsStyle {};    ///< The Quick Controls style in effect.
    std::string qtVersion {};        ///< The Qt version rendered with.
};

/// The manifest's header line.
inline constexpr std::string_view ManifestHeader =
    "file\tscript\trow\tedition\tappearance\tplatform\tdpr\tfont\tcontrols\tqt\n";

/// One manifest line.
/// @param variant The picture.
/// @param facts The machine facts to record with it.
/// @return A tab-separated line ending in a newline, with the device pixel ratio formatted `{:g}`.
[[nodiscard]] inline std::string manifestLine(SnapshotVariant const& variant, SnapshotFacts const& facts)
{
    return std::format("{}\t{}\t{}\t{}\t{}\t{}\t{:g}\t{}\t{}\t{}\n",
                       snapshotFileName(variant),
                       scriptRow(variant.script).name,
                       config::chromeStyleName(variant.row),
                       config::configEnumToken(variant.edition),
                       appearanceName(variant.appearance),
                       facts.platform,
                       facts.devicePixelRatio,
                       facts.chromeFontFamily,
                       facts.controlsStyle,
                       facts.qtVersion);
}

} // namespace contour::test::snapshot
