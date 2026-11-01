// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/AccessibilityPreferences.hpp>
#include <contour/config/ChromeDensity.hpp>
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/MaterialSupport.hpp>
#include <contour/config/UiStyle.hpp>

#include <vtbackend/core/Color.hpp>

#include <array>
#include <cstddef>

// The per-style token rows, apart from the token types in ChromeTokenGroups.hpp: a file that needs a token
// type must not parse (and constant-evaluate) every row.

namespace contour::config
{

namespace detail
{
    /// @param paneGap       The gap between split panes, and their inset from the content edge.
    /// @param paneHeaderGap The gap between a pane header's items.
    /// @return A native row's spacing.
    constexpr SpaceTokens nativeSpace(int paneGap, int paneHeaderGap, int tabStripLeading) noexcept
    {
        return SpaceTokens { .paneGap = paneGap,
                             .splitHit = 10,
                             .paneInset = paneGap,
                             .scale = { 2, 4, 6, 8, 10, 12, 16, 20, 24, 32 },
                             .dragReserve = 40,
                             .revealEdge = 2,
                             .toolbarChipPadding = 9,
                             .tabStripLeading = tabStripLeading,
                             .paneGapRows = paneGap,
                             .paneHeaderGap = paneHeaderGap,
                             .footerLeading = 14,
                             .noticeCardPadX = 14,
                             .noticeCardPadY = 12,
                             .paletteInset = 8,
                             .findBarInset = 8,
                             .findBarInsetRows = 8,
                             .findBarCountGap = 0,
                             .scrollbarInset = 4,
                             .blockGutter = 22,
                             .railInset = 8,
                             .railEnd = 2 };
    }

    inline constexpr MotionTokens NativeMotion { .fastMs = 100,
                                                 .standardMs = 160,
                                                 .emphasizedMs = 220,
                                                 .travel = 8,
                                                 .spinnerMs = 1200,
                                                 .pulseMs = 700,
                                                 .style = MotionStyle::Full,
                                                 .headerRevealMs = 500,
                                                 .popoverScalePercent = 97,
                                                 .lingerMs = 600,
                                                 .tooltipDelayMs = 600,
                                                 .tooltipWarmMs = 1500 };

    inline constexpr ElevationTokens NativeElevation {
        .levels = { ElevationLevel { .offsetX = 0, .offsetY = 0, .blur = 0, .opacityPercent = 0 },
                    ElevationLevel { .offsetX = 0, .offsetY = 1, .blur = 2, .opacityPercent = 12 },
                    ElevationLevel { .offsetX = 0, .offsetY = 4, .blur = 16, .opacityPercent = 25 },
                    ElevationLevel { .offsetX = 0, .offsetY = 12, .blur = 40, .opacityPercent = 35 } }
    };

    inline constexpr ContrastTokens StandardContrast { .separatorPercent = 10,
                                                       .activeCardRing = 0.0,
                                                       .paneDimPercent = std::nullopt };

    /// @param slot The scheme slot.
    /// @return A @c Scheme source at full opacity.
    constexpr ChromeColorSource schemeColor(SchemeSlot slot) noexcept
    {
        return ChromeColorSource { .kind = ColorSourceKind::Scheme,
                                   .rgb = 0,
                                   .status = StatusColorRole::Danger,
                                   .slot = slot,
                                   .alphaPercent = 100,
                                   .level = 0 };
    }

    /// @param level The kit's @c fill step, 1 to 4.
    /// @return A @c Fill source.
    constexpr ChromeColorSource fill(int level) noexcept
    {
        return ChromeColorSource { .kind = ColorSourceKind::Fill,
                                   .rgb = 0,
                                   .status = StatusColorRole::Danger,
                                   .slot = SchemeSlot::Background,
                                   .alphaPercent = 100,
                                   .level = level };
    }

    /// @param level The kit's @c label step, 1 (primary) to 3 (tertiary).
    /// @return A @c Label source.
    constexpr ChromeColorSource label(int level) noexcept
    {
        return ChromeColorSource { .kind = ColorSourceKind::Label,
                                   .rgb = 0,
                                   .status = StatusColorRole::Danger,
                                   .slot = SchemeSlot::Background,
                                   .alphaPercent = 100,
                                   .level = level };
    }

    /// @param level        The kit's @c label step, 1 (primary) to 3 (tertiary).
    /// @param alphaPercent The share of that step's own alpha (LabelAlphas) to draw it at.
    /// @return A @c Label source at a fraction of the step.
    constexpr ChromeColorSource label(int level, int alphaPercent) noexcept
    {
        return ChromeColorSource { .kind = ColorSourceKind::Label,
                                   .rgb = 0,
                                   .status = StatusColorRole::Danger,
                                   .slot = SchemeSlot::Background,
                                   .alphaPercent = alphaPercent,
                                   .level = level };
    }

    /// @param level The kit's surface: 1 elevated, 2 secondary.
    /// @return A @c Surface source.
    constexpr ChromeColorSource surface(int level) noexcept
    {
        return ChromeColorSource { .kind = ColorSourceKind::Surface,
                                   .rgb = 0,
                                   .status = StatusColorRole::Danger,
                                   .slot = SchemeSlot::Background,
                                   .alphaPercent = 100,
                                   .level = level };
    }

    /// @param slot The scheme hue to ink with where it reads on the bar.
    /// @return A @c SchemeOnBar source.
    constexpr ChromeColorSource schemeOnBar(SchemeSlot slot) noexcept
    {
        return ChromeColorSource { .kind = ColorSourceKind::SchemeOnBar,
                                   .rgb = 0,
                                   .status = StatusColorRole::Danger,
                                   .slot = slot,
                                   .alphaPercent = 100,
                                   .level = 0 };
    }

    /// @param slot  The scheme slot to shade.
    /// @param level The HSL lightness points to move it by; negative darkens.
    /// @return A @c SchemeShade source.
    constexpr ChromeColorSource schemeShade(SchemeSlot slot, int level) noexcept
    {
        return ChromeColorSource { .kind = ColorSourceKind::SchemeShade,
                                   .rgb = 0,
                                   .status = StatusColorRole::Danger,
                                   .slot = slot,
                                   .alphaPercent = 100,
                                   .level = level };
    }

    /// @param lightRgb          The colour on a light chrome, @c 0xRRGGBB.
    /// @param lightAlphaPercent Its opacity there.
    /// @param darkRgb           The colour on a dark chrome, @c 0xRRGGBB.
    /// @param darkAlphaPercent  Its opacity there.
    /// @return A @c Tone source.
    constexpr ChromeColorSource tone(uint32_t lightRgb,
                                     int lightAlphaPercent,
                                     uint32_t darkRgb,
                                     int darkAlphaPercent) noexcept
    {
        return ChromeColorSource { .kind = ColorSourceKind::Tone,
                                   .rgb = lightRgb,
                                   .status = StatusColorRole::Danger,
                                   .slot = SchemeSlot::Background,
                                   .alphaPercent = lightAlphaPercent,
                                   .level = 0,
                                   .darkRgb = darkRgb,
                                   .darkAlphaPercent = darkAlphaPercent };
    }

    /// @return A source that paints nothing.
    constexpr ChromeColorSource noColor() noexcept
    {
        return ChromeColorSource { .kind = ColorSourceKind::Fixed,
                                   .rgb = 0,
                                   .status = StatusColorRole::Danger,
                                   .slot = SchemeSlot::Background,
                                   .alphaPercent = 0,
                                   .level = 0 };
    }

