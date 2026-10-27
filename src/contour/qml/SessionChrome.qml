// vim:syntax=qml
// Shared per-session chrome for a ContourTerminal display.
//
// Both the single-pane view (Terminal.qml) and each split-tree leaf (TerminalPane.qml) need the same
// per-session widgets and wiring: the vertical scrollbar (bidirectionally bound to the VT viewport),
// the deferred bell sound, the four permission-wall dialogs, the window alert, and the relay of the
// session's own notices. This component owns all of that so neither host has to hand-maintain a second
// copy — previously TerminalPane.qml duplicated ~120 lines of Terminal.qml's chrome, and the two
// drifted (e.g. the pane hardcoded a right-side scrollbar and dropped stepSize).
//
// The host embeds this as a child and forwards its own session pointer:
//
//     SessionChrome { id: chrome; session: <root>.session; displayItem: <root> }
//
// The per-session wiring is declarative (the Connections element below): it re-targets — and
// auto-disconnects the previous session — on every `session` rebind, so a pane switched between
// sessions never accumulates stale cross-session connections (an imperative wireSession() that only
// ever connect()ed made a background tab's permission dialog answer on the WRONG session and played
// one bell N times after N tab switches). The host keeps whatever extra, non-shared wiring it has
// (TerminalPane.qml's focus border).
//
// Every session access is null-guarded: a split pane's session can be rebound to null during teardown,
// and the single-pane session is never null, so the guards are safe for both.
// SessionChrome is a plain Item (not a ContourTerminal), so it needs no Contour.Terminal import; its
// sibling components (RequestPermission, DeferredBell) resolve via the implicit same-directory import.
import QtQuick
import QtQuick.Controls
import QtQuick.Window

