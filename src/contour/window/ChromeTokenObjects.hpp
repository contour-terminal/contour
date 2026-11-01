// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The QML-facing objects of the 1a token groups: one QObject per group of config::ChromeTokenRow,
/// published as the `chromeStyle.size`, `chromeStyle.space`, ... sub-objects. Each is built from its
/// resolved group and the chrome's cell units, so every extent it reports is already in logical
/// pixels and the QML never learns which style is active.

#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/session/EndReason.hpp>
#include <contour/window/DropZonePolicy.hpp>
#include <contour/window/PaneHeaderPolicy.hpp>
#include <contour/window/PaneState.hpp>
#include <contour/window/SecureInputPolicy.hpp>
#include <contour/window/StickyHeaderPlacement.hpp>
#include <contour/window/TabBadge.hpp>

#include <vtbackend/shell/ScrollbarMarks.hpp>

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtGui/QFont>

#include <array>
#include <cstddef>

namespace contour::window
{

class UiStyleProvider;

/// What one unit of a token extent means, in logical pixels.
///
/// Both are 1 for a pixel-counting style; a cell-counting one sets them to the chrome font's cell,
/// as ChromeMetrics::widthQuantum and ChromeMetrics::cellHeight already are.
struct ChromeUnits
{
    qreal horizontal = 1.0; ///< One horizontal unit: a cell's width, or one pixel.
    qreal vertical = 1.0;   ///< One vertical unit: a cell's height, or one pixel.
};

/// Component extents, `chromeStyle.size`.
class ChromeSizeTokens: public QObject
{
    Q_OBJECT
    Q_PROPERTY(qreal titleBar READ titleBar CONSTANT)
    Q_PROPERTY(qreal tabWell READ tabWell CONSTANT)
    Q_PROPERTY(qreal tab READ tab CONSTANT)
    Q_PROPERTY(qreal control READ control CONSTANT)
    Q_PROPERTY(qreal paneHeader READ paneHeader CONSTANT)
    Q_PROPERTY(qreal toast READ toast CONSTANT)
    Q_PROPERTY(qreal tooltipPadX READ tooltipPadX CONSTANT)
    Q_PROPERTY(qreal tooltipPadTop READ tooltipPadTop CONSTANT)
    Q_PROPERTY(qreal tooltipPadBottom READ tooltipPadBottom CONSTANT)
    Q_PROPERTY(qreal tooltipEdge READ tooltipEdge CONSTANT)
    Q_PROPERTY(qreal tooltipMaxWidth READ tooltipMaxWidth CONSTANT)
    Q_PROPERTY(int tooltipElevation READ tooltipElevation CONSTANT)
    Q_PROPERTY(qreal tooltipGap READ tooltipGap CONSTANT)
    Q_PROPERTY(qreal dialogWidth READ dialogWidth CONSTANT)
    Q_PROPERTY(qreal swatchWidth READ swatchWidth CONSTANT)
    Q_PROPERTY(qreal swatchHeight READ swatchHeight CONSTANT)
    Q_PROPERTY(qreal settingsWidth READ settingsWidth CONSTANT)
    Q_PROPERTY(qreal settingsHeight READ settingsHeight CONSTANT)
    Q_PROPERTY(qreal settingsMinWidth READ settingsMinWidth CONSTANT)
    Q_PROPERTY(qreal settingsMinHeight READ settingsMinHeight CONSTANT)
    Q_PROPERTY(qreal tabIcon READ tabIcon CONSTANT)
    Q_PROPERTY(qreal tabClose READ tabClose CONSTANT)
    Q_PROPERTY(qreal badge READ badge CONSTANT)
    Q_PROPERTY(qreal badgeDot READ badgeDot CONSTANT)
    Q_PROPERTY(qreal progressBar READ progressBar CONSTANT)
    Q_PROPERTY(qreal hostChip READ hostChip CONSTANT)
    Q_PROPERTY(qreal hostDot READ hostDot CONSTANT)
    Q_PROPERTY(qreal toolbarButton READ toolbarButton CONSTANT)
    Q_PROPERTY(qreal toolbarChip READ toolbarChip CONSTANT)
    Q_PROPERTY(qreal stripButton READ stripButton CONSTANT)
    Q_PROPERTY(qreal profileButton READ profileButton CONSTANT)
    Q_PROPERTY(qreal flyoutSwatch READ flyoutSwatch CONSTANT)
    Q_PROPERTY(qreal bareBand READ bareBand CONSTANT)
    Q_PROPERTY(qreal grabber READ grabber CONSTANT)
    Q_PROPERTY(qreal grabberLength READ grabberLength CONSTANT)
    Q_PROPERTY(qreal paneHeaderAction READ paneHeaderAction CONSTANT)
    Q_PROPERTY(qreal paneFooter READ paneFooter CONSTANT)
    Q_PROPERTY(qreal footerButton READ footerButton CONSTANT)
    Q_PROPERTY(qreal noticeCard READ noticeCard CONSTANT)
    Q_PROPERTY(qreal paletteWidth READ paletteWidth CONSTANT)
    Q_PROPERTY(qreal paletteMaxHeight READ paletteMaxHeight CONSTANT)
    Q_PROPERTY(qreal paletteField READ paletteField CONSTANT)
    Q_PROPERTY(qreal paletteRow READ paletteRow CONSTANT)
    Q_PROPERTY(qreal paletteGroupHeader READ paletteGroupHeader CONSTANT)
    Q_PROPERTY(qreal paletteFooter READ paletteFooter CONSTANT)
    Q_PROPERTY(qreal connectWidth READ connectWidth CONSTANT)
    Q_PROPERTY(qreal connectHeight READ connectHeight CONSTANT)
    Q_PROPERTY(qreal connectRow READ connectRow CONSTANT)
    Q_PROPERTY(qreal findBar READ findBar CONSTANT)
    Q_PROPERTY(qreal findBarWidth READ findBarWidth CONSTANT)
    Q_PROPERTY(qreal findToggleWidth READ findToggleWidth CONSTANT)
    Q_PROPERTY(qreal findToggleHeight READ findToggleHeight CONSTANT)
    Q_PROPERTY(qreal findButtonWidth READ findButtonWidth CONSTANT)
    Q_PROPERTY(qreal findButtonHeight READ findButtonHeight CONSTANT)
    Q_PROPERTY(qreal findIcon READ findIcon CONSTANT)
    Q_PROPERTY(qreal linkPill READ linkPill CONSTANT)
    Q_PROPERTY(qreal scrollbar READ scrollbar CONSTANT)
    Q_PROPERTY(qreal secureInputGlyph READ secureInputGlyph CONSTANT)
    Q_PROPERTY(qreal focusRing READ focusRing CONSTANT)
    Q_PROPERTY(qreal blockActions READ blockActions CONSTANT)
    Q_PROPERTY(qreal scrollTick READ scrollTick CONSTANT)
    Q_PROPERTY(qreal scrollTickOutline READ scrollTickOutline CONSTANT)
    Q_PROPERTY(qreal windowButton READ windowButton CONSTANT)
    Q_PROPERTY(qreal windowButtonGlyph READ windowButtonGlyph CONSTANT)
    Q_PROPERTY(qreal headerIcon READ headerIcon CONSTANT)
    Q_PROPERTY(qreal controlsSeparator READ controlsSeparator CONSTANT)

  public:
    /// @param tokens The resolved size group.
    /// @param units  What one token unit means in pixels.
    /// @param parent Qt ownership.
    ChromeSizeTokens(config::SizeTokens tokens, ChromeUnits units, QObject* parent);

    /// @return The title bar height in logical pixels.
    [[nodiscard]] qreal titleBar() const noexcept { return _tokens.titleBar * _units.vertical; }
    /// @return The tab well height in logical pixels.
    [[nodiscard]] qreal tabWell() const noexcept { return _tokens.tabWell * _units.vertical; }
    /// @return The tab height in logical pixels.
    [[nodiscard]] qreal tab() const noexcept { return _tokens.tab * _units.vertical; }
    /// @return The control extent in logical pixels (horizontal).
    [[nodiscard]] qreal control() const noexcept { return _tokens.control * _units.horizontal; }
    /// @return The pane header height in logical pixels.
    [[nodiscard]] qreal paneHeader() const noexcept { return _tokens.paneHeader * _units.vertical; }
    /// @return The toast height in logical pixels.
    [[nodiscard]] qreal toast() const noexcept { return _tokens.toast * _units.vertical; }
    /// @return A tooltip's leading and trailing padding in logical pixels.
    [[nodiscard]] qreal tooltipPadX() const noexcept { return _tokens.tooltipPadX * _units.horizontal; }
    /// @return A tooltip's top padding in logical pixels.
    [[nodiscard]] qreal tooltipPadTop() const noexcept { return _tokens.tooltipPadTop * _units.vertical; }
    /// @return A tooltip's bottom padding in logical pixels.
    [[nodiscard]] qreal tooltipPadBottom() const noexcept
    {
        return _tokens.tooltipPadBottom * _units.vertical;
    }
    /// @return The shadow level (0 to 3) a tooltip lifts itself by; 0 casts none.
    [[nodiscard]] int tooltipElevation() const noexcept { return _tokens.tooltipElevation; }
    /// @return The widest a tooltip's text grows before it wraps, in logical pixels.
    [[nodiscard]] qreal tooltipMaxWidth() const noexcept
    {
        return _tokens.tooltipMaxWidth * _units.horizontal;
    }
    /// @return A tooltip's edge in whole logical pixels; 0 asks for one device pixel.
    [[nodiscard]] qreal tooltipEdge() const noexcept { return _tokens.tooltipEdge; }
    /// @return The gap between a tooltip's label and its shortcut in logical pixels.
    [[nodiscard]] qreal tooltipGap() const noexcept { return _tokens.tooltipGap * _units.horizontal; }
    /// @return The dialog width in logical pixels.
    [[nodiscard]] qreal dialogWidth() const noexcept { return _tokens.dialogWidth * _units.horizontal; }
    /// @return The width of a menu's colour swatch in logical pixels.
    [[nodiscard]] qreal swatchWidth() const noexcept { return _tokens.swatchWidth * _units.horizontal; }
    /// @return The height of a menu's colour swatch in logical pixels.
    [[nodiscard]] qreal swatchHeight() const noexcept { return _tokens.swatchHeight * _units.vertical; }
    /// @return The Settings window's default width in logical pixels.
    [[nodiscard]] qreal settingsWidth() const noexcept { return _tokens.settingsWidth * _units.horizontal; }
    /// @return The Settings window's default height in logical pixels.
    [[nodiscard]] qreal settingsHeight() const noexcept { return _tokens.settingsHeight * _units.vertical; }
    /// @return The Settings window's minimum width in logical pixels.
    [[nodiscard]] qreal settingsMinWidth() const noexcept
    {
        return _tokens.settingsMinWidth * _units.horizontal;
    }
    /// @return The Settings window's minimum height in logical pixels.
    [[nodiscard]] qreal settingsMinHeight() const noexcept
    {
        return _tokens.settingsMinHeight * _units.vertical;
    }

    // The band's square and round marks are widths, as a traffic-light dot is: a circle quantized to
    // different horizontal and vertical cells would be an ellipse. The bar and the chips are heights.
    /// @return The edge of a tab's icon square in logical pixels.
    [[nodiscard]] qreal tabIcon() const noexcept { return _tokens.tabIcon * _units.horizontal; }
    /// @return The edge of a tab's close button in logical pixels.
    [[nodiscard]] qreal tabClose() const noexcept { return _tokens.tabClose * _units.horizontal; }
    /// @return The diameter of a badge disc or ring in logical pixels.
    [[nodiscard]] qreal badge() const noexcept { return _tokens.badge * _units.horizontal; }
    /// @return The diameter of the unseen-output dot in logical pixels.
    [[nodiscard]] qreal badgeDot() const noexcept { return _tokens.badgeDot * _units.horizontal; }
    /// @return The height of a tab's progress bar in logical pixels.
    [[nodiscard]] qreal progressBar() const noexcept { return _tokens.progressBar * _units.vertical; }
    /// @return The height of the host chip in logical pixels.
    [[nodiscard]] qreal hostChip() const noexcept { return _tokens.hostChip * _units.vertical; }
    /// @return The diameter of the host chip's dot in logical pixels.
    [[nodiscard]] qreal hostDot() const noexcept { return _tokens.hostDot * _units.horizontal; }
    /// @return The width of a band toolbar button in logical pixels.
    [[nodiscard]] qreal toolbarButton() const noexcept { return _tokens.toolbarButton * _units.horizontal; }
    /// @return The height of a band toolbar chip in logical pixels.
    [[nodiscard]] qreal toolbarChip() const noexcept { return _tokens.toolbarChip * _units.vertical; }
    /// @return The height of a joined strip's "+" and profile buttons in logical pixels.
    [[nodiscard]] qreal stripButton() const noexcept { return _tokens.stripButton * _units.vertical; }
    /// @return The width of a joined strip's profile button in logical pixels.
    [[nodiscard]] qreal profileButton() const noexcept { return _tokens.profileButton * _units.horizontal; }
    /// @return The edge of a tab colour flyout swatch in logical pixels; a width, so the swatch stays square.
    [[nodiscard]] qreal flyoutSwatch() const noexcept { return _tokens.flyoutSwatch * _units.horizontal; }

