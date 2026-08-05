// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/UiStyle.h>

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtGui/QFont>

#include <string>

namespace contour::config
{
struct Config;
}

namespace contour::window
{

/// Every chrome extent one style paints with, resolved to logical pixels.
///
/// Resolving is the whole job: UiStyle.h counts extents in cells for the terminal style and in pixels
/// for the native one, and doing that arithmetic once, here, leaves the QML with no policy to get
/// wrong and no knowledge of which style is active.
struct ChromeMetrics
{
    qreal cellWidth = 0.0;    ///< Advance width of one character cell of the chrome font.
    qreal cellHeight = 0.0;   ///< Line height of one character cell of the chrome font.
    qreal widthQuantum = 1.0; ///< What a chrome width snaps to: one cell, or one pixel (no snap).

    qreal chromeHeight = 0.0;
    qreal tabHeight = 0.0;
    qreal labelPadding = 0.0;
    qreal labelGap = 0.0;
    qreal trailingPadding = 0.0;
    qreal minTabWidth = 0.0;
    qreal maxTabWidth = 0.0;
    qreal tabSlack = 0.0;
    qreal controlWidth = 0.0;
    qreal controlHeight = 0.0;
    qreal stripButtonWidth = 0.0;
    qreal windowControlWidth = 0.0;
    qreal badgeWidth = 0.0;
    qreal badgeHeight = 0.0;
};

/// The fonts the chrome draws with. A style pins a control's own size only where it wants one
/// different from the chrome font's; otherwise these are all the same font.
struct ChromeFonts
{
    QFont base;          ///< Labels and anything with no size of its own.
    QFont close;         ///< The per-tab close glyph.
    QFont newTab;        ///< The "new tab" button.
    QFont menu;          ///< The profile-dropdown button.
    QFont windowControl; ///< The minimize/maximize/close window buttons.
    QFont badge;         ///< The zoom badge. Always bold -- it is a marker, not text.
};

/// The decorative glyphs one style draws its chrome with.
struct ChromeGlyphs
{
    QString tabSeparator; ///< Drawn between adjacent tabs. Empty in a style that sets them flush.
    QString close;
    QString zoom;
    QString newTab;
    QString menu;
    QString submenu;
};

/// The GUI chrome's design tokens, published to QML as the @c chromeStyle context property.
///
/// Every number the chrome QML used to carry as a literal comes from here, already resolved (see
/// ChromeMetrics), so a QML file never learns which style is active.
///
/// Everything is CONSTANT because the style cannot change within a process: the Qt Quick Controls
/// half of it is chosen once, before the first control exists (see ContourGuiApp), so a live-updating
/// geometry half would only produce a half-switched window. @see UiStyle.
///
/// Deliberately a context property rather than a registered QML singleton. Two separate QML modules
/// read these tokens -- @c Contour.Ui and the @c ContourTui Quick Controls style -- and a singleton
/// would make the style module import the application's own C++ URI, which a style resolvable by name
/// must not do. It also lets each test engine carry its own style (see test/QmlChromeStyle.h). The
/// price is real and worth naming: a context property is untyped, so qmlcachegen cannot ahead-of-time
/// compile the bindings that read it.
class UiStyleProvider: public QObject
{
    Q_OBJECT

    Q_PROPERTY(QFont font READ font CONSTANT)
    Q_PROPERTY(QFont newTabFont READ newTabFont CONSTANT)
    Q_PROPERTY(QFont menuFont READ menuFont CONSTANT)
    Q_PROPERTY(QFont windowControlFont READ windowControlFont CONSTANT)
    Q_PROPERTY(QFont badgeFont READ badgeFont CONSTANT)
    Q_PROPERTY(QFont closeFont READ closeFont CONSTANT)

    Q_PROPERTY(qreal cellWidth READ cellWidth CONSTANT)
    Q_PROPERTY(qreal cellHeight READ cellHeight CONSTANT)
    Q_PROPERTY(qreal widthQuantum READ widthQuantum CONSTANT)