    /// @return A @c Separator source: the kit's separator ink, by the ground's tone.
    constexpr ChromeColorSource separator() noexcept
    {
        return ChromeColorSource { .kind = ColorSourceKind::Separator,
                                   .rgb = 0,
                                   .status = StatusColorRole::Danger,
                                   .slot = SchemeSlot::Background,
                                   .alphaPercent = 100,
                                   .level = 0 };
    }

    /// @param role The status hue.
    /// @return A @c StatusMark source at full opacity.
    constexpr ChromeColorSource statusMarkSource(StatusColorRole role) noexcept
    {
        return ChromeColorSource { .kind = ColorSourceKind::StatusMark,
                                   .rgb = 0,
                                   .status = role,
                                   .slot = SchemeSlot::Background,
                                   .alphaPercent = 100,
                                   .level = 0 };
    }

    /// @param kind A source kind that reads no field, e.g. @c PaletteHighlight.
    /// @return That kind at full opacity.
    constexpr ChromeColorSource sourceOf(ColorSourceKind kind) noexcept
    {
        return ChromeColorSource { .kind = kind,
                                   .rgb = 0,
                                   .status = StatusColorRole::Danger,
                                   .slot = SchemeSlot::Background,
                                   .alphaPercent = 100,
                                   .level = 0 };
    }

    inline constexpr ChromeColorSource StatusWarning { .kind = ColorSourceKind::Status,
                                                       .rgb = 0,
                                                       .status = StatusColorRole::Warning,
                                                       .slot = SchemeSlot::Background,
                                                       .alphaPercent = 100,
                                                       .level = 0 };

    /// The native rows' colours: the 1a roles, then the kit's tables for the band (kit-components).
    inline constexpr std::array<ChromeColorSource, ChromeColorRoleCount> NativeColors {
        ChromeColorSource { .kind = ColorSourceKind::PaletteWindow,
                            .rgb = 0,
                            .status = StatusColorRole::Danger,
                            .slot = SchemeSlot::Background,
                            .alphaPercent = 100,
                            .level = 0 },
        ChromeColorSource { .kind = ColorSourceKind::PaletteWindowText,
                            .rgb = 0,
                            .status = StatusColorRole::Danger,
                            .slot = SchemeSlot::Background,
                            .alphaPercent = 100,
                            .level = 0 },
        StatusWarning,
        ChromeColorSource { .kind = ColorSourceKind::Fixed,
                            .rgb = 0x28282C,
                            .status = StatusColorRole::Danger,
                            .slot = SchemeSlot::Background,
                            .alphaPercent = 92,
                            .level = 0 },
        ChromeColorSource { .kind = ColorSourceKind::Fixed,
                            .rgb = 0xFFFFFF,
                            .status = StatusColorRole::Danger,
                            .slot = SchemeSlot::Background,
                            .alphaPercent = 100,
                            .level = 0 },
        ChromeColorSource { .kind = ColorSourceKind::PaletteWindowText,
                            .rgb = 0,
                            .status = StatusColorRole::Danger,
                            .slot = SchemeSlot::Background,
                            .alphaPercent = 100,
                            .level = 0 },
        fill(4),    // TabWellFill
        surface(1), // TabActiveFill
        fill(3),    // TabHoverFill
        fill(3),    // TabCloseFill
        fill(2),    // ControlPressedFill
        label(2),   // TabText
        label(1),   // TabTextActive
        label(3),   // TabTextDimmed
        label(1),   // TabTextTinted
        fill(4),    // ChipFill
        label(2),   // ChipText
        label(1),   // ChipTextStrong
        fill(4),    // CaptionWash
        label(2),   // CaptionGlyphPressed
        label(3),   // CaptionGlyphInactive
        // The gap is the chrome scheme's background 4 L points darker (mac-dark: #101216 under #1C1E24).
        schemeShade(SchemeSlot::Background, -4), // PaneGapFill
        separator(),                             // PaneCardBorder
        noColor(),                               // PaneHeaderFill: the card shows through.
        label(1),                                // PaneHeaderText
        label(2),                                // PaneHeaderTextDimmed
        separator(),                             // PaneSplitLine
        // The canvas's 28 % grabber and 8 % footer are not kit steps; the nearest ones are.
        fill(1),  // PaneGrabber
        fill(3),  // PaneFooterFill
        label(1), // PaneFooterText
        // The kit's strongest fill, by the pane's own tone: the band's tabHoverFill follows the chrome's.
        fill(1), // PaneHeaderActionHover
        // The palette is the kit's elevated surface; its selection is the OS's own selected-row pair
        // (white on the accent reads 4.0:1 where macOS's selected-content colour reads 5.3:1).
        surface(1),                                        // PaletteSurface
        label(1),                                          // PaletteText
        label(2),                                          // PaletteSecondaryText
        sourceOf(ColorSourceKind::PaletteHighlight),       // PaletteSelectionFill
        sourceOf(ColorSourceKind::PaletteHighlightedText), // PaletteSelectionText
        separator(),                                       // PaletteSeparator
        // A lit toggle is accent-tinted, its 22 % wash a mark (1c's text/mark split). Its glyph is the
        // primary label, not the accent: the accent is the user's, and a dark one on its own wash on a dark
        // chrome falls to 1.3:1.
        ChromeColorSource { .kind = ColorSourceKind::StatusMark,
                            .rgb = 0,
                            .status = StatusColorRole::Running,
                            .slot = SchemeSlot::Background,
                            .alphaPercent = 22,
                            .level = 0 }, // FindToggleOnFill
        label(1),                         // FindToggleOnText
        label(2),                         // FindToggleText
        label(2),                         // FindCountText
        // The wash alone can fall under 3:1 against an unlit toggle, so a lit one is also ringed: the accent,
        // or the chrome's ink where the accent would not reach 3:1 on the window (B-I4).
        ChromeColorSource { .kind = ColorSourceKind::StatusMarkOnWindow,
                            .rgb = 0,
                            .status = StatusColorRole::Running,
                            .slot = SchemeSlot::Background,
                            .alphaPercent = 100,
                            .level = 0 }, // FindToggleOnRing
        // The Danger status hue as text, by the chrome's tone: what UiStyleProvider::errorColor paints.
        ChromeColorSource { .kind = ColorSourceKind::Status,
                            .rgb = 0,
                            .status = StatusColorRole::Danger,
                            .slot = SchemeSlot::Background,
                            .alphaPercent = 100,
                            .level = 0 }, // FindErrorText
        // The link pill's hint: white at 60 % on the Toast's fill. The board's 50 % reads 4.3:1 over a light
        // pane, 60 % reads 5.3:1 (R-h).
        ChromeColorSource { .kind = ColorSourceKind::Fixed,
                            .rgb = 0xFFFFFF,
                            .status = StatusColorRole::Danger,
                            .slot = SchemeSlot::Background,
                            .alphaPercent = 60,
                            .level = 0 }, // ToastTextSecondary
        // label.1 at about 30, 50 and 10 % on either chrome: a share of the kit's own alpha (88 / 92).
        label(1, 33), // ScrollbarThumb
        label(1, 55), // ScrollbarThumbActive
        label(1, 11), // ScrollbarTrack
        // The ring is a mark, so it takes the accent only where that reaches 3:1 on the window, else the
        // window's own ink (as the find toggle's ring does). The board's accent at 60 % would not: over a
        // light window it reads about 1.9:1.
        ChromeColorSource { .kind = ColorSourceKind::StatusMarkOnWindow,
                            .rgb = 0,
                            .status = StatusColorRole::Running,
                            .slot = SchemeSlot::Background,
                            .alphaPercent = 100,
                            .level = 0 }, // FocusRing
        // The ring leaves the item's own fill alone.
        noColor(), // FocusFill
        // The Settings window: the kit's secondary surface for the sidebar and its elevated one for a group,
        // on the OS's own window colour; the sidebar's selection is the OS's selected-row pair.
        surface(2),                                        // SettingsSidebarFill
        sourceOf(ColorSourceKind::PaletteWindow),          // SettingsPageFill
        surface(1),                                        // SettingsGroupFill
        separator(),                                       // SettingsSeparator
        sourceOf(ColorSourceKind::PaletteWindowText),      // SettingsLabel
        label(2),                                          // SettingsHelp
        sourceOf(ColorSourceKind::PaletteHighlight),       // SettingsSelectionFill
        sourceOf(ColorSourceKind::PaletteHighlightedText), // SettingsSelectionText
        // The primary label, not the accent: macOS's blue reads 4.0:1 on a white group, under the 4.5:1 a
        // word needs (as FindToggleOnText, above).
        label(1), // SettingsModified
        // A neutral tile with a secondary glyph, so one table serves the rows that draw the tile and those
        // that draw the glyph on the group.
        fill(3),  // SettingsIconTile
        label(2), // SettingsIconGlyph
        // A lit switch is a mark in the accent; an unlit one, the tertiary label at 95 % of its alpha, which
        // stands off the group at 3:1 on either chrome, with a white knob that stands off it in turn (the
        // full alpha leaves the knob 2.99:1 on the dark elevated surface).
        statusMarkSource(StatusColorRole::Running), // SwitchOnFill
        label(3, 95),                               // SwitchOffFill
        ChromeColorSource { .kind = ColorSourceKind::Fixed,
                            .rgb = 0xFFFFFF,
                            .status = StatusColorRole::Danger,
                            .slot = SchemeSlot::Background,
                            .alphaPercent = 100,
                            .level = 0 }, // SwitchKnob
        fill(4),                          // ControlFill
        label(1),                         // ControlText
        separator(),                      // PreviewFrame
        fill(3),                          // WindowButtonFill: the Linux board's white 11 % on the bar.
        fill(2),                          // WindowButtonHoverFill
        fill(1),                          // WindowButtonPressedFill
        label(1),                         // WindowButtonGlyph: the bar's own ink.
        separator(),                      // HeaderSeparator
        // The macOS tooltip: a near-white (#F6F6F6) or the elevated dark surface, the label at 85 / 88 %
        // and the shortcut at 45 / 50 % of the ink, a hairline of 18 / 14 %.
        tone(0xF6F6F6, 100, 0x2C2C2E, 100), // TooltipFill
        tone(0x000000, 85, 0xFFFFFF, 88),   // TooltipText
        tone(0x000000, 45, 0xFFFFFF, 50),   // TooltipTextSecondary
        tone(0x000000, 18, 0xFFFFFF, 14),   // TooltipEdge
    };

