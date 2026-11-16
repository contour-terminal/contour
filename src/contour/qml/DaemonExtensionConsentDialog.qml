// vim:syntax=qml
// The window's dialog for a daemon-scoped extension's grant (window::DaemonExtensionConsentModel): the
// question the daemon rendered -- its title, then its lines as plain text -- with Deny and Allow. It belongs to
// the window, not to a pane: the question is the daemon's.
//
// Return is Deny, never Allow. Allow stays disabled in this window while it is not active, and for a moment
// each time a question is shown or the window becomes active (model.isAllowHeldFor), so a keystroke or click
// aimed at what was there -- or the click that activated the window -- cannot grant it. Escape and the close
// control dismiss: nothing is answered, and the daemon asks again. A question sent again while shown changes nothing here and
// takes no focus. Every way it closes hands the focus back to `focusTarget`.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

KitDialog {
    id: root
    objectName: "daemonExtensionConsentDialog"

    /// The window's window::DaemonExtensionConsentModel; null for a window of no daemon.
    property var model: null
    /// Where the focus goes once the dialog closes.
    property var focusTarget: null

    // This window's own hold (DaemonExtensionConsentModel::attachViewer): Allow is armed here only while this
    // window is active, and a moment after it became so.
    property int _viewer: -1
    property var _viewedModel: null
    readonly property bool _windowActive: root._window !== null && root._window.active

    function _attach() {
        if (root._viewedModel !== null && root._viewer >= 0)
            root._viewedModel.detachViewer(root._viewer);
        root._viewedModel = root.model;
        root._viewer = root.model !== null ? root.model.attachViewer() : -1;
        if (root.model !== null)
            root.model.setViewerActive(root._viewer, root._windowActive);
    }

    readonly property var _lines: root.model !== null ? root.model.lines : []
    // The most the lines may take: half of what the dialog hangs in.
    readonly property real _linesRoom: root.parent ? root.parent.height / 2 : linesColumn.implicitHeight

    heading: root.model !== null ? root.model.title : ""
    message: ""
    description: root._lines.join("\n")
    acceptText: qsTr("Allow")
    rejectText: qsTr("Deny")
    acceptEnabled: root.model !== null && root.model.revision >= 0 && !root.model.isAllowHeldFor(root._viewer)
    defaultButton: KitDialog.Reject
    // Escape and the close control are no answer: they are handled below, as a dismissal.
    closePolicy: Popup.NoAutoClose
    closable: true

    onAccepted: if (root.model !== null) root.model.allowFrom(root._viewer)
    onRejected: if (root.model !== null) root.model.deny()
    onEscaped: if (root.model !== null) root.model.dismiss()
    onClosed: if (root.focusTarget !== null && root.focusTarget !== undefined) root.focusTarget.forceActiveFocus()
    onModelChanged: {
        root._attach();
        root._follow();
    }
    on_WindowActiveChanged: if (root.model !== null) root.model.setViewerActive(root._viewer, root._windowActive)
    Component.onCompleted: root._attach()
    Component.onDestruction: if (root._viewedModel !== null && root._viewer >= 0) root._viewedModel.detachViewer(root._viewer)

    /// Opens the dialog while the model shows a question, closes it once it shows none.
    function _follow() {
        if (root.model !== null && root.model.isVisible) {
            if (!root.visible)
                root.open();
        } else if (root.visible) {
            root.close();
        }
    }

    Connections {
        target: root.model
        ignoreUnknownSignals: true
        function onChanged() { root._follow() }
        // A question put up: opened, and so focused. A re-sent one never asks.
        function onFocusRequested() { root._follow() }
    }

    bodyData: [
        // The detail scrolls once it is taller than half the window, so both buttons always stay in view.
        ScrollView {
            id: consentLines
            objectName: "daemonExtensionConsentLines"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(linesColumn.implicitHeight, root._linesRoom)
            clip: true
            contentWidth: availableWidth
            contentHeight: linesColumn.implicitHeight
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                id: linesColumn
                // Clear of the scroll bar where the style lays it over the content.
                width: consentLines.availableWidth
                       - (consentLines.ScrollBar.vertical.visible ? consentLines.ScrollBar.vertical.width : 0)
                spacing: chromeStyle.space.s3

                Repeater {
                    model: root._lines

                    Label {
                        required property var modelData
                        objectName: "daemonExtensionConsentLine"
                        Layout.fillWidth: true
                        text: modelData
                        // The daemon's rendering of an extension's own text: text, never markup.
                        textFormat: Text.PlainText
                        font: chromeStyle.type.footnote
                        color: sys.windowText
                        // Between words where it can, inside a path where it must.
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    }
                }
            }
        }
    ]

    SystemPalette {
        id: sys
        colorGroup: SystemPalette.Active
    }
}