Item {
    id: chrome
    anchors.fill: parent

    // The TerminalSession this chrome is bound to (forwarded by the host; may be null transiently).
    required property var session

    // The enclosing ContourTerminal display, used to relay showNotification and to grab keyboard focus.
    property var displayItem: parent

    // The pane header's find slot and whether the pane has an inline header (PaneCard, through
    // TerminalPane): where the Terminal row's find bar goes. None outside a card.
    property Item headerFindSlot: null
    property int headerPresent: 0
    // A bool by the QML-facing carve-out: it relays SearchBar.occupiesHeaderRow, a predicate derived from
    // the placement's named anchor, up to TerminalPane; it decides nothing itself.
    readonly property bool findInHeaderRow: searchBar.occupiesHeaderRow

    // Whether the scrollbar is moving to follow the session's viewport (@see updateScrollBarPosition): a move
    // of the session's own, which onScrollBarPositionChanged does not write back.
    property bool followingSession: false

    // The scrollbar's tick marks (`scrollbar.marks`): one { position, kind, targetLine } per tick, top
    // first, and one colour per kind. Guarded against `undefined`, not merely against a null session, for
    // the reason hyperlinkTip gives: other tests bind a mock session that carries no marks at all. Each
    // list is read from the session once: every read converts the whole of it to JavaScript.
    readonly property var scrollbarMarks: {
        let marks = chrome.session ? chrome.session.scrollbarMarks : undefined;
        return marks !== undefined ? marks : [];
    }
    readonly property var scrollbarMarkColors: {
        let colors = chrome.session ? chrome.session.scrollbarMarkColors : undefined;
        return colors !== undefined ? colors : [];
    }

    // OSC 8 hyperlink tooltip. A plain Item carrying an attached ToolTip rather than a Popup of its
    // own, so it follows the same idiom TabItem.qml uses and inherits the app palette for free.
    //
    // Placed above the anchor cell where there is room and below it otherwise, and clamped to the
    // chrome's width: a link near the right edge would otherwise hang the tooltip off the pane.
    // Bound rather than assigned on a signal, because C++ already pushes only on a real transition --
    // moving within one link announces nothing, so the show delay is not restarted per cell.
    Item {
        id: hyperlinkTip
        objectName: "hyperlinkTooltip"

        // Guarded against `undefined`, not merely against a null session: other tests bind this
        // component to a MOCK session that has no tooltip properties at all, and assigning undefined to
        // a typed property is a QML warning the run-wide diagnostic gate fails the suite on.
        readonly property rect anchorRect: (chrome.session && chrome.session.hyperlinkTooltipAnchor !== undefined)
                                           ? chrome.session.hyperlinkTooltipAnchor
                                           : Qt.rect(0, 0, 0, 0)
        readonly property string tipText: (chrome.session && chrome.session.hyperlinkTooltipText !== undefined)
                                          ? chrome.session.hyperlinkTooltipText
                                          : ""
        readonly property bool showAbove: anchorRect.y > height + 8

        width: 1
        height: 1
        x: Math.max(0, Math.min(anchorRect.x, chrome.width - 1))
        y: showAbove ? anchorRect.y : anchorRect.y + anchorRect.height

        ToolTip.text: hyperlinkTip.tipText
        // Withheld while the window is inactive: a tooltip left standing over an unfocused window is
        // the pointer describing something the user is no longer looking at.
        ToolTip.visible: hyperlinkTip.tipText !== ""
                         && chrome.Window.window !== null
                         && chrome.Window.window.active
        ToolTip.delay: 600
    }

    // The gutter's block-column tooltip: how the hovered command ended, how long it took, when and where it
    // ran, and what it was. The anchor is a NOTIFY property; the text is read through the session's
    // commandBlockTooltip() each time that property's signal fires, so a hover that never happens formats
    // nothing. Guarded against mock sessions without the members, as hyperlinkTip is.
    Item {
        id: blockTip
        objectName: "commandBlockTooltip"

        readonly property rect anchorRect: (chrome.session && chrome.session.commandBlockTooltipAnchor !== undefined)
                                           ? chrome.session.commandBlockTooltipAnchor
                                           : Qt.rect(0, 0, 0, 0)
        // Bound to the SESSION's property, not to anchorRect: output scrolling under a stationary pointer
        // lets the next block be entered at the very cell the last one was, and an anchor that did not
        // change would leave this showing the last block's text.
        readonly property string tipText: (chrome.session
                                           && chrome.session.commandBlockTooltipAnchor !== undefined
                                           && chrome.session.commandBlockTooltipAnchor.width > 0
                                           && typeof chrome.session.commandBlockTooltip === "function")
                                          ? chrome.session.commandBlockTooltip()
                                          : ""
        readonly property bool showAbove: anchorRect.y > height + 8
        // Half the hyperlink tip's: a pointer brought to the gutter is there on purpose, where one crossing
        // a link is usually on its way to something else.
        readonly property int showDelay: 300

        width: 1
        height: 1
        // On the grid side of the gutter cell: over the strip it would cover the column being read.
        x: Math.max(0, Math.min(anchorRect.x + anchorRect.width, chrome.width - 1))
        y: showAbove ? anchorRect.y : anchorRect.y + anchorRect.height

        ToolTip.text: blockTip.tipText
        ToolTip.visible: blockTip.tipText !== ""
                         && chrome.Window.window !== null
                         && chrome.Window.window.active
        ToolTip.delay: blockTip.showDelay
    }

    // Vertical scrollbar, bidirectionally bound to the VT viewport (drag -> viewport here; viewport ->
    // thumb via the session Connections below). Honors the session's left/right placement preference;
    // all reads are null-guarded. The self-handlers are declared here (not connect()ed per session)
    // so they exist exactly once regardless of how often the session rebinds.
    ScrollBar {
        id: vbar
        objectName: "verticalScrollBar"

        // Which edge the bar floats against. ONE binding drives `x` — deliberately NOT a pair of
        // conditional left/right anchors, which is what used to make the bar span the WHOLE pane:
        //
        // QML re-evaluates two such bindings one at a time, and the null-session fallback anchored left,
        // so every null->Right rebind (a split, a tab switch, a teardown) transiently left BOTH edges
        // anchored. QQuickAnchors answers a left+right pair by *stretching* the item — setItemWidth(),
        // which routes through QQuickItem::setWidth() and latches widthValid. Resetting the other anchor
        // an instant later does not undo that: implicitWidth could never re-apply, and a right-only anchor
        // merely repositions a bar that is still parent-wide. Binding `x` leaves the width purely implicit,
        // so no anchor can ever size this item horizontally.
        readonly property bool atRightEdge: chrome.session ? chrome.session.isScrollbarRight : true

        anchors.top: parent.top
        anchors.bottom: parent.bottom
        x: vbar.atRightEdge ? parent.width - vbar.width : 0
        visible: (chrome.session ? chrome.session.isScrollbarVisible : false) && vbar.hasScrollback
        orientation: Qt.Vertical
        policy: visible ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
        minimumSize: 0.1
        size: chrome.session
            ? chrome.session.pageLineCount / (chrome.session.pageLineCount + chrome.session.historyLineCount)
            : 1.0
        stepSize: chrome.session
            ? 1.0 / (chrome.session.pageLineCount + chrome.session.historyLineCount)
            : 0.0
        onPositionChanged: chrome.onScrollBarPositionChanged()
        onSizeChanged: chrome.updateScrollBarPosition()
        onAvailableHeightChanged: chrome.publishMarkTrack()

        // Comfortable hit target that floats just off the terminal edge. Overlay only — no gutter is
        // reserved, so the grid/window geometry is untouched.
        implicitWidth: 12
        padding: 2

        // A stock ScrollBar inherits the app-pinned Fusion style's thin, low-contrast, hover-only handle
        // (see ContourGuiApp's QQuickStyle::setStyle("Fusion")), nearly invisible over a terminal's
        // per-profile background. An explicit contentItem/background — the same remedy WindowControls.qml
        // uses for Fusion's illegible defaults — makes the handle style-independent: clearly visible at
        // rest, emphasized on hover/drag.

        // Auto-contrast handle color derived from the profile background (QColor r/g/b are 0..1), so it
        // reads on any theme without plumbing a foreground color to QML: light handle over dark
        // backgrounds, dark handle over light ones. Null-guarded during split-teardown rebinds.
        readonly property color handleColor: {
            let bg = chrome.session ? chrome.session.backgroundColor : Qt.color("black");
            let luminance = 0.2126 * bg.r + 0.7152 * bg.g + 0.0722 * bg.b;
            return luminance < 0.5 ? Qt.color("white") : Qt.color("black");
        }

        // True only while there is scrollback to move through. Because we supply our own contentItem, the
        // style's "hide when content fits" no longer applies, so we gate the whole bar's visibility on
        // size (< 1.0 means history exists) ourselves — this also keeps the overlay from swallowing edge
        // clicks/selections when there is nothing to scroll (an invisible-but-present bar would still
        // capture mouse events in its strip).
        readonly property bool hasScrollback: vbar.size < 1.0

        contentItem: Rectangle {
            // Above the tick layer below, so the handle is drawn over the ticks it passes.
            z: 1
            implicitWidth: 8
            radius: width / 2
            color: vbar.handleColor
            // Visible at rest, brighter on hover, brightest while dragging. Opacity is ours (not the
            // style's active-driven fade), so the handle never vanishes when merely idle.
            opacity: vbar.pressed ? 0.55 : vbar.hovered ? 0.45 : 0.28
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }

        background: Rectangle {
            color: vbar.handleColor
            // A faint track that only surfaces while the user interacts, keeping the resting look clean.
            opacity: (vbar.hovered || vbar.pressed) ? 0.10 : 0.0
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }

        // Tick marks over the track (`scrollbar.marks`): where commands ran, failed or are still running, and
        // the lines a vi `mm` marked. Each sits at its mark's fraction of the track, its row among all the
        // rows. While the handle is drawn at its true size, that is where the handle's top goes once that row
        // is the viewport's top line. Below `minimumSize` it is not: Qt draws the handle larger, over a track
        // shortened to match, so its top lies above the top row's tick and it covers the ticks of rows off
        // the screen as well. A click on a tick is therefore a jump of its own, wherever the handle is.
        //
        // Stacked between the bar itself and the handle (the contentItem, z 1). Not below the bar: a child with
        // a negative z is hit-tested beneath its parent as well as drawn there, so the bar would take every
        // press before a tick saw it. A child of the bar, so it is hidden exactly when the bar is: position
        // Hidden, hide_in_alt_screen, or no scrollback at all.
        Item {
            id: markLayer
            objectName: "scrollbarMarkLayer"
            z: 0.5
            x: vbar.leftPadding
            y: vbar.topPadding
            width: vbar.availableWidth
            height: vbar.availableHeight

            // A tick's drawn height, and how far past it the tick takes a press on either side: 3 + 2 * 3 makes a
            // target nine pixels tall, where a two-pixel line taking one more on each side was a hard one.
            readonly property int tickHeight: 3
            readonly property int hitMargin: 3

            Repeater {
                id: markRepeater
                model: chrome.scrollbarMarks

                delegate: Rectangle {
                    id: tick
                    objectName: "scrollbarMark"

                    required property var modelData

                    x: 0
                    // Clamped, so a mark on the track's last pixel is drawn inside it rather than below.
                    y: Math.min(markLayer.height - tick.height,
                                Math.round(tick.modelData.position * markLayer.height))
                    width: markLayer.width
                    height: markLayer.tickHeight
                    radius: 1
                    color: chrome.markColor(tick.modelData.kind)

                    MouseArea {
                        // The hit area reaches past the line without moving what is drawn.
                        anchors.fill: parent
                        anchors.topMargin: -markLayer.hitMargin
                        anchors.bottomMargin: -markLayer.hitMargin
                        cursorShape: Qt.PointingHandCursor

                        // Clear of the handle the tick keeps the press, so the bar neither moves to it nor starts
                        // a drag. Under the handle the press goes on to the bar, so a drag that starts on the tick
                        // still moves the handle; the bar's TapHandler makes a click there the jump all the same.
                        onPressed: (mouse) => { mouse.accepted = !chrome.isUnderHandle(tick.y + tick.height / 2); }
                        onClicked: chrome.jumpToMark(tick.modelData.targetLine)
                    }
                }
            }
        }

        // A click on a tick under the handle: a press the bar took, released without a drag past the threshold.
        // On the bar itself, not on the tick: a handler accepts the mouse point it wants, which keeps the press
        // from every item beneath it, while the bar's own handlers see the press just before the bar does. Its
        // grab is passive, so the bar still drags. A tick clear of the handle keeps its own press, so any tick
        // found here is one the handle covered.
        TapHandler {
            gesturePolicy: TapHandler.DragThreshold
            onTapped: (eventPoint) => chrome.jumpToMarkAt(markLayer.mapFromItem(vbar, eventPoint.pressPosition).y)
        }
    }

    // Bell sound, loaded once multimedia is ready; a bell rung before then plays when it is.
    DeferredBell {
        id: bell
    }

    // Permission-wall dialogs.
    RequestPermission {
        id: requestFontChangeDialog
        text: qsTr("The host application is requesting to change the display font.")
        onYesToAllClicked: if (chrome.session !== null) chrome.session.applyPendingFontChange(true, true);
        onYesClicked: if (chrome.session !== null) chrome.session.applyPendingFontChange(true, false);
        onNoToAllClicked: if (chrome.session !== null) chrome.session.applyPendingFontChange(false, true);
        onNoClicked: if (chrome.session !== null) chrome.session.applyPendingFontChange(false, false);
        onRejected: if (chrome.session !== null) chrome.session.applyPendingFontChange(false, false);
    }

    RequestPermission {
        id: requestLargeFilePaste
        text: qsTr("The host application is going to paste a large file. Are you sure?")
        onYesToAllClicked: if (chrome.session !== null) chrome.session.applyPendingPaste(true, true);
        onYesClicked: if (chrome.session !== null) chrome.session.applyPendingPaste(true, false);
        onNoToAllClicked: if (chrome.session !== null) chrome.session.applyPendingPaste(false, true);
        onNoClicked: if (chrome.session !== null) chrome.session.applyPendingPaste(false, false);
        onRejected: if (chrome.session !== null) chrome.session.applyPendingPaste(false, false);
    }

    RequestPermission {
        id: requestBufferCaptureDialog
        text: qsTr("The host application is requesting to capture the terminal buffer.")
        onYesToAllClicked: if (chrome.session !== null) chrome.session.executePendingBufferCapture(true, true);
        onYesClicked: if (chrome.session !== null) chrome.session.executePendingBufferCapture(true, false);
        onNoToAllClicked: if (chrome.session !== null) chrome.session.executePendingBufferCapture(false, true);
        onNoClicked: if (chrome.session !== null) chrome.session.executePendingBufferCapture(false, false);
        onRejected: if (chrome.session !== null) chrome.session.executePendingBufferCapture(false, false);
    }

    RequestPermission {
        id: requestShowHostWritableStatusLine
        text: qsTr("The host application is requesting to show the host-writable status line.")
        onYesToAllClicked: if (chrome.session !== null) chrome.session.executeShowHostWritableStatusLine(true, true);
        onYesClicked: if (chrome.session !== null) chrome.session.executeShowHostWritableStatusLine(true, false);
        onNoToAllClicked: if (chrome.session !== null) chrome.session.executeShowHostWritableStatusLine(false, true);
        onNoClicked: if (chrome.session !== null) chrome.session.executeShowHostWritableStatusLine(false, false);
        onRejected: if (chrome.session !== null) chrome.session.executeShowHostWritableStatusLine(false, false);
    }

    // Asked before RerunCommand types a command at the prompt and enters it: the command line is what the
    // shell reported, or what the screen showed, and a program writing to the terminal can forge either. The
    // text is set as the dialog opens, from the command the session holds for it.
    RequestPermission {
        id: requestRerunCommandDialog
        title: qsTr("Run this command again?")
        onYesToAllClicked: if (chrome.session !== null) chrome.session.applyPendingRerun(true, true);
        onYesClicked: if (chrome.session !== null) chrome.session.applyPendingRerun(true, false);
        onNoToAllClicked: if (chrome.session !== null) chrome.session.applyPendingRerun(false, true);
        onNoClicked: if (chrome.session !== null) chrome.session.applyPendingRerun(false, false);
        onRejected: if (chrome.session !== null) chrome.session.applyPendingRerun(false, false);
    }

    // Asked when a status-line extension first runs one of the commands its manifest declares, with
    // `run_command: ask`. The text names the extension, its module and every command it declares, made safe to
    // show by the session. Every answer is remembered in the grants file, so "to all" means the same as the
    // plain one; Abort, or closing the dialog, is no answer.
    RequestPermission {
        id: requestExtensionCommandsDialog
        objectName: "extensionCommandsDialog"
        title: qsTr("Allow this status-line extension to run commands?")
        onYesToAllClicked: if (chrome.session !== null) chrome.session.allowPendingExtension();
        onYesClicked: if (chrome.session !== null) chrome.session.allowPendingExtension();
        onNoToAllClicked: if (chrome.session !== null) chrome.session.denyPendingExtension();
        onNoClicked: if (chrome.session !== null) chrome.session.denyPendingExtension();
        onRejected: if (chrome.session !== null) chrome.session.dismissPendingExtension();
    }

    // The find bar (Ctrl+Shift+F, or `/` in Vi normal mode). Per pane, not per window: each session
    // keeps its own pattern, count and highlights, so a split shows the bar over the pane being
    // searched. Opened by the session's searchBarRequested signal, wired in Connections below.
    SearchBar {
        id: searchBar
        session: chrome.session
        displayItem: chrome.displayItem
        header: chrome.headerPresent
        headerSlot: chrome.headerFindSlot
    }

    // Play the session's bell, at once or once multimedia is ready.
    function playBell(volume) {
        bell.play(chrome.session ? chrome.session.bellSource : "", volume);
    }

    // Flash the window's task-bar entry / urgency hint.
    function doAlert() {
        if (Window.window !== null)
            Window.window.alert(0);
    }

    // Relay one of the session's own notices to the window's tray/notification logic. Routed through
    // Window.window (the ApplicationWindow's showNotification) rather than the display item: a
    // ContourTerminal's showNotification is a C++ signal, so connecting a session's notification to it for a
    // split pane would dead-end (signal->signal) and never reach the tray. Window-level relay works for
    // every host uniformly.
    function relayNotification(title, body) {
        if (Window.window !== null && Window.window.showNotification !== undefined)
            Window.window.showNotification(title, body);
    }

    // Update the VT's viewport whenever the user moves the scrollbar.
    //
    // Only the user: a move the bar makes to follow the session is the session's own, and written back while
    // the bar is active -- the pointer resting on it -- it reset the smooth scroll, which ended a glide at its
    // first frame. Rounded rather than truncated by the int property: the position is an offset divided by
    // the total, and multiplying back can land a hair under the integer -- an offset one row nearer the
    // bottom, which puts the row below on top.
    function onScrollBarPositionChanged() {
        let vt = chrome.session;
        if (vt === null || chrome.followingSession)
            return;
        let totalLineCount = (vt.pageLineCount + vt.historyLineCount);
        if (vbar.active)
            vt.scrollOffset = Math.round(vt.historyLineCount - vbar.position * totalLineCount);
    }

    // The colour of a tick of @p kind, from the session's colour scheme; the handle's own colour while the
    // session has published none (a mock, or the instant before its first refresh).
    function markColor(kind) {
        return kind >= 0 && kind < chrome.scrollbarMarkColors.length ? chrome.scrollbarMarkColors[kind]
                                                                       : vbar.handleColor;
    }

    // Whether @p y, in the mark layer's coordinates, lies under the scrollbar's handle as it is drawn.
    function isUnderHandle(y) {
        let top = vbar.visualPosition * markLayer.height;
        return y >= top && y <= top + vbar.visualSize * markLayer.height;
    }

    // Asks the session to put a tick's line at the top of the viewport.
    function jumpToMark(targetLine) {
        if (chrome.session && chrome.session.scrollToScrollbarMark !== undefined)
            chrome.session.scrollToScrollbarMark(targetLine);
    }

    // Jumps to the tick a press at @p y, in the mark layer's coordinates, lands on: the topmost one whose hit
    // area holds it, as the ticks' own MouseAreas take a press. Nothing when no tick is there.
    function jumpToMarkAt(y) {
        for (let index = markRepeater.count - 1; index >= 0; --index) {
            let tick = markRepeater.itemAt(index);
            if (y >= tick.y - markLayer.hitMargin && y <= tick.y + tick.height + markLayer.hitMargin) {
                chrome.jumpToMark(tick.modelData.targetLine);
                return;
            }
        }
    }

    // Tells the session how tall the track is, so its pixel buckets are the pixels drawn here.
    function publishMarkTrack() {
        if (chrome.session && chrome.session.setScrollbarMarkTrackPixels !== undefined)
            chrome.session.setScrollbarMarkTrackPixels(Math.round(vbar.availableHeight));
    }

    // A rebound session has never been told this pane's track height.
    onSessionChanged: chrome.publishMarkTrack()
    Component.onCompleted: chrome.publishMarkTrack()

    // Update the GUI scrollbar whenever the VT's viewport changes.
    function updateScrollBarPosition() {
        let vt = chrome.session;
        if (vt === null)
            return;
        let totalLineCount = (vt.pageLineCount + vt.historyLineCount);
        chrome.followingSession = true;
        vbar.position = (vt.historyLineCount - vt.scrollOffset) / totalLineCount;
        chrome.followingSession = false;
    }

    // Wire the per-session signals to this chrome's widgets. Declarative on purpose: Connections
    // re-targets on every `session` rebind and disconnects the previous session automatically, so a
    // pane switched between sessions never keeps stale cross-session wiring. A null target (the
    // transient split-teardown state) simply connects nothing. ignoreUnknownSignals tolerates mock
    // sessions in the offscreen tests. NB: the doubled `onOn…` prefixes are correct — the C++ signals
    // are named onBell/onAlert.
    Connections {
        target: chrome.session
        ignoreUnknownSignals: true

        // Bell, window alert, and the session's own notices (relayed to the window's tray logic).
        function onOnBell(volume) { chrome.playBell(volume); }
        function onOnAlert() { chrome.doAlert(); }
        function onShowNotification(title, body) { chrome.relayNotification(title, body); }

        // Viewport -> scrollbar thumb (the drag direction lives on the ScrollBar's own handlers).
        function onScrollOffsetChanged() { chrome.updateScrollBarPosition(); }

        // open() is a no-op on an already-open Popup (and onOpened does not re-fire), so focusing
        // afterwards covers both cases: a second Ctrl+Shift+F means "search for something else"
        // rather than doing nothing.
        // A request may also have set the pattern (Use Selection for Find), which an open bar must then show.
        function onSearchBarRequested() {
            searchBar.showSessionPattern();
            searchBar.open();
            searchBar.focusField();
        }

        // Permission-wall hooks.
        function onRequestPermissionForFontChange() { requestFontChangeDialog.open(); }
        function onRequestPermissionForBufferCapture() { requestBufferCaptureDialog.open(); }
        function onRequestPermissionForShowHostWritableStatusLine() { requestShowHostWritableStatusLine.open(); }
        function onRequestPermissionForPasteLargeFile() { requestLargeFilePaste.open(); }
        function onRequestPermissionForExtension() {
            requestExtensionCommandsDialog.text = chrome.session.pendingExtensionPermissionText();
            requestExtensionCommandsDialog.open();
        }
        function onRequestPermissionForRerunCommand() {
            requestRerunCommandDialog.text = qsTr("It is typed at the prompt and entered:") + "\n\n"
                                             + chrome.session.pendingRerunCommandLine();
            requestRerunCommandDialog.open();
        }
    }
}
