// vim:syntax=qml
// ComboBox for the ContourTui style: a bordered field with a one-cell dropdown marker, opening an
// opaque bordered list of ItemDelegates. The settings page uses this for every enum, so it and Switch
// are most of what that page looks like under this style.
//
// See ToolButton.qml for what a style file is.
import QtQuick
import QtQuick.Templates as T

T.ComboBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)

    leftPadding: chromeStyle.labelPadding
    rightPadding: chromeStyle.labelPadding + (indicator ? indicator.width : 0)
    topPadding: 0
    bottomPadding: 0
    spacing: chromeStyle.labelGap
    font: chromeStyle.font

    delegate: ItemDelegate {
        required property var model
        required property int index

        width: ListView.view.width
        text: model[control.textRole] !== undefined ? model[control.textRole] : modelData
        highlighted: control.highlightedIndex === index
        hoverEnabled: control.hoverEnabled
    }

    indicator: Text {
        x: control.mirrored ? control.leftPadding : control.width - width - control.rightPadding
                              + chromeStyle.labelPadding
        y: control.topPadding + ((control.availableHeight - height) / 2)
        width: chromeStyle.cellWidth
        text: chromeStyle.menuGlyph
        font: control.font
        color: control.palette.text
        opacity: control.enabled ? 1.0 : 0.5
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    contentItem: T.TextField {
        text: control.editable ? control.editText : control.displayText
        enabled: control.editable
        autoScroll: control.editable
        readOnly: control.down
        inputMethodHints: control.inputMethodHints
        validator: control.validator
        font: control.font
        color: control.palette.text
        selectionColor: control.palette.highlight
        selectedTextColor: control.palette.highlightedText
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        implicitWidth: chromeStyle.cellWidth * 16
        implicitHeight: chromeStyle.controlHeight
        radius: chromeStyle.radius
        color: control.palette.base
        border.width: chromeStyle.borderWidth
        border.color: control.visualFocus || control.down ? control.palette.highlight
                                                          : control.palette.mid
    }

    popup: T.Popup {
        id: comboPopup
        y: control.height
        width: control.width
        // Spelled out rather than left at QQuickPopup's default of -1, which means "do not push this
        // popup back inside the window" -- and, subtracted below, would ADD two pixels where the term
        // is there to reserve margin. One cell, so the gap to the window edge is on the grid too.
        topMargin: chromeStyle.cellHeight
        bottomMargin: chromeStyle.cellHeight
        // Ten rows is the same order of magnitude a TUI list shows before it starts scrolling, and the
        // ScrollBar style above draws the track when it does.
        height: Math.min(contentItem.implicitHeight + (2 * chromeStyle.borderWidth),
                         chromeStyle.cellHeight * 10,
                         // Window.height is 0 until the ComboBox has been parented into a window, and
                         // clamping to it then would collapse the popup to nothing on the binding's
                         // first evaluation. Infinity leaves the two real limits above in charge.
                         control.Window.height > 0
                         ? control.Window.height - comboPopup.topMargin - comboPopup.bottomMargin
                         : Number.POSITIVE_INFINITY)
        padding: chromeStyle.borderWidth

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.delegateModel
            currentIndex: control.highlightedIndex
            highlightMoveDuration: 0
            // The style's own ScrollBar rather than a ScrollIndicator: this style does not define a
            // ScrollIndicator, and an overlay hint is the wrong idea anyway -- a TUI list draws its
            // track as part of the frame.
            T.ScrollBar.vertical: ScrollBar {}
        }

        background: Rectangle {
            radius: chromeStyle.radius
            // Opaque: the application window is transparent so the terminal can show through, and a
            // translucent list would render over whatever is scrolling underneath.
            color: control.palette.window
            border.width: chromeStyle.borderWidth
            border.color: control.palette.mid
        }
    }
}
