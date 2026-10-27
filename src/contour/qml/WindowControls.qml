// vim:syntax=qml
// Minimize / maximize-restore / close buttons for the frameless (client-side-decorated) window.
//
// Which buttons, in which order, on which side and in which shape all come from the
// `windowControls` context property (the configured window_control_style), and every extent comes
// from `chromeStyle` (the configured ui_style). Nothing here knows which of either is active, so a
// new style is a new row in one of those two tables -- unless it wants a shape neither delegate
// below draws, which is the one change that also needs QML.
//
// Shown only while the native frame is hidden; with it shown, the OS draws these itself and ours
// would duplicate them. TitleBar owns that gate.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

RowLayout {
    id: root

    required property var window
    // The WindowController — the ONLY component allowed to change window show modes. Calling
    // window.showMaximized()/showNormal() directly here would skip its size-increment protocol
    // (stale WM increments while maximized, no grid-snap hints after restore).
    required property var controller

    // Whether the group as a whole is hovered. Traffic lights reveal their glyphs together rather
    // than one at a time, which is how macOS does it and what makes the three read as one control.
    // The handler sits on the controls' own row rather than on this layout, so the inert inset
    // beside them does not reveal anything: the pointer has to have reached a control.
    // On Windows the OS drives the maximise control while the pointer is over it (Snap Layouts), and Qt then
    // sees no hover of its own: the controller's word counts as the pointer reaching the group too.
    readonly property bool groupHovered: groupHover.hovered || !!root.controller.nativeMaximizeHovered

    // Whether the window is maximized, so the maximize control can show its restore face. Read once
    // here rather than per delegate.
    readonly property bool windowMaximized: root.window.visibility === Window.Maximized

    // Whether the controls are drawn as traffic lights. Read once here: it decides the inset, the
    // band the group occupies, and which delegate each control loads.
    readonly property bool trafficLights: windowControls.presentation === "trafficLight"

    // Inset from the bar's outer edge, so the controls do not sit flush against the window corner.
    // Only a traffic-light group takes it: a button style sits flush in the corner, which is what
    // every Windows-like title bar draws and what Contour has always drawn -- and on a maximized
    // window it is what keeps "throw the pointer into the screen corner and click" landing on close
    // rather than in a dead strip beside it.
    //
    // real, not int: under a cell-counting chrome this is one CELL, and a cell width is fractional
    // for essentially every monospace font -- truncating it would take the whole group off the very
    // character grid the token exists to keep it on.
    readonly property real outerInset: root.trafficLights ? chromeStyle.windowControlInset : 0

    // Which of the group's two margins faces the window's edge. The other one faces the rest of the
    // bar, and takes the gutter instead.
    readonly property bool leadingSide: windowControls.side === "leading"

    // The vertical band a traffic-light group occupies, and so the extent its hover reveal covers.
    // As tall as one control is wide -- deliberately more than the dot itself, because these are the
    // smallest targets in the chrome -- but bounded rather than the full bar height: the rest of a
    // title bar is drag region, and its top edge belongs to the resize border. Anything that lights
    // up on hover has to be something a click can reach.
    // Against chromeStyle.chromeHeight rather than root.height: the bar's height is what the chrome
    // says it is, and reading it back off this layout -- whose own implicit height this band feeds --
    // would be a binding loop.
    readonly property real trafficLightBand: Math.min(chromeStyle.chromeHeight,
                                                      chromeStyle.trafficLightDotSize
                                                      + chromeStyle.trafficLightGap)

    // Flush. What separates two traffic lights is part of each control's own width (see the
    // delegates below), not space between them -- layout spacing is not clickable, and these are the
    // smallest targets in the chrome. Buttons want no separation at all, which is what a title bar
    // has always looked like.
    spacing: 0

    // Live OS palette so the glyphs adapt to dark/light themes. The window is
    // transparent, so an explicit contentItem with a palette-driven color keeps every control glyph
    // legible in both light and dark modes regardless of the active style or hover state.
    SystemPalette {
        id: systemPalette
        colorGroup: SystemPalette.Active
    }

    // What each `action` does, and what a screen reader calls it: two tables keyed by the action the
    // provider sent, rather than two chains of string comparisons. The provider's rows ARE data, so
    // a control kind added to the token table but forgotten here is one lookup that fails loudly at
    // the single place that resolves it -- instead of a control that renders correctly, announces
    // itself as "" and silently does nothing when clicked.
    //
    // Close is the one that does NOT go through the controller: closing is the window's own
    // business, and Main.qml's onClosing takes it from there.
    readonly property var actionHandlers: ({
        "minimize": () => root.controller.minimizeWindow(),
        "maximize": () => root.controller.toggleMaximized(),
        "close": () => root.window.close()
    })

    // The translated names. Kept in QML rather than in the provider so the strings stay in the
    // translation catalog; the provider supplies only the untranslatable `action` key to look them
    // up by. Each is a function because maximize's name depends on the window's current state.
    readonly property var actionNames: ({
        "minimize": () => qsTr("Minimize"),
        "maximize": () => root.windowMaximized ? qsTr("Restore") : qsTr("Maximize"),
        "close": () => qsTr("Close window")
    })

    // Runs what `action` names.
    function activate(action) {
        const run = root.actionHandlers[action]
        if (run)
            run()
        else
            console.warn("WindowControls: no handler for window control action '" + action + "'")
    }

    // The name a screen reader announces for `action`.
    function accessibleNameFor(action) {
        const name = root.actionNames[action]
        return name ? name() : ""
    }

    // The face a control shows right now: its restore glyph only where it has a distinct one.
    function glyphFor(button) {
        return root.windowMaximized ? button.restoreGlyph : button.glyph
    }

    // The font a button draws its glyph in: the style's icon font where one is installed (the provider
    // decides), else the chrome's own, at the chrome's size either way.
    function glyphFontFor(button) {
        const base = chromeStyle.windowControlFont
        if (button.font === "")
            return base
        return base.pixelSize > 0 ? Qt.font({ family: button.font, pixelSize: base.pixelSize })
                                  : Qt.font({ family: button.font, pointSize: base.pointSize })
    }

    // The fill a hovered or pressed button takes: the style's own (the close button's red), else its wash --
    // the kit's fill on Windows, the chrome's ordinary highlight wash elsewhere.
    function fillFor(button) {
        if (button.hoverFill !== "")
            return button.hoverFill
        return button.wash === "kitFill" ? chromeStyle.colors.captionWash : chromeStyle.wash(systemPalette.highlight)
    }

    // A button's glyph: legible on the style's own fill, dimmed on an inactive window, a step down while
    // pressed, the palette's text otherwise.
    function glyphColorFor(button, lit, held) {
        if (lit && button.hoverFill !== "")
            return chromeStyle.onColor(button.hoverFill)
        if (!root.window.active)
            return chromeStyle.colors.captionGlyphInactive
        if (held)
            return chromeStyle.colors.captionGlyphPressed
        return systemPalette.windowText
    }

    // A dot's colour: the scheme's status hue on the Terminal row (the provider's `hasDot`), the style's own
    // elsewhere; and a window-level fade while the window is inactive.
    function dotColorFor(button) {
        if (!root.window.active)
            return button.hasDot ? chromeStyle.colors.captionGlyphInactive : windowControls.inactiveDotColor
        return button.hasDot ? chromeStyle.statusInk(button.dotRole) : button.dotColor
    }

    // Asked of every control whenever the group may have moved in the scene, so each reports its rectangle
    // again: the caption adapter hit-tests the maximise control against it (Snap Layouts). The group's own
    // place, the slot TitleBar holds it in (which a layout pass moves after a resize), and the window's size.
    signal sceneGeometryChanged()
    onXChanged: root.sceneGeometryChanged()
    onYChanged: root.sceneGeometryChanged()
    Connections {
        target: root.parent
        ignoreUnknownSignals: true
        function onXChanged() { root.sceneGeometryChanged() }
        function onYChanged() { root.sceneGeometryChanged() }
    }
    Connections {
        target: root.window
        // A stand-in window (a test's) may have no size of its own to follow.
        ignoreUnknownSignals: true
        function onWidthChanged() { root.sceneGeometryChanged() }
        function onHeightChanged() { root.sceneGeometryChanged() }
    }

    // One traffic light, minus the light itself: both chromes draw the same CONTROL -- same width,
    // same accessible button, same hit target -- and differ only in what paints the dot, so that
    // difference is all either delegate below has to state.
    component TrafficLight: Item {
        id: light

        /// The provider's row for this control.
        required property var button
        /// Whether this control is the last in the group.
        required property bool lastInGroup

        // Dot plus the gap that follows it, so two adjacent lights have no dead strip between them
        // and the dot still starts on whatever grid the chrome counts in. The LAST control has no
        // neighbour: its gap would be a clickable strip in what reads as bare title bar, so it does
        // not get one. @see UiStyleTokens::trafficLightGapUnits.
        implicitWidth: chromeStyle.trafficLightDotSize
                     + (light.lastInGroup ? 0 : chromeStyle.trafficLightGap)

        Accessible.role: Accessible.Button
        Accessible.name: root.accessibleNameFor(light.button.action)
        Accessible.onPressAction: root.activate(light.button.action)

        // The whole frame is the click target, which is what keeps it identical to the extent the
        // group-hover reveal covers -- the band is stated once, on the group.
        TapHandler {
            // ReleaseWithinBounds rather than the default DragThreshold, which takes no exclusive
            // grab and drops the tap as soon as the pointer drifts past the drag threshold while
            // down. These are the smallest targets in the chrome, so a little jitter must still
            // activate them -- the same reason TitleBar's context-menu handler states it.
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: root.activate(light.button.action)
        }
    }

    // The controls' own row, and the extent the group-hover handler covers. Nested rather than hung
    // off this layout directly so that the inset stays outside it: the inset exists to keep the
    // controls off the window corner, which means it must be inert.
    RowLayout {
        id: controlGroup

        // The inset keeps the group off the window's corner; the gutter keeps it off whatever the bar
        // puts beside it -- the tab strip at the leading edge, the drag region at the trailing one.
        // Both are margins rather than width, so neither is clickable: the space between the last
        // light and the first tab must not activate the light it is beside.
        //
        // The gutter is unconditional where the inset is a traffic light's alone. A button style
        // sits flush in the CORNER by design -- that is what keeps "throw the pointer into the
        // screen corner and click" landing on close -- but nothing wants to sit flush against its
        // neighbour, and a leading-edge button style would otherwise begin where the tabs end.
        Layout.leftMargin: root.leadingSide ? root.outerInset : chromeStyle.windowControlGutter
        Layout.rightMargin: root.leadingSide ? chromeStyle.windowControlGutter : root.outerInset

        // A button presentation spans the bar -- its hover fill IS a full-height rectangle. Traffic
        // lights take the bounded band instead, centred, so the group's extent is exactly the union
        // of its controls' click targets in both axes.
        Layout.alignment: Qt.AlignVCenter
        Layout.fillHeight: !root.trafficLights
        Layout.preferredHeight: root.trafficLights ? root.trafficLightBand : -1
        spacing: 0

        onXChanged: root.sceneGeometryChanged()
        onYChanged: root.sceneGeometryChanged()

        HoverHandler {
            id: groupHover
        }

        Repeater {
            id: repeater

            model: windowControls.buttons

            // The three shapes are declared INSIDE the delegate rather than beside the Repeater: a
            // Component's bindings resolve ids lexically, so one declared at the file's top level
            // could not see this delegate's own `modelData`. Component is a factory, not an item, so
            // the two unused ones cost an object each and instantiate nothing.
            delegate: Loader {
                id: slot

                required property var modelData
                required property int index

                readonly property bool lastInGroup: slot.index === repeater.count - 1

                // Whether the OS drives this control (the maximise control, under Snap Layouts), and how.
                readonly property bool isMaximize: slot.modelData.action === "maximize"
                readonly property bool nativeHovered: slot.isMaximize && !!root.controller.nativeMaximizeHovered
                readonly property bool nativePressed: slot.isMaximize && !!root.controller.nativeMaximizePressed

                // Tells the controller where this control sits in the scene, in logical pixels, or that it
                // is gone. The slot is the owner token: on a change of side the new controls may report
                // before these are destroyed, and only what this slot reported may it clear.
                function reportRect() {
                    if (!slot.visible) {
                        root.controller.clearCaptionButtonRect(slot, slot.modelData.kind)
                        return
                    }
                    const rect = slot.mapToItem(null, 0, 0, slot.width, slot.height)
                    root.controller.setCaptionButtonRect(slot, slot.modelData.kind, rect.x, rect.y, rect.width, rect.height)
                }
                Component.onCompleted: slot.reportRect()
                Component.onDestruction: root.controller.clearCaptionButtonRect(slot, slot.modelData.kind)
                onXChanged: slot.reportRect()
                onYChanged: slot.reportRect()
                onWidthChanged: slot.reportRect()
                onHeightChanged: slot.reportRect()
                onVisibleChanged: slot.reportRect()
                Connections {
                    target: root
                    function onSceneGeometryChanged() { slot.reportRect() }
                }

                // Named after what it does rather than where it sits, so a test (or an accessibility
                // inspector) can find the close button without knowing which style put it where.
                objectName: "windowControl_" + modelData.action

                // The Loader is what the layout sees, so the layout attached properties belong here
                // and not on the items below; the Loader takes its implicit size from whichever it
                // loads.
                Layout.fillHeight: true

                sourceComponent: root.trafficLights
                               ? (chromeStyle.trafficLightGlyph !== "" ? trafficLightGlyph : trafficLightShape)
                               : glyphButton

                // The rectangular button every Windows-like style draws: a glyph on a hover fill that
                // spans the full height of the bar. `hoverCornerPercent` is what turns that fill into
                // Breeze's circle.
                Component {
                    id: glyphButton

                    ToolButton {
                        id: control

                        // Wider than a tab-strip control, and from the token table rather than a
                        // literal: these sit in the same bar as the tab strip, so in a one-row chrome
                        // a pinned 44px would be the one part that never joins the grid.
                        implicitWidth: chromeStyle.windowControlWidth

                        // Lit while hovered, here or (the maximise control) where the OS drives it; held while
                        // pressed, likewise.
                        readonly property bool lit: control.hovered || slot.nativeHovered
                        readonly property bool held: control.pressed || slot.nativePressed
                        // The glyphs are Unicode window-control symbols, not code, so they render fine
                        // in the default UI font. The chrome font is used rather than a pinned point
                        // size, and resolveChromeFont() is what decides whether that is the platform UI
                        // font or the terminal's — so no "monospace"/"Monospace" family is ever pinned
                        // HERE, which on some platforms (notably macOS) has no match and forces an
                        // expensive font-family-alias populate on startup.
                        font: root.glyphFontFor(slot.modelData)
                        // Window controls must not steal keyboard focus from the terminal.
                        focusPolicy: Qt.NoFocus

                        text: root.glyphFor(slot.modelData)
                        Accessible.name: root.accessibleNameFor(slot.modelData.action)
                        onClicked: root.activate(slot.modelData.action)

                        // Flat, transparent chrome so the control blends into the title bar instead of
                        // showing an opaque style button panel. A style that states a hover fill of its
                        // own gets it — that is the close button's red — and everything else gets the
                        // chrome's ordinary hover wash.
                        background: Item {
                            Rectangle {
                                objectName: "windowControlFill_" + slot.modelData.action
                                anchors.centerIn: parent

                                // How round the style wants its fill, as a fraction. At 0 the fill is
                                // the full-height rectangle a Windows title bar draws; at 1 it has to
                                // be a CIRCLE, which is what Breeze draws -- so the fill narrows to the
                                // button's shorter side as it rounds, rather than staying full width
                                // and turning into a capsule spanning the whole bar height.
                                readonly property real roundness: windowControls.hoverCornerPercent / 100

                                width: parent.width
                                     - (parent.width - Math.min(parent.width, parent.height)) * roundness
                                height: parent.height
                                radius: Math.min(width, height) / 2 * roundness

                                color: control.lit || control.held ? root.fillFor(slot.modelData) : "transparent"
                            }
                        }

                        // Explicit, theme-aware glyph rendering keeps the glyph legible on the
                        // transparent title bar in both light and dark modes. On a style's own hover
                        // fill it goes white instead: those fills are saturated enough that the
                        // palette's text color would disappear into them.
                        contentItem: Text {
                            objectName: "windowControlGlyph_" + slot.modelData.action
                            text: control.text
                            font: control.font
                            color: root.glyphColorFor(slot.modelData, control.lit, control.held)
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }

                // A traffic light drawn as the circle it is, for a chrome measured in pixels. The dot
                // sits at the leading edge rather than centred, so it keeps whatever grid the chrome
                // counts in; everything around it is the shared frame's.
                Component {
                    id: trafficLightShape

                    TrafficLight {
                        button: slot.modelData
                        lastInGroup: slot.lastInGroup

                        Rectangle {
                            id: dot
                            x: 0
                            anchors.verticalCenter: parent.verticalCenter
                            width: chromeStyle.trafficLightDotSize
                            height: width
                            radius: width / 2
                            // All three grey out together while the window is inactive: that is most of
                            // how a stack of macOS windows reads at a glance, and it is a window-level
                            // fact rather than a per-button one.
                            color: root.dotColorFor(slot.modelData)

                            // The glyph appears inside the dot only while the group is hovered, sized
                            // to fit within the dot rather than at the chrome font's own size.
                            Text {
                                anchors.centerIn: parent
                                visible: root.groupHovered
                                text: root.glyphFor(slot.modelData)
                                // Dark rather than palette-driven: the dot colors are fixed, light and
                                // saturated in every theme, so the glyph's contrast is against the dot
                                // and not against the window behind it.
                                color: chromeStyle.dotGlyphColor
                                font.pixelSize: Math.max(1, Math.round(dot.width * 0.7))
                            }
                        }
                    }
                }

                // The same traffic light drawn as one cell of text, for a chrome measured in character
                // cells. This is what keeps the macOS controls on the grid under `ui_style: terminal`:
                // that style draws every other affordance as a glyph, and a vector circle would be the
                // one part of the bar that did not line up with the terminal below it.
                Component {
                    id: trafficLightGlyph

                    TrafficLight {
                        button: slot.modelData
                        lastInGroup: slot.lastInGroup

                        Text {
                            objectName: "windowControlCell_" + slot.modelData.action
                            x: 0
                            width: chromeStyle.trafficLightDotSize
                            anchors.verticalCenter: parent.verticalCenter
                            font: chromeStyle.windowControlFont
                            // The dot at rest, the action's own glyph while the group is hovered — the
                            // same reveal the vector delegate does, in the vocabulary this style speaks.
                            text: root.groupHovered ? root.glyphFor(slot.modelData)
                                                    : chromeStyle.trafficLightGlyph
                            // Shrink a glyph that does not fit its cell rather than let it overflow
                            // onto the neighbouring light: the revealed glyphs are not in every
                            // monospace face, and a fallback face can hand back one wider than the cell
                            // the chrome measured. The vector delegate solves the same problem by
                            // sizing its glyph to the dot.
                            //
                            // BOTH minimums, because Qt consults whichever matches how the font ended
                            // up sized -- minimumPointSize for a point-sized font, minimumPixelSize for
                            // a pixel-sized one -- and each defaults to 12, which is large enough to be
                            // the binding constraint rather than the cell. The chrome font is
                            // point-sized today (resolveChromeFont), so stating only the pixel one left
                            // this guard doing nothing at all.
                            fontSizeMode: Text.HorizontalFit
                            minimumPointSize: 1
                            minimumPixelSize: 1
                            // The light keeps its own color through the reveal -- what changes is the
                            // SHAPE it is drawn as, never its identity. The vector delegate says the
                            // same thing by keeping the dot and putting the glyph inside it; here the
                            // glyph IS the light, so it takes the light's color. Taking the palette's
                            // text color instead dropped all three to one monochrome row for as long
                            // as the pointer was over the group, which read as the macOS controls
                            // turning into some other style's under the cursor.
                            color: root.dotColorFor(slot.modelData)
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
            }
        }
    }
}