    /// The Windows 11 row's colours: the native ones, with Fluent's own tooltip (#F9F9F9 / #2C2C2C).
    inline constexpr std::array<ChromeColorSource, ChromeColorRoleCount> FluentColors = [] {
        auto colors = NativeColors;
        colors[static_cast<size_t>(ChromeColorRole::TooltipFill)] = tone(0xF9F9F9, 100, 0x2C2C2C, 100);
        colors[static_cast<size_t>(ChromeColorRole::TooltipText)] = tone(0x000000, 90, 0xFFFFFF, 100);
        colors[static_cast<size_t>(ChromeColorRole::TooltipTextSecondary)] = tone(0x000000, 55, 0xFFFFFF, 60);
        colors[static_cast<size_t>(ChromeColorRole::TooltipEdge)] = tone(0x000000, 6, 0xFFFFFF, 8);
        return colors;
    }();

    /// ContourLinux's colours: the native ones, with the Linux board's active tab, white 11 % on the bar
    /// (the kit's fill.3) rather than an elevated surface.
    inline constexpr std::array<ChromeColorSource, ChromeColorRoleCount> LinuxColors = [] {
        auto colors = NativeColors;
        colors[static_cast<size_t>(ChromeColorRole::TabActiveFill)] = fill(3);
        // The Linux board's tooltip: white / #38383D, the label at 87 / 92 %, the shortcut at 55 %.
        colors[static_cast<size_t>(ChromeColorRole::TooltipFill)] = tone(0xFFFFFF, 100, 0x38383D, 100);
        colors[static_cast<size_t>(ChromeColorRole::TooltipText)] = tone(0x000000, 87, 0xFFFFFF, 92);
        colors[static_cast<size_t>(ChromeColorRole::TooltipTextSecondary)] = tone(0x000000, 55, 0xFFFFFF, 55);
        colors[static_cast<size_t>(ChromeColorRole::TooltipEdge)] = tone(0x000000, 12, 0xFFFFFF, 10);
        return colors;
    }();

    /// Every row's unfocused-pane dim (kit-tokens' @c pane.dim).
    inline constexpr DimTokens PaneDim { .darkPercent = 15, .lightPercent = 10 };

    /// The native rows' Minimal extents: no header, a hairline gap, a narrower hit area, no inset or radius.
    inline constexpr MinimalDensityTokens NativeMinimal {
        .paneHeader = 0, .paneGap = 1, .paneGapRows = 1, .splitHit = 8, .paneInset = 0, .paneCardRadius = 0
    };

    /// The native rows' block chips, the canvas's `blocks` board: 11-pt labels -- semibold for a failure and
    /// a running command, regular for a success's duration and the fold chip -- the status pills padded 1 x 8
    /// at radius 9, the fold chip 0 x 10, 20 pt tall at radius 10 (size.foldChip, radius.chip) with a 0.5-pt
    /// ring; a rail mark's glyph 9 pt bold on its 13-pt disc (size.railMark); a 1-pt ring around a hovered
    /// block.
    inline constexpr BlockChipTokens NativeBlockChips {
        .pills = { PillTokens { .type = 11, .weight = 400 },
                   PillTokens { .type = 11, .weight = 600, .padX = 8, .padY = 1, .radius = 9 },
                   PillTokens { .type = 11, .weight = 600, .padX = 8, .padY = 1, .radius = 9 },
                   PillTokens {
                       .type = 11, .weight = 400, .padX = 10, .height = 20, .radius = 10, .ring = 0.5 } },
        .markType = 9,
        .markWeight = 700,
        .hoverRing = 1,
    };

    /// The native rows' status inks, indexed by @c StatusColorRole: the status hues' mark inks, since every
    /// ink read from here is a mark (a badge, the host chip's dot, a progress bar).
    inline constexpr std::array<ChromeColorSource, StatusColorRoleCount> NativeStatusInk {
        statusMarkSource(StatusColorRole::Danger),
        statusMarkSource(StatusColorRole::Warning),
        statusMarkSource(StatusColorRole::Success),
        statusMarkSource(StatusColorRole::Running),
    };

    /// How much of a row's label and control remain where it cannot be edited (preflight F16).
    inline constexpr int SettingsDisabledPercent = 35;

