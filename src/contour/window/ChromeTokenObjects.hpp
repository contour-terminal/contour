// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The QML-facing objects of the 1a token groups: one QObject per group of config::ChromeTokenRow,
/// published as the `chromeStyle.size`, `chromeStyle.space`, ... sub-objects. Each is built from its
/// resolved group and the chrome's cell units, so every extent it reports is already in logical
/// pixels and the QML never learns which style is active.

#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/session/EndReason.hpp>
#include <contour/window/TabBadge.hpp>

#include <QtCore/QObject>
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
    Q_PROPERTY(qreal flyoutSwatch READ flyoutSwatch CONSTANT)
    Q_PROPERTY(qreal bareBand READ bareBand CONSTANT)
    Q_PROPERTY(qreal grabber READ grabber CONSTANT)
    Q_PROPERTY(qreal grabberLength READ grabberLength CONSTANT)
    Q_PROPERTY(qreal paneHeaderAction READ paneHeaderAction CONSTANT)
    Q_PROPERTY(qreal paneFooter READ paneFooter CONSTANT)
    Q_PROPERTY(qreal footerButton READ footerButton CONSTANT)

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

  private:
    config::SizeTokens _tokens {};
    ChromeUnits _units {};
};

/// Gaps and the spacing scale, `chromeStyle.space`. Every value but @c revealEdge and @c paneGapRows is
/// horizontal.
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
    Q_PROPERTY(qreal paneGapRows READ paneGapRows CONSTANT)
    Q_PROPERTY(qreal paneHeaderGap READ paneHeaderGap CONSTANT)
    Q_PROPERTY(qreal footerLeading READ footerLeading CONSTANT)

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

    /// @return The band width that always stays draggable, in logical pixels.
    [[nodiscard]] qreal dragReserve() const noexcept { return _tokens.dragReserve * _units.horizontal; }
    /// @return The height of the full-screen reveal edge, in logical pixels (vertical, unlike the rest).
    [[nodiscard]] qreal revealEdge() const noexcept { return _tokens.revealEdge * _units.vertical; }
    /// @return The horizontal padding inside a toolbar chip, in logical pixels.
    [[nodiscard]] qreal toolbarChipPadding() const noexcept
    {
        return _tokens.toolbarChipPadding * _units.horizontal;
    }

    /// @return The gap between stacked panes in logical pixels (vertical: one cell's height on Terminal).
    [[nodiscard]] qreal paneGapRows() const noexcept { return _tokens.paneGapRows * _units.vertical; }
    /// @return The gap between a pane header's items in logical pixels.
    [[nodiscard]] qreal paneHeaderGap() const noexcept { return _tokens.paneHeaderGap * _units.horizontal; }
    /// @return The exited footer's leading padding in logical pixels.
    [[nodiscard]] qreal footerLeading() const noexcept { return _tokens.footerLeading * _units.horizontal; }

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
    Q_PROPERTY(qreal paneCard READ paneCard CONSTANT)
    Q_PROPERTY(qreal toast READ toast CONSTANT)
    Q_PROPERTY(qreal tab READ tab CONSTANT)
    Q_PROPERTY(qreal tabWell READ tabWell CONSTANT)
    Q_PROPERTY(qreal tabIcon READ tabIcon CONSTANT)
    Q_PROPERTY(qreal tabClose READ tabClose CONSTANT)
    Q_PROPERTY(qreal badge READ badge CONSTANT)
    Q_PROPERTY(qreal hostChip READ hostChip CONSTANT)
    Q_PROPERTY(qreal toolbarChip READ toolbarChip CONSTANT)
    Q_PROPERTY(qreal grabber READ grabber CONSTANT)

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
    /// @return The radius of pane cards.
    [[nodiscard]] qreal paneCard() const noexcept { return _tokens.paneCard * _units.horizontal; }
    /// @return The radius of toasts.
    [[nodiscard]] qreal toast() const noexcept { return _tokens.toast * _units.horizontal; }
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

  private:
    QFont _caption {};
    QFont _footnote {};
    QFont _body {};
    QFont _tab {};
    QFont _title {};
    QFont _paneHeader {};
    int _paneHeaderWeight = 0;
    QFont _paneHeaderAction {};
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
    // A bool for QML (the QML-facing carve-out): a read-only predicate over `tabShape`, which stays the
    // source of truth as a config::TabShape; the one place the shape is compared, so the tab, the strip and
    // the badge slot cannot disagree on it.
    Q_PROPERTY(bool isPill READ isPill CONSTANT)
    Q_PROPERTY(int profileMenu READ profileMenu CONSTANT)
    Q_PROPERTY(int caret READ caret CONSTANT)
    Q_PROPERTY(int secondaryClick READ secondaryClick CONSTANT)
    Q_PROPERTY(int tabColors READ tabColors CONSTANT)
    Q_PROPERTY(int palette READ palette CONSTANT)
    Q_PROPERTY(int roles READ roles CONSTANT)
    Q_PROPERTY(int paneActions READ paneActions CONSTANT)
    Q_PROPERTY(int onHover READ onHover CONSTANT)
    Q_PROPERTY(int onHoverOrFocus READ onHoverOrFocus CONSTANT)

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
    /// @return Whether the tabs are pills in a well (config::TabShape::Pill).
    [[nodiscard]] bool isPill() const noexcept { return _tokens.tabShape == config::TabShape::Pill; }

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

  signals:
    /// Every role may have changed colour.
    void changed();

  private:
    Sources _sources {};
    config::ChromeSchemeColors _scheme {};
    int _separatorPercent = 0;
    UiStyleProvider const& _provider;
};

/// The QML names of the pane colour roles, `chromeStyle.roles`, for UiStyleProvider::colorOn().
///
/// Properties rather than an exported enum: a Q_ENUM is not reachable through a context-property instance
/// (see UiStyleProvider::statusDanger). One property per role from @c PaneGapFill on.
class ChromePaneRoleNames: public QObject
{
    Q_OBJECT
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

  public:
    /// @param parent Qt ownership.
    explicit ChromePaneRoleNames(QObject* parent);

    /// @return Each role's config::ChromeColorRole as its underlying value.
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

  private:
    [[nodiscard]] static constexpr int of(TabBadgeKind kind) noexcept { return static_cast<int>(kind); }
};

/// The QML names of an ended pane's ends and restart offers, `chromeStyle.paneEnds`, for what a
/// TerminalSession's `sessionEnd` and `restartOffer` answer.
class ChromePaneEndNames: public QObject
{
    Q_OBJECT
    Q_PROPERTY(int none READ none CONSTANT)
    Q_PROPERTY(int exited READ exited CONSTANT)
    Q_PROPERTY(int signalled READ signalled CONSTANT)
    Q_PROPERTY(int startFailed READ startFailed CONSTANT)
    Q_PROPERTY(int disconnected READ disconnected CONSTANT)
    Q_PROPERTY(int restartUnavailable READ restartUnavailable CONSTANT)
    Q_PROPERTY(int restartOffered READ restartOffered CONSTANT)

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

    /// @return Each session::FooterRestart as its underlying value.
    [[nodiscard]] static constexpr int restartUnavailable() noexcept
    {
        return static_cast<int>(session::FooterRestart::Unavailable);
    }
    [[nodiscard]] static constexpr int restartOffered() noexcept
    {
        return static_cast<int>(session::FooterRestart::Offered);
    }

  private:
    [[nodiscard]] static constexpr int of(session::EndReason end) noexcept { return static_cast<int>(end); }
};

} // namespace contour::window