    Q_PROPERTY(qreal chromeHeight READ chromeHeight CONSTANT)
    Q_PROPERTY(qreal tabHeight READ tabHeight CONSTANT)
    Q_PROPERTY(qreal labelPadding READ labelPadding CONSTANT)
    Q_PROPERTY(qreal labelGap READ labelGap CONSTANT)
    Q_PROPERTY(qreal trailingPadding READ trailingPadding CONSTANT)
    Q_PROPERTY(qreal minTabWidth READ minTabWidth CONSTANT)
    Q_PROPERTY(qreal maxTabWidth READ maxTabWidth CONSTANT)
    Q_PROPERTY(qreal tabSlack READ tabSlack CONSTANT)
    Q_PROPERTY(qreal controlWidth READ controlWidth CONSTANT)
    Q_PROPERTY(qreal controlHeight READ controlHeight CONSTANT)
    Q_PROPERTY(qreal stripButtonWidth READ stripButtonWidth CONSTANT)
    Q_PROPERTY(qreal windowControlWidth READ windowControlWidth CONSTANT)
    Q_PROPERTY(qreal badgeWidth READ badgeWidth CONSTANT)
    Q_PROPERTY(qreal badgeHeight READ badgeHeight CONSTANT)

    Q_PROPERTY(int radius READ radius CONSTANT)
    Q_PROPERTY(int borderWidth READ borderWidth CONSTANT)
    Q_PROPERTY(int dropCaretWidth READ dropCaretWidth CONSTANT)

    Q_PROPERTY(QString tabSeparator READ tabSeparator CONSTANT)
    Q_PROPERTY(QString closeGlyph READ closeGlyph CONSTANT)
    Q_PROPERTY(QString zoomGlyph READ zoomGlyph CONSTANT)
    Q_PROPERTY(QString newTabGlyph READ newTabGlyph CONSTANT)
    Q_PROPERTY(QString menuGlyph READ menuGlyph CONSTANT)
    Q_PROPERTY(QString submenuGlyph READ submenuGlyph CONSTANT)

  public:
    /// @param style      The configured chrome style.
    /// @param chromeFont The font the chrome is drawn with, already resolved against its fallbacks
    ///                   (see resolveChromeFont). Its metrics define the cell every terminal-style
    ///                   extent is counted in.
    /// @param parent     Qt ownership.
    UiStyleProvider(config::UiStyle style, QFont const& chromeFont, QObject* parent = nullptr);

    [[nodiscard]] QFont const& font() const noexcept { return _fonts.base; }
    [[nodiscard]] QFont const& newTabFont() const noexcept { return _fonts.newTab; }
    [[nodiscard]] QFont const& menuFont() const noexcept { return _fonts.menu; }
    [[nodiscard]] QFont const& windowControlFont() const noexcept { return _fonts.windowControl; }
    [[nodiscard]] QFont const& badgeFont() const noexcept { return _fonts.badge; }
    [[nodiscard]] QFont const& closeFont() const noexcept { return _fonts.close; }

    [[nodiscard]] qreal cellWidth() const noexcept { return _metrics.cellWidth; }
    [[nodiscard]] qreal cellHeight() const noexcept { return _metrics.cellHeight; }
    [[nodiscard]] qreal widthQuantum() const noexcept { return _metrics.widthQuantum; }

    [[nodiscard]] qreal chromeHeight() const noexcept { return _metrics.chromeHeight; }
    [[nodiscard]] qreal tabHeight() const noexcept { return _metrics.tabHeight; }
    [[nodiscard]] qreal labelPadding() const noexcept { return _metrics.labelPadding; }
    [[nodiscard]] qreal labelGap() const noexcept { return _metrics.labelGap; }
    [[nodiscard]] qreal trailingPadding() const noexcept { return _metrics.trailingPadding; }
    [[nodiscard]] qreal minTabWidth() const noexcept { return _metrics.minTabWidth; }
    [[nodiscard]] qreal maxTabWidth() const noexcept { return _metrics.maxTabWidth; }
    [[nodiscard]] qreal tabSlack() const noexcept { return _metrics.tabSlack; }
    [[nodiscard]] qreal controlWidth() const noexcept { return _metrics.controlWidth; }
    [[nodiscard]] qreal controlHeight() const noexcept { return _metrics.controlHeight; }
    [[nodiscard]] qreal stripButtonWidth() const noexcept { return _metrics.stripButtonWidth; }
    [[nodiscard]] qreal windowControlWidth() const noexcept { return _metrics.windowControlWidth; }
    [[nodiscard]] qreal badgeWidth() const noexcept { return _metrics.badgeWidth; }
    [[nodiscard]] qreal badgeHeight() const noexcept { return _metrics.badgeHeight; }