    /// The Settings window on the macOS row (mac-settings).
    inline constexpr SettingsTokens MacSettings { .sidebarWidth = 230,
                                                  .sidebarRow = 28,
                                                  .iconTile = 20,
                                                  .iconGlyph = 12,
                                                  .searchField = 28,
                                                  .headerBar = 52,
                                                  .listMaxWidth = 640,
                                                  .groupRadius = 10,
                                                  .row = 44,
                                                  .rowWithHelp = 52,
                                                  .rowPadding = 14,
                                                  .cardGap = 0,
                                                  .groupGap = 16,
                                                  .groupTitleGap = 6,
                                                  .control = 22,
                                                  .switchWidth = 32,
                                                  .switchHeight = 19,
                                                  .slider = 160,
                                                  .stepperField = 72,
                                                  .colorWell = 22,
                                                  .previewHeight = 104,
                                                  .previewWidth = 0,
                                                  .previewRadius = 10,
                                                  .disabledPercent = SettingsDisabledPercent,
                                                  .labelType = 13,
                                                  .helpType = 11,
                                                  .groupTitleType = 13,
                                                  .pageTitleType = 15 };

    /// The Settings window on the Fluent row (win-settings).
    inline constexpr SettingsTokens FluentSettings { .sidebarWidth = 290,
                                                     .sidebarRow = 36,
                                                     .iconTile = 0,
                                                     .iconGlyph = 16,
                                                     .searchField = 32,
                                                     .headerBar = 64,
                                                     .listMaxWidth = 0,
                                                     .groupRadius = 4,
                                                     .row = 68,
                                                     .rowWithHelp = 68,
                                                     .rowPadding = 16,
                                                     .cardGap = 4,
                                                     .groupGap = 20,
                                                     .groupTitleGap = 6,
                                                     .control = 32,
                                                     .switchWidth = 40,
                                                     .switchHeight = 20,
                                                     .slider = 180,
                                                     .stepperField = 120,
                                                     .colorWell = 32,
                                                     .previewHeight = 128,
                                                     .previewWidth = 360,
                                                     .previewRadius = 6,
                                                     .disabledPercent = SettingsDisabledPercent,
                                                     .labelType = 14,
                                                     .helpType = 12,
                                                     .groupTitleType = 14,
                                                     .pageTitleType = 28 };

    /// The Settings window on the ContourLinux row (linux-settings).
    inline constexpr SettingsTokens LinuxSettings { .sidebarWidth = 270,
                                                    .sidebarRow = 38,
                                                    .iconTile = 0,
                                                    .iconGlyph = 16,
                                                    .searchField = 34,
                                                    .headerBar = 46,
                                                    .listMaxWidth = 640,
                                                    .groupRadius = 12,
                                                    .row = 52,
                                                    .rowWithHelp = 60,
                                                    .rowPadding = 14,
                                                    .cardGap = 0,
                                                    .groupGap = 22,
                                                    .groupTitleGap = 10,
                                                    .control = 34,
                                                    .switchWidth = 40,
                                                    .switchHeight = 22,
                                                    .slider = 160,
                                                    .stepperField = 72,
                                                    .colorWell = 34,
                                                    .previewHeight = 120,
                                                    .previewWidth = 0,
                                                    .previewRadius = 12,
                                                    .disabledPercent = SettingsDisabledPercent,
                                                    .labelType = 14,
                                                    .helpType = 12.5,
                                                    .groupTitleType = 14,
                                                    .pageTitleType = 15 };

    /// The Settings window on the Terminal row (tui), in cells; the type is the cell font's own.
    inline constexpr SettingsTokens TerminalSettings { .sidebarWidth = 24,
                                                       .sidebarRow = 1,
                                                       .iconTile = 0,
                                                       .iconGlyph = 1,
                                                       .searchField = 1,
                                                       .headerBar = 1,
                                                       .listMaxWidth = 80,
                                                       .groupRadius = 0,
                                                       .row = 1,
                                                       .rowWithHelp = 2,
                                                       .rowPadding = 1,
                                                       .cardGap = 0,
                                                       .groupGap = 1,
                                                       .groupTitleGap = 0,
                                                       .control = 1,
                                                       .switchWidth = 3,
                                                       .switchHeight = 1,
                                                       .slider = 11,
                                                       .stepperField = 7,
                                                       .colorWell = 2,
                                                       .previewHeight = 6,
                                                       .previewWidth = 0,
                                                       .previewRadius = 0,
                                                       .disabledPercent = SettingsDisabledPercent,
                                                       .labelType = 0,
                                                       .helpType = 0,
                                                       .groupTitleType = 0,
                                                       .pageTitleType = 0 };

    inline constexpr ChromeTokenRow MacRow {
        .size = { .titleBar = 38,
                  .tabWell = 28,
                  .tab = 24,
                  .control = 28,
                  .paneHeader = 22,
                  .toast = 36,
                  .tooltipPadX = 8,
                  .tooltipPadTop = 3,
                  .tooltipPadBottom = 3,
                  .tooltipEdge = 0,
                  .tooltipGap = 14,
                  .tooltipMaxWidth = 360,
                  .tooltipElevation = 2,
                  .dialogWidth = 280,
                  .swatchWidth = 16,
                  .swatchHeight = 16,
                  .settingsWidth = 960,
                  .settingsHeight = 820,
                  .settingsMinWidth = 720,
                  .settingsMinHeight = 480,
                  .tabIcon = 14,
                  .tabClose = 16,
                  .badge = 14,
                  .badgeDot = 6,
                  .progressBar = 2,
                  .hostChip = 24,
                  .hostDot = 7,
                  .toolbarButton = 28,
                  .toolbarChip = 22,
                  .stripButton = 0,
                  .profileButton = 0,
                  .flyoutSwatch = 30,
                  .bareBand = 32,
                  .grabber = 2,
                  .grabberLength = 32,
                  .paneHeaderAction = 22,
                  .paneFooter = 34,
                  .footerButton = 24,
                  .noticeCard = 330,
                  .paletteWidth = 640,
                  .paletteMaxHeight = 420,
                  .paletteField = 44,
                  .paletteRow = 32,
                  .paletteGroupHeader = 24,
                  .paletteFooter = 28,
                  .connectWidth = 560,
                  .connectHeight = 520,
                  .connectRow = 48,
                  .findBar = 32,
                  .findBarWidth = 380,
                  .findToggleWidth = 24,
                  .findToggleHeight = 22,
                  .findButtonWidth = 22,
                  .findButtonHeight = 22,
                  .findIcon = 14,
                  .linkPill = 22,
                  .scrollbar = 8,
                  .secureInputGlyph = 11,
                  .focusRing = 3,
                  .rail = 3,
                  .railMark = 13,
                  .blockActions = 22,
                  .foldChip = 20,
                  .foldChipHit = 24,
                  .scrollTick = 3,
                  .scrollTickOutline = 1,
                  // The kit draws no round buttons here; window_control_style: linux takes ContourLinux's.
                  .windowButton = 24,
                  .windowButtonGlyph = 10,
                  .headerIcon = 16,
                  .controlsSeparator = 0 },
        .space = nativeSpace(6, 6, 0),
        .radii = { .control = 6,
                   .popover = 14,
                   .menu = 10,
                   .menuRow = 5,
                   .paneCard = 10,
                   .noticeCard = 12,
                   .toast = 18,
                   .tooltip = 6,
                   .tab = 12,
                   .tabWell = 14,
                   .tabIcon = 3,
                   .tabClose = 8,
                   .badge = 7,
                   .hostChip = 12,
                   .toolbarChip = 11,
                   .grabber = 1,
                   .findBar = 8,
                   .linkPill = 11,
                   .scrollbar = 4,
                   .rail = 2,
                   .blockActions = 6,
                   .chip = 10,
                   .window = 0,
                   .windowButton = 12,
                   .quickTerminal = 14 },
        .type = { .caption = 10,
                  .footnote = 11,
                  .body = 13,
                  .tab = 12,
                  .title = 15,
                  .paneHeader = 11,
                  .paneHeaderWeight = 500,
                  .paneHeaderAction = 15,
                  .paletteInput = 17,
                  .shortcut = 12 },
        .motion = NativeMotion,
        .elevation = NativeElevation,
        // The band is Solid, not Glass (ruling S6): under AppKit's title-bar material the NSTitlebarView
        // takes the band's clicks, so the band is the client's paint over the compact toolbar. Glass behind
        // it returns with a within-window NSVisualEffectView of our own, behind the Qt content.
        .material = { .titleBand = Material::Solid, .popover = Material::Glass },
        .contrast = StandardContrast,
        .tui = { .inactivePaneOpacity = 1.0, .toastStyle = ToastStyle::Floating },
        .layout = { .dialog = DialogPlacement::Sheet,
                    .menu = MenuPresentation::Native,
                    .tabShape = TabShape::Pill,
                    .profileMenu = ProfileMenuAffordance::SecondaryClick,
                    .tabColors = TabColorSource::Roles,
                    .activeTabWeight = ActiveTabWeight::Regular,
                    .paneActions = PaneActionsReveal::OnHover,
                    .paletteTopPercent = 18,
                    .findBar = FindBarHost::Overlay,
                    .blockColumn = vtbackend::BlockColumnForm::Rail,
                    .settings = SettingsLayout::InsetGrouped,
                    .headerBar = HeaderBarArrangement::Band },
        .tint = { .tabPercent = 20 },
        .colors = NativeColors,
        .statusInk = NativeStatusInk,
        .dim = PaneDim,
        .minimal = NativeMinimal,
        .blockChips = NativeBlockChips,
        .settings = MacSettings,
    };