    /// @return The height of a band with neither tabs nor a title, in logical pixels.
    [[nodiscard]] qreal bareBand() const noexcept { return _tokens.bareBand * _units.vertical; }
    /// @return The split grabber's thickness in logical pixels, across a side-by-side split's divider.
    [[nodiscard]] qreal grabber() const noexcept { return _tokens.grabber * _units.horizontal; }
    /// @return The split grabber's length in logical pixels, along a side-by-side split's divider.
    [[nodiscard]] qreal grabberLength() const noexcept { return _tokens.grabberLength * _units.vertical; }
    /// @return The hit extent of a pane header's action button in logical pixels.
    [[nodiscard]] qreal paneHeaderAction() const noexcept
    {
        return _tokens.paneHeaderAction * _units.horizontal;
    }
    /// @return The exited footer's height in logical pixels.
    [[nodiscard]] qreal paneFooter() const noexcept { return _tokens.paneFooter * _units.vertical; }
    /// @return The height of an exited footer's button in logical pixels.
    [[nodiscard]] qreal footerButton() const noexcept { return _tokens.footerButton * _units.vertical; }
    [[nodiscard]] qreal noticeCard() const noexcept { return _tokens.noticeCard * _units.horizontal; }

    /// @return The command palette's width in logical pixels.
    [[nodiscard]] qreal paletteWidth() const noexcept { return _tokens.paletteWidth * _units.horizontal; }
    /// @return The command palette's greatest height in logical pixels.
    [[nodiscard]] qreal paletteMaxHeight() const noexcept
    {
        return _tokens.paletteMaxHeight * _units.vertical;
    }
    /// @return The height of the palette's query row in logical pixels.
    [[nodiscard]] qreal paletteField() const noexcept { return _tokens.paletteField * _units.vertical; }
    /// @return The height of a palette row in logical pixels.
    [[nodiscard]] qreal paletteRow() const noexcept { return _tokens.paletteRow * _units.vertical; }
    /// @return The height of a palette group header in logical pixels.
    [[nodiscard]] qreal paletteGroupHeader() const noexcept
    {
        return _tokens.paletteGroupHeader * _units.vertical;
    }
    /// @return The height of the palette's footer in logical pixels.
    [[nodiscard]] qreal paletteFooter() const noexcept { return _tokens.paletteFooter * _units.vertical; }
    /// @return The Connect picker window's default width in logical pixels.
    [[nodiscard]] qreal connectWidth() const noexcept { return _tokens.connectWidth * _units.horizontal; }
    /// @return The Connect picker window's default height in logical pixels.
    [[nodiscard]] qreal connectHeight() const noexcept { return _tokens.connectHeight * _units.vertical; }
    /// @return The height of a two-line Connect target row in logical pixels.
    [[nodiscard]] qreal connectRow() const noexcept { return _tokens.connectRow * _units.vertical; }

    /// @return The find bar's height in logical pixels.
    [[nodiscard]] qreal findBar() const noexcept { return _tokens.findBar * _units.vertical; }
    /// @return The find bar's width in logical pixels.
    [[nodiscard]] qreal findBarWidth() const noexcept { return _tokens.findBarWidth * _units.horizontal; }
    /// @return A find option toggle's width in logical pixels.
    [[nodiscard]] qreal findToggleWidth() const noexcept
    {
        return _tokens.findToggleWidth * _units.horizontal;
    }
    /// @return A find option toggle's height in logical pixels.
    [[nodiscard]] qreal findToggleHeight() const noexcept
    {
        return _tokens.findToggleHeight * _units.vertical;
    }
    /// @return The width of the find bar's previous, next and close buttons in logical pixels.
    [[nodiscard]] qreal findButtonWidth() const noexcept
    {
        return _tokens.findButtonWidth * _units.horizontal;
    }
    /// @return Their height in logical pixels.
    [[nodiscard]] qreal findButtonHeight() const noexcept
    {
        return _tokens.findButtonHeight * _units.vertical;
    }
    /// @return The magnifier's edge in logical pixels; 0 draws none.
    [[nodiscard]] qreal findIcon() const noexcept { return _tokens.findIcon * _units.horizontal; }

    /// @return The link pill's height in logical pixels.
    [[nodiscard]] qreal linkPill() const noexcept { return _tokens.linkPill * _units.vertical; }
    /// @return The overlay scrollbar's track width in logical pixels.
    [[nodiscard]] qreal scrollbar() const noexcept { return _tokens.scrollbar * _units.horizontal; }
    /// @return The secure-input lock's icon edge in logical pixels; 0 on the Terminal row, which sets text.
    [[nodiscard]] qreal secureInputGlyph() const noexcept
    {
        return _tokens.secureInputGlyph * _units.vertical;
    }
    /// @return The keyboard focus ring's outset in logical pixels; 0 draws none.
    [[nodiscard]] qreal focusRing() const noexcept { return _tokens.focusRing * _units.horizontal; }

    /// @return The edge of a command block's square ⋯ button in logical pixels.
    [[nodiscard]] qreal blockActions() const noexcept { return _tokens.blockActions * _units.horizontal; }
    /// @return A scrollbar tick's height in logical pixels.
    [[nodiscard]] qreal scrollTick() const noexcept { return _tokens.scrollTick * _units.vertical; }
    /// @return A hollow scrollbar tick's stroke in logical pixels.
    [[nodiscard]] qreal scrollTickOutline() const noexcept
    {
        return _tokens.scrollTickOutline * _units.horizontal;
    }

    // A round button and its icons are widths, as the band's round marks are.
    /// @return The edge of a round window button in logical pixels.
    [[nodiscard]] qreal windowButton() const noexcept { return _tokens.windowButton * _units.horizontal; }
    /// @return The edge of a window button's glyph icon in logical pixels.
    [[nodiscard]] qreal windowButtonGlyph() const noexcept
    {
        return _tokens.windowButtonGlyph * _units.horizontal;
    }
    /// @return The edge of a header-bar button's icon in logical pixels.
    [[nodiscard]] qreal headerIcon() const noexcept { return _tokens.headerIcon * _units.horizontal; }
    /// @return The height of the separator before the window buttons in logical pixels; 0 draws none.
    [[nodiscard]] qreal controlsSeparator() const noexcept
    {
        return _tokens.controlsSeparator * _units.vertical;
    }

  private:
    config::SizeTokens _tokens {};
    ChromeUnits _units {};
};

/// Gaps and the spacing scale, `chromeStyle.space`. Every value but @c revealEdge, @c paneGapRows and
/// @c findBarInsetRows is horizontal.
class ChromeSpaceTokens: public QObject
{
    Q_OBJECT
    Q_PROPERTY(qreal paneGap READ paneGap CONSTANT)
    Q_PROPERTY(qreal splitHit READ splitHit CONSTANT)
    Q_PROPERTY(qreal paneInset READ paneInset CONSTANT)
    Q_PROPERTY(qreal s1 READ s1 CONSTANT)
    Q_PROPERTY(qreal s2 READ s2 CONSTANT)
    Q_PROPERTY(qreal s3 READ s3 CONSTANT)
    Q_PROPERTY(qreal s4 READ s4 CONSTANT)
    Q_PROPERTY(qreal s5 READ s5 CONSTANT)
    Q_PROPERTY(qreal s6 READ s6 CONSTANT)
    Q_PROPERTY(qreal s7 READ s7 CONSTANT)
    Q_PROPERTY(qreal s8 READ s8 CONSTANT)
    Q_PROPERTY(qreal s9 READ s9 CONSTANT)
    Q_PROPERTY(qreal s10 READ s10 CONSTANT)
    Q_PROPERTY(qreal dragReserve READ dragReserve CONSTANT)
    Q_PROPERTY(qreal revealEdge READ revealEdge CONSTANT)
    Q_PROPERTY(qreal toolbarChipPadding READ toolbarChipPadding CONSTANT)
    Q_PROPERTY(qreal tabStripLeading READ tabStripLeading CONSTANT)
    Q_PROPERTY(qreal paneGapRows READ paneGapRows CONSTANT)
    Q_PROPERTY(qreal paneHeaderGap READ paneHeaderGap CONSTANT)
    Q_PROPERTY(qreal footerLeading READ footerLeading CONSTANT)
    Q_PROPERTY(qreal noticeCardPadX READ noticeCardPadX CONSTANT)
    Q_PROPERTY(qreal noticeCardPadY READ noticeCardPadY CONSTANT)
    Q_PROPERTY(qreal paletteInset READ paletteInset CONSTANT)
    Q_PROPERTY(qreal findBarInset READ findBarInset CONSTANT)
    Q_PROPERTY(qreal findBarInsetRows READ findBarInsetRows CONSTANT)
    Q_PROPERTY(qreal findBarCountGap READ findBarCountGap CONSTANT)
    Q_PROPERTY(qreal scrollbarInset READ scrollbarInset CONSTANT)
    Q_PROPERTY(qreal blockGutter READ blockGutter CONSTANT)

  public:
    /// @param tokens The resolved space group.
    /// @param units  What one token unit means in pixels.
    /// @param parent Qt ownership.
    ChromeSpaceTokens(config::SpaceTokens tokens, ChromeUnits units, QObject* parent);

    /// @return The gap between split panes in logical pixels.
    [[nodiscard]] qreal paneGap() const noexcept { return _tokens.paneGap * _units.horizontal; }
    /// @return The split divider's hit target in logical pixels.
    [[nodiscard]] qreal splitHit() const noexcept { return _tokens.splitHit * _units.horizontal; }
    /// @return The pane cards' inset from the content edge in logical pixels.
    [[nodiscard]] qreal paneInset() const noexcept { return _tokens.paneInset * _units.horizontal; }

    /// @param index Zero-based position in the scale: 0 is s1.
    /// @return That step of the spacing scale in logical pixels.
    [[nodiscard]] qreal step(size_t index) const noexcept { return _tokens.scale[index] * _units.horizontal; }

    /// @return The spacing scale's steps s1..s10 in logical pixels.
    [[nodiscard]] qreal s1() const noexcept { return step(0); }
    [[nodiscard]] qreal s2() const noexcept { return step(1); }
    [[nodiscard]] qreal s3() const noexcept { return step(2); }
    [[nodiscard]] qreal s4() const noexcept { return step(3); }
    [[nodiscard]] qreal s5() const noexcept { return step(4); }
    [[nodiscard]] qreal s6() const noexcept { return step(5); }
    [[nodiscard]] qreal s7() const noexcept { return step(6); }
    [[nodiscard]] qreal s8() const noexcept { return step(7); }
    [[nodiscard]] qreal s9() const noexcept { return step(8); }
    [[nodiscard]] qreal s10() const noexcept { return step(9); }

    /// @return The spacing scale's s7 in vertical units (a cell's height on the Terminal row): the command
    ///         palette's margin to the window's bottom.
    [[nodiscard]] qreal s7Rows() const noexcept { return _tokens.scale[6] * _units.vertical; }

    /// @return The band width that always stays draggable, in logical pixels.
    [[nodiscard]] qreal dragReserve() const noexcept { return _tokens.dragReserve * _units.horizontal; }
    /// @return The height of the full-screen reveal edge, in logical pixels (vertical, unlike the rest).
    [[nodiscard]] qreal revealEdge() const noexcept { return _tokens.revealEdge * _units.vertical; }
    /// @return The horizontal padding inside a toolbar chip, in logical pixels.
    [[nodiscard]] qreal toolbarChipPadding() const noexcept
    {
        return _tokens.toolbarChipPadding * _units.horizontal;
    }

    /// @return The inset from the band's leading edge to a joined strip's first tab, in logical pixels.
    [[nodiscard]] qreal tabStripLeading() const noexcept
    {
        return _tokens.tabStripLeading * _units.horizontal;
    }

    /// @return The gap between stacked panes in logical pixels (vertical: one cell's height on Terminal).
    [[nodiscard]] qreal paneGapRows() const noexcept { return _tokens.paneGapRows * _units.vertical; }
    /// @return The gap between a pane header's items in logical pixels.
    [[nodiscard]] qreal paneHeaderGap() const noexcept { return _tokens.paneHeaderGap * _units.horizontal; }
    /// @return The exited footer's leading padding in logical pixels.
    [[nodiscard]] qreal footerLeading() const noexcept { return _tokens.footerLeading * _units.horizontal; }
    [[nodiscard]] qreal noticeCardPadX() const noexcept { return _tokens.noticeCardPadX * _units.horizontal; }
    [[nodiscard]] qreal noticeCardPadY() const noexcept { return _tokens.noticeCardPadY * _units.vertical; }
    /// @return The palette rows' horizontal inset and the list's side padding, in logical pixels.
    [[nodiscard]] qreal paletteInset() const noexcept { return _tokens.paletteInset * _units.horizontal; }
    /// @return The find bar's inset from the pane's right edge in logical pixels.
    [[nodiscard]] qreal findBarInset() const noexcept { return _tokens.findBarInset * _units.horizontal; }
    /// @return Its inset from the pane's top edge in logical pixels (vertical: one row on Terminal).
    [[nodiscard]] qreal findBarInsetRows() const noexcept
    {
        return _tokens.findBarInsetRows * _units.vertical;
    }
    /// @return What the count keeps from the first toggle beyond the bar's spacing, in logical pixels.
    [[nodiscard]] qreal findBarCountGap() const noexcept
    {
        return _tokens.findBarCountGap * _units.horizontal;
    }
    /// @return The overlay scrollbar's gap to the pane's edge, in logical pixels.
    [[nodiscard]] qreal scrollbarInset() const noexcept { return _tokens.scrollbarInset * _units.horizontal; }
    /// @return The width of the strip the block rail sits in, in logical pixels; 0 where there is none.
    [[nodiscard]] qreal blockGutter() const noexcept { return _tokens.blockGutter * _units.horizontal; }

