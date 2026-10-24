// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/AccessibilityPreferences.hpp>
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/platform/PlatformAppearance.hpp>
#include <contour/window/ChromeTokenObjects.hpp>

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
    qreal windowControlInset = 0.0;
    qreal windowControlGutter = 0.0;
    qreal trafficLightDotSize = 0.0;
    qreal trafficLightGap = 0.0;
    qreal badgeWidth = 0.0;
    qreal badgeHeight = 0.0;
    qreal shadowOffsetY = 0.0;
    qreal shadowMargin = 0.0;
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
    QFont caption;       ///< Secondary text, smaller than the chrome font.
    QFont headline;      ///< Text an overlay announces a state with, larger than the chrome font.
};

/// The decorative glyphs one style draws its chrome with.
struct ChromeGlyphs
{
    QString tabSeparator; ///< Drawn between adjacent tabs. Empty in a style that sets them flush.
    QString trafficLight; ///< Empty in a style that draws a traffic-light dot as a vector circle.
    QString close;
    QString zoom;
    QString newTab;
    QString menu;
    QString submenu;
};

/// Everything a UiStyleProvider is built from.
///
/// A value type: two providers built from equal inputs are the same chrome, which is how
/// ChromeStyleHost tells a rebuild that changes nothing from one that does.
struct ChromeInputs
{
    config::ChromeStyle row = config::ChromeStyle::Terminal; ///< The chrome token row.
    QFont font {}; ///< The chrome font, already resolved (see resolveChromeFont).
    config::AccessibilityPreferences accessibility {}; ///< The OS accessibility choices to apply.
    platform::PlatformAppearance appearance {};        ///< What the OS publishes about its own look.
    config::ChromeSchemeColors scheme {};              ///< The terminal scheme the Terminal row paints with.

    [[nodiscard]] bool operator==(ChromeInputs const&) const = default;
};

/// The inputs for @p row and @p font with nothing else set: default accessibility, no platform
/// appearance, and the default terminal palette's scheme.
/// @param row  The chrome token row.
/// @param font The chrome font.
/// @return The inputs.
[[nodiscard]] ChromeInputs chromeInputsFor(config::ChromeStyle row, QFont const& font);

