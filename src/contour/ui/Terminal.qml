// vim:syntax=qml
import Contour.Terminal
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import Qt5Compat.GraphicalEffects

ContourTerminal
{
    id: vtWidget
    visible: true

    session: terminalSessions.createSession()

    // Background layers sit *behind* the terminal grid. The terminal now renders through a scene-graph
    // node (TerminalRenderNode) as this item's own content, and a QQuickItem's children paint on top of
    // its node content by default — so without a negative z these opaque background fills would cover the
    // grid (a black terminal). z:-1 places them below the node content, matching TerminalPane.qml.
    Rectangle {
        id : backgroundColor
        z: -1
        anchors.centerIn: parent
        width:  vtWidget.width
        height:  vtWidget.height
        opacity : session.isImageBackground
            ? session.isBlurBackground ? 1.0 - session.opacityBackground : 1.0
            : 1.0
        color: session.backgroundColor
        visible : true
        focus : false
    }

    Image {
        id: backgroundImage
        z: -1
        width:  vtWidget.width
        height:  vtWidget.height
        opacity : session.opacityBackground
        focus: false
        visible : session.isImageBackground
        source :  session.pathToBackground
    }


    FastBlur {
        z: -1
        visible: session.isBlurBackground
        anchors.fill: backgroundImage
        source: backgroundImage
        radius: 32
    }


    Rectangle {
        z: -1
        anchors.centerIn: parent
        width:  vtWidget.width
        height:  vtWidget.height
        color: session.backgroundColor
        opacity : session.isImageBackground
                ? session.isBlurBackground ? 1.0 - session.opacityBackground : 0.0
                : 0.0
        visible : true
        focus : false
    }


    Rectangle {
        Timer {
            id: sizeWidgetTimer
            interval: 1000;
            running: false;
            onTriggered: sizeWidget.visible = false
        }
        id : sizeWidget
        anchors.centerIn: parent
        border.width: 1
        border.color: "black"
        property int margin: 10
        color: "white"
        visible : false
        focus : false
        Text {
            id : sizeWidgetText
            anchors.centerIn: parent
            font.pointSize: vtWidget.fontSize
            text :  "Size: " + session.pageColumnsCount.toString() + " x " + session.pageLineCount.toString()
        }
    }

    // Shared per-session chrome (scrollbar, bell, permission dialogs, notification/alert wiring),
    // shared with the split-pane view (TerminalPane.qml).
    SessionChrome {
        id: chrome
        session: vtWidget.session
        displayItem: vtWidget
    }

    function updateSizeWidget() {
        if (vtWidget.session.upTime > 1.0 && vtWidget.session.showResizeIndicator)
        {
            sizeWidget.visible = true
            sizeWidgetTimer.running = true
            sizeWidgetText.text = "Size: " + vtWidget.session.pageColumnsCount.toString() + " x " + vtWidget.session.pageLineCount.toString()
            sizeWidget.width = sizeWidgetText.contentWidth + sizeWidget.margin
            sizeWidget.height = sizeWidgetText.contentHeight
        }
    }

    onTerminated: {
        console.log("Client process terminated. Closing the window.");
        if (terminalSessions.canCloseWindow())
            Window.window.close(); // https://stackoverflow.com/a/53829662/386670
    }

    function updateFontSize() {
        sizeWidgetText.font.pointSize = vtWidget.session.fontSize
    }

    function onCreateNewTab() {
        terminalSessions.createNewTab();
    }

    function delay(duration) { // In milliseconds
        var timeStart = new Date().getTime();
        while (new Date().getTime() - timeStart < duration) {
            // Do nothing
        }
    }


    onSessionChanged: (s) => {
        let vt = vtWidget.session;

        // Link opacityChanged signal (single-pane / window-level only, not shared with SessionChrome).
        vt.onOpacityChanged.connect(vtWidget.opacityChanged);

        // Update font size of elements
        vt.fontSizeChanged.connect(updateFontSize);
        updateFontSize();

        // Show cell-dimensions popup in case of page size changes
        vt.lineCountChanged.connect(updateSizeWidget);
        vt.columnsCountChanged.connect(updateSizeWidget);

        // Shared per-session wiring (bell, alert, notifications, scrollbar, permission dialogs).
        chrome.wireSession(vt);

        forceActiveFocus();
    }
}
