// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/Shortcut.hpp>
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
};

/// How a script's active tab is divided into panes.
enum class PaneShape : uint8_t
{
    SingleLeaf = 0, //!< One pane.
    SplitThree,     //!< `[ a | split(b, c) ]`, with `a` active.
};

/// What fills the window's content area.
enum class ContentPage : uint8_t
{
    Terminal = 0,
    Settings,
};

/// An interaction performed after the window is shown.
enum class ScriptAction : uint8_t
{
    None = 0,
    OpenPalette,
};

/// Whether the window is attached to a host, and in which state.
enum class HostConnection : uint8_t
{
    Local = 0,
    Reconnecting,
};

/// One row of the script table.
struct SnapshotScriptRow
{
    SnapshotScript script;      ///< The script this row describes.
    std::string_view name;      ///< Its name in file names and the manifest.
    int tabCount;               ///< How many tabs the window holds.
    PaneShape panes;            ///< The active tab's pane layout.
    ContentPage page;           ///< What the content area shows.
    ScriptAction action;        ///< What is done after showing the window.
    HostConnection connection;  ///< The attached host's state.
    std::string_view hostLabel; ///< The attached host's label; empty when local.
};

/// The rows of the script table, in enumerator order.
inline constexpr auto SnapshotScripts = std::array {
    SnapshotScriptRow { .script = SnapshotScript::SingleTab,
                        .name = "single-tab",
                        .tabCount = 1,
                        .panes = PaneShape::SingleLeaf,
                        .page = ContentPage::Terminal,
                        .action = ScriptAction::None,
                        .connection = HostConnection::Local,
                        .hostLabel = "" },
    SnapshotScriptRow { .script = SnapshotScript::ThreeTabs,
                        .name = "three-tabs",
                        .tabCount = 3,
                        .panes = PaneShape::SingleLeaf,
                        .page = ContentPage::Terminal,
                        .action = ScriptAction::None,
                        .connection = HostConnection::Local,
                        .hostLabel = "" },
    SnapshotScriptRow { .script = SnapshotScript::SplitThree,
                        .name = "split-three",
                        .tabCount = 1,
                        .panes = PaneShape::SplitThree,
                        .page = ContentPage::Terminal,
                        .action = ScriptAction::None,
                        .connection = HostConnection::Local,
                        .hostLabel = "" },
    SnapshotScriptRow { .script = SnapshotScript::Palette,
                        .name = "palette",
                        .tabCount = 1,
                        .panes = PaneShape::SingleLeaf,
                        .page = ContentPage::Terminal,
                        .action = ScriptAction::OpenPalette,
                        .connection = HostConnection::Local,
                        .hostLabel = "" },
    SnapshotScriptRow { .script = SnapshotScript::Settings,
                        .name = "settings",
                        .tabCount = 1,
                        .panes = PaneShape::SingleLeaf,
                        .page = ContentPage::Settings,
                        .action = ScriptAction::None,
                        .connection = HostConnection::Local,
                        .hostLabel = "" },
    SnapshotScriptRow { .script = SnapshotScript::RemoteReconnecting,
                        .name = "remote-reconnecting",
                        .tabCount = 1,
                        .panes = PaneShape::SingleLeaf,
                        .page = ContentPage::Terminal,
                        .action = ScriptAction::None,
                        .connection = HostConnection::Reconnecting,
                        .hostLabel = "build-01" },
};

static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, SnapshotScripts.size()),
                                  [](std::size_t i) {
                                      return static_cast<std::size_t>(SnapshotScripts[i].script) == i;
                                  }),
              "SnapshotScripts must list one row per SnapshotScript, in enumerator order");
static_assert(SnapshotScripts.size() == static_cast<std::size_t>(SnapshotScript::RemoteReconnecting) + 1,
              "every SnapshotScript needs a row");

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
    config::ChromeStyle row;          ///< The chrome row.
    config::HostPlatform host;        ///< The platform whose window controls the row shows.
    command::ShortcutStyle shortcuts; ///< How shortcuts are spelled on that platform.
};

/// The rows of the row-host table, in @c config::ChromeStyle order.
inline constexpr auto SnapshotRowHosts = std::array {
    SnapshotRowHost { .row = config::ChromeStyle::MacOS,
                      .host = config::HostPlatform::MacOS,
                      .shortcuts = command::ShortcutStyle::MacGlyphs },
    SnapshotRowHost { .row = config::ChromeStyle::Fluent,
                      .host = config::HostPlatform::Windows,
                      .shortcuts = command::ShortcutStyle::Text },
    SnapshotRowHost { .row = config::ChromeStyle::ContourLinux,
                      .host = config::HostPlatform::Other,
                      .shortcuts = command::ShortcutStyle::Text },
    SnapshotRowHost { .row = config::ChromeStyle::Terminal,
                      .host = config::HostPlatform::Other,
                      .shortcuts = command::ShortcutStyle::Text },
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
///         `resolveWindowControlStyle(WindowControlStyle::Auto, rowHost(row).host)`.
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
    SnapshotScript script;                 ///< The window state.
    config::ChromeStyle row;               ///< The chrome row.
    config::DefaultsEdition edition;       ///< The defaults edition.
    vtbackend::ColorPreference appearance; ///< Dark or light.

    [[nodiscard]] bool operator==(SnapshotVariant const&) const = default;
};

/// The chrome rows whose Quick Controls style is @p controlsStyle.
/// @param controlsStyle The style in effect, as `uiStyleTokens(row).quickControlsStyle` spells it.
/// @return The matching rows, in enumerator order; empty when the name matches none.
[[nodiscard]] inline std::vector<config::ChromeStyle> rowsRenderableUnder(std::string_view controlsStyle)
{
    auto const matches = [&](std::size_t i) {
        return config::uiStyleTokens(static_cast<config::ChromeStyle>(i)).quickControlsStyle == controlsStyle;
    };
    auto rows = std::vector<config::ChromeStyle> {};
    for (auto const i:
         std::views::iota(std::size_t { 0 }, config::detail::ChromeStyleCount) | std::views::filter(matches))
        rows.push_back(static_cast<config::ChromeStyle>(i));
    return rows;
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
    vtbackend::RGBColor terminalBackground; ///< The terminal's default background.
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

/// The Quick Controls style the snapshots render under unless the environment says otherwise.
inline constexpr std::string_view DefaultControlsStyle = "Fusion";
/// The variable that selects the Quick Controls style.
inline constexpr std::string_view ControlsStyleVariable = "CONTOUR_TEST_CONTROLS_STYLE";
/// The variable that names the directory snapshots are written to.
inline constexpr std::string_view SnapshotDirectoryVariable = "CONTOUR_SNAPSHOT_DIR";

/// The Quick Controls style to render under.
/// @param env The environment to read.
/// @return The variable's value, or @c DefaultControlsStyle when it is unset or empty.
[[nodiscard]] inline std::string controlsStyleFor(core::Environment const& env)
{
    if (auto value = env.get(ControlsStyleVariable); value && !value->empty())
        return *std::move(value);
    return std::string { DefaultControlsStyle };
}

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
