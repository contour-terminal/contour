// vim:syntax=qml
// The in-pane lock pill: the lock where no pane header, tab well or always-shown band can hold it -- one tab
// of one pane under `tab_bar: multiple` on the Flat and Terminal rows (window::SecureIndicatorRules). It sits
// at the top right of the grid over blank cells, else at its bottom right, like the link pill, and yields to
// the link pill and the find bar (window::placeSecureLock, through chromeStyle.secureLockPlacement). The
// native rows draw the lock icon and the state's words in a Toast-filled pill; the Terminal row sets the
// `[secure]` cell run on the scheme's inverse, as the toast does.
//
// Presentational: the session says whether it is locked, SessionChrome says whether the rule puts the lock
// here and what else holds the places.
import QtQuick
import QtQuick.Controls

Control {
    id: pill
    objectName: "secureInputPill"

    /// The TerminalSession whose lock this is; null while a pane rebinds.
    property var session: null
    /// The grid area the placement is measured against: the pane's chrome.
    property Item gridItem: null
    /// Where the pill sits over the grid: an item laid exactly over gridItem, above the pane's dim.
    property Item gridHost: gridItem
    /// Whether the rule puts the lock in the pane (SessionChrome.lockInPane).
    property bool enabledByRule: false
    /// Which of the link pill's places the pane's find bar holds (a window::FindBarPresence).
    property int findBar: chromeStyle.findBarPresence("")
    /// Where the link pill is now, by name (window::linkPillAnchorName); `hidden` while it is not shown.
    property string linkPillAnchor: "hidden"

    // Guarded against `undefined`: a mock session may carry no indicator at all.
    readonly property bool locked: session !== null && session !== undefined
                                   && session.secureIndicator !== undefined
                                   && session.secureIndicator >= chromeStyle.secureInputs.locked
    /// What the pane shows (a window::SecureIndicator's value): its words claim secure input only if secured.
    readonly property int indicator: locked ? session.secureIndicator : chromeStyle.secureInputs.none
    readonly property bool bracketed: chromeStyle.chipOpenGlyph !== ""

    // Where the pill goes now; re-read as the clearance's revision moves: the page under it changed.
    readonly property var placement: {
        if (!pill.enabledByRule || !pill.locked || pill.gridItem === null)
            return { anchor: "hidden", x: 0, y: 0 }
        var revision = pill.session.overlayClearanceRevision
        var clearance = pill.session.overlayClearance !== undefined ? pill.session.overlayClearance() : {}
        var placed = chromeStyle.secureLockPlacement(pill.findBar, pill.linkPillAnchor, pill.gridItem.width,
                                                     pill.gridItem.height, pill.implicitWidth, pill.implicitHeight,
                                                     clearance)
        // An input the provider refuses answers an empty map: no place.
        return placed.anchor !== undefined ? placed : { anchor: "hidden", x: 0, y: 0 }
    }

    parent: pill.gridHost
    x: pill.placement.x
    y: pill.placement.y
    z: 20
    visible: pill.placement.anchor !== "hidden"

    // The Terminal row's cells bracket the lock themselves; the others pad it.
    leftPadding: bracketed ? 0 : chromeStyle.space.s5
    rightPadding: leftPadding
    implicitWidth: lockRow.implicitWidth + leftPadding + rightPadding
    implicitHeight: chromeStyle.size.linkPill

    Accessible.role: Accessible.Indicator
    Accessible.name: chromeStyle.secureInputText(pill.indicator)

    background: Rectangle {
        color: chromeStyle.colors.toastFill
        radius: chromeStyle.radii.linkPill
    }

    contentItem: Row {
        id: lockRow
        spacing: chromeStyle.space.s3

        SecureInputLock {
            id: lock
            anchors.verticalCenter: parent.verticalCenter
            color: chromeStyle.colors.toastText
            Accessible.ignored: true
        }

        // The words beside the icon; the Terminal row's cell text already says it.
        Text {
            objectName: "secureInputPillText"
            visible: !lock.drawsText
            anchors.verticalCenter: parent.verticalCenter
            text: chromeStyle.secureInputText(pill.indicator)
            font: chromeStyle.type.footnote
            color: chromeStyle.colors.toastText
            Accessible.ignored: true
        }
    }
}
