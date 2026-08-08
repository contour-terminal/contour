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
    readonly property bool groupHovered: groupHover.hovered

    // Whether the window is maximized, so the maximize control can show its restore face. Read once
    // here rather than per delegate.
    readonly property bool windowMaximized: root.window.visibility === Window.Maximized

    // Flush. What separates two traffic lights is part of each control's own width (see the
    // delegates below), not space between them -- layout spacing is not clickable, and these are the
    // smallest targets in the chrome. Buttons want no separation at all, which is what a title bar
    // has always looked like.
    spacing: 0

    HoverHandler {
        id: groupHover
    }

    // Live OS palette so the glyphs adapt to dark/light themes (see TabContextMenu). The window is
    // transparent, so an explicit contentItem with a palette-driven color keeps every control glyph
    // legible in both light and dark modes regardless of the active style or hover state.
    SystemPalette {
        id: systemPalette
        colorGroup: SystemPalette.Active
    }

    // Runs what `action` names. A string rather than an index, so this reads as what it does and a
    // reordered style changes nothing here. Close is the one that does NOT go through the
    // controller: closing is the window's own business, and Main.qml's onClosing takes it from there.
    function activate(action) {
        if (action === "minimize")
            root.controller.minimizeWindow()
        else if (action === "maximize")
            root.controller.toggleMaximized()
        else if (action === "close")
            root.window.close()
    }

    // The translated name a screen reader announces. Kept in QML rather than in the provider so the
    // strings stay in the translation catalog; the provider supplies only the untranslatable
    // `action` key to look them up by.
    function accessibleNameFor(action) {
        if (action === "minimize")
            return qsTr("Minimize")
        if (action === "maximize")
            return root.windowMaximized ? qsTr("Restore") : qsTr("Maximize")
        if (action === "close")
            return qsTr("Close window")
        return ""
    }

    // The face a control shows right now: its restore glyph only where it has a distinct one.
    function glyphFor(button) {
        return root.windowMaximized ? button.restoreGlyph : button.glyph
    }

    // Inset from the bar's outer edge, so the controls do not sit flush against the window corner.
    // Only the spacer on the side facing outwards takes it: with the group at the leading edge that
    // is this one, with it at the trailing edge it is the one after the Repeater.
    Item {
        implicitWidth: windowControls.side === "leading" ? chromeStyle.windowControlInset : 0
    }

    Repeater {
        model: windowControls.buttons

        // The three shapes are declared INSIDE the delegate rather than beside the Repeater: a
        // Component's bindings resolve ids lexically, so one declared at the file's top level could
        // not see this delegate's own `button`. Component is a factory, not an item, so the two
        // unused ones cost an object each and instantiate nothing.
        delegate: Loader {
            id: slot

            required property var modelData

            // Named after what it does rather than where it sits, so a test (or an accessibility
            // inspector) can find the close button without knowing which style put it where.
            objectName: "windowControl_" + modelData.action

            // The Loader is what the layout sees, so the layout attached properties belong here and
            // not on the items below; the Loader takes its implicit size from whichever it loads.
            Layout.fillHeight: true

            sourceComponent: windowControls.presentation === "trafficLight"
                           ? (chromeStyle.trafficLightGlyph !== "" ? trafficLightGlyph : trafficLightShape)
                           : glyphButton

            // The rectangular button every Windows-like style draws: a glyph on a hover fill that
            // spans the full height of the bar. `hoverCornerPercent` is what turns that fill into
            // Breeze's circle.
            Component {
                id: glyphButton

                ToolButton {
                    id: control

                    // Wider than a tab-strip control, and from the token table rather than a literal:
                    // these sit in the same bar as the tab strip, so in a one-row chrome a pinned
                    // 44px would be the one part that never joins the grid.
                    implicitWidth: chromeStyle.windowControlWidth
                    // The glyphs are Unicode window-control symbols, not code, so they render fine in
                    // the default UI font. The chrome font is used rather than a pinned point size, and
                    // resolveChromeFont() is what decides whether that is the platform UI font or the
                    // terminal's — so no "monospace"/"Monospace" family is ever pinned HERE, which on
                    // some platforms (notably macOS) has no match and forces an expensive
                    // font-family-alias populate on startup.
                    font: chromeStyle.windowControlFont
                    // Window controls must not steal keyboard focus from the terminal.
                    focusPolicy: Qt.NoFocus

                    text: root.glyphFor(slot.modelData)
                    Accessible.name: root.accessibleNameFor(slot.modelData.action)
                    onClicked: root.activate(slot.modelData.action)

                    // Flat, transparent chrome so the control blends into the title bar instead of
                    // showing an opaque style button panel. A style that states a hover fill of its own
                    // gets it — that is the close button's red — and everything else gets the chrome's
                    // ordinary hover wash.
                    background: Rectangle {
                        color: !control.hovered ? "transparent"
                             : slot.modelData.hoverFill !== "" ? slot.modelData.hoverFill
                             : chromeStyle.wash(systemPalette.highlight)
                        radius: Math.min(width, height) / 2 * (windowControls.hoverCornerPercent / 100)
                    }

                    // Explicit, theme-aware glyph rendering keeps the glyph legible on the transparent
                    // title bar in both light and dark modes. On a style's own hover fill it goes white
                    // instead: those fills are saturated enough that the palette's text color would
                    // disappear into them.
                    contentItem: Text {
                        text: control.text
                        font: control.font
                        color: control.hovered && slot.modelData.hoverFill !== ""
                             ? "white" : systemPalette.windowText
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }

            // A traffic light drawn as the circle it is, for a chrome measured in pixels.
            Component {
                id: trafficLightShape

                Item {
                    // Dot plus gap: the gap belongs to the control so that it is clickable, and the
                    // dot sits at the leading edge rather than centred so that it keeps whatever
                    // grid the chrome counts in. @see UiStyleTokens::trafficLightGapUnits.
                    implicitWidth: chromeStyle.trafficLightDotSize + chromeStyle.trafficLightGap

                    Accessible.role: Accessible.Button
                    Accessible.name: root.accessibleNameFor(slot.modelData.action)
                    Accessible.onPressAction: root.activate(slot.modelData.action)

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
                        color: root.window.active ? slot.modelData.dotColor
                                                  : windowControls.inactiveDotColor

                        // The glyph appears inside the dot only while the group is hovered, sized to
                        // fit within the dot rather than at the chrome font's own size.
                        Text {
                            anchors.centerIn: parent
                            visible: root.groupHovered
                            text: root.glyphFor(slot.modelData)
                            // Dark rather than palette-driven: the dot colors are fixed, light and
                            // saturated in every theme, so the glyph's contrast is against the dot
                            // and not against the window behind it.
                            color: "#00000099"
                            font.pixelSize: Math.max(1, Math.round(dot.width * 0.7))
                        }
                    }

                    TapHandler {
                        onTapped: root.activate(slot.modelData.action)
                    }
                }
            }

            // The same traffic light drawn as one cell of text, for a chrome measured in character
            // cells. This is what keeps the macOS controls on the grid under `ui_style: terminal`:
            // that style draws every other affordance as a glyph, and a vector circle would be the
            // one part of the bar that did not line up with the terminal below it.
            Component {
                id: trafficLightGlyph

                Item {
                    // Dot plus gap, as above: two cells per control, so "● ● ●" spans six and each
                    // glyph still starts on a cell boundary.
                    implicitWidth: chromeStyle.trafficLightDotSize + chromeStyle.trafficLightGap

                    Accessible.role: Accessible.Button
                    Accessible.name: root.accessibleNameFor(slot.modelData.action)
                    Accessible.onPressAction: root.activate(slot.modelData.action)

                    Text {
                        x: 0
                        width: chromeStyle.trafficLightDotSize
                        anchors.verticalCenter: parent.verticalCenter
                        font: chromeStyle.windowControlFont
                        // The dot at rest, the action's own glyph while the group is hovered — the
                        // same reveal the vector delegate does, in the vocabulary this style speaks.
                        text: root.groupHovered ? root.glyphFor(slot.modelData)
                                                : chromeStyle.trafficLightGlyph
                        // Hovered, the glyph is a symbol and takes the palette's text color; at rest
                        // it IS the traffic light and takes the light's own.
                        color: root.groupHovered ? systemPalette.windowText
                             : root.window.active ? slot.modelData.dotColor
                             : windowControls.inactiveDotColor
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    TapHandler {
                        onTapped: root.activate(slot.modelData.action)
                    }
                }
            }
        }
    }

    Item {
        implicitWidth: windowControls.side === "trailing" ? chromeStyle.windowControlInset : 0
    }
}
