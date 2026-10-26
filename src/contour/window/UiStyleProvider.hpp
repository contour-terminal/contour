// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/AccessibilityPreferences.hpp>
#include <contour/config/ChromeDensity.hpp>
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/platform/PlatformAppearance.hpp>
#include <contour/window/ChromeTokenObjects.hpp>
#include <contour/window/TitleBandGeometry.hpp>
#include <contour/window/TitleBandLayout.hpp>

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariantMap>
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
    QString tabColor;  ///< A coloured tab's leading cell; empty where the colour tints the tab.
    QString chipOpen;  ///< Opens a chip; empty where the chip is a shape.
    QString chipClose; ///< Closes a chip; empty where the chip is a shape.
    QString hostDot;   ///< The host chip's state dot; empty where it is a vector circle.

    QStringList paneActions; ///< A pane header's action buttons, indexed by config::PaneHeaderAction.
    QStringList splitLines;  ///< A split's line, side by side then stacked; empty where the gap draws it.
    QString restart;         ///< Marks the exited footer's Restart button.
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
    config::ChromeDensity density = config::ChromeDensity::Comfortable; ///< The configured chrome density.

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
    /// The pane dim follows the chrome's tone, so it notifies with the palette.
    Q_PROPERTY(QColor paneDimColor READ paneDimColor NOTIFY paletteRevisionChanged)
    Q_PROPERTY(QColor dotGlyphColor READ dotGlyphColor CONSTANT)

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

    /// "This failed", the @c Danger status colour; it follows the application palette.
    Q_PROPERTY(QColor errorColor READ errorColor NOTIFY paletteRevisionChanged)

    /// The title band's height: @c size.titleBar under the @c Pill shape, else today's strip height
    /// (@c chromeHeight), so the Flat rows keep their geometry while @c size.titleBar records the kit's.
    Q_PROPERTY(qreal bandHeight READ bandHeight CONSTANT)

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
    Q_PROPERTY(QObject* tint READ tint CONSTANT)
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
    Q_PROPERTY(QString tabColorGlyph READ tabColorGlyph CONSTANT)
    Q_PROPERTY(QString chipOpenGlyph READ chipOpenGlyph CONSTANT)
    Q_PROPERTY(QString chipCloseGlyph READ chipCloseGlyph CONSTANT)
    Q_PROPERTY(QString hostDotGlyph READ hostDotGlyph CONSTANT)

    // {{{ Panes (Step 2)
    /// The configured chrome density: "comfortable" or "minimal".
    Q_PROPERTY(QString density READ densityName CONSTANT)
    Q_PROPERTY(QStringList paneActionGlyphs READ paneActionGlyphs CONSTANT)
    Q_PROPERTY(QStringList splitLineGlyphs READ splitLineGlyphs CONSTANT)
    Q_PROPERTY(QString restartGlyph READ restartGlyph CONSTANT)
    /// What assistive technology announces for each header action, indexed by config::PaneHeaderAction.
    Q_PROPERTY(QStringList paneHeaderActionNames READ paneHeaderActionNames CONSTANT)
    /// The QML names of the pane colour roles, for colorOn().
    Q_PROPERTY(QObject* roles READ roles CONSTANT)
    /// The QML names of the tab badge kinds, for badgeLook().
    Q_PROPERTY(QObject* badgeKinds READ badgeKinds CONSTANT)
    /// The QML names of an ended pane's ends and restart offers.
    Q_PROPERTY(QObject* paneEnds READ paneEnds CONSTANT)
    /// The height of a pane header the held modifier reveals over its pane (Minimal).
    Q_PROPERTY(qreal overlayPaneHeader READ overlayPaneHeader CONSTANT)
    // }}}

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

    /// @return What this provider was built from.
    [[nodiscard]] ChromeInputs const& inputs() const noexcept { return _inputs; }

    /// The token row this provider publishes: its style's row with the density and the accessibility
    /// preferences applied. For C++ consumers that need the same numbers the QML reads.
    [[nodiscard]] config::ChromeTokenRow const& resolvedRow() const noexcept { return _groups; }

    /// What one token unit is, in logical pixels: a chrome cell on the Terminal row, one pixel elsewhere.
    [[nodiscard]] ChromeUnits const& units() const noexcept { return _units; }

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

    // {{{ Panes (Step 2)

    /// The scrim laid over a pane to dim it (an unfocused or reconnecting one): black at the kit's
    /// @c pane.dim for the chrome's tone (decided as statusColor() decides it), or at
    /// @c contrast.paneDimPercent under Increased Contrast. Reads the palette.
    [[nodiscard]] QColor paneDimColor() const;

    /// @return The configured chrome density's name: "comfortable" or "minimal".
    [[nodiscard]] QString densityName() const;
    [[nodiscard]] QStringList const& paneActionGlyphs() const noexcept { return _glyphs.paneActions; }
    [[nodiscard]] QStringList const& splitLineGlyphs() const noexcept { return _glyphs.splitLines; }
    [[nodiscard]] QString const& restartGlyph() const noexcept { return _glyphs.restart; }
    /// @return The header actions' accessible names, indexed by config::PaneHeaderAction.
    [[nodiscard]] QStringList paneHeaderActionNames() const;
    [[nodiscard]] QObject* roles() noexcept { return &_paneRoles; }
    [[nodiscard]] QObject* badgeKinds() noexcept { return &_badgeKinds; }
    [[nodiscard]] QObject* paneEnds() noexcept { return &_paneEnds; }
    /// An overlay takes no room from its pane, so Minimal, whose sub-row zeroes the inline header's
    /// @c size.paneHeader for the size solver, still draws it at the row's own header height.
    /// @return The row's pane header height before the density applies, in logical pixels.
    [[nodiscard]] qreal overlayPaneHeader() const noexcept;

    /// Decides a pane's header (window::resolvePaneHeader) with this provider's density, its
    /// @c size.paneHeader, one chrome cell as the least content height, and its @c layout.paneActions.
    ///
    /// Ints rather than enums: the QML-facing carve-out. A value out of range answers an empty map.
    /// @param reveal       A window::HeaderReveal.
    /// @param pointer      A window::PointerPresence.
    /// @param focus        A window::PaneFocus.
    /// @param visiblePanes The panes visible in the tab.
    /// @param width        The pane's width in logical pixels.
    /// @param height       The pane's height in logical pixels.
    /// @return `{ content, actions, placement, emphasis }` as their names.
    [[nodiscard]] Q_INVOKABLE QVariantMap
    paneHeader(int reveal, int pointer, int focus, int visiblePanes, qreal width, qreal height) const;

    /// The name a pane header shows (window::paneHeaderName).
    /// @param processName The foreground process name.
    /// @param title       The pane's title.
    /// @return @p processName unless empty, else @p title.
    [[nodiscard]] Q_INVOKABLE QString paneHeaderName(QString const& processName, QString const& title) const;

    /// The colour of @p role on a pane whose own background is @p background.
    ///
    /// The kit's @c Fill, @c Label, @c Surface and @c Separator sources take their light or dark half by
    /// @p background's tone (window::toneOf) rather than the palette window's, so a light-profile pane
    /// under a dark chrome keeps a legible header; every other kind resolves as @c colors does, which
    /// reads the palette, so a binding over it also reads @ref paletteRevision.
    /// @param role       A config::ChromeColorRole as its underlying value (see @c roles).
    /// @param background The pane's own background.
    /// @return The colour; transparent for a role outside the table.
    [[nodiscard]] Q_INVOKABLE QColor colorOn(int role, QColor const& background) const;

    /// How far a pane card keeps its terminal in from every edge (window::cardContentInset) at its radius.
    /// @param devicePixelRatio The window's device-pixel ratio.
    /// @return The inset in logical pixels, on the device grid; 0 for a square card.
    [[nodiscard]] Q_INVOKABLE qreal paneCardContentInset(qreal devicePixelRatio) const;

    /// One device pixel, in logical pixels: the pane card's border width.
    /// @param devicePixelRatio The window's device-pixel ratio.
    /// @return @c 1 / @p devicePixelRatio, or 1 for a ratio of 0 or less.
    [[nodiscard]] Q_INVOKABLE qreal hairline(qreal devicePixelRatio) const;

    /// How strongly an unfocused pane dims (R-g).
    ///
    /// Under Minimal, where dimming is the only focus cue (D5), at least the kit's @c pane.dim for the
    /// chrome's tone; under Comfortable @p profileDim. Either is capped at @c contrast.paneDimPercent
    /// where Increased Contrast sets it. Reads the palette, so a binding over it also reads
    /// @ref paletteRevision.
    /// @param profileDim The profile's @c dim_unfocused, 0 to 1.
    /// @return The dim, 0 to 1.
    [[nodiscard]] Q_INVOKABLE qreal unfocusedDim(qreal profileDim) const;

    // }}}

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

    /// The colour of the glyph drawn on a traffic-light dot. Dark and translucent: the dots are
    /// saturated in every theme, so the glyph contrasts with the dot, not with the window.
    [[nodiscard]] QColor dotGlyphColor() const;

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

    /// The color a tab's OSC 9;4 progress bar is painted in for @p state: the row's status ink for
    /// that state's role (config::ProgressStatusRoles), so running is the accent.
    ///
    /// @param state A vtbackend::ProgressState as its underlying value, as the tab model reports it.
    /// @return The ink; transparent for a state that paints nothing.
    [[nodiscard]] Q_INVOKABLE QColor progressColor(int state) const;

    /// What @p role is inked in on this row's chrome: the status hue itself on the native rows, and
    /// on the Terminal row the scheme's hue where it reads on the bar, else the bar's text.
    ///
    /// Reads the application palette, so a binding over it also reads @ref paletteRevision.
    /// @param role A config::StatusColorRole as its underlying value.
    /// @return The ink; transparent for a role outside the table.
    [[nodiscard]] Q_INVOKABLE QColor statusInk(int role) const;

    /// How a tab badge kind looks.
    /// @param kind A window::TabBadgeKind as its underlying value.
    /// @return `{ glyph, mark, ink, name }`: the Flat glyph, the Pill mark's name (badgeMarkName), the
    ///         status ink and the accessible name; empty for a kind outside the table.
    [[nodiscard]] Q_INVOKABLE QVariantMap badgeLook(int kind) const;

    /// How the host chip looks in @p state.
    /// @param state A window::HostChipState as its underlying value.
    /// @return `{ dot, status, accessibleState }`: the dot's ink and the two texts; empty for a state
    ///         outside the table.
    [[nodiscard]] Q_INVOKABLE QVariantMap hostChipLook(int state) const;

    /// Whether a window attached to @p hostLabel shows the host chip (window::hostChipPresence).
    /// @param hostLabel The window's host label; empty for a local window.
    /// @return Whether the chip shows: a QML-facing bool, the documented carve-out.
    [[nodiscard]] Q_INVOKABLE bool isHostChipShown(QString const& hostLabel) const;

    /// Where the title band and the tabs go, for this row's tab shape (window::resolveTitleBand).
    ///
    /// Ints rather than enums: this is the QML-facing carve-out, whose one caller (Main.qml) passes the
    /// enumerators' underlying values; a value out of range answers an empty map.
    /// @param frame         A window::WindowFrame.
    /// @param mode          A window::WindowMode.
    /// @param strip         A window::TabStripGate.
    /// @param position      A config::TabBarPosition.
    /// @param hostLabel     The window's host label; empty for a local window.
    /// @param hostChipState A window::HostChipState: what the host chip shows, which a remote window's
    ///                      band carries whatever the gate says while it is not Connected
    ///                      (window::hostNoticeOf).
    /// @param tabs          A window::OpenTabs: whether the window holds one tab.
    /// @return `{ band, bandEdge, centre, tabBar }` as their names.
    [[nodiscard]] Q_INVOKABLE QVariantMap titleBand(int frame,
                                                    int mode,
                                                    int strip,
                                                    int position,
                                                    QString const& hostLabel,
                                                    int hostChipState,
                                                    int tabs) const;

    /// The chrome row's side of the band decision: this provider's row, its tab shape and its density.
    /// @return The row window::resolveTitleBand(TitleBandRow const&, TitleBandInputs) takes.
    [[nodiscard]] TitleBandRow titleBandRow() const noexcept
    {
        return TitleBandRow { .style = _inputs.row,
                              .shape = _groups.layout.tabShape,
                              .density = _inputs.density };
    }

    /// Where the title band and the tabs go (window::resolveTitleBand) for this provider's row.
    /// @param inputs The window's side of the decision: frame, mode, gate, tab count, edge and the host's
    ///               notice; its row fields are overridden by titleBandRow().
    /// @return The band.
    [[nodiscard]] TitleBand titleBandOf(TitleBandInputs const& inputs) const noexcept
    {
        return resolveTitleBand(titleBandRow(), inputs);
    }

    /// The tab well's widths for @p tabCount tabs in @p available logical pixels (window::layoutTabWell).
    /// @param available The width the band leaves the well.
    /// @param tabCount  The number of tabs.
    /// @return `{ tabWidth, wellWidth, label, scrolls }`: the widths, the label mode's name, and whether
    ///         the well scrolls (a QML-facing bool, the documented carve-out).
    [[nodiscard]] Q_INVOKABLE QVariantMap tabWellLayout(qreal available, int tabCount) const;

    /// The font the active tab's label is set in: @p base, in bold where the row sets the active tab
    /// apart by weight (config::ActiveTabWeight), because its fill can be the bar's own colour there.
    /// @param base The font every other tab's label is set in.
    /// @return The active label's font.
    [[nodiscard]] Q_INVOKABLE QFont activeTabFont(QFont const& base) const;

    /// @p length rounded up to whole chrome cells (R-q): a cell on the Terminal row, a pixel elsewhere.
    /// @param length A horizontal extent in logical pixels.
    /// @return The extent, snapped up to the width quantum.
    [[nodiscard]] Q_INVOKABLE qreal snapToCells(qreal length) const;

    /// @return The title band's height in logical pixels. @see bandHeight
    [[nodiscard]] qreal bandHeight() const noexcept;

    /// The title band's tokens as this provider resolves them: its row, the row's resolved title-band
    /// material, the band's height -- @c size.bareBand for a band whose middle is empty, else bandHeight() --
    /// on the Terminal row one cell's height (0 elsewhere), and the density.
    /// @param centre What the band's middle holds (titleBandOf()).
    /// @return The tokens window::resolveTitleBandGeometry() takes.
    [[nodiscard]] TitleBandTokens titleBandTokens(BarCentre centre) const noexcept
    {
        return TitleBandTokens {
            .style = _inputs.row,
            .material = _groups.material.titleBand,
            .height = centre == BarCentre::Empty ? _size.bareBand() : bandHeight(),
            .cellHeight = _inputs.row == config::ChromeStyle::Terminal ? cellHeight() : 0.0,
            .density = _inputs.density,
        };
    }

    /// What the title band paints: its surface (window::resolveTitleBandSurface) for this row's resolved
    /// @c material.titleBand, @p frame and @p mode, as a colour.
    ///
    /// - @c Palette: the application palette's window colour (the band before Step 1d);
    /// - @c Clear: transparent, so a material behind the band shows;
    /// - @c Tint: window::legibleBandTint of the chrome scheme's background for the chrome's tone (decided
    ///   as statusColor() decides it), else the palette's window colour.
    /// - @c Background: window::legibleBandBackground, the chrome scheme's background itself (Minimal's bare
    ///   band) where it has the chrome's tone, else the palette's window colour.
    ///
    /// Reads the palette, so a binding over it also reads @ref paletteRevision. Ints and a name rather than
    /// enums: the QML-facing carve-out, whose one caller (Main.qml) passes the values it passes titleBand()
    /// and the centre titleBand() answered.
    /// @param frame  A window::WindowFrame.
    /// @param mode   A window::WindowMode.
    /// @param centre What the band's middle holds, as titleBand() names it ("empty", "tabs" or "title"):
    ///               under Minimal an empty band is the bare band, in the terminal background.
    /// @return The fill; the palette's window colour for a value out of range or an unknown centre.
    [[nodiscard]] Q_INVOKABLE QColor bandFill(int frame, int mode, QString const& centre) const;

    /// The colour that says @p role, for the chrome the application palette currently paints.
    ///
    /// Picks the light or dark variant by the luminance of the palette's @c Window colour, so a status
    /// hue keeps its contrast on either chrome. @c Running is the palette's highlight instead of a
    /// fixed hue.
    ///
    /// This is the text ink (config::StatusInkUse::Text): what QML colours words and filled buttons with.
    ///
    /// @param role A config::StatusColorRole as its underlying value, as QML passes it.
    /// @return The colour; transparent for a role outside the table.
    [[nodiscard]] Q_INVOKABLE QColor statusColor(int role) const;

    /// The colour that says @p role when drawn as @p use, for the chrome the application palette paints.
    /// @param role The status role.
    /// @param use  Whether it colours text or a mark (config::StatusInkUse).
    /// @return The colour; transparent for a role outside the table. @see statusColor
    [[nodiscard]] QColor statusColor(config::StatusColorRole role, config::StatusInkUse use) const;

    /// Black or white, whichever has the higher contrast ratio against @p background.
    /// @param background The fill the text is drawn on.
    /// @return The legible text colour for that fill.
    [[nodiscard]] Q_INVOKABLE QColor onColor(QColor const& background) const;

    /// The mark ink of @p role for a mark drawn on @p background, rather than on the chrome the palette
    /// paints: the role's mark hue for the background's tone, or black or white (onColor()) where that hue
    /// falls short of config::MinimumMarkContrast there. For a mark on a surface of its own, such as a
    /// pane's pill on the toast fill. Running is the accent, as in statusColor().
    /// @param role       A config::StatusColorRole as its underlying value.
    /// @param background The opaque fill the mark is drawn on.
    /// @return The ink; transparent for a role outside the table.
    [[nodiscard]] Q_INVOKABLE QColor statusMarkOn(int role, QColor const& background) const;

    /// The text colour to draw on statusColor(@p role).
    /// @param role A config::StatusColorRole as its underlying value.
    /// @return Black or white, see onColor().
    [[nodiscard]] Q_INVOKABLE QColor onStatusColor(int role) const;

    /// The colour this style says "this failed" in: @c statusColor(Danger).
    ///
    /// The status token rather than the bar's ink (@ref statusInk): error text sits on pages, fields
    /// and the find bar, not on the Terminal row's bar, so it keeps the hue the status table pins.
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
    [[nodiscard]] QObject* tint() noexcept { return &_tint; }
    [[nodiscard]] QObject* colors() noexcept { return &_colors; }

    [[nodiscard]] QString const& tabSeparator() const noexcept { return _glyphs.tabSeparator; }
    [[nodiscard]] QString const& trafficLightGlyph() const noexcept { return _glyphs.trafficLight; }
    [[nodiscard]] QString const& closeGlyph() const noexcept { return _glyphs.close; }
    [[nodiscard]] QString const& zoomGlyph() const noexcept { return _glyphs.zoom; }
    [[nodiscard]] QString const& newTabGlyph() const noexcept { return _glyphs.newTab; }
    [[nodiscard]] QString const& menuGlyph() const noexcept { return _glyphs.menu; }
    [[nodiscard]] QString const& submenuGlyph() const noexcept { return _glyphs.submenu; }
    [[nodiscard]] QString const& tabColorGlyph() const noexcept { return _glyphs.tabColor; }
    [[nodiscard]] QString const& chipOpenGlyph() const noexcept { return _glyphs.chipOpen; }
    [[nodiscard]] QString const& chipCloseGlyph() const noexcept { return _glyphs.chipClose; }
    [[nodiscard]] QString const& hostDotGlyph() const noexcept { return _glyphs.hostDot; }

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

    /// Watches the application object for QEvent::ApplicationPaletteChange and bumps paletteRevision.
    /// An event filter rather than QGuiApplication::paletteChanged, which Qt 6 deprecates for the event.
    /// Public as QObject declares it: an override does not narrow the visibility of what it overrides.
    bool eventFilter(QObject* watched, QEvent* event) override;

  signals:
    /// The application palette changed, so every palette-derived colour (statusColor()) may have too.
    void paletteRevisionChanged();

  private:
    // By value rather than by reference: the rows have static storage duration, but a reference member
    // would delete move-assignment for no gain -- the row is a handful of ints and string_views.
    ChromeInputs _inputs; ///< What this provider was built from.
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
    ChromeTintTokens _tint;
    ChromeColorTokens _colors;
    ChromePaneRoleNames _paneRoles;
    ChromeBadgeKindNames _badgeKinds;
    ChromePaneEndNames _paneEnds;
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
