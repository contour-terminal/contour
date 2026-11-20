// vim:syntax=qml
// A tab's icon (window::tabIconOf, as a { kind, text } map): a tinted square with the running program's
// initial in it, or the profile's image. The tab strip and the sidebar's tab rows draw the same mark; each
// decides where it stands, how big it is, and what colour fills it.
import QtQuick

Rectangle {
    id: mark
    objectName: "tabIcon"   // findChild() handle for the GUI test

    /// The icon: { kind, text }, kind one of chromeStyle.tabIconKinds; empty is the row's own (`row`).
    property var info: ({})
    /// The face the initial is set in.
    property string fontFamily: ""
    /// How much of the square's height the initial takes.
    readonly property real initialShare: 0.7

    readonly property int kind: mark.info.kind ?? chromeStyle.tabIconKinds.row
    readonly property bool isHidden: mark.kind === chromeStyle.tabIconKinds.hidden
    readonly property bool isImage: mark.kind === chromeStyle.tabIconKinds.image
    readonly property bool isMonogram: mark.kind === chromeStyle.tabIconKinds.monogram

    color: mark.isImage ? "transparent" : chromeStyle.accentColor

    Text {
        objectName: "tabIconInitial"   // findChild() handle for the GUI test
        anchors.centerIn: parent
        visible: mark.isMonogram
        text: visible ? mark.info.text : ""
        font.family: mark.fontFamily
        font.pixelSize: Math.round(parent.height * mark.initialShare)
        font.bold: true
        // Black or white, whichever reads on the square.
        color: chromeStyle.onColor(parent.color)
        Accessible.ignored: true
    }

    Image {
        objectName: "tabIconImage"   // findChild() handle for the GUI test
        anchors.fill: parent
        visible: mark.isImage
        source: mark.isImage ? mark.info.text : ""
        sourceSize: Qt.size(width * Screen.devicePixelRatio, height * Screen.devicePixelRatio)
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        smooth: true
        Accessible.ignored: true
    }
}
