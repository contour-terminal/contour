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
// sibling components (PermissionDialog, DeferredBell) resolve via the implicit same-directory import.
import QtQuick
import QtQuick.Controls
import QtQuick.Window
import Contour.Terminal

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
    // the reason LinkPill gives: other tests bind a mock session that carries no marks at all. Each
    // list is read from the session once: every read converts the whole of it to JavaScript.
    readonly property var scrollbarMarks: {
        let marks = chrome.session ? chrome.session.scrollbarMarks : undefined;
        return marks !== undefined ? marks : [];
    }
    readonly property var scrollbarMarkColors: {
        let colors = chrome.session ? chrome.session.scrollbarMarkColors : undefined;
        return colors !== undefined ? colors : [];
    }

    /// Where the pane's own overlays (the link pill) sit over the grid: an item its host lays exactly over this
    /// chrome, above the pane's dim and outside any fade. Null: this chrome.
    property Item overlayHost: null
    /// The pane header's link pill slot, where the pane shows an inline header; null where it shows none.
    property Item headerLinkSlot: null
    /// Whether the link pill sits in the pane header (QML-facing): the header hides what it would cover --
    /// until the pill has faded out, not from when it starts to, so nothing comes back under it.
    readonly property bool linkPillInHeader: linkPill.inHeader && linkPill.visible

    // The link under the pointer: its target and how to open it, over blank cells or in the pane header. It
    // replaces the hover tooltip.
    LinkPill {
        id: linkPill
        session: chrome.session
        gridItem: chrome
        gridHost: chrome.overlayHost ? chrome.overlayHost : chrome
        headerSlot: chrome.headerLinkSlot
        headerPresent: chrome.headerPresent
        // Where the find bar is while it is open: the pill keeps out of the places it holds.
        findBar: chromeStyle.findBarPresence(searchBar.visible ? searchBar.placement.anchor : "")
    }

    // The gutter's block-column tooltip: how the hovered command ended, how long it took, when and where it
    // ran, and what it was. The anchor is a NOTIFY property; the text is read through the session's
    // commandBlockTooltip() each time that property's signal fires, so a hover that never happens formats
    // nothing. Guarded against mock sessions without the members, as LinkPill is.
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
        // Whether the bar is wanted: the session's decision (an overlay bar hides at the bottom), and history to
        // move through. It shows and hides by fading over motion.standard (a cross-fade under Reduce Motion);
        // only once fully faded out is it gone, so a hidden bar takes no press.
        readonly property bool wanted: (chrome.session ? chrome.session.isScrollbarVisible : false)
                                       && vbar.hasScrollback
        opacity: vbar.wanted ? 1.0 : 0.0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: chromeStyle.motion.standard } }
        orientation: Qt.Vertical
        policy: vbar.wanted ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
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

        // The track and, toward the pane's edge, the inset beside it: inside the hit area, so a press at the
        // very edge still takes the bar. Overlay only: no gutter is reserved, so the grid is untouched.
        implicitWidth: chromeStyle.size.scrollbar + chromeStyle.space.scrollbarInset
        padding: 0
        leftPadding: vbar.atRightEdge ? 0 : chromeStyle.space.scrollbarInset
        rightPadding: vbar.atRightEdge ? chromeStyle.space.scrollbarInset : 0

        // True only while there is scrollback to move through. Because we supply our own contentItem, the
        // style's "hide when content fits" no longer applies, so we gate the whole bar's visibility on
        // size (< 1.0 means history exists) ourselves — this also keeps the overlay from swallowing edge
        // clicks/selections when there is nothing to scroll (an invisible-but-present bar would still
        // capture mouse events in its strip).
        readonly property bool hasScrollback: vbar.size < 1.0

        // The Terminal row draws the thumb as a column of a glyph (one per cell row); the native rows as a
        // rounded shape. Hovered or dragged, the thumb takes its active colour and the track shows.
        readonly property bool glyphThumb: chromeStyle.scrollThumbGlyph !== ""
        readonly property bool engaged: vbar.hovered || vbar.pressed
        // The native thumb resolves on the pane's own background (colorOn), so a light profile under a dark
        // chrome keeps a legible one; the Terminal row's is the scheme's, chrome-wide.
        readonly property color paneBackground: chrome.session ? chrome.session.backgroundColor : "transparent"
        readonly property color thumbColor: {
            if (chromeStyle.paletteRevision < 0)
                return "transparent"
            if (vbar.glyphThumb)
                return vbar.pressed ? chromeStyle.colors.scrollbarThumbActive : chromeStyle.colors.scrollbarThumb
            return chromeStyle.colorOn(vbar.engaged ? chromeStyle.roles.scrollbarThumbActive
                                                    : chromeStyle.roles.scrollbarThumb,
                                       vbar.paneBackground)
        }

        contentItem: Rectangle {
            id: thumb
            objectName: "scrollbarThumb"
            // Above the tick layer below, so the handle is drawn over the ticks it passes.
            z: 1
            implicitWidth: chromeStyle.size.scrollbar
            radius: chromeStyle.radii.scrollbar
            color: vbar.glyphThumb ? "transparent" : vbar.thumbColor
            Behavior on color { ColorAnimation { duration: chromeStyle.motion.fast } }

            Column {
                visible: vbar.glyphThumb
                Repeater {
                    model: vbar.glyphThumb ? Math.max(1, Math.floor(thumb.height / chromeStyle.cellHeight)) : 0
                    Text {
                        objectName: "scrollbarThumbGlyph"
                        width: thumb.width
                        height: chromeStyle.cellHeight
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        text: chromeStyle.scrollThumbGlyph
                        font: chromeStyle.type.body
                        color: vbar.thumbColor
                        Behavior on color { ColorAnimation { duration: chromeStyle.motion.fast } }
                    }
                }
            }
        }

        background: Rectangle {
            objectName: "scrollbarTrack"
            radius: chromeStyle.radii.scrollbar
            color: vbar.engaged && !vbar.glyphThumb && chromeStyle.paletteRevision >= 0
                ? chromeStyle.colorOn(chromeStyle.roles.scrollbarTrack, vbar.paneBackground)
                : "transparent"
            Behavior on color { ColorAnimation { duration: chromeStyle.motion.fast } }
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

    // The session's permission question: one at a time, the next shown once this one is answered. What it
    // shows is the session's pendingPermission -- program-supplied text the session has already made safe to
    // show. An answer goes to the session that asked, naming the question it answers, so a late or doubled
    // answer can never answer a question the user did not see.
    PermissionDialog {
        id: permissionDialog
        prompt: ({})
        focusTarget: chrome.displayItem

        // The session whose question is shown, and its id; null and -1 while none is.
        property var askingSession: null
        property int questionId: -1

        // Unbound before the answer goes: the session may put its next question up from inside the answer,
        // binding it here, and clearing afterwards would leave that one with nobody to answer it.
        onAnswered: (decision, memory) => {
            let session = askingSession;
            let id = questionId;
            askingSession = null;
            questionId = -1;
            if (session !== null && id >= 0)
                session.answerPermission(id, decision, memory);
        }

        // Shows what @p session asks now, or closes the dialog when it asks nothing.
        function follow(session) {
            let asked = session !== null && session.pendingPermission !== undefined ? session.pendingPermission : ({});
            if (asked.title === undefined) {
                askingSession = null;
                questionId = -1;
                if (visible)
                    close();
                return;
            }
            // Another question while one is shown -- the same one asked again with more to it, or the next --
            // is shown anew: the dialog closes and opens with it, so a click aimed at what was shown can never
            // reach the question that replaced it. The session says the new one to a screen reader.
            if (visible && (askingSession !== session || questionId !== asked.id)) {
                askingSession = null;
                questionId = -1;
                close();
            }
            askingSession = session;
            questionId = asked.id;
            // Put up from inside the answer to the last one, it is presented once that answer's dialog has
            // closed -- presented now, the close would take it too -- and only if it is still the one asked,
            // as the session publishes it then (Allow may have been held or given back meanwhile).
            if (answering) {
                Qt.callLater(() => {
                    if (askingSession === session && questionId === asked.id
                        && session.pendingPermission.id === asked.id)
                        present(session.pendingPermission);
                });
                return;
            }
            present(asked);
        }
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

    // The colour of a tick of @p kind, from the session's colour scheme; the thumb's own colour while the
    // session has published none (a mock, or the instant before its first refresh).
    function markColor(kind) {
        return kind >= 0 && kind < chrome.scrollbarMarkColors.length ? chrome.scrollbarMarkColors[kind]
                                                                       : vbar.thumbColor;
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

    // A rebound session has never been told this pane's track height, and may have asked its question while
    // no chrome showed it: the dialog follows what the session asks now.
    onSessionChanged: {
        chrome.publishMarkTrack();
        permissionDialog.follow(chrome.session);
    }
    Component.onCompleted: {
        chrome.publishMarkTrack();
        permissionDialog.follow(chrome.session);
    }

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

        // The session asks another question, or what the one shown asks changed.
        function onPendingPermissionChanged() { permissionDialog.follow(chrome.session); }
    }
}