/// The GUI chrome's design tokens, published to QML as the @c chromeStyle context property.
///
/// Every number the chrome QML used to carry as a literal comes from here, already resolved (see
/// ChromeMetrics), so a QML file never learns which style is active.
///
/// Everything but @ref paletteRevision (and what derives from the palette) is CONSTANT: the row cannot
/// change within a process -- the Qt Quick Controls half of it is chosen once, before the first control
/// exists (see ContourGuiApp), so a live-updating geometry half would only produce a half-switched
/// window. What can change -- the font on reload, the OS accessibility and appearance, the terminal
/// scheme -- is answered by building a NEW provider from new ChromeInputs and republishing it
/// (ChromeStyleHost), so every binding re-reads at once. @see UiStyle.
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

    /// Which token row is active: "macos", "fluent", "linux" or "terminal".
    Q_PROPERTY(QString chromeStyle READ chromeStyleName CONSTANT)
    Q_PROPERTY(QFont font READ font CONSTANT)
    Q_PROPERTY(QFont newTabFont READ newTabFont CONSTANT)
    Q_PROPERTY(QFont menuFont READ menuFont CONSTANT)
    Q_PROPERTY(QFont windowControlFont READ windowControlFont CONSTANT)
    Q_PROPERTY(QFont badgeFont READ badgeFont CONSTANT)
    Q_PROPERTY(QFont closeFont READ closeFont CONSTANT)
    Q_PROPERTY(QFont captionFont READ captionFont CONSTANT)
    Q_PROPERTY(QFont headlineFont READ headlineFont CONSTANT)

    Q_PROPERTY(int statusDanger READ statusDanger CONSTANT)
    Q_PROPERTY(int statusWarning READ statusWarning CONSTANT)
    Q_PROPERTY(int statusSuccess READ statusSuccess CONSTANT)
    Q_PROPERTY(int statusRunning READ statusRunning CONSTANT)
    Q_PROPERTY(QColor paneDimColor READ paneDimColor CONSTANT)
    Q_PROPERTY(QColor paneDimTextColor READ paneDimTextColor CONSTANT)
    Q_PROPERTY(QColor dotGlyphColor READ dotGlyphColor CONSTANT)
    Q_PROPERTY(qreal hostingNoticeFillAlpha READ hostingNoticeFillAlpha CONSTANT)
    Q_PROPERTY(qreal hostingNoticeBorderAlpha READ hostingNoticeBorderAlpha CONSTANT)

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
    Q_PROPERTY(qreal windowControlInset READ windowControlInset CONSTANT)
    Q_PROPERTY(qreal windowControlGutter READ windowControlGutter CONSTANT)
    Q_PROPERTY(qreal trafficLightDotSize READ trafficLightDotSize CONSTANT)
    Q_PROPERTY(qreal trafficLightGap READ trafficLightGap CONSTANT)
    Q_PROPERTY(qreal badgeWidth READ badgeWidth CONSTANT)
    Q_PROPERTY(qreal badgeHeight READ badgeHeight CONSTANT)

    Q_PROPERTY(int radius READ radius CONSTANT)
    Q_PROPERTY(int borderWidth READ borderWidth CONSTANT)
    Q_PROPERTY(int dropCaretWidth READ dropCaretWidth CONSTANT)
    Q_PROPERTY(int resizeBorderThickness READ resizeBorderThickness CONSTANT)

    Q_PROPERTY(int shadowBlur READ shadowBlur CONSTANT)
    Q_PROPERTY(qreal shadowOffsetY READ shadowOffsetY CONSTANT)
    Q_PROPERTY(qreal shadowMargin READ shadowMargin CONSTANT)
    Q_PROPERTY(bool hasPopupShadow READ hasPopupShadow CONSTANT)

    Q_PROPERTY(QColor errorColor READ errorColor CONSTANT)

    /// The 1a token groups, each a sub-object whose extents are already in logical pixels.
    Q_PROPERTY(QObject* size READ size CONSTANT)
    Q_PROPERTY(QObject* space READ space CONSTANT)
    Q_PROPERTY(QObject* radii READ radii CONSTANT)
    Q_PROPERTY(QObject* type READ type CONSTANT)
    Q_PROPERTY(QObject* motion READ motion CONSTANT)
    Q_PROPERTY(QObject* elevation READ elevation CONSTANT)
    Q_PROPERTY(QObject* material READ material CONSTANT)
    Q_PROPERTY(QObject* contrast READ contrast CONSTANT)
    Q_PROPERTY(QObject* tui READ tui CONSTANT)
    Q_PROPERTY(QObject* layout READ layout CONSTANT)
    /// The chrome colour roles. The object is CONSTANT; its own properties notify.
    Q_PROPERTY(QObject* colors READ colors CONSTANT)

    /// The accent the chrome highlights with: the platform's when it publishes one, else the palette's.
    Q_PROPERTY(QColor accentColor READ accentColor NOTIFY paletteRevisionChanged)

    /// Bumped whenever the application palette changes. statusColor() and onStatusColor() read that
    /// palette, and a binding cannot see through an invokable, so each QML call site reads this first,
    /// using it rather than merely mentioning it (a bare read is dropped by the JS engine):
    /// `chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.statusColor(role)`.
    Q_PROPERTY(int paletteRevision READ paletteRevision NOTIFY paletteRevisionChanged)

    Q_PROPERTY(QString tabSeparator READ tabSeparator CONSTANT)
    Q_PROPERTY(QString trafficLightGlyph READ trafficLightGlyph CONSTANT)
    Q_PROPERTY(QString closeGlyph READ closeGlyph CONSTANT)
    Q_PROPERTY(QString zoomGlyph READ zoomGlyph CONSTANT)
    Q_PROPERTY(QString newTabGlyph READ newTabGlyph CONSTANT)
    Q_PROPERTY(QString menuGlyph READ menuGlyph CONSTANT)
    Q_PROPERTY(QString submenuGlyph READ submenuGlyph CONSTANT)

  public:
    /// @param inputs What the chrome is built from. @c inputs.font is the font the chrome is drawn
    ///               with, already resolved against its fallbacks (see resolveChromeFont); its metrics
    ///               define the cell every terminal-style extent is counted in.
    /// @param parent Qt ownership.
    explicit UiStyleProvider(ChromeInputs inputs, QObject* parent = nullptr);

    /// @param row        The chrome token row, see config::resolveChromeStyle.
    /// @param chromeFont The font the chrome is drawn with. @see chromeInputsFor for the rest.
    /// @param parent     Qt ownership.
    UiStyleProvider(config::ChromeStyle row, QFont const& chromeFont, QObject* parent = nullptr);

    /// How many times the application palette has changed since this provider was built.
    /// @see paletteRevisionChanged
    [[nodiscard]] int paletteRevision() const noexcept { return _paletteRevision; }

    /// The active row's name for QML: "macos", "fluent", "linux" or "terminal".
    [[nodiscard]] QString chromeStyleName() const;

    [[nodiscard]] QFont const& font() const noexcept { return _fonts.base; }
    [[nodiscard]] QFont const& newTabFont() const noexcept { return _fonts.newTab; }
    [[nodiscard]] QFont const& menuFont() const noexcept { return _fonts.menu; }
    [[nodiscard]] QFont const& windowControlFont() const noexcept { return _fonts.windowControl; }
    [[nodiscard]] QFont const& badgeFont() const noexcept { return _fonts.badge; }
    [[nodiscard]] QFont const& closeFont() const noexcept { return _fonts.close; }
    [[nodiscard]] QFont const& captionFont() const noexcept { return _fonts.caption; }
    [[nodiscard]] QFont const& headlineFont() const noexcept { return _fonts.headline; }

    /// The scrim laid over a pane to dim it (an unfocused or reconnecting one).
    [[nodiscard]] QColor paneDimColor() const;

    /// The QML names of the status roles, for statusColor() and onStatusColor(). Properties rather than
    /// an exported enum: a Q_ENUM is not reachable through a context-property instance, so
    /// `chromeStyle.Danger` was silently undefined.
    [[nodiscard]] static constexpr int statusDanger() noexcept
    {
        return static_cast<int>(config::StatusColorRole::Danger);
    }
    [[nodiscard]] static constexpr int statusWarning() noexcept
    {
        return static_cast<int>(config::StatusColorRole::Warning);
    }
    [[nodiscard]] static constexpr int statusSuccess() noexcept
    {
        return static_cast<int>(config::StatusColorRole::Success);
    }
    [[nodiscard]] static constexpr int statusRunning() noexcept
    {
        return static_cast<int>(config::StatusColorRole::Running);
    }

    /// The colour of text drawn on the pane-dim scrim: whichever of black and white contrasts more
    /// with the scrim's black, so it is legible in every theme (a palette's bright-text role is not).
    [[nodiscard]] QColor paneDimTextColor() const;

    /// The colour of the glyph drawn on a traffic-light dot. Dark and translucent: the dots are
    /// saturated in every theme, so the glyph contrasts with the dot, not with the window.
    [[nodiscard]] QColor dotGlyphColor() const;

    /// Opacity, 0 to 1, of the hosting-notice bar's palette-derived fill.
    [[nodiscard]] qreal hostingNoticeFillAlpha() const noexcept;

    /// Opacity, 0 to 1, of the hosting-notice bar's palette-derived border.
    [[nodiscard]] qreal hostingNoticeBorderAlpha() const noexcept;

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
    [[nodiscard]] qreal windowControlInset() const noexcept { return _metrics.windowControlInset; }
    [[nodiscard]] qreal windowControlGutter() const noexcept { return _metrics.windowControlGutter; }
    [[nodiscard]] qreal trafficLightDotSize() const noexcept { return _metrics.trafficLightDotSize; }
    [[nodiscard]] qreal trafficLightGap() const noexcept { return _metrics.trafficLightGap; }
    [[nodiscard]] qreal badgeWidth() const noexcept { return _metrics.badgeWidth; }
    [[nodiscard]] qreal badgeHeight() const noexcept { return _metrics.badgeHeight; }

    [[nodiscard]] int radius() const noexcept { return _tokens.cornerRadiusPixels; }
    [[nodiscard]] int borderWidth() const noexcept { return _tokens.borderWidthPixels; }
    [[nodiscard]] int dropCaretWidth() const noexcept { return _tokens.dropCaretPixels; }

    /// How far a frameless window's resize border reaches in from each edge, in logical pixels.
    /// @see geometry::ResizeBorderThickness, geometry::gutterLeadIn
    [[nodiscard]] int resizeBorderThickness() const noexcept;

    /// Blur radius of a popup's drop shadow, in logical pixels. 0 draws a hard-edged one.
    [[nodiscard]] int shadowBlur() const noexcept { return _tokens.shadowBlurPixels; }
    [[nodiscard]] qreal shadowOffsetY() const noexcept { return _metrics.shadowOffsetY; }
    [[nodiscard]] qreal shadowMargin() const noexcept { return _metrics.shadowMargin; }

    /// Whether this style asks for a popup shadow at all.
    ///
    /// A style may cast a hard-edged shadow (no blur, some offset) or a soft one with no offset, so
    /// neither token alone answers it -- which is why the QML asks this rather than testing one.
    [[nodiscard]] bool hasPopupShadow() const noexcept
    {
        return _tokens.shadowBlurPixels > 0 || _tokens.shadowOffsetUnits > 0;
    }

    /// The color a tab's OSC 9;4 progress bar is painted in for @p state.
    ///
    /// @param state A vtbackend::ProgressState as its underlying value, as the tab model reports it.
    /// @return The style's color for that state; transparent for a state that paints nothing.
    [[nodiscard]] Q_INVOKABLE QColor progressColor(int state) const;

    /// The colour that says @p role, for the chrome the application palette currently paints.
    ///
    /// Picks the light or dark variant by the luminance of the palette's @c Window colour, so a status
    /// hue keeps its contrast on either chrome. @c Running is the palette's highlight instead of a
    /// fixed hue.
    ///
    /// @param role A config::StatusColorRole as its underlying value, as QML passes it.
    /// @return The colour; transparent for a role outside the table.
    [[nodiscard]] Q_INVOKABLE QColor statusColor(int role) const;

    /// Black or white, whichever has the higher contrast ratio against @p background.
    /// @param background The fill the text is drawn on.
    /// @return The legible text colour for that fill.
    [[nodiscard]] Q_INVOKABLE QColor onColor(QColor const& background) const;

    /// The text colour to draw on statusColor(@p role).
    /// @param role A config::StatusColorRole as its underlying value.
    /// @return Black or white, see onColor().
    [[nodiscard]] Q_INVOKABLE QColor onStatusColor(int role) const;

    /// The colour this style says "this failed" in.
    ///
    /// Taken from the same token row the progress bar's failure state uses, rather than invented
    /// afresh or derived from the OS palette: which hue reads as an error is a statement about
    /// meaning, and a theme-picked one could render a failure green. @see UiStyleTokens.
    [[nodiscard]] QColor errorColor() const;

    /// The accent the chrome highlights with, and the colour of @c Running.
    /// @return The platform accent when the OS publishes one, else the palette's Highlight.
    [[nodiscard]] QColor accentColor() const;

    /// The token groups for QML. Non-const because QML receives them as mutable QObject pointers; the
    /// objects themselves expose only CONSTANT (or palette-notifying) properties.
    /// @return This provider's group object, owned by it.
    [[nodiscard]] QObject* size() noexcept { return &_size; }
    [[nodiscard]] QObject* space() noexcept { return &_space; }
    [[nodiscard]] QObject* radii() noexcept { return &_radii; }
    [[nodiscard]] QObject* type() noexcept { return &_type; }
    [[nodiscard]] QObject* motion() noexcept { return &_motion; }
    [[nodiscard]] QObject* elevation() noexcept { return &_elevation; }
    [[nodiscard]] QObject* material() noexcept { return &_material; }
    [[nodiscard]] QObject* contrast() noexcept { return &_contrast; }
    [[nodiscard]] QObject* tui() noexcept { return &_tui; }
    [[nodiscard]] QObject* layout() noexcept { return &_layout; }
    [[nodiscard]] QObject* colors() noexcept { return &_colors; }

    [[nodiscard]] QString const& tabSeparator() const noexcept { return _glyphs.tabSeparator; }
    [[nodiscard]] QString const& trafficLightGlyph() const noexcept { return _glyphs.trafficLight; }
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

    /// The color a popup's drop shadow is drawn in: @p base at the style's shadow alpha.
    ///
    /// Same shape and same reason as @ref modalScrim -- how heavy a shadow reads is one number in
    /// the token table rather than a literal repeated in every popup surface.
    [[nodiscard]] Q_INVOKABLE QColor shadowTint(QColor const& base) const;

  signals:
    /// The application palette changed, so every palette-derived colour (statusColor()) may have too.
    void paletteRevisionChanged();

  protected:
    /// Watches the application object for QEvent::ApplicationPaletteChange and bumps paletteRevision.
    /// An event filter rather than QGuiApplication::paletteChanged, which Qt 6 deprecates for the event.
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    // By value rather than by reference: the rows have static storage duration, but a reference member
    // would delete move-assignment for no gain -- the row is a handful of ints and string_views.
    ChromeInputs _inputs; ///< What this provider was built from.
    config::ChromeStyle _row;
    config::UiStyleTokens _tokens;
    ChromeFonts _fonts;
    ChromeMetrics _metrics;
    ChromeGlyphs _glyphs;
    config::ChromeTokenRow _groups {}; ///< The 1a row, resolved against accessibility.
    ChromeUnits _units {};             ///< What one token unit is, in pixels.
    // The group objects, children of this provider by value: they die with it, before ~QObject runs.
    ChromeSizeTokens _size;
    ChromeSpaceTokens _space;
    ChromeRadiusTokens _radii;
    ChromeTypeTokens _type;
    ChromeMotionTokens _motion;
    ChromeElevationTokens _elevation;
    ChromeMaterialTokens _material;
    ChromeContrastTokens _contrast;
    ChromeTuiTokens _tui;
    ChromeLayoutTokens _layout;
    ChromeColorTokens _colors;
    int _paletteRevision = 0;
};

/// The font @p config says the chrome should be drawn with.
///
/// A style whose row asks for the platform UI font gets it, because that is what makes a native
/// chrome look native. One asking for the terminal's font takes `ui_font_family` / `ui_font_size`
/// where they are set and otherwise inherits @p profileName's regular font -- which is what makes
/// the chrome match the terminal grid below it without the user configuring anything twice.
///
/// @param config      The loaded configuration.
/// @param row         The resolved chrome row, see config::resolveChromeStyle.
/// @param profileName The profile this window is opening with, i.e. ContourGuiApp::profileName().
///                    The *running* profile rather than @c default_profile, because `-p/--profile`
///                    overrides the latter and the chrome has to match the grid actually on screen.
///                    A name that resolves to no profile leaves the platform font in place.
/// @return A font ready to hand to UiStyleProvider.
[[nodiscard]] QFont resolveChromeFont(config::Config const& config,
                                      config::ChromeStyle row,
                                      std::string const& profileName);

} // namespace contour::window
