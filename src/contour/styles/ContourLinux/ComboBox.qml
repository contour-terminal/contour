// vim:syntax=qml
// ComboBox for the ContourLinux style: a button-like field with a trailing dropdown marker, opening a
// popover list of ItemDelegates. The settings page uses this for every enum. See ToolButton.qml for
// what a style file is.
import QtQuick
import QtQuick.Templates as T
import Contour.Ui

T.ComboBox {
    id: control

    // The widest entry of the model, as the style's font sets it: the field fits every option, not just the
    // one showing, so none is cut off.
    readonly property real modelWidth: {
        let widest = 0;
        for (let index = 0; index < control.count; ++index)
            widest = Math.max(widest, Math.ceil(modelMetrics.advanceWidth(control.textAt(index))));
        return widest;
    }

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            Math.max(implicitContentWidth, modelWidth) + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)

    leftPadding: chromeStyle.space.s4
    rightPadding: chromeStyle.space.s4 + (indicator ? indicator.width + control.spacing : 0)
    topPadding: 0
    bottomPadding: 0
    spacing: chromeStyle.space.s3
    font: chromeStyle.font

    FontMetrics {
        id: modelMetrics
        font: control.font
    }

    delegate: ItemDelegate {
        required property var model
        required property int index

        width: ListView.view.width
        text: model[control.textRole] !== undefined ? model[control.textRole] : modelData
        highlighted: control.highlightedIndex === index
        hoverEnabled: control.hoverEnabled
    }

    indicator: Text {
        x: control.mirrored ? chromeStyle.space.s4 : control.width - width - chromeStyle.space.s4
        y: control.topPadding + ((control.availableHeight - height) / 2)
        width: chromeStyle.size.headerIcon
        text: chromeStyle.menuGlyph
        font: control.font
        color: control.palette.buttonText
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
        color: control.palette.buttonText
        selectionColor: control.palette.highlight
        selectedTextColor: control.palette.highlightedText
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        implicitWidth: chromeStyle.minTabWidth
        implicitHeight: chromeStyle.size.control
        radius: chromeStyle.radii.control
        color: control.down ? chromeStyle.colors.controlPressedFill
             : control.hovered ? chromeStyle.colors.tabHoverFill
             : chromeStyle.colors.chipFill
        border.width: control.visualFocus ? chromeStyle.borderWidth : 0
        border.color: control.palette.highlight
    }

    popup: T.Popup {
        id: comboPopup
        // The widest option's row: its text, the row's padding at both ends, and the list's.
        // What a row pads its text with at one end: the delegate's own padding and inset.
        readonly property real rowSidePadding: chromeStyle.labelPadding + chromeStyle.space.s1
        readonly property real widestRowWidth: control.modelWidth + comboPopup.leftPadding + comboPopup.rightPadding
                                               + rowSidePadding + rowSidePadding
        y: control.height + chromeStyle.space.s1
        width: Math.max(control.width, comboPopup.widestRowWidth)
        // Spelled out rather than QQuickPopup's -1 ("do not push back inside the window"), which,
        // subtracted below, would add where it should reserve.
        topMargin: chromeStyle.space.s4
        bottomMargin: chromeStyle.space.s4
        height: Math.min(contentItem.implicitHeight + topPadding + bottomPadding,
                         // Window.height is 0 until the ComboBox is in a window; Infinity leaves the
                         // list's own height in charge until then.
                         control.Window.height > 0
                         ? control.Window.height - comboPopup.topMargin - comboPopup.bottomMargin
                         : Number.POSITIVE_INFINITY)
        padding: chromeStyle.space.s1

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.delegateModel
            currentIndex: control.highlightedIndex
            highlightMoveDuration: 0
            T.ScrollBar.vertical: ScrollBar {}
        }

        background: Rectangle {
            radius: chromeStyle.radii.popover
            // Opaque, for the reason Popup.qml states.
            color: control.palette.window
            border.width: chromeStyle.borderWidth
            border.color: control.palette.mid

            ElevationShadow {
                anchors.fill: parent
                z: -1
                level: chromeStyle.elevation.e3
                radius: parent.radius
            }
        }
    }
}
