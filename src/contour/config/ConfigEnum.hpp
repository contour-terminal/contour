// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigEnumInfo.hpp>

#include <vtbackend/core/LinkDecoration.hpp> // LinkDecorationMode
#include <vtbackend/core/Search.hpp>
#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/screen/GutterSettings.hpp> // LineNumberMode
#include <vtbackend/shell/Folding.hpp>         // FoldJumpBehavior
#include <vtbackend/shell/MarkArbiter.hpp>
#include <vtbackend/shell/ScrollbarMarks.hpp> // ScrollbarMarkSource
#include <vtbackend/shell/StickyHeader.hpp>   // StickyHeaderMode

#include <array>
#include <span>

namespace contour::config
{

namespace detail
{
    /// What a targeted Vi jump does when its target sits inside a collapsed fold.
    ///
    /// Owned by vtbackend, so the table hangs off the type from here -- the reader, the settings-page
    /// combo box and the written-back spelling then all come from this one row set.
    inline constexpr auto FoldJumpBehaviorTable = std::array {
        ConfigEnumInfo<vtbackend::FoldJumpBehavior> {
            vtbackend::FoldJumpBehavior::Expand, "expand", "Expand the block" },
        ConfigEnumInfo<vtbackend::FoldJumpBehavior> {
            vtbackend::FoldJumpBehavior::Skip, "skip", "Stop at the prompt line" },
    };
    /// Whether OSC 3008 may stand in for a shell integration that is not installed.
    inline constexpr auto ContextMarkPolicyTable = std::array {
        ConfigEnumInfo<vtbackend::ContextMarkPolicy> {
            vtbackend::ContextMarkPolicy::Never, "never", "Only OSC 133 leaves marks" },
        ConfigEnumInfo<vtbackend::ContextMarkPolicy> {
            vtbackend::ContextMarkPolicy::WhenAlone, "when_alone", "Only while OSC 133 has said nothing" },
    };

    /// Which context types a color scheme's tint map may paint.
    inline constexpr auto ContextTintScopeTable = std::array {
        ConfigEnumInfo<vtbackend::ContextTintScope> { vtbackend::ContextTintScope::Off, "off", "Never tint" },
        ConfigEnumInfo<vtbackend::ContextTintScope> {
            vtbackend::ContextTintScope::Boundaries, "boundaries", "Only privilege and machine boundaries" },
        ConfigEnumInfo<vtbackend::ContextTintScope> {
            vtbackend::ContextTintScope::All, "all", "Every type the scheme sets" },
    };

    /// How a search compares letters by case.
    ///
    /// Owned by vtbackend, so the table hangs off the type from here, exactly as FoldJumpBehavior's
    /// does -- the reader, the settings-page combo box and the written-back spelling all come from
    /// this one row set.
    inline constexpr auto SearchCaseSensitivityTable = std::array {
        ConfigEnumInfo<vtbackend::SearchCaseSensitivity> {
            vtbackend::SearchCaseSensitivity::Smart, "smart", "Smart (capital makes it exact)" },
        ConfigEnumInfo<vtbackend::SearchCaseSensitivity> {
            vtbackend::SearchCaseSensitivity::Insensitive, "insensitive", "Ignore case" },
        ConfigEnumInfo<vtbackend::SearchCaseSensitivity> {
            vtbackend::SearchCaseSensitivity::Sensitive, "sensitive", "Match case" },
    };

    /// Which line numbers the gutter shows, in vim's terms.
    inline constexpr auto LineNumberModeTable = std::array {
        ConfigEnumInfo<vtbackend::LineNumberMode> { vtbackend::LineNumberMode::Off, "off", "None" },
        ConfigEnumInfo<vtbackend::LineNumberMode> {
            vtbackend::LineNumberMode::Absolute, "absolute", "Absolute (vim: number)" },
        ConfigEnumInfo<vtbackend::LineNumberMode> {
            vtbackend::LineNumberMode::Relative, "relative", "Relative to the cursor (vim: relativenumber)" },
        ConfigEnumInfo<vtbackend::LineNumberMode> {
            vtbackend::LineNumberMode::Hybrid, "hybrid", "Hybrid (vim: number relativenumber)" },
    };