  private:
    config::SpaceTokens _tokens {};
    ChromeUnits _units {};
};

/// Corner radii, `chromeStyle.radii`, in logical pixels (horizontal units).
class ChromeRadiusTokens: public QObject
{
    Q_OBJECT
    Q_PROPERTY(qreal control READ control CONSTANT)
    Q_PROPERTY(qreal popover READ popover CONSTANT)
    Q_PROPERTY(qreal menu READ menu CONSTANT)
    Q_PROPERTY(qreal menuRow READ menuRow CONSTANT)
    Q_PROPERTY(qreal paneCard READ paneCard CONSTANT)
    Q_PROPERTY(qreal noticeCard READ noticeCard CONSTANT)
    Q_PROPERTY(qreal toast READ toast CONSTANT)
    Q_PROPERTY(qreal tooltip READ tooltip CONSTANT)
    Q_PROPERTY(qreal tab READ tab CONSTANT)
    Q_PROPERTY(qreal tabWell READ tabWell CONSTANT)
    Q_PROPERTY(qreal tabIcon READ tabIcon CONSTANT)
    Q_PROPERTY(qreal tabClose READ tabClose CONSTANT)
    Q_PROPERTY(qreal badge READ badge CONSTANT)
    Q_PROPERTY(qreal hostChip READ hostChip CONSTANT)
    Q_PROPERTY(qreal toolbarChip READ toolbarChip CONSTANT)
    Q_PROPERTY(qreal grabber READ grabber CONSTANT)
    Q_PROPERTY(qreal findBar READ findBar CONSTANT)
    Q_PROPERTY(qreal linkPill READ linkPill CONSTANT)
    Q_PROPERTY(qreal scrollbar READ scrollbar CONSTANT)
    Q_PROPERTY(qreal blockActions READ blockActions CONSTANT)
    Q_PROPERTY(qreal window READ window CONSTANT)
    Q_PROPERTY(qreal windowButton READ windowButton CONSTANT)
    Q_PROPERTY(qreal quickTerminal READ quickTerminal CONSTANT)

  public:
    /// @param tokens The resolved radius group.
    /// @param units  What one token unit means in pixels.
    /// @param parent Qt ownership.
    ChromeRadiusTokens(config::RadiusTokens tokens, ChromeUnits units, QObject* parent);

    /// @return The radius of buttons and fields.
    [[nodiscard]] qreal control() const noexcept { return _tokens.control * _units.horizontal; }
    /// @return The radius of popovers.
    [[nodiscard]] qreal popover() const noexcept { return _tokens.popover * _units.horizontal; }
    /// @return The radius of menus.
    [[nodiscard]] qreal menu() const noexcept { return _tokens.menu * _units.horizontal; }
    /// @return The radius of a menu row's highlight.
    [[nodiscard]] qreal menuRow() const noexcept { return _tokens.menuRow * _units.horizontal; }
    /// @return The radius of pane cards.
    [[nodiscard]] qreal paneCard() const noexcept { return _tokens.paneCard * _units.horizontal; }
    [[nodiscard]] qreal noticeCard() const noexcept { return _tokens.noticeCard * _units.horizontal; }
    /// @return The radius of toasts.
    [[nodiscard]] qreal toast() const noexcept { return _tokens.toast * _units.horizontal; }
    /// @return The radius of tooltips.
    [[nodiscard]] qreal tooltip() const noexcept { return _tokens.tooltip * _units.horizontal; }
    /// @return The radius of a tab pill.
    [[nodiscard]] qreal tab() const noexcept { return _tokens.tab * _units.horizontal; }
    /// @return The radius of the tab well.
    [[nodiscard]] qreal tabWell() const noexcept { return _tokens.tabWell * _units.horizontal; }
    /// @return The radius of a tab's icon square.
    [[nodiscard]] qreal tabIcon() const noexcept { return _tokens.tabIcon * _units.horizontal; }
    /// @return The radius of a tab's close button.
    [[nodiscard]] qreal tabClose() const noexcept { return _tokens.tabClose * _units.horizontal; }
    /// @return The radius of a badge disc.
    [[nodiscard]] qreal badge() const noexcept { return _tokens.badge * _units.horizontal; }
    /// @return The radius of the host chip.
    [[nodiscard]] qreal hostChip() const noexcept { return _tokens.hostChip * _units.horizontal; }
    /// @return The radius of a toolbar chip.
    [[nodiscard]] qreal toolbarChip() const noexcept { return _tokens.toolbarChip * _units.horizontal; }
    /// @return The radius of the split grabber.
    [[nodiscard]] qreal grabber() const noexcept { return _tokens.grabber * _units.horizontal; }
    /// @return The radius of the find bar.
    [[nodiscard]] qreal findBar() const noexcept { return _tokens.findBar * _units.horizontal; }
    /// @return The radius of the link pill.
    [[nodiscard]] qreal linkPill() const noexcept { return _tokens.linkPill * _units.horizontal; }
    /// @return The radius of the overlay scrollbar's thumb.
    [[nodiscard]] qreal scrollbar() const noexcept { return _tokens.scrollbar * _units.horizontal; }
    /// @return The radius of a command block's ⋯ button.
    [[nodiscard]] qreal blockActions() const noexcept { return _tokens.blockActions * _units.horizontal; }
    /// @return The radius of the window's top corners where the client draws its frame.
    [[nodiscard]] qreal window() const noexcept { return _tokens.window * _units.horizontal; }
    /// @return The radius of a round window button.
    [[nodiscard]] qreal windowButton() const noexcept { return _tokens.windowButton * _units.horizontal; }
    /// @return The radius of the quick terminal's bottom corners.
    [[nodiscard]] qreal quickTerminal() const noexcept { return _tokens.quickTerminal * _units.horizontal; }

  private:
    config::RadiusTokens _tokens {};
    ChromeUnits _units {};
};

/// Chrome fonts, `chromeStyle.type`.
class ChromeTypeTokens: public QObject
{
    Q_OBJECT
    Q_PROPERTY(QFont caption READ caption CONSTANT)
    Q_PROPERTY(QFont footnote READ footnote CONSTANT)
    Q_PROPERTY(QFont body READ body CONSTANT)
    Q_PROPERTY(QFont tab READ tab CONSTANT)
    Q_PROPERTY(QFont title READ title CONSTANT)
    Q_PROPERTY(QFont paneHeader READ paneHeader CONSTANT)
    Q_PROPERTY(int paneHeaderWeight READ paneHeaderWeight CONSTANT)
    Q_PROPERTY(QFont paneHeaderAction READ paneHeaderAction CONSTANT)
    Q_PROPERTY(QFont paletteInput READ paletteInput CONSTANT)
    Q_PROPERTY(QFont shortcut READ shortcut CONSTANT)
    // The weighted variants QML would otherwise make by copying a font and setting its weight -- which does
    // not stick on a CONSTANT QFont property: the copy keeps the token's weight.
    Q_PROPERTY(QFont bodyStrong READ bodyStrong CONSTANT)
    Q_PROPERTY(QFont bodyMedium READ bodyMedium CONSTANT)
    Q_PROPERTY(QFont footnoteStrong READ footnoteStrong CONSTANT)
    Q_PROPERTY(QFont footnoteSemibold READ footnoteSemibold CONSTANT)

  public:
    /// @param tokens The resolved type group; a size of 0 keeps @p font's own.
    /// @param font   The chrome font every size is applied to.
    /// @param parent Qt ownership.
    ChromeTypeTokens(config::TypeTokens tokens, QFont const& font, QObject* parent);

    /// @return The caption font.
    [[nodiscard]] QFont const& caption() const noexcept { return _caption; }
    /// @return The footnote font.
    [[nodiscard]] QFont const& footnote() const noexcept { return _footnote; }
    /// @return The body font.
    [[nodiscard]] QFont const& body() const noexcept { return _body; }
    /// @return The tab title font.
    [[nodiscard]] QFont const& tab() const noexcept { return _tab; }
    /// @return The title font.
    [[nodiscard]] QFont const& title() const noexcept { return _title; }
    /// @return The pane header font, at the row's header weight.
    [[nodiscard]] QFont const& paneHeader() const noexcept { return _paneHeader; }
    /// @return The pane header's weight on the CSS 100-900 scale.
    [[nodiscard]] int paneHeaderWeight() const noexcept { return _paneHeaderWeight; }
    /// @return The font a pane header button's glyph is drawn in.
    [[nodiscard]] QFont const& paneHeaderAction() const noexcept { return _paneHeaderAction; }
    /// @return The command palette's query font.
    [[nodiscard]] QFont const& paletteInput() const noexcept { return _paletteInput; }
    /// @return The font of a command palette row's shortcut.
    [[nodiscard]] QFont const& shortcut() const noexcept { return _shortcut; }
    /// @return The body font, bold: a dialog's heading.
    [[nodiscard]] QFont const& bodyStrong() const noexcept { return _bodyStrong; }
    /// @return The body font, medium: the band's title.
    [[nodiscard]] QFont const& bodyMedium() const noexcept { return _bodyMedium; }
    /// @return The footnote font, bold: the host chip's name, a permission line that can run commands.
    [[nodiscard]] QFont const& footnoteStrong() const noexcept { return _footnoteStrong; }
    /// @return The footnote font, semibold: a command palette group's header.
    [[nodiscard]] QFont const& footnoteSemibold() const noexcept { return _footnoteSemibold; }

  private:
    QFont _caption {};
    QFont _footnote {};
    QFont _body {};
    QFont _tab {};
    QFont _title {};
    QFont _paneHeader {};
    int _paneHeaderWeight = 0;
    QFont _paneHeaderAction {};
    QFont _paletteInput {};
    QFont _shortcut {};
    QFont _bodyStrong {};
    QFont _bodyMedium {};
    QFont _footnoteStrong {};
    QFont _footnoteSemibold {};
};

/// Animation durations and travel, `chromeStyle.motion`.
class ChromeMotionTokens: public QObject
{
    Q_OBJECT
    Q_PROPERTY(int fast READ fast CONSTANT)
    Q_PROPERTY(int standard READ standard CONSTANT)
    Q_PROPERTY(int emphasized READ emphasized CONSTANT)
    Q_PROPERTY(qreal travel READ travel CONSTANT)
    Q_PROPERTY(int spinner READ spinner CONSTANT)
    Q_PROPERTY(int pulse READ pulse CONSTANT)
    Q_PROPERTY(int style READ style CONSTANT)
    Q_PROPERTY(int full READ full CONSTANT)
    // A bool for QML (the QML-facing carve-out): a read-only predicate over `style`, which stays the source
    // of truth as a config::MotionStyle; QML would otherwise compare it to `full` at every use.
    Q_PROPERTY(bool animates READ animates CONSTANT)
    Q_PROPERTY(int crossFade READ crossFade CONSTANT)
    Q_PROPERTY(int headerReveal READ headerReveal CONSTANT)
    Q_PROPERTY(qreal popoverScale READ popoverScale CONSTANT)
    Q_PROPERTY(int linger READ linger CONSTANT)
    Q_PROPERTY(int tooltipDelay READ tooltipDelay CONSTANT)
    Q_PROPERTY(int tooltipWarm READ tooltipWarm CONSTANT)

  public:
    /// @param tokens The resolved motion group.
    /// @param units  What one token unit means in pixels.
    /// @param parent Qt ownership.
    ChromeMotionTokens(config::MotionTokens tokens, ChromeUnits units, QObject* parent);

    /// @return The fast duration in milliseconds.
    [[nodiscard]] int fast() const noexcept { return _tokens.fastMs; }
    /// @return The standard duration in milliseconds.
    [[nodiscard]] int standard() const noexcept { return _tokens.standardMs; }
    /// @return The emphasised duration in milliseconds.
    [[nodiscard]] int emphasized() const noexcept { return _tokens.emphasizedMs; }
    /// @return How far a sliding element travels, in logical pixels.
    [[nodiscard]] qreal travel() const noexcept { return _tokens.travel * _units.horizontal; }
    /// @return One turn of the Working spinner, in milliseconds.
    [[nodiscard]] int spinner() const noexcept { return _tokens.spinnerMs; }
    /// @return One half-cycle of the progress pulse, in milliseconds.
    [[nodiscard]] int pulse() const noexcept { return _tokens.pulseMs; }
    /// @return How the chrome animates, a config::MotionStyle as its underlying value.
    [[nodiscard]] int style() const noexcept { return static_cast<int>(_tokens.style); }
    /// @return How long the reveal modifier is held before Minimal's headers show, in milliseconds.
    [[nodiscard]] int headerReveal() const noexcept { return _tokens.headerRevealMs; }
    /// @return The scale a popover opens from: 1 opens at full size (a fade only).
    [[nodiscard]] qreal popoverScale() const noexcept { return _tokens.popoverScalePercent / 100.0; }
    /// @return How long the size indicator stays after the last change, in milliseconds.
    [[nodiscard]] int linger() const noexcept { return _tokens.lingerMs; }
    /// @return How long a pointer rests on a control before its first tooltip shows, in milliseconds.
    [[nodiscard]] int tooltipDelay() const noexcept { return _tokens.tooltipDelayMs; }
    /// @return How long after a tooltip the next one shows with no delay, in milliseconds.
    [[nodiscard]] int tooltipWarm() const noexcept { return _tokens.tooltipWarmMs; }

