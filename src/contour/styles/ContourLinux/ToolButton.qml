// vim:syntax=qml
// ToolButton for the ContourLinux Qt Quick Controls style.
//
// Every file in this directory is a Qt Quick Controls style implementation: a QtQuick.Templates
// control (the behaviour, with no appearance of its own) given a contentItem and a background, built
// the way ContourTui's are. The style is selected app-wide by QQuickStyle::setStyle("ContourLinux") in
// ContourGuiApp when the chrome resolves to the ContourLinux row, which is why these files never test
// for the style themselves.
//
// The metrics come from the `chromeStyle` context property (UiStyleProvider): the kit's ContourLinux
// column -- 34-pixel controls with 8-pixel corners, 10-pixel menus, 6-pixel menu rows, 12-pixel
// popovers. Colours come from the control's palette and the kit's fills, so the OS light/dark theme and
// the portal's accent (the palette's highlight) drive them. Text is the system UI font.
//
// A header-bar button names a freedesktop icon (`icon.name`) with a bundled fallback (`icon.source`),
// tinted with `icon.color`; IconLabel resolves the name through the icon theme.
import QtQuick
import QtQuick.Controls.impl
import QtQuick.Templates as T

T.ToolButton {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    padding: chromeStyle.space.s2
    spacing: chromeStyle.space.s3
    font: chromeStyle.font

    icon.width: chromeStyle.size.headerIcon
    icon.height: chromeStyle.size.headerIcon
    icon.color: control.palette.buttonText

    contentItem: IconLabel {
        spacing: control.spacing
        mirrored: control.mirrored
        display: control.display
        icon: control.icon
        text: control.text
        font: control.font
        color: control.palette.buttonText
        opacity: control.enabled ? 1.0 : 0.5
    }

    background: Rectangle {
        implicitWidth: chromeStyle.size.toolbarButton
        implicitHeight: chromeStyle.size.control
        radius: chromeStyle.radii.control
        // Flat at rest, as a header bar's buttons are; the kit's fills on hover and press, and the
        // pressed fill for a checked toggle too.
        color: control.down || control.checked ? chromeStyle.colors.controlPressedFill
             : control.hovered ? chromeStyle.colors.tabHoverFill
             : "transparent"
        border.width: control.visualFocus ? chromeStyle.borderWidth : 0
        border.color: control.palette.highlight
    }
}