    /// When the sticky command header may cover the top row.
    inline constexpr auto StickyHeaderModeTable = std::array {
        ConfigEnumInfo<vtbackend::StickyHeaderMode> { vtbackend::StickyHeaderMode::Never, "never", "Never" },
        ConfigEnumInfo<vtbackend::StickyHeaderMode> {
            vtbackend::StickyHeaderMode::Scrolled, "scrolled", "While scrolled back" },
        ConfigEnumInfo<vtbackend::StickyHeaderMode> {
            vtbackend::StickyHeaderMode::Always, "always", "Also while following output" },
    };

    /// When links are decorated: hyperlink_decoration.mode.
    inline constexpr auto LinkDecorationModeTable = std::array {
        ConfigEnumInfo<vtbackend::LinkDecorationMode> {
            vtbackend::LinkDecorationMode::Always, "always", "Always" },
        ConfigEnumInfo<vtbackend::LinkDecorationMode> {
            vtbackend::LinkDecorationMode::Hover, "hover", "Only under the pointer" },
        ConfigEnumInfo<vtbackend::LinkDecorationMode> {
            vtbackend::LinkDecorationMode::Modifier, "modifier", "While the follow modifier is held" },
        ConfigEnumInfo<vtbackend::LinkDecorationMode> {
            vtbackend::LinkDecorationMode::Never, "never", "Never" },
    };

    /// The groups of marks the scrollbar can show (`scrollbar.marks`), one token of its list each.
    ///
    /// Owned by vtbackend, so the table hangs off the type from here, as FoldJumpBehavior's does: the
    /// reader and the written-back list both come from these rows.
    inline constexpr auto ScrollbarMarkSourceTable = std::array {
        ConfigEnumInfo<vtbackend::ScrollbarMarkSource> {
            vtbackend::ScrollbarMarkSource::Failures, "failures", "Failed commands" },
        ConfigEnumInfo<vtbackend::ScrollbarMarkSource> {
            vtbackend::ScrollbarMarkSource::Commands, "commands", "Other commands" },
        ConfigEnumInfo<vtbackend::ScrollbarMarkSource> {
            vtbackend::ScrollbarMarkSource::UserMarks, "user_marks", "Vi line marks" },
    };
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<vtbackend::FoldJumpBehavior> const> configEnumValues() noexcept
{
    return detail::FoldJumpBehaviorTable;
}

template <>
constexpr std::span<ConfigEnumInfo<vtbackend::ContextMarkPolicy> const> configEnumValues() noexcept
{
    return detail::ContextMarkPolicyTable;
}

template <>
constexpr std::span<ConfigEnumInfo<vtbackend::ContextTintScope> const> configEnumValues() noexcept
{
    return detail::ContextTintScopeTable;
}

template <>
constexpr std::span<ConfigEnumInfo<vtbackend::SearchCaseSensitivity> const> configEnumValues() noexcept
{
    return detail::SearchCaseSensitivityTable;
}

template <>
constexpr std::span<ConfigEnumInfo<vtbackend::LineNumberMode> const> configEnumValues() noexcept
{
    return detail::LineNumberModeTable;
}

template <>
constexpr std::span<ConfigEnumInfo<vtbackend::StickyHeaderMode> const> configEnumValues() noexcept
{
    return detail::StickyHeaderModeTable;
}

template <>
constexpr std::span<ConfigEnumInfo<vtbackend::LinkDecorationMode> const> configEnumValues() noexcept
{
    return detail::LinkDecorationModeTable;
}

template <>
constexpr std::span<ConfigEnumInfo<vtbackend::ScrollbarMarkSource> const> configEnumValues() noexcept
{
    return detail::ScrollbarMarkSourceTable;
}

} // namespace contour::config
