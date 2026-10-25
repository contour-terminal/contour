// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Config.hpp>
#include <contour/config/GlobalOverrides.hpp>

#include <algorithm>
#include <array>
#include <string>

namespace contour::config
{

namespace
{
    /// The row of the setting held in @p Member of the configuration, overridden as @p shape.
    /// @param key   Its contour.yml key.
    /// @param shape How settings.yml spells it.
    /// @return The row, applied by the loader of @p Member's type.
    template <auto Member>
    constexpr GlobalOverride overridable(std::string_view key, GlobalOverrideShape shape)
    {
        return { .key = key,
                 .shape = shape,
                 .apply = [](YAMLConfigReader& reader, Config& config, std::string const& entry) {
                     reader.loadFromEntry(entry, config.*Member);
                 } };
    }

    /// A top-level setting held in @p Member.
    template <auto Member>
    constexpr GlobalOverride scalar(std::string_view key)
    {
        return overridable<Member>(key, GlobalOverrideShape::Scalar);
    }

    /// A section held in @p Member, whose leaves are overridden one by one.
    template <auto Member>
    constexpr GlobalOverride section(std::string_view key)
    {
        return overridable<Member>(key, GlobalOverrideShape::Section);
    }

    /// Every global setting settings.yml may override, as globalOverrides() hands them out. A section's
    /// row applies its loader by name, so a change to how that loader reads its leaves applies here too.
    constexpr auto GlobalOverrideTable = std::array {
        scalar<&Config::wordDelimiters>("word_delimiters"),
        scalar<&Config::ptyReadBufferSize>("read_buffer_size"),
        scalar<&Config::ptyBufferObjectSize>("pty_buffer_size"),
        scalar<&Config::commandPaletteRecentCount>("command_palette_recent_count"),
        scalar<&Config::spawnNewProcess>("spawn_new_process"),
        scalar<&Config::reflowOnResize>("reflow_on_resize"),
        scalar<&Config::snapToRows>("snap_to_rows"),
        scalar<&Config::rememberWindowPosition>("remember_window_position"),
        scalar<&Config::tabSwitchOnHorizontalWheel>("tab_switch_on_horizontal_wheel"),
        scalar<&Config::accessibilityAnnouncements>("accessibility_announcements"),
        scalar<&Config::accessibilityCaretReporting>("accessibility_caret_reporting"),
        scalar<&Config::hyperlinkHoverTooltip>("hyperlink_hover_tooltip"),
        scalar<&Config::progressTimeout>("progress_timeout"),
        scalar<&Config::notificationCloseTimeout>("notification_close_timeout"),
        scalar<&Config::tabBarPosition>("tab_bar_position"),
        scalar<&Config::tabBarVisibility>("tab_bar_visibility"),
        scalar<&Config::theme>("theme"),
        scalar<&Config::uiStyle>("ui_style"),
        scalar<&Config::windowControlStyle>("window_control_style"),
        scalar<&Config::chromeDensity>("chrome_density"),
        scalar<&Config::uiFontFamily>("ui_font_family"),
        scalar<&Config::uiFontSize>("ui_font_size"),
        scalar<&Config::earlyExitThreshold>("early_exit_threshold"),
        scalar<&Config::textScalingMethod>("text_scaling_method"),
        scalar<&Config::graphemeClustering>("grapheme_clustering"),
        section<&Config::renderer>("renderer"),
        section<&Config::folding>("folding"),
        section<&Config::gutter>("gutter"),
        section<&Config::commandBlocks>("command_blocks"),
    };
} // namespace

std::span<GlobalOverride const> globalOverrides() noexcept
{
    return GlobalOverrideTable;
}

GlobalOverride const* globalOverrideFor(std::string_view keyPath) noexcept
{
    auto const row = std::ranges::find_if(GlobalOverrideTable,
                                          [keyPath](auto const& entry) { return entry.covers(keyPath); });
    return row != GlobalOverrideTable.end() ? &*row : nullptr;
}

} // namespace contour::config
