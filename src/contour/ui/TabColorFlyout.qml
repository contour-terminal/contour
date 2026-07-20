// vim:syntax=qml
// A popup with a grid of predefined color swatches for trivially colorizing a tab (Windows-Terminal
// style). The palette comes from the authoritative vtmux model via the controller, so the GUI and
// any future network client offer the same choices. One click on a swatch sets the tab color; a hex
// field below the swatches lets the user enter an arbitrary RGB color.
//
// The grid is fully keyboard-drivable, which is what the SetTabColor action opens it for: cursor keys
// and the vim motions h/j/k/l walk the swatches, Enter/Space picks the current one, Escape dismisses,
// and Tab moves on to the hex field and the reset button.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    required property var controller
    required property int tabIndex

    // The tab's color right now, so opening the flyout lands the keyboard cursor on the swatch the tab
    // already wears rather than always at the top-left one. Transparent (an uncolored tab) matches no
    // swatch, which is exactly the -1 that onOpened falls back from.
    property color currentColor: "transparent"

    modal: true
    focus: true
    padding: 8

    // Keep the whole picker inside the window, wherever the tab it hangs off happens to be. A Popup is
    // positioned against its PARENT (this tab), and Qt's default `margins: -1` means it is never pushed
    // back inside the window — so a fixed "below the tab" puts the entire picker off-window when the tab
    // strip sits at the bottom (`tab_bar_position: Bottom`), where the tab's bottom edge IS the window's
    // bottom edge. Being modal, it would then be invisible AND still swallow every click, leaving the user
    // unable to color the tab at all. Two lines, one per axis:
    //
    //   - `margins: 0` lets Qt shift the popup inwards on any axis it overflows on (a narrow window would
    //     otherwise clip it off the right edge), and is the backstop if the placement below is ever wrong;
    //   - placeAgainstParent() picks the vertical side, so a bottom strip opens the picker ABOVE its tab
    //     rather than leaving Qt to shove it up over the tab it belongs to.
    margins: 0

    // Placed per open, not as a binding on y: the tab's position in the window is reached through
    // mapToItem(), which is a snapshot rather than something QML re-evaluates the binding on — and at
    // construction time the tab has not been laid out yet, so a binding would answer for a tab still at
    // 0,0 and never flip.
    onAboutToShow: root.placeAgainstParent()

    // Below the parent tab when the window has room for the whole flyout there, above it otherwise.
    function placeAgainstParent() {
        const overlay = root.Overlay.overlay; // the window, in the coordinates a Popup is placed in
        if (!root.parent || !overlay)
            return;
        const tabBottom = root.parent.mapToItem(overlay, 0, root.parent.height).y;
        const fitsBelow = tabBottom + root.implicitHeight <= overlay.height;
        root.y = fitsBelow ? root.parent.height : -root.implicitHeight;
    }

    // The palette, read once here rather than per binding. Null-guarded like TabStrip's currentIndex:
    // the controller dies before this QML tree on window close, and this re-evaluates during teardown.
    // NOT named `palette` — that is a Popup property already (and one this file sets below).
    readonly property var swatchColors: root.controller ? root.controller.tabColorPalette() : []

    // Live OS palette handle so the flyout frame adapts to dark/light in realtime (see TabContextMenu).
    SystemPalette {
        id: systemPalette
        colorGroup: SystemPalette.Active
    }

    // Inherited by the "Reset Color" Button so its label stays readable in both modes.
    palette {
        button: systemPalette.button
        buttonText: systemPalette.buttonText
        window: systemPalette.window
        windowText: systemPalette.windowText
    }

    background: Rectangle {
        color: systemPalette.window
        border.color: systemPalette.mid
        border.width: 1
        radius: 4
    }

    contentItem: Column {
        spacing: 8

        // A GridView rather than a Grid+Repeater: it already implements cursor-key navigation over a
        // 2D cell layout (and the moveCurrentIndex*() primitives the vim motions below reuse), so the
        // keyboard half of this picker is Qt's tested code rather than our own index arithmetic.
        GridView {
            id: grid
            objectName: "tabColorGrid"

            // Swatch geometry, named once: the cell, the delegate and the grid's own size derive from
            // these, so the layout stays coherent if a swatch ever changes size.
            readonly property int swatchSize: 30
            readonly property int swatchSpacing: 8
            readonly property int swatchColumns: 8

            model: root.swatchColors
            cellWidth: swatchSize + swatchSpacing
            cellHeight: cellWidth
            width: swatchColumns * cellWidth
            height: Math.ceil(root.swatchColors.length / swatchColumns) * cellHeight

            // The flyout is small enough to show every swatch, so there is nothing to flick. Key
            // navigation is off as well: the cursor keys are walked by the motion table below, together
            // with the vim motions, rather than by GridView itself — see there for why.
            interactive: false
            keyNavigationEnabled: false

            activeFocusOnTab: true // Shift+Tab comes back here from the hex field below

            // Where the keyboard is: ONE ring, which the view moves onto the current cell and sizes to it,
            // so it frames the (smaller, centered) swatch. Hidden once Tab hands the focus to the hex
            // field — it marks where the keys are, not a selection that outlives them.
            highlight: Rectangle {
                radius: 9
                color: "transparent"
                border.width: 2
                border.color: systemPalette.highlight
                visible: grid.activeFocus
            }
            highlightMoveDuration: 0

            // Every key that walks the swatch grid, as one step in (column, row). The cursor keys sit in
            // this table rather than being left to GridView because GridView's own moveCurrentIndexLeft /
            // moveCurrentIndexRight step ±1 through the FLAT model index: `h` (or Left) on the first swatch
            // of a row lands on the LAST swatch of the row ABOVE, and the Enter that follows then applies a
            // color from a row the user never navigated to. A step in (column, row), clamped, stays put at
            // the edges — the same thing GridView's own up/down already do.
            readonly property var motions: ({
                [Qt.Key_H]: { columns: -1, rows: 0 },
                [Qt.Key_Left]: { columns: -1, rows: 0 },
                [Qt.Key_L]: { columns: 1, rows: 0 },
                [Qt.Key_Right]: { columns: 1, rows: 0 },
                [Qt.Key_K]: { columns: 0, rows: -1 },
                [Qt.Key_Up]: { columns: 0, rows: -1 },
                [Qt.Key_J]: { columns: 0, rows: 1 },
                [Qt.Key_Down]: { columns: 0, rows: 1 }
            })

            // A chord is not a motion: Qt reports the same event.key for Ctrl+L as for a bare `l`, so
            // without this mask a reflexive Ctrl+L (clear screen) or Shift+J would move the keyboard cursor
            // — and be swallowed — leaving the next Enter to apply a swatch the user never aimed at.
            // KeypadModifier is deliberately absent from the mask: the keypad's arrows are arrows.
            readonly property int chordModifiers: Qt.ShiftModifier | Qt.ControlModifier
                                                | Qt.AltModifier | Qt.MetaModifier

            Keys.onPressed: (event) => {
                if (event.modifiers & grid.chordModifiers)
                    return
                const motion = grid.motions[event.key]
                if (!motion)
                    return
                grid.moveCursorBy(motion)
                event.accepted = true
            }

            // Moves the keyboard cursor by one step, clamped on both axes: a step past an edge stays where
            // it is rather than wrapping into a neighbouring row, and the final row may be short — so a step
            // down from a full row would otherwise land past the end of the palette.
            function moveCursorBy(motion) {
                const count = root.swatchColors.length
                if (count === 0)
                    return
                const rowCount = Math.ceil(count / grid.swatchColumns)
                const column = grid.currentIndex % grid.swatchColumns
                const row = Math.floor(grid.currentIndex / grid.swatchColumns)
                const targetColumn = Math.max(0, Math.min(grid.swatchColumns - 1, column + motion.columns))
                const targetRow = Math.max(0, Math.min(rowCount - 1, row + motion.rows))
                grid.currentIndex = Math.min(count - 1, (targetRow * grid.swatchColumns) + targetColumn)
            }

            Keys.onReturnPressed: root.applySwatch(grid.currentIndex)
            Keys.onEnterPressed: root.applySwatch(grid.currentIndex) // the keypad's Enter
            Keys.onSpacePressed: root.applySwatch(grid.currentIndex)

            delegate: Item {
                id: cell
                required property var modelData
                required property int index
                width: grid.cellWidth
                height: grid.cellHeight

                Rectangle {
                    objectName: "tabColorSwatch"
                    anchors.centerIn: parent
                    width: grid.swatchSize
                    height: grid.swatchSize
                    radius: 5
                    color: cell.modelData
                    border.width: 1
                    border.color: Qt.rgba(0, 0, 0, 0.4)

                    HoverHandler { id: swatchHover }
                    scale: swatchHover.hovered ? 1.15 : 1.0
                    Behavior on scale { NumberAnimation { duration: 80 } }

                    TapHandler {
                        onTapped: root.applySwatch(cell.index)
                    }
                }
            }
        }

        // Arbitrary-color entry. Deliberately dependency-free (no QtQuick.Dialogs): a hard import of that
        // module is not installed in every runtime, and its absence would cascade up and break the whole
        // main.qml. Instead the user types a 6-digit hex RGB value; the preview updates live and Enter
        // (or clicking the preview) applies it. The "#rrggbb" string is coerced to a QColor by the same
        // controller.setTabColor() the preset swatches use, so the backend needs no new API.
        RowLayout {
            width: grid.width
            spacing: 8

            Label {
                text: "#"
                Accessible.name: qsTr("Custom color, as hex")
                Layout.alignment: Qt.AlignVCenter
            }

            TextField {
                id: hexField
                objectName: "customColorField"
                Layout.fillWidth: true
                placeholderText: qsTr("RRGGBB")
                maximumLength: 6
                validator: RegularExpressionValidator { regularExpression: /[0-9A-Fa-f]{6}/ }
                onAccepted: root.applyCustomColor()
            }

            // Live preview of the typed color; click it to apply.
            Rectangle {
                Layout.preferredWidth: 30
                Layout.preferredHeight: 30
                radius: 5
                border.width: 1
                border.color: Qt.rgba(0, 0, 0, 0.4)
                color: hexField.acceptableInput ? ("#" + hexField.text) : "transparent"
                TapHandler {
                    enabled: hexField.acceptableInput
                    onTapped: root.applyCustomColor()
                }
            }
        }

        Button {
            objectName: "tabColorResetButton"
            width: grid.width
            text: qsTr("Reset Color")
            onClicked: root.resetColor()
        }
    }

    // Arm the picker on every open, not once at construction — which is what makes it re-usable: the
    // previous visit left the cursor wherever it stopped, and the hex field holding whatever was typed
    // into it. BOTH are reset here. Leaving the hex text behind would carry a color the user typed and
    // then abandoned with Escape into the next visit, where its live preview swatch is one click (or one
    // Tab+Enter) away from being applied to the tab.
    //
    // The cursor lands on the tab's own color (Qt.colorEqual, not ===: these are color values, and JS
    // identity on them compares nothing useful), or on the first swatch when the tab wears none —
    // findIndex's -1 for "no such swatch" is what Math.max floors to 0.
    onOpened: {
        hexField.clear();
        grid.currentIndex =
            Math.max(0, root.swatchColors.findIndex(c => Qt.colorEqual(c, root.currentColor)));
        grid.forceActiveFocus();
    }

    // The one way a color leaves this flyout: a clicked swatch, Enter on the keyboard cursor, and the hex
    // field all funnel through here, so no entry point can forget the guard or the close. Null-guarded
    // because the controller is torn down before this popup on window close.
    function applyColor(color) {
        if (!root.controller)
            return;
        root.controller.setTabColor(root.tabIndex, color);
        root.close();
    }

    function applySwatch(index) {
        if (index >= 0 && index < root.swatchColors.length)
            root.applyColor(root.swatchColors[index]);
    }

    function applyCustomColor() {
        if (hexField.acceptableInput) // i.e. a complete, valid RGB value
            root.applyColor("#" + hexField.text);
    }

    // Drops the user's color, so any color the application gave the tab (DECAC) resurfaces.
    function resetColor() {
        if (!root.controller)
            return;
        root.controller.resetTabColor(root.tabIndex);
        root.close();
    }
}