    /// @return The QML names of config::MotionStyle's enumerators.
    [[nodiscard]] static constexpr int full() noexcept { return static_cast<int>(config::MotionStyle::Full); }
    /// @return Whether the chrome runs its spinners and pulses: false under Reduce Motion (CrossFade).
    [[nodiscard]] bool animates() const noexcept { return _tokens.style == config::MotionStyle::Full; }
    [[nodiscard]] static constexpr int crossFade() noexcept
    {
        return static_cast<int>(config::MotionStyle::CrossFade);
    }

  private:
    config::MotionTokens _tokens {};
    ChromeUnits _units {};
};

/// One drop shadow, `chromeStyle.elevation.e0` .. `e3`.
class ChromeElevationLevel: public QObject
{
    Q_OBJECT
    Q_PROPERTY(qreal offsetX READ offsetX CONSTANT)
    Q_PROPERTY(qreal offsetY READ offsetY CONSTANT)
    Q_PROPERTY(qreal blur READ blur CONSTANT)
    Q_PROPERTY(qreal opacity READ opacity CONSTANT)

  public:
    /// @param level  The resolved shadow.
    /// @param units  What one token unit means in pixels.
    /// @param parent Qt ownership.
    ChromeElevationLevel(config::ElevationLevel level, ChromeUnits units, QObject* parent);

    /// @return The horizontal offset in logical pixels.
    [[nodiscard]] qreal offsetX() const noexcept { return _level.offsetX * _units.horizontal; }
    /// @return The vertical offset in logical pixels.
    [[nodiscard]] qreal offsetY() const noexcept { return _level.offsetY * _units.vertical; }
    /// @return The blur radius in logical pixels.
    [[nodiscard]] qreal blur() const noexcept { return _level.blur * _units.vertical; }
    /// @return The shadow opacity, 0 to 1.
    [[nodiscard]] qreal opacity() const noexcept { return _level.opacityPercent / 100.0; }

  private:
    config::ElevationLevel _level {};
    ChromeUnits _units {};
};

/// The shadows e0..e3, `chromeStyle.elevation`.
class ChromeElevationTokens: public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject* e0 READ e0 CONSTANT)
    Q_PROPERTY(QObject* e1 READ e1 CONSTANT)
    Q_PROPERTY(QObject* e2 READ e2 CONSTANT)
    Q_PROPERTY(QObject* e3 READ e3 CONSTANT)
    Q_PROPERTY(QObject* inPane READ inPane CONSTANT)

  public:
    /// @param tokens The resolved elevation group.
    /// @param units  What one token unit means in pixels.
    /// @param parent Qt ownership.
    ChromeElevationTokens(config::ElevationTokens const& tokens, ChromeUnits units, QObject* parent);

    /// @return The shadows, e0 first. Non-const because QML reads them as mutable QObject pointers.
    [[nodiscard]] QObject* e0() noexcept { return &_e0; }
    [[nodiscard]] QObject* e1() noexcept { return &_e1; }
    [[nodiscard]] QObject* e2() noexcept { return &_e2; }
    [[nodiscard]] QObject* e3() noexcept { return &_e3; }

    /// The shadow a pill drawn in a pane (the size indicator, the reconnecting pill) casts: e2 where the
    /// row's shadows are soft, none where they are the Terminal row's hard offset copy, which would read as a
    /// second row beneath it. Only a real popover casts that one.
    [[nodiscard]] QObject* inPane() noexcept { return _e2.blur() > 0 ? &_e2 : &_e0; }

  private:
    ChromeElevationLevel _e0;
    ChromeElevationLevel _e1;
    ChromeElevationLevel _e2;
    ChromeElevationLevel _e3;
};

/// Surface materials, `chromeStyle.material`, each a config::Material as its underlying value.
///
/// The enumerators are named by properties rather than an exported enum: a Q_ENUM is not reachable
/// through a context-property instance (see UiStyleProvider::statusDanger).
class ChromeMaterialTokens: public QObject
{
    Q_OBJECT
    Q_PROPERTY(int titleBand READ titleBand CONSTANT)
    Q_PROPERTY(int popover READ popover CONSTANT)
    Q_PROPERTY(int none READ none CONSTANT)
    Q_PROPERTY(int glass READ glass CONSTANT)
    Q_PROPERTY(int mica READ mica CONSTANT)
    Q_PROPERTY(int solid READ solid CONSTANT)

  public:
    /// @param tokens The resolved material group.
    /// @param parent Qt ownership.
    ChromeMaterialTokens(config::MaterialTokens tokens, QObject* parent);

    /// @return The title band's material.
    [[nodiscard]] int titleBand() const noexcept { return static_cast<int>(_tokens.titleBand); }
    /// @return The popovers' material.
    [[nodiscard]] int popover() const noexcept { return static_cast<int>(_tokens.popover); }

    /// @return The QML names of config::Material's enumerators.
    [[nodiscard]] static constexpr int none() noexcept { return static_cast<int>(config::Material::None); }
    [[nodiscard]] static constexpr int glass() noexcept { return static_cast<int>(config::Material::Glass); }
    [[nodiscard]] static constexpr int mica() noexcept { return static_cast<int>(config::Material::Mica); }
    [[nodiscard]] static constexpr int solid() noexcept { return static_cast<int>(config::Material::Solid); }

  private:
    config::MaterialTokens _tokens {};
};

/// Separation strength, `chromeStyle.contrast`.
class ChromeContrastTokens: public QObject
{
    Q_OBJECT
    Q_PROPERTY(qreal separatorAlpha READ separatorAlpha CONSTANT)
    Q_PROPERTY(qreal activeCardRing READ activeCardRing CONSTANT)

  public:
    /// @param tokens The resolved contrast group.
    /// @param parent Qt ownership.
    ChromeContrastTokens(config::ContrastTokens tokens, QObject* parent);

    /// @return The separators' opacity, 0 to 1.
    [[nodiscard]] qreal separatorAlpha() const noexcept { return _tokens.separatorPercent / 100.0; }
    /// @return The active pane card's ring width in logical pixels.
    [[nodiscard]] qreal activeCardRing() const noexcept { return _tokens.activeCardRing; }

  private:
    config::ContrastTokens _tokens {};
};

/// Terminal-style specifics, `chromeStyle.tui`.
class ChromeTuiTokens: public QObject
{
    Q_OBJECT
    Q_PROPERTY(qreal inactivePaneOpacity READ inactivePaneOpacity CONSTANT)
    Q_PROPERTY(int toastStyle READ toastStyle CONSTANT)
    Q_PROPERTY(int floating READ floating CONSTANT)
    Q_PROPERTY(int inverse READ inverse CONSTANT)

  public:
    /// @param tokens The resolved Terminal-style group.
    /// @param parent Qt ownership.
    ChromeTuiTokens(config::TuiTokens tokens, QObject* parent);

    /// @return An inactive pane's opacity, 0 to 1.
    [[nodiscard]] qreal inactivePaneOpacity() const noexcept { return _tokens.inactivePaneOpacity; }
    /// @return How toasts are painted, a config::ToastStyle as its underlying value.
    [[nodiscard]] int toastStyle() const noexcept { return static_cast<int>(_tokens.toastStyle); }

    /// @return The QML names of config::ToastStyle's enumerators.
    [[nodiscard]] static constexpr int floating() noexcept
    {
        return static_cast<int>(config::ToastStyle::Floating);
    }
    [[nodiscard]] static constexpr int inverse() noexcept
    {
        return static_cast<int>(config::ToastStyle::Inverse);
    }

  private:
    config::TuiTokens _tokens {};
};

/// Tints over chrome surfaces, `chromeStyle.tint`.
class ChromeTintTokens: public QObject
{
    Q_OBJECT
    Q_PROPERTY(qreal tab READ tab CONSTANT)

  public:
    /// @param tokens The resolved tint group.
    /// @param parent Qt ownership.
    ChromeTintTokens(config::TintTokens tokens, QObject* parent);

    /// @return A tab colour's opacity over its pill, 0 to 1.
    [[nodiscard]] qreal tab() const noexcept { return _tokens.tabPercent / 100.0; }

  private:
    config::TintTokens _tokens {};
};

/// The Settings window's extents and type, `chromeStyle.settings`. Widths, edges and radii are in
/// horizontal units, heights and gaps in vertical ones; a type size is in logical pixels, 0 for the chrome
/// font's own.
class ChromeSettingsTokens: public QObject
{
    Q_OBJECT
    Q_PROPERTY(qreal sidebarWidth READ sidebarWidth CONSTANT)
    Q_PROPERTY(qreal sidebarRow READ sidebarRow CONSTANT)
    Q_PROPERTY(qreal iconTile READ iconTile CONSTANT)
    Q_PROPERTY(qreal iconGlyph READ iconGlyph CONSTANT)
    Q_PROPERTY(qreal searchField READ searchField CONSTANT)
    Q_PROPERTY(qreal headerBar READ headerBar CONSTANT)
    Q_PROPERTY(qreal listMaxWidth READ listMaxWidth CONSTANT)
    Q_PROPERTY(qreal groupRadius READ groupRadius CONSTANT)
    Q_PROPERTY(qreal row READ row CONSTANT)
    Q_PROPERTY(qreal rowWithHelp READ rowWithHelp CONSTANT)
    Q_PROPERTY(qreal rowPadding READ rowPadding CONSTANT)
    Q_PROPERTY(qreal cardGap READ cardGap CONSTANT)
    Q_PROPERTY(qreal groupGap READ groupGap CONSTANT)
    Q_PROPERTY(qreal groupTitleGap READ groupTitleGap CONSTANT)
    Q_PROPERTY(qreal control READ control CONSTANT)
    Q_PROPERTY(qreal switchWidth READ switchWidth CONSTANT)
    Q_PROPERTY(qreal switchHeight READ switchHeight CONSTANT)
    Q_PROPERTY(qreal slider READ slider CONSTANT)
    Q_PROPERTY(qreal stepperField READ stepperField CONSTANT)
    Q_PROPERTY(qreal colorWell READ colorWell CONSTANT)
    Q_PROPERTY(qreal previewHeight READ previewHeight CONSTANT)
    Q_PROPERTY(qreal previewWidth READ previewWidth CONSTANT)
    Q_PROPERTY(qreal previewRadius READ previewRadius CONSTANT)
    Q_PROPERTY(qreal disabledOpacity READ disabledOpacity CONSTANT)
    Q_PROPERTY(qreal labelType READ labelType CONSTANT)
    Q_PROPERTY(qreal helpType READ helpType CONSTANT)
    Q_PROPERTY(qreal groupTitleType READ groupTitleType CONSTANT)
    Q_PROPERTY(qreal pageTitleType READ pageTitleType CONSTANT)

  public:
    /// @param tokens The resolved Settings group.
    /// @param units  What one unit of an extent is.
    /// @param parent Qt ownership.
    ChromeSettingsTokens(config::SettingsTokens tokens, ChromeUnits units, QObject* parent);

    /// @return The sidebar's width.
    [[nodiscard]] qreal sidebarWidth() const noexcept { return _tokens.sidebarWidth * _units.horizontal; }
    /// @return A sidebar row's height.
    [[nodiscard]] qreal sidebarRow() const noexcept { return _tokens.sidebarRow * _units.vertical; }
    /// @return The tile behind a sidebar icon, its edge; 0 draws none.
    [[nodiscard]] qreal iconTile() const noexcept { return _tokens.iconTile * _units.horizontal; }
    /// @return A sidebar icon's edge.
    [[nodiscard]] qreal iconGlyph() const noexcept { return _tokens.iconGlyph * _units.horizontal; }
    /// @return The search field's height.
    [[nodiscard]] qreal searchField() const noexcept { return _tokens.searchField * _units.vertical; }
    /// @return The page header's height.
    [[nodiscard]] qreal headerBar() const noexcept { return _tokens.headerBar * _units.vertical; }
    /// @return The widest the list of groups grows; 0 fills the page.
    [[nodiscard]] qreal listMaxWidth() const noexcept { return _tokens.listMaxWidth * _units.horizontal; }
    /// @return A group's corner radius.
    [[nodiscard]] qreal groupRadius() const noexcept { return _tokens.groupRadius * _units.horizontal; }
    /// @return A row's height.
    [[nodiscard]] qreal row() const noexcept { return _tokens.row * _units.vertical; }
    /// @return A row's height when it carries a help line.
    [[nodiscard]] qreal rowWithHelp() const noexcept { return _tokens.rowWithHelp * _units.vertical; }
    /// @return A row's horizontal padding.
    [[nodiscard]] qreal rowPadding() const noexcept { return _tokens.rowPadding * _units.horizontal; }
    /// @return The gap between the rows of one group.
    [[nodiscard]] qreal cardGap() const noexcept { return _tokens.cardGap * _units.vertical; }
    /// @return The gap between groups.
    [[nodiscard]] qreal groupGap() const noexcept { return _tokens.groupGap * _units.vertical; }
    /// @return The gap from a group's title to its list.
    [[nodiscard]] qreal groupTitleGap() const noexcept { return _tokens.groupTitleGap * _units.vertical; }
    /// @return A pop-up, button or field's height.
    [[nodiscard]] qreal control() const noexcept { return _tokens.control * _units.vertical; }
    /// @return A switch's width.
    [[nodiscard]] qreal switchWidth() const noexcept { return _tokens.switchWidth * _units.horizontal; }
    /// @return A switch's height.
    [[nodiscard]] qreal switchHeight() const noexcept { return _tokens.switchHeight * _units.vertical; }
    /// @return A slider's track width.
    [[nodiscard]] qreal slider() const noexcept { return _tokens.slider * _units.horizontal; }
    /// @return A stepper's value field width.
    [[nodiscard]] qreal stepperField() const noexcept { return _tokens.stepperField * _units.horizontal; }
    /// @return A colour well's edge.
    [[nodiscard]] qreal colorWell() const noexcept { return _tokens.colorWell * _units.horizontal; }
    /// @return The live preview's height.
    [[nodiscard]] qreal previewHeight() const noexcept { return _tokens.previewHeight * _units.vertical; }
    /// @return The live preview's width; 0 fills the page.
    [[nodiscard]] qreal previewWidth() const noexcept { return _tokens.previewWidth * _units.horizontal; }
    /// @return The live preview's corner radius.
    [[nodiscard]] qreal previewRadius() const noexcept { return _tokens.previewRadius * _units.horizontal; }
    /// @return The opacity of a row's label and control where it cannot be edited, 0 to 1.
    [[nodiscard]] qreal disabledOpacity() const noexcept { return _tokens.disabledPercent / 100.0; }
    /// @return A row's label size in logical pixels; 0 is the chrome font's own.
    [[nodiscard]] qreal labelType() const noexcept { return _tokens.labelType; }
    /// @return A row's help line size in logical pixels; 0 is the chrome font's own.
    [[nodiscard]] qreal helpType() const noexcept { return _tokens.helpType; }
    /// @return A group's title size in logical pixels; 0 is the chrome font's own.
    [[nodiscard]] qreal groupTitleType() const noexcept { return _tokens.groupTitleType; }
    /// @return A page's title size in logical pixels; 0 is the chrome font's own.
    [[nodiscard]] qreal pageTitleType() const noexcept { return _tokens.pageTitleType; }