    [[nodiscard]] int radius() const noexcept { return _tokens.cornerRadiusPixels; }
    [[nodiscard]] int borderWidth() const noexcept { return _tokens.borderWidthPixels; }
    [[nodiscard]] int dropCaretWidth() const noexcept { return _tokens.dropCaretPixels; }

    [[nodiscard]] QString const& tabSeparator() const noexcept { return _glyphs.tabSeparator; }
    [[nodiscard]] QString const& closeGlyph() const noexcept { return _glyphs.close; }
    [[nodiscard]] QString const& zoomGlyph() const noexcept { return _glyphs.zoom; }
    [[nodiscard]] QString const& newTabGlyph() const noexcept { return _glyphs.newTab; }
    [[nodiscard]] QString const& menuGlyph() const noexcept { return _glyphs.menu; }
    [[nodiscard]] QString const& submenuGlyph() const noexcept { return _glyphs.submenu; }

    /// @p base laid over the chrome at hover strength.
    ///
    /// Every control that shows hover feedback -- tabs, tab-strip buttons, window buttons, and the
    /// controls in the Quick Controls style -- goes through this, so how strongly "hovered" reads is
    /// one number in the token table rather than a repeated literal in six QML files.
    ///
    /// @param base The color to wash, usually the palette's highlight.
    /// @return @p base at the style's hover alpha.
    [[nodiscard]] Q_INVOKABLE QColor wash(QColor const& base) const;

    /// The scrim a modal popup dims its window with.
    ///
    /// Every popup surface in the Quick Controls half of a style declares one of these as its
    /// `T.Overlay.modal`; Qt creates no dimming item at all when it is left unset, which leaves a
    /// modal popup floating over a fully-lit window that still swallows every click.
    ///
    /// @param base The color to dim with, conventionally the palette's shadow.
    /// @return @p base at the style's modal-scrim alpha.
    [[nodiscard]] Q_INVOKABLE QColor modalScrim(QColor const& base) const;

    /// The lighter scrim a merely dimmed, non-modal popup uses. @see modalScrim.
    /// @param base The color to dim with, conventionally the palette's shadow.
    /// @return @p base at the style's modeless-scrim alpha.
    [[nodiscard]] Q_INVOKABLE QColor modelessScrim(QColor const& base) const;

  private:
    // By value rather than by reference: the rows have static storage duration, but a reference member
    // would delete move-assignment for no gain -- the row is a handful of ints and string_views.
    config::UiStyleTokens _tokens;
    ChromeFonts _fonts;
    ChromeMetrics _metrics;
    ChromeGlyphs _glyphs;
};

/// The font @p config says the chrome should be drawn with.
///
/// A style whose row asks for the platform UI font gets it, because that is what makes a native
/// chrome look native. One asking for the terminal's font takes `ui_font_family` / `ui_font_size`
/// where they are set and otherwise inherits @p profileName's regular font -- which is what makes
/// the chrome match the terminal grid below it without the user configuring anything twice.
///
/// @param config      The loaded configuration.
/// @param profileName The profile this window is opening with, i.e. ContourGuiApp::profileName().
///                    The *running* profile rather than @c default_profile, because `-p/--profile`
///                    overrides the latter and the chrome has to match the grid actually on screen.
///                    A name that resolves to no profile leaves the platform font in place.
/// @return A font ready to hand to UiStyleProvider.
[[nodiscard]] QFont resolveChromeFont(config::Config const& config, std::string const& profileName);

} // namespace contour::window
