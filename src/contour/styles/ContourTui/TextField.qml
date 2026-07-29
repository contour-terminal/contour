// vim:syntax=qml
// TextField for the ContourTui style: a bordered single-row field in the chrome font, so typed text
// lands on the same grid as the terminal content. See ToolButton.qml for what a style file is.
import QtQuick
import QtQuick.Templates as T

T.TextField {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            Math.max(contentWidth, placeholder.implicitWidth) + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding,
                             placeholder.implicitHeight + topPadding + bottomPadding)

    // Spelled out rather than using the `horizontalPadding` grouping the other style files use:
    // T.TextField derives from TextInput, not Control, so that grouped property does not exist here.
    leftPadding: chromeStyle.labelPadding
    rightPadding: chromeStyle.labelPadding
    topPadding: 0
    bottomPadding: 0

    font: chromeStyle.font
    color: palette.text
    selectionColor: palette.highlight
    selectedTextColor: palette.highlightedText
    placeholderTextColor: Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.5)
    verticalAlignment: TextInput.AlignVCenter

    // The placeholder hint. T.TextField carries the `placeholderText` property but draws nothing for
    // it -- rendering it is the style's job, and a style that only sets `placeholderTextColor` hands
    // the user an unlabeled empty box: the command palette with no "Type to search commands…", a hex
    // color field with no "#RRGGBB". A plain Text rather than Controls' own PlaceholderText, so this
    // style keeps its two-import surface; the visibility rule below is the one that type applies.
    Text {
        id: placeholder
        x: control.leftPadding
        y: control.topPadding
        width: control.width - (control.leftPadding + control.rightPadding)
        height: control.height - (control.topPadding + control.bottomPadding)

        text: control.placeholderText
        font: control.font
        color: control.placeholderTextColor
        verticalAlignment: control.verticalAlignment
        // Hidden as soon as there is anything real to show, including uncommitted input-method text.
        // The centered-alignment clause matches the built-in styles: a centered field keeps its hint
        // while focused so the caret does not appear to sit in the middle of nowhere.
        visible: !control.length && !control.preeditText
                 && (!control.activeFocus || control.horizontalAlignment !== Qt.AlignHCenter)
        elide: Text.ElideRight
        renderType: control.renderType
    }

    background: Rectangle {
        implicitWidth: chromeStyle.cellWidth * 16
        implicitHeight: chromeStyle.controlHeight
        radius: chromeStyle.radius
        color: control.palette.base
        border.width: chromeStyle.borderWidth
        // A focused field is the one being typed into; a highlight border is the terminal equivalent
        // of a focus ring, without adding a glow the rest of the style has nowhere else.
        border.color: control.activeFocus ? control.palette.highlight : control.palette.mid
    }
}