    inline constexpr ChromeTokenRow FluentRow {
        .size = { .titleBar = 40,
                  .tabWell = 0,
                  .tab = 34,
                  .control = 32,
                  .paneHeader = 28,
                  .toast = 36,
                  .tooltipPadX = 9,
                  .tooltipPadTop = 5,
                  .tooltipPadBottom = 7,
                  .tooltipEdge = 1,
                  .tooltipGap = 14,
                  .tooltipMaxWidth = 360,
                  .tooltipElevation = 2,
                  .dialogWidth = 280,
                  .swatchWidth = 16,
                  .swatchHeight = 16,
                  .settingsWidth = 1110,
                  .settingsHeight = 920,
                  .settingsMinWidth = 720,
                  .settingsMinHeight = 480,
                  .tabIcon = 16,
                  .tabClose = 24,
                  .badge = 14,
                  .badgeDot = 6,
                  .progressBar = 2,
                  .hostChip = 24,
                  .hostDot = 7,
                  .toolbarButton = 32,
                  .toolbarChip = 22,
                  .stripButton = 30,
                  .profileButton = 28,
                  .flyoutSwatch = 30,
                  .bareBand = 28,
                  .grabber = 2,
                  .grabberLength = 32,
                  .paneHeaderAction = 22,
                  .paneFooter = 34,
                  .footerButton = 24,
                  .noticeCard = 330,
                  .paletteWidth = 640,
                  .paletteMaxHeight = 420,
                  .paletteField = 44,
                  .paletteRow = 36,
                  .paletteGroupHeader = 24,
                  .paletteFooter = 28,
                  .connectWidth = 560,
                  .connectHeight = 520,
                  .connectRow = 52,
                  .findBar = 32,
                  .findBarWidth = 380,
                  .findToggleWidth = 24,
                  .findToggleHeight = 22,
                  .findButtonWidth = 22,
                  .findButtonHeight = 22,
                  .findIcon = 14,
                  .linkPill = 22,
                  .scrollbar = 8,
                  .secureInputGlyph = 11,
                  .focusRing = 2,
                  .rail = 3,
                  .railMark = 13,
                  .blockActions = 22,
                  .foldChip = 20,
                  .foldChipHit = 24,
                  .scrollTick = 3,
                  .scrollTickOutline = 1,
                  // The kit draws no round buttons here; window_control_style: linux takes ContourLinux's.
                  .windowButton = 24,
                  .windowButtonGlyph = 10,
                  .headerIcon = 16,
                  .controlsSeparator = 0 },
        .space = nativeSpace(4, 8, 8),
        .radii = { .control = 4,
                   .popover = 8,
                   .menu = 8,
                   .menuRow = 4,
                   .paneCard = 4,
                   .noticeCard = 8,
                   .toast = 18,
                   .tooltip = 8,
                   .tab = 6,
                   .tabWell = 0,
                   .tabIcon = 3,
                   .tabClose = 4,
                   .badge = 7,
                   .hostChip = 12,
                   .toolbarChip = 11,
                   .grabber = 1,
                   .findBar = 8,
                   .linkPill = 11,
                   .scrollbar = 4,
                   .rail = 2,
                   .blockActions = 6,
                   .chip = 10,
                   .window = 8,
                   .windowButton = 12,
                   .quickTerminal = 8 },
        .type = { .caption = 12,
                  .footnote = 12,
                  .body = 14,
                  .tab = 12,
                  .title = 20,
                  .paneHeader = 12,
                  .paneHeaderWeight = 600,
                  .paneHeaderAction = 15,
                  .paletteInput = 16,
                  .shortcut = 12 },
        .motion = NativeMotion,
        .elevation = NativeElevation,
        .material = { .titleBand = Material::Mica, .popover = Material::Solid },
        .contrast = StandardContrast,
        .tui = { .inactivePaneOpacity = 1.0, .toastStyle = ToastStyle::Floating },
        .layout = { .dialog = DialogPlacement::Centered,
                    // In-scene, not native (see docs/internals/native-chrome.md): Windows' native menu is
                    // an HMENU, which draws no swatch row, ignores the terminal scheme the chrome follows and
                    // has no supported dark mode.
                    .menu = MenuPresentation::InScene,
                    .tabShape = TabShape::Joined,
                    .profileMenu = ProfileMenuAffordance::Caret,
                    .tabColors = TabColorSource::Palette,
                    .activeTabWeight = ActiveTabWeight::Regular,
                    .paneActions = PaneActionsReveal::OnHover,
                    .paletteTopPercent = 18,
                    .findBar = FindBarHost::Overlay,
                    .blockColumn = vtbackend::BlockColumnForm::Rail,
                    .settings = SettingsLayout::Cards,
                    .headerBar = HeaderBarArrangement::Band },
        .tint = { .tabPercent = 20 },
        .colors = FluentColors,
        .statusInk = NativeStatusInk,
        .dim = PaneDim,
        .minimal = NativeMinimal,
        .blockChips = NativeBlockChips,
        .settings = FluentSettings,
    };