  private:
    config::SettingsTokens _tokens {};
    ChromeUnits _units {};
};

/// What the Settings layout stands for, `chromeStyle.settingsTraits`: the traits as ints, and the QML names
/// of their enumerators, so a page compares a trait to a name and never to a bare number.
class ChromeSettingsTraits: public QObject
{
    Q_OBJECT
    Q_PROPERTY(int header READ header CONSTANT)
    Q_PROPERTY(int toolbar READ toolbar CONSTANT)
    Q_PROPERTY(int breadcrumb READ breadcrumb CONSTANT)
    Q_PROPERTY(int splitHeaderBar READ splitHeaderBar CONSTANT)
    Q_PROPERTY(int rows READ rows CONSTANT)
    Q_PROPERTY(int grouped READ grouped CONSTANT)
    Q_PROPERTY(int separateCards READ separateCards CONSTANT)
    Q_PROPERTY(int stepper READ stepper CONSTANT)
    Q_PROPERTY(int stacked READ stacked CONSTANT)
    Q_PROPERTY(int sideBySide READ sideBySide CONSTANT)
    Q_PROPERTY(int inlineArrows READ inlineArrows CONSTANT)

  public:
    /// @param layout The row's Settings layout, whose traits these are.
    /// @param parent Qt ownership.
    ChromeSettingsTraits(config::SettingsLayout layout, QObject* parent);

    /// @return Where a page's title sits, a config::SettingsHeader as its underlying value.
    [[nodiscard]] int header() const noexcept { return static_cast<int>(_traits.header); }
    /// @return The QML names of config::SettingsHeader's enumerators.
    [[nodiscard]] static constexpr int toolbar() noexcept
    {
        return static_cast<int>(config::SettingsHeader::Toolbar);
    }
    [[nodiscard]] static constexpr int breadcrumb() noexcept
    {
        return static_cast<int>(config::SettingsHeader::Breadcrumb);
    }
    [[nodiscard]] static constexpr int splitHeaderBar() noexcept
    {
        return static_cast<int>(config::SettingsHeader::SplitHeaderBar);
    }

    /// @return How a page's rows are shaped, a config::SettingsRowShape as its underlying value.
    [[nodiscard]] int rows() const noexcept { return static_cast<int>(_traits.rows); }
    /// @return The QML names of config::SettingsRowShape's enumerators.
    [[nodiscard]] static constexpr int grouped() noexcept
    {
        return static_cast<int>(config::SettingsRowShape::Grouped);
    }
    [[nodiscard]] static constexpr int separateCards() noexcept
    {
        return static_cast<int>(config::SettingsRowShape::SeparateCards);
    }

    /// @return How a stepper's arrows sit, a config::StepperArrangement as its underlying value.
    [[nodiscard]] int stepper() const noexcept { return static_cast<int>(_traits.stepper); }
    /// @return The QML names of config::StepperArrangement's enumerators.
    [[nodiscard]] static constexpr int stacked() noexcept
    {
        return static_cast<int>(config::StepperArrangement::Stacked);
    }
    [[nodiscard]] static constexpr int sideBySide() noexcept
    {
        return static_cast<int>(config::StepperArrangement::SideBySide);
    }
    [[nodiscard]] static constexpr int inlineArrows() noexcept
    {
        return static_cast<int>(config::StepperArrangement::Inline);
    }

  private:
    config::SettingsLayoutTraits _traits {};
};

/// Per-style layout choices, `chromeStyle.layout`.
class ChromeLayoutTokens: public QObject
{
    Q_OBJECT
    Q_PROPERTY(int dialog READ dialog CONSTANT)
    Q_PROPERTY(int centered READ centered CONSTANT)
    Q_PROPERTY(int sheet READ sheet CONSTANT)
    Q_PROPERTY(int tabShape READ tabShape CONSTANT)
    Q_PROPERTY(int flat READ flat CONSTANT)
    Q_PROPERTY(int pill READ pill CONSTANT)
    Q_PROPERTY(int joined READ joined CONSTANT)
    // Bools for QML (the QML-facing carve-out): read-only predicates over `tabShape`, which stays the
    // source of truth as a config::TabShape; the one place the shape is compared, so the tab, the strip and
    // the badge slot cannot disagree on it.
    Q_PROPERTY(bool isPill READ isPill CONSTANT)
    // A bool for QML (the same carve-out): a read-only predicate over `blockColumn`, which stays the source
    // of truth as a vtbackend::BlockColumnForm.
    Q_PROPERTY(bool isBlockRail READ isBlockRail CONSTANT)
    Q_PROPERTY(bool isJoined READ isJoined CONSTANT)
    Q_PROPERTY(int profileMenu READ profileMenu CONSTANT)
    Q_PROPERTY(int caret READ caret CONSTANT)
    Q_PROPERTY(int secondaryClick READ secondaryClick CONSTANT)
    Q_PROPERTY(int tabColors READ tabColors CONSTANT)
    Q_PROPERTY(int palette READ palette CONSTANT)
    Q_PROPERTY(int roles READ roles CONSTANT)
    Q_PROPERTY(int paneActions READ paneActions CONSTANT)
    Q_PROPERTY(int onHover READ onHover CONSTANT)
    Q_PROPERTY(int onHoverOrFocus READ onHoverOrFocus CONSTANT)
    Q_PROPERTY(int paletteTopPercent READ paletteTopPercent CONSTANT)
    Q_PROPERTY(int settings READ settings CONSTANT)
    Q_PROPERTY(int insetGrouped READ insetGrouped CONSTANT)
    Q_PROPERTY(int cards READ cards CONSTANT)
    Q_PROPERTY(int boxedList READ boxedList CONSTANT)
    Q_PROPERTY(int cells READ cells CONSTANT)
    Q_PROPERTY(int headerBar READ headerBar CONSTANT)
    Q_PROPERTY(int band READ band CONSTANT)
    Q_PROPERTY(int unified READ unified CONSTANT)
    Q_PROPERTY(int tabClose READ tabClose CONSTANT)
    Q_PROPERTY(int glyph READ glyph CONSTANT)
    Q_PROPERTY(int symbolicIcon READ symbolicIcon CONSTANT)
    Q_PROPERTY(QString tabCloseIconName READ tabCloseIconName CONSTANT)
    Q_PROPERTY(QString tabCloseIconSource READ tabCloseIconSource CONSTANT)

  public:
    /// @param tokens The resolved layout group.
    /// @param parent Qt ownership.
    ChromeLayoutTokens(config::LayoutTokens tokens, QObject* parent);

    /// @return Where dialogs appear, a config::DialogPlacement as its underlying value.
    [[nodiscard]] int dialog() const noexcept { return static_cast<int>(_tokens.dialog); }

    /// @return The QML names of config::DialogPlacement's enumerators.
    [[nodiscard]] static constexpr int centered() noexcept
    {
        return static_cast<int>(config::DialogPlacement::Centered);
    }
    [[nodiscard]] static constexpr int sheet() noexcept
    {
        return static_cast<int>(config::DialogPlacement::Sheet);
    }

    /// @return How the tabs are drawn, a config::TabShape as its underlying value.
    [[nodiscard]] int tabShape() const noexcept { return static_cast<int>(_tokens.tabShape); }
    /// @return The QML names of config::TabShape's enumerators.
    [[nodiscard]] static constexpr int flat() noexcept { return static_cast<int>(config::TabShape::Flat); }
    [[nodiscard]] static constexpr int pill() noexcept { return static_cast<int>(config::TabShape::Pill); }
    [[nodiscard]] static constexpr int joined() noexcept
    {
        return static_cast<int>(config::TabShape::Joined);
    }
    /// @return Whether the tabs are pills in a well (config::TabShape::Pill).
    [[nodiscard]] bool isPill() const noexcept { return _tokens.tabShape == config::TabShape::Pill; }
    /// @return Whether the block column is a pixel strip holding the rail (vtbackend::BlockColumnForm::Rail).
    [[nodiscard]] bool isBlockRail() const noexcept
    {
        return _tokens.blockColumn == vtbackend::BlockColumnForm::Rail;
    }
    /// @return Whether the tabs join the content as the title bar (config::TabShape::Joined).
    [[nodiscard]] bool isJoined() const noexcept { return _tokens.tabShape == config::TabShape::Joined; }

    /// @return How the profile menu is reached, a config::ProfileMenuAffordance as its underlying value.
    [[nodiscard]] int profileMenu() const noexcept { return static_cast<int>(_tokens.profileMenu); }
    /// @return The QML names of config::ProfileMenuAffordance's enumerators.
    [[nodiscard]] static constexpr int caret() noexcept
    {
        return static_cast<int>(config::ProfileMenuAffordance::Caret);
    }
    [[nodiscard]] static constexpr int secondaryClick() noexcept
    {
        return static_cast<int>(config::ProfileMenuAffordance::SecondaryClick);
    }

    /// @return Where a tab's colours come from, a config::TabColorSource as its underlying value.
    [[nodiscard]] int tabColors() const noexcept { return static_cast<int>(_tokens.tabColors); }
    /// @return The QML names of config::TabColorSource's enumerators.
    [[nodiscard]] static constexpr int palette() noexcept
    {
        return static_cast<int>(config::TabColorSource::Palette);
    }
    [[nodiscard]] static constexpr int roles() noexcept
    {
        return static_cast<int>(config::TabColorSource::Roles);
    }

    /// @return When a pane header shows its actions, a config::PaneActionsReveal as its underlying value.
    [[nodiscard]] int paneActions() const noexcept { return static_cast<int>(_tokens.paneActions); }
    /// @return The QML names of config::PaneActionsReveal's enumerators.
    [[nodiscard]] static constexpr int onHover() noexcept
    {
        return static_cast<int>(config::PaneActionsReveal::OnHover);
    }
    [[nodiscard]] static constexpr int onHoverOrFocus() noexcept
    {
        return static_cast<int>(config::PaneActionsReveal::OnHoverOrFocus);
    }

    /// @return The command palette's top edge, in percent of the window's height.
    [[nodiscard]] int paletteTopPercent() const noexcept { return _tokens.paletteTopPercent; }

    /// @return How the Settings window is laid out, a config::SettingsLayout as its underlying value.
    [[nodiscard]] int settings() const noexcept { return static_cast<int>(_tokens.settings); }
    /// @return The QML names of config::SettingsLayout's enumerators.
    [[nodiscard]] static constexpr int insetGrouped() noexcept
    {
        return static_cast<int>(config::SettingsLayout::InsetGrouped);
    }
    [[nodiscard]] static constexpr int cards() noexcept
    {
        return static_cast<int>(config::SettingsLayout::Cards);
    }
    [[nodiscard]] static constexpr int boxedList() noexcept
    {
        return static_cast<int>(config::SettingsLayout::BoxedList);
    }
    [[nodiscard]] static constexpr int cells() noexcept
    {
        return static_cast<int>(config::SettingsLayout::Cells);
    }

    /// @return How the window's top row is arranged, a config::HeaderBarArrangement as its underlying value.
    [[nodiscard]] int headerBar() const noexcept { return static_cast<int>(_tokens.headerBar); }
    /// @return The QML names of config::HeaderBarArrangement's enumerators.
    [[nodiscard]] static constexpr int band() noexcept
    {
        return static_cast<int>(config::HeaderBarArrangement::Band);
    }
    [[nodiscard]] static constexpr int unified() noexcept
    {
        return static_cast<int>(config::HeaderBarArrangement::Unified);
    }

