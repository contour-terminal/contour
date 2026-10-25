// vim:syntax=qml
// An ApplicationWindow themed from the application palette. Every top-level window of the app is one: the
// terminal windows (Main.qml) and the Settings window (SettingsWindow.qml).
import QtQuick
import QtQuick.Controls

ApplicationWindow
{
    // Force the window's Qt Quick Controls palette to follow the application palette (which
    // ContourGuiApp::applyGuiTheme drives for the theme: dark|light|system setting).
    //
    // Why this is needed: the pinned Fusion Quick Controls style derives an editable control's colors
    // (TextField/ComboBox/SpinBox `base`, `text`, …) from QStyleHints::colorScheme(), NOT from the
    // application QPalette. On desktops whose platform theme owns the color scheme (KDE Plasma, GNOME),
    // QStyleHints::setColorScheme() is inert, so those controls render in the OS scheme (e.g. dark input
    // fields) even when the forced GUI theme — and every SystemPalette-bound chrome element — is light.
    // Binding the window palette to a SystemPalette (which DOES follow the application palette) makes the
    // controls inherit an explicit palette that wins over the style default, so the whole window is themed
    // consistently from first render. In `theme: system` this simply mirrors the OS palette, i.e. a no-op.
    SystemPalette
    {
        id: appPalette
        colorGroup: SystemPalette.Active
    }
    palette.window: appPalette.window
    palette.windowText: appPalette.windowText
    palette.base: appPalette.base
    palette.alternateBase: appPalette.alternateBase
    palette.text: appPalette.text
    palette.button: appPalette.button
    palette.buttonText: appPalette.buttonText
    palette.brightText: appPalette.brightText
    palette.highlight: appPalette.highlight
    palette.highlightedText: appPalette.highlightedText
    palette.toolTipBase: appPalette.toolTipBase
    palette.toolTipText: appPalette.toolTipText
    palette.placeholderText: appPalette.placeholderText
    palette.light: appPalette.light
    palette.midlight: appPalette.midlight
    palette.mid: appPalette.mid
    palette.dark: appPalette.dark
    palette.shadow: appPalette.shadow
}