    inline constexpr ChromeTokenRow LinuxRow {
        .size = { .titleBar = 46,
                  .tabWell = 0,
                  .tab = 34,
                  .control = 34,
                  .paneHeader = 28,
                  .toast = 36,
                  .tooltipPadX = 10,
                  .tooltipPadTop = 5,
                  .tooltipPadBottom = 5,
                  .tooltipEdge = 1,
                  .tooltipGap = 16,
                  .tooltipMaxWidth = 360,
                  .tooltipElevation = 2,
                  .dialogWidth = 280,
                  .swatchWidth = 16,
                  .swatchHeight = 16,
                  .settingsWidth = 1100,
                  .settingsHeight = 920,
                  .settingsMinWidth = 720,
                  .settingsMinHeight = 480,
                  .tabIcon = 14,
                  .tabClose = 16,
                  .badge = 14,
                  .badgeDot = 6,
                  .progressBar = 2,
                  .hostChip = 24,
                  .hostDot = 7,
                  .toolbarButton = 34,
                  .toolbarChip = 22,
                  .stripButton = 0,
                  .profileButton = 0,
                  .flyoutSwatch = 30,
                  .bareBand = 28,
                  .grabber = 2,
                  .grabberLength = 32,
                  .paneHeaderAction = 22,
                  .paneFooter = 34,
                  .footerButton = 24,
                  .noticeCard = 330,
                  .paletteWidth = 640,
                  .paletteMaxHeight = 420,
                  .paletteField = 44,
                  .paletteRow = 38,
                  .paletteGroupHeader = 24,
                  .paletteFooter = 28,
                  .connectWidth = 560,
                  .connectHeight = 520,
                  .connectRow = 54,
                  .findBar = 34, // The kit's, the controls' height.
                  .findBarWidth = 380,
                  .findToggleWidth = 24,
                  .findToggleHeight = 22,
                  .findButtonWidth = 22,
                  .findButtonHeight = 22,
                  .findIcon = 14,
                  .linkPill = 22,
                  .scrollbar = 8,
                  .secureInputGlyph = 11,
                  .focusRing = 2,
                  .rail = 3,
                  .railMark = 13,
                  .blockActions = 22,
                  .foldChip = 20,
                  .foldChipHit = 24,
                  .scrollTick = 3,
                  .scrollTickOutline = 1,
                  .windowButton = 24,
                  .windowButtonGlyph = 10,
                  .headerIcon = 16,
                  .controlsSeparator = 20 },
        .space = nativeSpace(6, 8, 0),
        .radii = { .control = 8,
                   .popover = 12,
                   .menu = 10,
                   .menuRow = 6,
                   .paneCard = 6,
                   .noticeCard = 12,
                   .toast = 18,
                   .tooltip = 8,
                   .tab = 8,
                   .tabWell = 0,
                   .tabIcon = 3,
                   .tabClose = 8,
                   .badge = 7,
                   .hostChip = 12,
                   .toolbarChip = 11,
                   .grabber = 1,
                   .findBar = 8,
                   .linkPill = 11,
                   .scrollbar = 4,
                   .rail = 2,
                   .blockActions = 6,
                   .chip = 10,
                   .window = 12,
                   .windowButton = 12,
                   .quickTerminal = 12 },
        .type = { .caption = 12,
                  .footnote = 12.5,
                  .body = 14,
                  .tab = 13.5,
                  .title = 15,
                  .paneHeader = 12.5,
                  .paneHeaderWeight = 600,
                  .paneHeaderAction = 15,
                  .paletteInput = 16,
                  .shortcut = 12.5 },
        .motion = NativeMotion,
        .elevation = NativeElevation,
        .material = { .titleBand = Material::Solid, .popover = Material::Solid },
        .contrast = StandardContrast,
        .tui = { .inactivePaneOpacity = 1.0, .toastStyle = ToastStyle::Floating },
        .layout = { .dialog = DialogPlacement::Centered,
                    .menu = MenuPresentation::InScene,
                    // The Linux header bar: pills on the bar, new tab leading (a secondary click on it opens
                    // the profiles), search and the main menu trailing.
                    .tabShape = TabShape::Pill,
                    .profileMenu = ProfileMenuAffordance::SecondaryClick,
                    // The pills draw the roles: the board's active tab is fill.3 on the bar (TabActiveFill).
                    .tabColors = TabColorSource::Roles,
                    .activeTabWeight = ActiveTabWeight::Regular,
                    .paneActions = PaneActionsReveal::OnHover,
                    .paletteTopPercent = 18,
                    .findBar = FindBarHost::Overlay,
                    .blockColumn = vtbackend::BlockColumnForm::Rail,
                    .settings = SettingsLayout::BoxedList,
                    .headerBar = HeaderBarArrangement::Unified,
                    // The header bar's own close icon, as its window buttons and items draw theirs.
                    .tabClose = TabCloseMark::SymbolicIcon },
        .tint = { .tabPercent = 20 },
        .colors = LinuxColors,
        .statusInk = NativeStatusInk,
        .dim = PaneDim,
        .minimal = NativeMinimal,
        .blockChips = NativeBlockChips,
        .settings = LinuxSettings,
    };

    inline constexpr ElevationLevel TerminalShadow {
        .offsetX = 1, .offsetY = 1, .blur = 0, .opacityPercent = 60
    };