    /// @return What a tab's close button shows, a config::TabCloseMark as its underlying value.
    [[nodiscard]] int tabClose() const noexcept { return static_cast<int>(_tokens.tabClose); }
    /// @return The QML names of config::TabCloseMark's enumerators.
    [[nodiscard]] static constexpr int glyph() noexcept
    {
        return static_cast<int>(config::TabCloseMark::Glyph);
    }
    [[nodiscard]] static constexpr int symbolicIcon() noexcept
    {
        return static_cast<int>(config::TabCloseMark::SymbolicIcon);
    }
    /// @return The theme name of the icon a config::TabCloseMark::SymbolicIcon close button shows.
    [[nodiscard]] static QString tabCloseIconName()
    {
        return QString::fromUtf8(config::TabCloseIcon.name.data(),
                                 static_cast<qsizetype>(config::TabCloseIcon.name.size()));
    }
    /// @return The bundled copy of that icon, for a theme that lacks it.
    [[nodiscard]] static QString tabCloseIconSource()
    {
        return QString::fromUtf8(config::TabCloseIcon.bundled.data(),
                                 static_cast<qsizetype>(config::TabCloseIcon.bundled.size()));
    }

  private:
    config::LayoutTokens _tokens {};
};

/// The chrome colour roles, `chromeStyle.colors`.
///
/// Resolved on every read rather than cached: a palette source reads the application palette as it
/// is now, so @ref changed is re-emitted on the provider's paletteRevisionChanged and the bindings
/// re-read.
class ChromeColorTokens: public QObject
{
    Q_OBJECT
    Q_PROPERTY(QColor bannerFill READ bannerFill NOTIFY changed)
    Q_PROPERTY(QColor bannerText READ bannerText NOTIFY changed)
    Q_PROPERTY(QColor bannerGlyph READ bannerGlyph NOTIFY changed)
    Q_PROPERTY(QColor toastFill READ toastFill NOTIFY changed)
    Q_PROPERTY(QColor toastText READ toastText NOTIFY changed)
    Q_PROPERTY(QColor swatchRing READ swatchRing NOTIFY changed)
    Q_PROPERTY(QColor tabWellFill READ tabWellFill NOTIFY changed)
    Q_PROPERTY(QColor tabActiveFill READ tabActiveFill NOTIFY changed)
    Q_PROPERTY(QColor tabHoverFill READ tabHoverFill NOTIFY changed)
    Q_PROPERTY(QColor tabCloseFill READ tabCloseFill NOTIFY changed)
    Q_PROPERTY(QColor controlPressedFill READ controlPressedFill NOTIFY changed)
    Q_PROPERTY(QColor tabText READ tabText NOTIFY changed)
    Q_PROPERTY(QColor tabTextActive READ tabTextActive NOTIFY changed)
    Q_PROPERTY(QColor tabTextDimmed READ tabTextDimmed NOTIFY changed)
    Q_PROPERTY(QColor tabTextTinted READ tabTextTinted NOTIFY changed)
    Q_PROPERTY(QColor chipFill READ chipFill NOTIFY changed)
    Q_PROPERTY(QColor chipText READ chipText NOTIFY changed)
    Q_PROPERTY(QColor chipTextStrong READ chipTextStrong NOTIFY changed)
    Q_PROPERTY(QColor captionWash READ captionWash NOTIFY changed)
    Q_PROPERTY(QColor captionGlyphPressed READ captionGlyphPressed NOTIFY changed)
    Q_PROPERTY(QColor captionGlyphInactive READ captionGlyphInactive NOTIFY changed)
    Q_PROPERTY(QColor paneGapFill READ paneGapFill NOTIFY changed)
    Q_PROPERTY(QColor paneCardBorder READ paneCardBorder NOTIFY changed)
    Q_PROPERTY(QColor paneHeaderFill READ paneHeaderFill NOTIFY changed)
    Q_PROPERTY(QColor paneHeaderText READ paneHeaderText NOTIFY changed)
    Q_PROPERTY(QColor paneHeaderTextDimmed READ paneHeaderTextDimmed NOTIFY changed)
    Q_PROPERTY(QColor paneSplitLine READ paneSplitLine NOTIFY changed)
    Q_PROPERTY(QColor paneGrabber READ paneGrabber NOTIFY changed)
    Q_PROPERTY(QColor paneFooterFill READ paneFooterFill NOTIFY changed)
    Q_PROPERTY(QColor paneFooterText READ paneFooterText NOTIFY changed)
    Q_PROPERTY(QColor paneHeaderActionHover READ paneHeaderActionHover NOTIFY changed)
    Q_PROPERTY(QColor paletteSurface READ paletteSurface NOTIFY changed)
    Q_PROPERTY(QColor paletteText READ paletteText NOTIFY changed)
    Q_PROPERTY(QColor paletteSecondaryText READ paletteSecondaryText NOTIFY changed)
    Q_PROPERTY(QColor paletteSelectionFill READ paletteSelectionFill NOTIFY changed)
    Q_PROPERTY(QColor paletteSelectionText READ paletteSelectionText NOTIFY changed)
    Q_PROPERTY(QColor paletteSeparator READ paletteSeparator NOTIFY changed)
    Q_PROPERTY(QColor findToggleOnFill READ findToggleOnFill NOTIFY changed)
    Q_PROPERTY(QColor findToggleOnText READ findToggleOnText NOTIFY changed)
    Q_PROPERTY(QColor findToggleText READ findToggleText NOTIFY changed)
    Q_PROPERTY(QColor findCountText READ findCountText NOTIFY changed)
    Q_PROPERTY(QColor findToggleOnRing READ findToggleOnRing NOTIFY changed)
    Q_PROPERTY(QColor findErrorText READ findErrorText NOTIFY changed)
    Q_PROPERTY(QColor toastTextSecondary READ toastTextSecondary NOTIFY changed)
    Q_PROPERTY(QColor scrollbarThumb READ scrollbarThumb NOTIFY changed)
    Q_PROPERTY(QColor scrollbarThumbActive READ scrollbarThumbActive NOTIFY changed)
    Q_PROPERTY(QColor scrollbarTrack READ scrollbarTrack NOTIFY changed)
    Q_PROPERTY(QColor focusRing READ focusRing NOTIFY changed)
    Q_PROPERTY(QColor focusFill READ focusFill NOTIFY changed)
    Q_PROPERTY(QColor settingsSidebarFill READ settingsSidebarFill NOTIFY changed)
    Q_PROPERTY(QColor settingsPageFill READ settingsPageFill NOTIFY changed)
    Q_PROPERTY(QColor settingsGroupFill READ settingsGroupFill NOTIFY changed)
    Q_PROPERTY(QColor settingsSeparator READ settingsSeparator NOTIFY changed)
    Q_PROPERTY(QColor settingsLabel READ settingsLabel NOTIFY changed)
    Q_PROPERTY(QColor settingsHelp READ settingsHelp NOTIFY changed)
    Q_PROPERTY(QColor settingsSelectionFill READ settingsSelectionFill NOTIFY changed)
    Q_PROPERTY(QColor settingsSelectionText READ settingsSelectionText NOTIFY changed)
    Q_PROPERTY(QColor settingsModified READ settingsModified NOTIFY changed)
    Q_PROPERTY(QColor settingsIconTile READ settingsIconTile NOTIFY changed)
    Q_PROPERTY(QColor settingsIconGlyph READ settingsIconGlyph NOTIFY changed)
    Q_PROPERTY(QColor switchOnFill READ switchOnFill NOTIFY changed)
    Q_PROPERTY(QColor switchOffFill READ switchOffFill NOTIFY changed)
    Q_PROPERTY(QColor switchKnob READ switchKnob NOTIFY changed)
    Q_PROPERTY(QColor controlFill READ controlFill NOTIFY changed)
    Q_PROPERTY(QColor controlText READ controlText NOTIFY changed)
    Q_PROPERTY(QColor previewFrame READ previewFrame NOTIFY changed)
    Q_PROPERTY(QColor windowButtonFill READ windowButtonFill NOTIFY changed)
    Q_PROPERTY(QColor windowButtonHoverFill READ windowButtonHoverFill NOTIFY changed)
    Q_PROPERTY(QColor windowButtonPressedFill READ windowButtonPressedFill NOTIFY changed)
    Q_PROPERTY(QColor windowButtonGlyph READ windowButtonGlyph NOTIFY changed)
    Q_PROPERTY(QColor headerSeparator READ headerSeparator NOTIFY changed)
    Q_PROPERTY(QColor tooltipFill READ tooltipFill NOTIFY changed)
    Q_PROPERTY(QColor tooltipText READ tooltipText NOTIFY changed)
    Q_PROPERTY(QColor tooltipTextSecondary READ tooltipTextSecondary NOTIFY changed)
    Q_PROPERTY(QColor tooltipEdge READ tooltipEdge NOTIFY changed)

  public:
    /// The colour sources, indexed by config::ChromeColorRole.
    using Sources = std::array<config::ChromeColorSource, config::ChromeColorRoleCount>;

    /// @param sources          The row's colour sources.
    /// @param scheme           The terminal scheme a @c Scheme source reads.
    /// @param separatorPercent The separator ink's opacity a @c Separator source takes
    ///                         (@c contrast.separatorPercent).
    /// @param provider         The provider a @c Status source asks, whose palette changes this re-announces.
    ///                         Borrowed; it is also this object's Qt parent.
    ChromeColorTokens(Sources sources,
                      config::ChromeSchemeColors scheme,
                      int separatorPercent,
                      UiStyleProvider& provider);

    /// @param role The role to resolve.
    /// @return The role's colour as the application palette paints it now.
    [[nodiscard]] QColor color(config::ChromeColorRole role) const;

    /// Resolves any colour source of this row, on the chrome the application palette paints now.
    /// @param source The source, e.g. one of the row's status inks.
    /// @return Its colour.
    [[nodiscard]] QColor resolve(config::ChromeColorSource const& source) const;

    /// Resolves @p source as it reads on a chrome of colour scheme @p chrome: the kit's @c Fill,
    /// @c Label, @c Surface and @c Separator sources pick their light or dark half by it, every other kind
    /// ignores it.
    /// @param source The source.
    /// @param chrome Whether the ground it sits on is light or dark.
    /// @return Its colour.
    [[nodiscard]] QColor resolve(config::ChromeColorSource const& source, Qt::ColorScheme chrome) const;