    inline constexpr ChromeTokenRow TerminalRow {
        .size = { .titleBar = 1,
                  .tabWell = 1,
                  .tab = 1,
                  .control = 2,
                  .paneHeader = 1,
                  .toast = 1,
                  .tooltipPadX = 1,
                  .tooltipPadTop = 0,
                  .tooltipPadBottom = 0,
                  .tooltipEdge = 1,
                  .tooltipGap = 2,
                  .tooltipMaxWidth = 48,
                  .tooltipElevation = 2,
                  .dialogWidth = 32,
                  .swatchWidth = 2,
                  .swatchHeight = 1,
                  .settingsWidth = 120,
                  .settingsHeight = 40,
                  .settingsMinWidth = 90,
                  .settingsMinHeight = 24,
                  .tabIcon = 1,
                  .tabClose = 1,
                  .badge = 1,
                  .badgeDot = 1,
                  .progressBar = 1,
                  .hostChip = 1,
                  .hostDot = 1,
                  .toolbarButton = 2,
                  .toolbarChip = 1,
                  .stripButton = 0,
                  .profileButton = 0,
                  .flyoutSwatch = 2,
                  .bareBand = 1,
                  .grabber = 0,
                  .grabberLength = 0,
                  .paneHeaderAction = 1,
                  .paneFooter = 1,
                  .footerButton = 1,
                  .noticeCard = 0,
                  .paletteWidth = 48,
                  .paletteMaxHeight = 20,
                  .paletteField = 1,
                  .paletteRow = 1,
                  .paletteGroupHeader = 1,
                  .paletteFooter = 1,
                  .connectWidth = 60,
                  .connectHeight = 22,
                  .connectRow = 2,
                  .findBar = 1,
                  .findBarWidth = 48,
                  .findToggleWidth = 4,
                  .findToggleHeight = 1,
                  .findButtonWidth = 2,
                  .findButtonHeight = 1,
                  .findIcon = 0,
                  .linkPill = 1,
                  .scrollbar = 1,
                  .secureInputGlyph = 0,
                  .focusRing = 0,
                  .rail = 0,
                  .railMark = 1,
                  .blockActions = 1,
                  .foldChip = 1,
                  .foldChipHit = 1,
                  .scrollTick = 1,
                  .scrollTickOutline = 1,
                  .windowButton = 1,
                  .windowButtonGlyph = 1,
                  .headerIcon = 1,
                  .controlsSeparator = 1 },
        .space = { .paneGap = 1,
                   .splitHit = 1,
                   .paneInset = 0,
                   .scale = { 0, 0, 0, 1, 1, 1, 1, 1, 2, 3 },
                   .dragReserve = 4,
                   .revealEdge = 1,
                   .toolbarChipPadding = 1,
                   .tabStripLeading = 0,
                   .paneGapRows = 1,
                   .paneHeaderGap = 1,
                   .footerLeading = 1,
                   .noticeCardPadX = 1,
                   .noticeCardPadY = 0,
                   .paletteInset = 1,
                   .findBarInset = 1,
                   .findBarInsetRows = 1,
                   .findBarCountGap = 1,
                   .scrollbarInset = 0,
                   // The glyph column takes its cells from gutter.block_column_width: no strip.
                   .blockGutter = 0,
                   .railInset = 0,
                   .railEnd = 0 },
        .radii = { .control = 0,
                   .popover = 0,
                   .menu = 0,
                   .menuRow = 0,
                   .paneCard = 0,
                   .noticeCard = 0,
                   .toast = 0,
                   .tooltip = 0,
                   .tab = 0,
                   .tabWell = 0,
                   .tabIcon = 0,
                   .tabClose = 0,
                   .badge = 0,
                   .hostChip = 0,
                   .toolbarChip = 0,
                   .grabber = 0,
                   .findBar = 0,
                   .linkPill = 0,
                   .scrollbar = 0,
                   .rail = 0,
                   .blockActions = 0,
                   .chip = 0,
                   .window = 0,
                   .windowButton = 0,
                   .quickTerminal = 0 },
        .type = { .caption = 0,
                  .footnote = 0,
                  .body = 0,
                  .tab = 0,
                  .title = 0,
                  .paneHeader = 0,
                  .paneHeaderWeight = 400,
                  .paneHeaderAction = 0,
                  .paletteInput = 0,
                  .shortcut = 0 },
        .motion = { .fastMs = 100,
                    .standardMs = 160,
                    .emphasizedMs = 220,
                    .travel = 0,
                    .spinnerMs = 1200,
                    .pulseMs = 700,
                    .style = MotionStyle::Full,
                    .headerRevealMs = 500,
                    .popoverScalePercent = 100,
                    .lingerMs = 600,
                    .tooltipDelayMs = 600,
                    .tooltipWarmMs = 1500 },
        .elevation = { .levels = { ElevationLevel {
                                       .offsetX = 0, .offsetY = 0, .blur = 0, .opacityPercent = 0 },
                                   TerminalShadow,
                                   TerminalShadow,
                                   TerminalShadow } },
        .material = { .titleBand = Material::None, .popover = Material::None },
        .contrast = StandardContrast,
        .tui = { .inactivePaneOpacity = 0.62, .toastStyle = ToastStyle::Inverse },
        .layout = { .dialog = DialogPlacement::Centered,
                    .menu = MenuPresentation::InScene,
                    .tabShape = TabShape::Flat,
                    .profileMenu = ProfileMenuAffordance::Caret,
                    .tabColors = TabColorSource::Roles,
                    .activeTabWeight = ActiveTabWeight::Bold,
                    .paneActions = PaneActionsReveal::OnHoverOrFocus,
                    .paletteTopPercent = 18,
                    .findBar = FindBarHost::PaneHeaderRow,
                    .blockColumn = vtbackend::BlockColumnForm::Glyphs,
                    .settings = SettingsLayout::Cells,
                    .headerBar = HeaderBarArrangement::Band },
        // No tint: a tab's colour is its leading "▌" cell (UiStyleTokens::tabColorGlyph).
        .tint = { .tabPercent = 0 },
        // The banner's mark in the scheme's warning hue where that reads on the bar, else the bar's ink; the
        // toast an inverse strip where inverse video reads, else a bar strip.
        .colors = { schemeColor(SchemeSlot::Bar),
                    schemeColor(SchemeSlot::BarInk),
                    schemeOnBar(SchemeSlot::Warning),
                    schemeColor(SchemeSlot::Toast),
                    schemeColor(SchemeSlot::ToastInk),
                    schemeColor(SchemeSlot::Foreground),
                    schemeColor(SchemeSlot::Bar),           // TabWellFill
                    schemeColor(SchemeSlot::Selection),     // TabActiveFill
                    schemeColor(SchemeSlot::Bar),           // TabHoverFill
                    schemeColor(SchemeSlot::Bar),           // TabCloseFill
                    schemeColor(SchemeSlot::Selection),     // ControlPressedFill
                    schemeColor(SchemeSlot::BarInk),        // TabText
                    schemeColor(SchemeSlot::SelectionText), // TabTextActive
                    // Not Secondary: ANSI 8 on a light scheme's dark bar fails 4.5:1, so it does not dim.
                    schemeColor(SchemeSlot::BarInk), // TabTextDimmed
                    schemeColor(SchemeSlot::BarInk), // TabTextTinted (the row has no tint)
                    schemeColor(SchemeSlot::Bar),    // ChipFill
                    schemeColor(SchemeSlot::BarInk), // ChipText
                    schemeColor(SchemeSlot::BarInk), // ChipTextStrong
                    // A control is a cell: no fill behind it, the bar's ink pressed, ANSI 8 where it reads
                    // on the bar (else the ink) on an inactive window.
                    detail::noColor(),                  // CaptionWash
                    schemeColor(SchemeSlot::BarInk),    // CaptionGlyphPressed
                    schemeOnBar(SchemeSlot::Secondary), // CaptionGlyphInactive
                    // A pane is its own cells: the header is a bar row, the split a line in ANSI 8.
                    schemeColor(SchemeSlot::Background), // PaneGapFill
                    schemeColor(SchemeSlot::Background), // PaneCardBorder
                    schemeColor(SchemeSlot::Bar),        // PaneHeaderFill
                    // On the bar: its one ink, and the dimmed text ANSI 8 where that reads there (else the
                    // ink).
                    schemeColor(SchemeSlot::BarInk),    // PaneHeaderText
                    schemeOnBar(SchemeSlot::Secondary), // PaneHeaderTextDimmed
                    schemeColor(SchemeSlot::Secondary), // PaneSplitLine
                    schemeColor(SchemeSlot::Accent),    // PaneGrabber
                    schemeColor(SchemeSlot::Bar),       // PaneFooterFill
                    schemeColor(SchemeSlot::BarInk),    // PaneFooterText
                    // A cell row has no hover: the button is its glyph.
                    detail::noColor(), // PaneHeaderActionHover
                    // The palette is a bar-coloured block: the bar's ink, ANSI 8 where it reads there.
                    schemeColor(SchemeSlot::Bar),       // PaletteSurface
                    schemeColor(SchemeSlot::BarInk),    // PaletteText
                    schemeOnBar(SchemeSlot::Secondary), // PaletteSecondaryText
                    // A selected row on the bar-coloured surface must also stand off it (BarSelection).
                    schemeColor(SchemeSlot::BarSelection),     // PaletteSelectionFill
                    schemeColor(SchemeSlot::BarSelectionText), // PaletteSelectionText
                    schemeColor(SchemeSlot::Secondary),        // PaletteSeparator
                    // A lit toggle is its cells in inverse video -- the bar's ink as the fill, the bar as the
                    // glyph -- which stands off an unlit one on the bar at the ink's own 4.5:1; the scheme's
                    // selection on the bar can fall to 1.9:1. An unlit one and the count, the bar's inks.
                    schemeColor(SchemeSlot::BarInk),    // FindToggleOnFill
                    schemeColor(SchemeSlot::Bar),       // FindToggleOnText
                    schemeOnBar(SchemeSlot::Secondary), // FindToggleText
                    schemeColor(SchemeSlot::BarInk),    // FindCountText
                    // Inverse video already stands off the bar at 4.5:1: no ring.
                    detail::noColor(), // FindToggleOnRing
                    // The scheme's red where it reads on the bar at 4.5:1, else the bar's ink: the status
                    // hues follow the OS palette's tone, not the bar's.
                    schemeOnBar(SchemeSlot::Danger), // FindErrorText
                    // On the toast's fill, as its text: the hint is an ink that reads on it (Toast).
                    schemeColor(SchemeSlot::ToastHint), // ToastTextSecondary
                    // The bar's thumb is a column of "┃" (UiStyleTokens::scrollThumbGlyph): ANSI 8 at rest,
                    // the accent while engaged, each falling back to a colour that marks against the pane; no
                    // track.
                    schemeColor(SchemeSlot::ScrollbarThumb),       // ScrollbarThumb
                    schemeColor(SchemeSlot::ScrollbarThumbActive), // ScrollbarThumbActive
                    detail::noColor(),                             // ScrollbarTrack
                    // Cells cannot draw an outset: the focused item is a run of selection-coloured cells,
                    // the selection that also stands off the bar (BarSelection).
                    detail::noColor(),                     // FocusRing
                    schemeColor(SchemeSlot::BarSelection), // FocusFill
                    // The Settings window is a bar-coloured block, like the command palette: every text on it
                    // is the bar's own ink (which reads on any scheme), or ANSI 8 and the accent where they
                    // read on the bar, else that ink. A scheme's foreground on its background does not:
                    // one-dark's ends are 3.5:1 apart. A selected sidebar row must also stand off the bar
                    // (BarSelection).
                    schemeColor(SchemeSlot::Bar),              // SettingsSidebarFill
                    schemeColor(SchemeSlot::Bar),              // SettingsPageFill
                    schemeColor(SchemeSlot::Bar),              // SettingsGroupFill
                    schemeColor(SchemeSlot::Secondary),        // SettingsSeparator
                    schemeColor(SchemeSlot::BarInk),           // SettingsLabel
                    schemeOnBar(SchemeSlot::Secondary),        // SettingsHelp
                    schemeColor(SchemeSlot::BarSelection),     // SettingsSelectionFill
                    schemeColor(SchemeSlot::BarSelectionText), // SettingsSelectionText
                    schemeOnBar(SchemeSlot::Accent),           // SettingsModified
                    detail::noColor(),                         // SettingsIconTile
                    schemeColor(SchemeSlot::BarInk),           // SettingsIconGlyph
                    // A lit switch's "[x]" in the accent where it reads on the bar, an unlit one's "[ ]" in
                    // ANSI 8 likewise; a field is the pane's own cells, with the pane's ink.
                    schemeOnBar(SchemeSlot::Accent),     // SwitchOnFill
                    schemeOnBar(SchemeSlot::Secondary),  // SwitchOffFill
                    schemeColor(SchemeSlot::BarInk),     // SwitchKnob
                    schemeColor(SchemeSlot::Background), // ControlFill
                    schemeColor(SchemeSlot::PaneInk),    // ControlText
                    schemeOnBar(SchemeSlot::Secondary),  // PreviewFrame
                    // A cell has no round fill: the "● ● ●" cells sit on the bar, in its ink.
                    schemeColor(SchemeSlot::Bar),    // WindowButtonFill
                    schemeColor(SchemeSlot::Bar),    // WindowButtonHoverFill
                    schemeColor(SchemeSlot::Bar),    // WindowButtonPressedFill
                    schemeColor(SchemeSlot::BarInk), // WindowButtonGlyph
                    // The Terminal row draws no header bar.
                    detail::noColor(), // HeaderSeparator
                    // A tooltip is the surface of the row's in-scene menus (ContourTui/Menu.qml): the
                    // palette's window colour and ink, its mid colour as the border, and the row's e2 hard
                    // shadow. The shortcut is the ink at 60 %.
                    detail::sourceOf(ColorSourceKind::PaletteWindow),     // TooltipFill
                    detail::sourceOf(ColorSourceKind::PaletteWindowText), // TooltipText
                    ChromeColorSource { .kind = ColorSourceKind::PaletteWindowText,
                                        .alphaPercent = 60 },        // TooltipTextSecondary
                    detail::sourceOf(ColorSourceKind::PaletteMid) }, // TooltipEdge
        // The scheme's hues, each falling back to the bar's ink where it would not read on the bar.
        .statusInk = { schemeOnBar(SchemeSlot::Danger),
                       schemeOnBar(SchemeSlot::Warning),
                       schemeOnBar(SchemeSlot::Success),
                       schemeOnBar(SchemeSlot::Accent) },
        .dim = PaneDim,
        .minimal = { .paneHeader = 0,
                     .paneGap = 1,
                     .paneGapRows = 1,
                     .splitHit = 1,
                     .paneInset = 0,
                     .paneCardRadius = 0 },
        .settings = TerminalSettings,
    };
} // namespace detail

/// The 1a token rows, indexed by @c ChromeStyle.
inline constexpr std::array<ChromeTokenRow, detail::ChromeStyleCount> ChromeTokenRows {
    detail::MacRow,      // ChromeStyle::MacOS
    detail::FluentRow,   // ChromeStyle::Fluent
    detail::LinuxRow,    // ChromeStyle::ContourLinux
    detail::TerminalRow, // ChromeStyle::Terminal
};

static_assert(ChromeTokenRows.size() == detail::ChromeStyleCount,
              "Every ChromeStyle needs exactly one 1a token row.");

/// Resolves a style's token row against the chrome density, the user's accessibility preferences and what
/// the operating system can draw.
/// @param row The chrome style whose row to take.
/// @param preferences The accessibility choices to apply on top of it.
/// @param density The density whose extents to take: @c Minimal copies the row's @c minimal sub-row over
///                the pane extents, before the accessibility steps.
/// @param materials Whether the OS draws the row's materials: @c Unsupported turns them Solid, as Reduced
///                  transparency does; @c Unknown changes nothing.
/// @return The adjusted row.
[[nodiscard]] constexpr ChromeTokenRow resolveChromeTokens(
    ChromeStyle row,
    AccessibilityPreferences preferences,
    ChromeDensity density = ChromeDensity::Comfortable,
    MaterialSupport materials = MaterialSupport::Unknown) noexcept
{
    auto tokens = ChromeTokenRows[static_cast<size_t>(row)];

    if (density == ChromeDensity::Minimal)
    {
        tokens.size.paneHeader = tokens.minimal.paneHeader;
        tokens.space.paneGap = tokens.minimal.paneGap;
        tokens.space.paneGapRows = tokens.minimal.paneGapRows;
        tokens.space.splitHit = tokens.minimal.splitHit;
        tokens.space.paneInset = tokens.minimal.paneInset;
        tokens.radii.paneCard = tokens.minimal.paneCardRadius;
    }

    if (preferences.motion == MotionPreference::Reduced)
    {
        tokens.motion.fastMs = ReducedMotionCrossFadeMs;
        tokens.motion.standardMs = ReducedMotionCrossFadeMs;
        tokens.motion.emphasizedMs = ReducedMotionCrossFadeMs;
        tokens.motion.travel = 0;
        tokens.motion.style = MotionStyle::CrossFade;
        tokens.motion.popoverScalePercent = 100;
    }

    if (preferences.transparency == TransparencyPreference::Reduced
        || materials == MaterialSupport::Unsupported)
    {
        for (auto* material: { &tokens.material.titleBand, &tokens.material.popover })
            *material = opaqueMaterialOf(*material);
    }

    if (preferences.contrast == ContrastPreference::Increased)
    {
        tokens.contrast.separatorPercent *= 2;
        tokens.contrast.activeCardRing = 1.5;
        tokens.contrast.paneDimPercent = 5;
    }

    return tokens;
}

} // namespace contour::config