    /// @return The resolved colour of each role.
    [[nodiscard]] QColor bannerFill() const { return color(config::ChromeColorRole::BannerFill); }
    [[nodiscard]] QColor bannerText() const { return color(config::ChromeColorRole::BannerText); }
    [[nodiscard]] QColor bannerGlyph() const { return color(config::ChromeColorRole::BannerGlyph); }
    [[nodiscard]] QColor toastFill() const { return color(config::ChromeColorRole::ToastFill); }
    [[nodiscard]] QColor toastText() const { return color(config::ChromeColorRole::ToastText); }
    [[nodiscard]] QColor swatchRing() const { return color(config::ChromeColorRole::SwatchRing); }
    [[nodiscard]] QColor tabWellFill() const { return color(config::ChromeColorRole::TabWellFill); }
    [[nodiscard]] QColor tabActiveFill() const { return color(config::ChromeColorRole::TabActiveFill); }
    [[nodiscard]] QColor tabHoverFill() const { return color(config::ChromeColorRole::TabHoverFill); }
    [[nodiscard]] QColor tabCloseFill() const { return color(config::ChromeColorRole::TabCloseFill); }
    [[nodiscard]] QColor controlPressedFill() const
    {
        return color(config::ChromeColorRole::ControlPressedFill);
    }
    [[nodiscard]] QColor tabText() const { return color(config::ChromeColorRole::TabText); }
    [[nodiscard]] QColor tabTextActive() const { return color(config::ChromeColorRole::TabTextActive); }
    [[nodiscard]] QColor tabTextDimmed() const { return color(config::ChromeColorRole::TabTextDimmed); }
    [[nodiscard]] QColor tabTextTinted() const { return color(config::ChromeColorRole::TabTextTinted); }
    [[nodiscard]] QColor chipFill() const { return color(config::ChromeColorRole::ChipFill); }
    [[nodiscard]] QColor chipText() const { return color(config::ChromeColorRole::ChipText); }
    [[nodiscard]] QColor chipTextStrong() const { return color(config::ChromeColorRole::ChipTextStrong); }
    [[nodiscard]] QColor captionWash() const { return color(config::ChromeColorRole::CaptionWash); }
    [[nodiscard]] QColor captionGlyphPressed() const
    {
        return color(config::ChromeColorRole::CaptionGlyphPressed);
    }
    [[nodiscard]] QColor captionGlyphInactive() const
    {
        return color(config::ChromeColorRole::CaptionGlyphInactive);
    }
    [[nodiscard]] QColor paneGapFill() const { return color(config::ChromeColorRole::PaneGapFill); }
    [[nodiscard]] QColor paneCardBorder() const { return color(config::ChromeColorRole::PaneCardBorder); }
    [[nodiscard]] QColor paneHeaderFill() const { return color(config::ChromeColorRole::PaneHeaderFill); }
    [[nodiscard]] QColor paneHeaderText() const { return color(config::ChromeColorRole::PaneHeaderText); }
    [[nodiscard]] QColor paneHeaderTextDimmed() const
    {
        return color(config::ChromeColorRole::PaneHeaderTextDimmed);
    }
    [[nodiscard]] QColor paneSplitLine() const { return color(config::ChromeColorRole::PaneSplitLine); }
    [[nodiscard]] QColor paneGrabber() const { return color(config::ChromeColorRole::PaneGrabber); }
    [[nodiscard]] QColor paneFooterFill() const { return color(config::ChromeColorRole::PaneFooterFill); }
    [[nodiscard]] QColor paneFooterText() const { return color(config::ChromeColorRole::PaneFooterText); }
    [[nodiscard]] QColor paneHeaderActionHover() const
    {
        return color(config::ChromeColorRole::PaneHeaderActionHover);
    }
    [[nodiscard]] QColor paletteSurface() const { return color(config::ChromeColorRole::PaletteSurface); }
    [[nodiscard]] QColor paletteText() const { return color(config::ChromeColorRole::PaletteText); }
    [[nodiscard]] QColor paletteSecondaryText() const
    {
        return color(config::ChromeColorRole::PaletteSecondaryText);
    }
    [[nodiscard]] QColor paletteSelectionFill() const
    {
        return color(config::ChromeColorRole::PaletteSelectionFill);
    }
    [[nodiscard]] QColor paletteSelectionText() const
    {
        return color(config::ChromeColorRole::PaletteSelectionText);
    }
    [[nodiscard]] QColor paletteSeparator() const { return color(config::ChromeColorRole::PaletteSeparator); }
    [[nodiscard]] QColor findToggleOnFill() const { return color(config::ChromeColorRole::FindToggleOnFill); }
    [[nodiscard]] QColor findToggleOnText() const { return color(config::ChromeColorRole::FindToggleOnText); }
    [[nodiscard]] QColor findToggleText() const { return color(config::ChromeColorRole::FindToggleText); }
    [[nodiscard]] QColor findCountText() const { return color(config::ChromeColorRole::FindCountText); }
    [[nodiscard]] QColor findToggleOnRing() const { return color(config::ChromeColorRole::FindToggleOnRing); }
    [[nodiscard]] QColor findErrorText() const { return color(config::ChromeColorRole::FindErrorText); }
    [[nodiscard]] QColor toastTextSecondary() const
    {
        return color(config::ChromeColorRole::ToastTextSecondary);
    }
    /// The overlay scrollbar's roles by the palette's tone; a pane resolves them on its own background
    /// (UiStyleProvider::colorOn, with the @c roles names).
    [[nodiscard]] QColor scrollbarThumb() const { return color(config::ChromeColorRole::ScrollbarThumb); }
    [[nodiscard]] QColor scrollbarThumbActive() const
    {
        return color(config::ChromeColorRole::ScrollbarThumbActive);
    }
    [[nodiscard]] QColor scrollbarTrack() const { return color(config::ChromeColorRole::ScrollbarTrack); }
    /// The keyboard focus ring and, on the Terminal row, the focused item's fill.
    [[nodiscard]] QColor focusRing() const { return color(config::ChromeColorRole::FocusRing); }
    [[nodiscard]] QColor focusFill() const { return color(config::ChromeColorRole::FocusFill); }
    [[nodiscard]] QColor windowButtonFill() const { return color(config::ChromeColorRole::WindowButtonFill); }
    [[nodiscard]] QColor windowButtonHoverFill() const
    {
        return color(config::ChromeColorRole::WindowButtonHoverFill);
    }
    [[nodiscard]] QColor windowButtonPressedFill() const
    {
        return color(config::ChromeColorRole::WindowButtonPressedFill);
    }
    [[nodiscard]] QColor windowButtonGlyph() const
    {
        return color(config::ChromeColorRole::WindowButtonGlyph);
    }
    [[nodiscard]] QColor headerSeparator() const { return color(config::ChromeColorRole::HeaderSeparator); }

    /// The tooltips' roles, by the palette's tone.
    [[nodiscard]] QColor tooltipFill() const { return color(config::ChromeColorRole::TooltipFill); }
    [[nodiscard]] QColor tooltipText() const { return color(config::ChromeColorRole::TooltipText); }
    [[nodiscard]] QColor tooltipTextSecondary() const
    {
        return color(config::ChromeColorRole::TooltipTextSecondary);
    }
    [[nodiscard]] QColor tooltipEdge() const { return color(config::ChromeColorRole::TooltipEdge); }

    /// The Settings window's roles, by the palette's tone.
    [[nodiscard]] QColor settingsSidebarFill() const
    {
        return color(config::ChromeColorRole::SettingsSidebarFill);
    }
    [[nodiscard]] QColor settingsPageFill() const { return color(config::ChromeColorRole::SettingsPageFill); }
    [[nodiscard]] QColor settingsGroupFill() const
    {
        return color(config::ChromeColorRole::SettingsGroupFill);
    }
    [[nodiscard]] QColor settingsSeparator() const
    {
        return color(config::ChromeColorRole::SettingsSeparator);
    }
    [[nodiscard]] QColor settingsLabel() const { return color(config::ChromeColorRole::SettingsLabel); }
    [[nodiscard]] QColor settingsHelp() const { return color(config::ChromeColorRole::SettingsHelp); }
    [[nodiscard]] QColor settingsSelectionFill() const
    {
        return color(config::ChromeColorRole::SettingsSelectionFill);
    }
    [[nodiscard]] QColor settingsSelectionText() const
    {
        return color(config::ChromeColorRole::SettingsSelectionText);
    }
    [[nodiscard]] QColor settingsModified() const { return color(config::ChromeColorRole::SettingsModified); }
    [[nodiscard]] QColor settingsIconTile() const { return color(config::ChromeColorRole::SettingsIconTile); }
    [[nodiscard]] QColor settingsIconGlyph() const
    {
        return color(config::ChromeColorRole::SettingsIconGlyph);
    }
    [[nodiscard]] QColor switchOnFill() const { return color(config::ChromeColorRole::SwitchOnFill); }
    [[nodiscard]] QColor switchOffFill() const { return color(config::ChromeColorRole::SwitchOffFill); }
    [[nodiscard]] QColor switchKnob() const { return color(config::ChromeColorRole::SwitchKnob); }
    [[nodiscard]] QColor controlFill() const { return color(config::ChromeColorRole::ControlFill); }
    [[nodiscard]] QColor controlText() const { return color(config::ChromeColorRole::ControlText); }
    [[nodiscard]] QColor previewFrame() const { return color(config::ChromeColorRole::PreviewFrame); }

  signals:
    /// Every role may have changed colour.
    void changed();

  private:
    Sources _sources {};
    config::ChromeSchemeColors _scheme {};
    int _separatorPercent = 0;
    UiStyleProvider const& _provider;
};

/// The tab roles a joined active tab resolves on the content's colour (UiStyleProvider::colorOn): it fills
/// with @c PaneGapFill (R-k), whose tone is the terminal scheme's, not the chrome's.
inline constexpr auto JoinedTabColorRoles = std::array {
    config::ChromeColorRole::TabTextActive,
    config::ChromeColorRole::TabTextDimmed,
    config::ChromeColorRole::TabCloseFill,
};

/// The QML names of the pane colour roles, `chromeStyle.roles`, for UiStyleProvider::colorOn().
///
/// Properties rather than an exported enum: a Q_ENUM is not reachable through a context-property instance
/// (see UiStyleProvider::statusDanger). One property per pane role, @c PaneColorRoleBegin up to @c
/// PaneColorRoleEnd, and per find bar role, @c FindColorRoleBegin up to @c FindColorRoleEnd: the find bar
/// sits on the pane, and per overlay scrollbar role, which a pane resolves on its own background too; and
/// one per @c JoinedTabColorRoles entry, which sit on the content's colour.
class ChromePaneRoleNames: public QObject
{
    Q_OBJECT
    Q_PROPERTY(int tabTextActive READ tabTextActive CONSTANT)
    Q_PROPERTY(int tabTextDimmed READ tabTextDimmed CONSTANT)
    Q_PROPERTY(int tabCloseFill READ tabCloseFill CONSTANT)
    Q_PROPERTY(int paneGapFill READ paneGapFill CONSTANT)
    Q_PROPERTY(int paneCardBorder READ paneCardBorder CONSTANT)
    Q_PROPERTY(int paneHeaderFill READ paneHeaderFill CONSTANT)
    Q_PROPERTY(int paneHeaderText READ paneHeaderText CONSTANT)
    Q_PROPERTY(int paneHeaderTextDimmed READ paneHeaderTextDimmed CONSTANT)
    Q_PROPERTY(int paneSplitLine READ paneSplitLine CONSTANT)
    Q_PROPERTY(int paneGrabber READ paneGrabber CONSTANT)
    Q_PROPERTY(int paneFooterFill READ paneFooterFill CONSTANT)
    Q_PROPERTY(int paneFooterText READ paneFooterText CONSTANT)
    Q_PROPERTY(int paneHeaderActionHover READ paneHeaderActionHover CONSTANT)
    Q_PROPERTY(int findToggleOnFill READ findToggleOnFill CONSTANT)
    Q_PROPERTY(int findToggleOnText READ findToggleOnText CONSTANT)
    Q_PROPERTY(int findToggleText READ findToggleText CONSTANT)
    Q_PROPERTY(int findCountText READ findCountText CONSTANT)
    Q_PROPERTY(int findToggleOnRing READ findToggleOnRing CONSTANT)
    Q_PROPERTY(int findErrorText READ findErrorText CONSTANT)
    Q_PROPERTY(int scrollbarThumb READ scrollbarThumb CONSTANT)
    Q_PROPERTY(int scrollbarThumbActive READ scrollbarThumbActive CONSTANT)
    Q_PROPERTY(int scrollbarTrack READ scrollbarTrack CONSTANT)

  public:
    /// @param parent Qt ownership.
    explicit ChromePaneRoleNames(QObject* parent);

    /// @return Each role's config::ChromeColorRole as its underlying value.
    [[nodiscard]] static constexpr int tabTextActive() noexcept
    {
        return of(config::ChromeColorRole::TabTextActive);
    }
    [[nodiscard]] static constexpr int tabTextDimmed() noexcept
    {
        return of(config::ChromeColorRole::TabTextDimmed);
    }
    [[nodiscard]] static constexpr int tabCloseFill() noexcept
    {
        return of(config::ChromeColorRole::TabCloseFill);
    }
    [[nodiscard]] static constexpr int paneGapFill() noexcept
    {
        return of(config::ChromeColorRole::PaneGapFill);
    }
    [[nodiscard]] static constexpr int paneCardBorder() noexcept
    {
        return of(config::ChromeColorRole::PaneCardBorder);
    }
    [[nodiscard]] static constexpr int paneHeaderFill() noexcept
    {
        return of(config::ChromeColorRole::PaneHeaderFill);
    }
    [[nodiscard]] static constexpr int paneHeaderText() noexcept
    {
        return of(config::ChromeColorRole::PaneHeaderText);
    }
    [[nodiscard]] static constexpr int paneHeaderTextDimmed() noexcept
    {
        return of(config::ChromeColorRole::PaneHeaderTextDimmed);
    }
    [[nodiscard]] static constexpr int paneSplitLine() noexcept
    {
        return of(config::ChromeColorRole::PaneSplitLine);
    }
    [[nodiscard]] static constexpr int paneGrabber() noexcept
    {
        return of(config::ChromeColorRole::PaneGrabber);
    }
    [[nodiscard]] static constexpr int paneFooterFill() noexcept
    {
        return of(config::ChromeColorRole::PaneFooterFill);
    }
    [[nodiscard]] static constexpr int paneFooterText() noexcept
    {
        return of(config::ChromeColorRole::PaneFooterText);
    }
    [[nodiscard]] static constexpr int paneHeaderActionHover() noexcept
    {
        return of(config::ChromeColorRole::PaneHeaderActionHover);
    }
    [[nodiscard]] static constexpr int findToggleOnFill() noexcept
    {
        return of(config::ChromeColorRole::FindToggleOnFill);
    }
    [[nodiscard]] static constexpr int findToggleOnText() noexcept
    {
        return of(config::ChromeColorRole::FindToggleOnText);
    }
    [[nodiscard]] static constexpr int findToggleText() noexcept
    {
        return of(config::ChromeColorRole::FindToggleText);
    }
    [[nodiscard]] static constexpr int findCountText() noexcept
    {
        return of(config::ChromeColorRole::FindCountText);
    }
    [[nodiscard]] static constexpr int findToggleOnRing() noexcept
    {
        return of(config::ChromeColorRole::FindToggleOnRing);
    }
    [[nodiscard]] static constexpr int findErrorText() noexcept
    {
        return of(config::ChromeColorRole::FindErrorText);
    }
    [[nodiscard]] static constexpr int scrollbarThumb() noexcept
    {
        return of(config::ChromeColorRole::ScrollbarThumb);
    }
    [[nodiscard]] static constexpr int scrollbarThumbActive() noexcept
    {
        return of(config::ChromeColorRole::ScrollbarThumbActive);
    }
    [[nodiscard]] static constexpr int scrollbarTrack() noexcept
    {
        return of(config::ChromeColorRole::ScrollbarTrack);
    }

  private:
    [[nodiscard]] static constexpr int of(config::ChromeColorRole role) noexcept
    {
        return static_cast<int>(role);
    }
};

/// The QML names of the tab badge kinds, `chromeStyle.badgeKinds`, for UiStyleProvider::badgeLook().
class ChromeBadgeKindNames: public QObject
{
    Q_OBJECT
    Q_PROPERTY(int none READ none CONSTANT)
    Q_PROPERTY(int output READ output CONSTANT)
    Q_PROPERTY(int done READ done CONSTANT)
    Q_PROPERTY(int working READ working CONSTANT)
    Q_PROPERTY(int error READ error CONSTANT)
    Q_PROPERTY(int blocked READ blocked CONSTANT)
    Q_PROPERTY(int bell READ bell CONSTANT)

  public:
    /// @param parent Qt ownership.
    explicit ChromeBadgeKindNames(QObject* parent);

    /// @return Each kind's window::TabBadgeKind as its underlying value.
    [[nodiscard]] static constexpr int none() noexcept { return of(TabBadgeKind::None); }
    [[nodiscard]] static constexpr int output() noexcept { return of(TabBadgeKind::Output); }
    [[nodiscard]] static constexpr int done() noexcept { return of(TabBadgeKind::Done); }
    [[nodiscard]] static constexpr int working() noexcept { return of(TabBadgeKind::Working); }
    [[nodiscard]] static constexpr int error() noexcept { return of(TabBadgeKind::Error); }
    [[nodiscard]] static constexpr int blocked() noexcept { return of(TabBadgeKind::Blocked); }
    [[nodiscard]] static constexpr int bell() noexcept { return of(TabBadgeKind::Bell); }

  private:
    [[nodiscard]] static constexpr int of(TabBadgeKind kind) noexcept { return static_cast<int>(kind); }
};

/// The QML names of an ended pane's ends and of the footers a pane shows, `chromeStyle.paneEnds`, for what
/// a TerminalSession's `sessionEnd` and `paneFooter` answer.
class ChromePaneEndNames: public QObject
{
    Q_OBJECT
    Q_PROPERTY(int none READ none CONSTANT)
    Q_PROPERTY(int exited READ exited CONSTANT)
    Q_PROPERTY(int signalled READ signalled CONSTANT)
    Q_PROPERTY(int startFailed READ startFailed CONSTANT)
    Q_PROPERTY(int disconnected READ disconnected CONSTANT)
    Q_PROPERTY(int footerNone READ footerNone CONSTANT)
    Q_PROPERTY(int footerEnded READ footerEnded CONSTANT)
    Q_PROPERTY(int footerInterrupted READ footerInterrupted CONSTANT)

  public:
    /// @param parent Qt ownership.
    explicit ChromePaneEndNames(QObject* parent);

    /// @return Each session::EndReason as its underlying value.
    [[nodiscard]] static constexpr int none() noexcept { return of(session::EndReason::None); }
    [[nodiscard]] static constexpr int exited() noexcept { return of(session::EndReason::Exited); }
    [[nodiscard]] static constexpr int signalled() noexcept { return of(session::EndReason::Signalled); }
    [[nodiscard]] static constexpr int startFailed() noexcept { return of(session::EndReason::StartFailed); }
    [[nodiscard]] static constexpr int disconnected() noexcept
    {
        return of(session::EndReason::Disconnected);
    }

    /// @return Each window::PaneFooterKind as its underlying value.
    [[nodiscard]] static constexpr int footerNone() noexcept { return of(PaneFooterKind::None); }
    [[nodiscard]] static constexpr int footerEnded() noexcept { return of(PaneFooterKind::Ended); }
    [[nodiscard]] static constexpr int footerInterrupted() noexcept
    {
        return of(PaneFooterKind::Interrupted);
    }

  private:
    [[nodiscard]] static constexpr int of(session::EndReason end) noexcept { return static_cast<int>(end); }
    [[nodiscard]] static constexpr int of(PaneFooterKind footer) noexcept { return static_cast<int>(footer); }
};

/// The QML names of a pane's states, `chromeStyle.paneStates`, for what a TerminalSession's `paneState`
/// answers (window::paneStateOf): the pane card's pill and the ended pane's footer compare against these
/// rather than deciding the state again.
class ChromePaneStateNames: public QObject
{
    Q_OBJECT
    Q_PROPERTY(int live READ live CONSTANT)
    Q_PROPERTY(int reconnecting READ reconnecting CONSTANT)
    Q_PROPERTY(int ended READ ended CONSTANT)

  public:
    /// @param parent Qt ownership.
    explicit ChromePaneStateNames(QObject* parent);

    /// @return Each window::PaneState as its underlying value.
    [[nodiscard]] static constexpr int live() noexcept { return of(PaneState::Live); }
    [[nodiscard]] static constexpr int reconnecting() noexcept { return of(PaneState::Reconnecting); }
    [[nodiscard]] static constexpr int ended() noexcept { return of(PaneState::Ended); }

  private:
    [[nodiscard]] static constexpr int of(PaneState state) noexcept { return static_cast<int>(state); }
};

/// The QML names of what holds a pane header's trailing area, `chromeStyle.headerOccupants`, for
/// PaneHeader's `trailingOccupant`.
class ChromeHeaderOccupantNames: public QObject
{
    Q_OBJECT
    Q_PROPERTY(int none READ none CONSTANT)
    Q_PROPERTY(int find READ find CONSTANT)
    Q_PROPERTY(int linkPill READ linkPill CONSTANT)

  public:
    /// @param parent Qt ownership.
    explicit ChromeHeaderOccupantNames(QObject* parent);

    /// @return Each window::HeaderTrailingOccupant as its underlying value.
    [[nodiscard]] static constexpr int none() noexcept { return of(HeaderTrailingOccupant::None); }
    [[nodiscard]] static constexpr int find() noexcept { return of(HeaderTrailingOccupant::Find); }
    [[nodiscard]] static constexpr int linkPill() noexcept { return of(HeaderTrailingOccupant::LinkPill); }

  private:
    [[nodiscard]] static constexpr int of(HeaderTrailingOccupant occupant) noexcept
    {
        return static_cast<int>(occupant);
    }
};

/// The QML names of secure input's values, `chromeStyle.secureInputs`: what a pane's `secureIndicator`
/// holds (window::SecureIndicator), and what secureIndicatorSpots() takes (window::SpotShown).
class ChromeSecureInputNames: public QObject
{
    Q_OBJECT
    Q_PROPERTY(int none READ none CONSTANT)
    Q_PROPERTY(int locked READ locked CONSTANT)
    Q_PROPERTY(int lockedSecured READ lockedSecured CONSTANT)
    Q_PROPERTY(int spotHidden READ spotHidden CONSTANT)
    Q_PROPERTY(int spotShown READ spotShown CONSTANT)

  public:
    /// @param parent Qt ownership.
    explicit ChromeSecureInputNames(QObject* parent);

    /// @return Each window::SecureIndicator as its underlying value.
    [[nodiscard]] static constexpr int none() noexcept { return static_cast<int>(SecureIndicator::None); }
    [[nodiscard]] static constexpr int locked() noexcept { return static_cast<int>(SecureIndicator::Locked); }
    [[nodiscard]] static constexpr int lockedSecured() noexcept
    {
        return static_cast<int>(SecureIndicator::LockedSecured);
    }
    /// @return Each window::SpotShown as its underlying value.
    [[nodiscard]] static constexpr int spotHidden() noexcept { return static_cast<int>(SpotShown::No); }
    [[nodiscard]] static constexpr int spotShown() noexcept { return static_cast<int>(SpotShown::Yes); }
};

/// The QML names of the command blocks' values, `chromeStyle.blocks`: where a sticky header lives
/// (window::StickyHeaderHost), what stickyHeaderHost() is asked about the pane's header and the title band,
/// and how a scrollbar tick is drawn (vtbackend::ScrollbarTickStyle).
class ChromeBlockNames: public QObject
{
    Q_OBJECT
    Q_PROPERTY(int hostGrid READ hostGrid CONSTANT)
    Q_PROPERTY(int hostPaneHeader READ hostPaneHeader CONSTANT)
    Q_PROPERTY(int hostTitleBand READ hostTitleBand CONSTANT)
    Q_PROPERTY(int headerHidden READ headerHidden CONSTANT)
    Q_PROPERTY(int headerShown READ headerShown CONSTANT)
    Q_PROPERTY(int bandAbsent READ bandAbsent CONSTANT)
    Q_PROPERTY(int bandShown READ bandShown CONSTANT)
    Q_PROPERTY(int tickSolid READ tickSolid CONSTANT)
    Q_PROPERTY(int tickHollow READ tickHollow CONSTANT)

  public:
    /// @param parent Qt ownership.
    explicit ChromeBlockNames(QObject* parent);

    /// @return Each window::StickyHeaderHost as its underlying value.
    [[nodiscard]] static constexpr int hostGrid() noexcept
    {
        return static_cast<int>(StickyHeaderHost::Grid);
    }
    [[nodiscard]] static constexpr int hostPaneHeader() noexcept
    {
        return static_cast<int>(StickyHeaderHost::PaneHeader);
    }
    [[nodiscard]] static constexpr int hostTitleBand() noexcept
    {
        return static_cast<int>(StickyHeaderHost::TitleBand);
    }
    /// @return A pane header that shows nothing / something, as a window::PaneHeaderContent.
    [[nodiscard]] static constexpr int headerHidden() noexcept
    {
        return static_cast<int>(PaneHeaderContent::Hidden);
    }
    [[nodiscard]] static constexpr int headerShown() noexcept
    {
        return static_cast<int>(PaneHeaderContent::Name);
    }
    /// @return Each window::TitleBandPresence as its underlying value.
    [[nodiscard]] static constexpr int bandAbsent() noexcept
    {
        return static_cast<int>(TitleBandPresence::Absent);
    }
    [[nodiscard]] static constexpr int bandShown() noexcept
    {
        return static_cast<int>(TitleBandPresence::Shown);
    }
    /// @return Each vtbackend::ScrollbarTickStyle as its underlying value.
    [[nodiscard]] static constexpr int tickSolid() noexcept
    {
        return static_cast<int>(vtbackend::ScrollbarTickStyle::Solid);
    }
    [[nodiscard]] static constexpr int tickHollow() noexcept
    {
        return static_cast<int>(vtbackend::ScrollbarTickStyle::Hollow);
    }
};

/// The QML names of what a drop over a pane would do (window::DropZone), `chromeStyle.dropZones`, for
/// the DropArea of a pane card to compare WindowController::dropZoneAt()'s answer against.
class ChromeDropZoneNames: public QObject
{
    Q_OBJECT
    Q_PROPERTY(int none READ none CONSTANT)
    Q_PROPERTY(int insertText READ insertText CONSTANT)
    Q_PROPERTY(int passThrough READ passThrough CONSTANT)
    Q_PROPERTY(int splitLeft READ splitLeft CONSTANT)
    Q_PROPERTY(int splitRight READ splitRight CONSTANT)
    Q_PROPERTY(int splitTop READ splitTop CONSTANT)
    Q_PROPERTY(int splitBottom READ splitBottom CONSTANT)
    Q_PROPERTY(int swap READ swap CONSTANT)
    Q_PROPERTY(int noOp READ noOp CONSTANT)
    Q_PROPERTY(int refused READ refused CONSTANT)

  public:
    /// @param parent Qt ownership.
    explicit ChromeDropZoneNames(QObject* parent);

    /// @return Each window::DropZone as its underlying value.
    [[nodiscard]] static constexpr int none() noexcept { return of(DropZone::None); }
    [[nodiscard]] static constexpr int insertText() noexcept { return of(DropZone::InsertText); }
    [[nodiscard]] static constexpr int passThrough() noexcept { return of(DropZone::PassThrough); }
    [[nodiscard]] static constexpr int splitLeft() noexcept { return of(DropZone::SplitLeft); }
    [[nodiscard]] static constexpr int splitRight() noexcept { return of(DropZone::SplitRight); }
    [[nodiscard]] static constexpr int splitTop() noexcept { return of(DropZone::SplitTop); }
    [[nodiscard]] static constexpr int splitBottom() noexcept { return of(DropZone::SplitBottom); }
    [[nodiscard]] static constexpr int swap() noexcept { return of(DropZone::Swap); }
    [[nodiscard]] static constexpr int noOp() noexcept { return of(DropZone::NoOp); }
    [[nodiscard]] static constexpr int refused() noexcept { return of(DropZone::Refused); }

  private:
    [[nodiscard]] static constexpr int of(DropZone zone) noexcept { return static_cast<int>(zone); }
};

/// The QML names of what is dragged over a pane (window::DropPayload), `chromeStyle.dropPayloads`, for
/// WindowController::dropZoneAt()'s `payload`.
class ChromeDropPayloadNames: public QObject
{
    Q_OBJECT
    Q_PROPERTY(int none READ none CONSTANT)
    Q_PROPERTY(int files READ files CONSTANT)
    Q_PROPERTY(int text READ text CONSTANT)
    Q_PROPERTY(int tab READ tab CONSTANT)
    Q_PROPERTY(int pane READ pane CONSTANT)

  public:
    /// @param parent Qt ownership.
    explicit ChromeDropPayloadNames(QObject* parent);

    /// @return Each window::DropPayload as its underlying value.
    [[nodiscard]] static constexpr int none() noexcept { return of(DropPayload::None); }
    [[nodiscard]] static constexpr int files() noexcept { return of(DropPayload::Files); }
    [[nodiscard]] static constexpr int text() noexcept { return of(DropPayload::Text); }
    [[nodiscard]] static constexpr int tab() noexcept { return of(DropPayload::Tab); }
    [[nodiscard]] static constexpr int pane() noexcept { return of(DropPayload::Pane); }

  private:
    [[nodiscard]] static constexpr int of(DropPayload payload) noexcept { return static_cast<int>(payload); }
};

} // namespace contour::window
