// vim:syntax=qml
// The title band: the window's own bar, holding its controls, the host chip, the tabs or the active
// session's title, a draggable region, the daemon drift chip and (under the Pill shape) the new-tab button.
//
// Because the window is frameless, this bar owns everything the native decoration used to: the tabs, an
// empty draggable region (drag to move the window, double-click to maximize/restore), and the
// min/max/close controls. Which END of the bar those controls sit at is the configured
// window_control_style's to decide (`windowControls.side`), which is why there is a slot for them at each
// edge and never an anchor pinning them to one.
//
// Where the band goes and what its middle holds is UiStyleProvider::titleBand's table, which Main.qml reads
// and hands down as `centre` and `presence`. A second instance with `role: "tabs"` is the bar that holds
// only the tabs when they sit at the bottom under a band at the top: it has no controls, chips, toolbar or
// new-tab menu, so nothing of the band's exists twice -- not even its title-bar role.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Item {
    id: root

    // The window has one title bar: a bar holding only the tabs is a plain pane, and its strip carries the
    // name (§2.6).
    Accessible.role: root.isBand ? Accessible.TitleBar : Accessible.Pane
    Accessible.name: root.isBand ? qsTr("Title bar") : ""

    required property var controller // this window's WindowController (Main.qml: appWindow.win)
    required property var window

    /// Which bar this is: "band" (the window's own bar) or "tabs" (a bar holding only the tabs).
    property string role: "band"
    /// What the middle holds, as UiStyleProvider::titleBand names it: "tabs", "title" or "empty".
    property string centre: "tabs"
    /// Whether the bar is on screen, as UiStyleProvider::titleBand names it: "hidden", "shown", "reveal"
    /// (full screen: it overlays the top rows while the pointer is at the screen edge or on it) or "pinned"
    /// (full screen: it overlays them for as long as the remote connection is interrupted).
    property string presence: "shown"
    /// Whether a "reveal" bar is revealed. Assigned by Main.qml, from the reveal edge and `hovered`.
    property bool revealed: false
    /// How far a hidden "reveal" bar is slid above its resting place: its height under full motion, and 0
    /// under Reduce Motion, where it fades instead. Main.qml adds it to the resting `y`.
    readonly property real slideOffset: root.isConcealed && chromeStyle.motion.animates ? -root.height : 0
    /// Whether the pointer is over the bar.
    readonly property bool hovered: bandHover.hovered
    /// Whether the bar has a popup or an editor of its own open -- the new-tab menu, the tab menu, a
    /// tab's colour flyout or rename -- which a HoverHandler cannot see, since popups live in the overlay.
    readonly property bool engaged: (newTabMenuLoader.item !== null && newTabMenuLoader.item.visible)
                                    || (tabStripLoader.item !== null && tabStripLoader.item.engaged)
    /// Whether keyboard focus is on one of the bar's own controls, such as the "+" or the host chip, so a
    /// full-screen band a keyboard user is on does not hide under them (see Main.qml). Popups are not
    /// beneath the bar (they live in the overlay); `engaged` covers those.
    readonly property bool holdsFocus: {
        let item = root.window ? root.window.activeFocusItem : null;
        while (item) {
            if (item === root)
                return true;
            item = item.parent;
        }
        return false;
    }
    /// The trailing slot the drift chip is placed in while the band is shown (see Main.qml).
    readonly property Item driftSlot: driftSlotItem

    readonly property bool isBand: root.role === "band"
    readonly property bool isPill: chromeStyle.layout.isPill
    // A "reveal" bar that is not revealed: slid away, or faded out.
    readonly property bool isConcealed: root.presence === "reveal" && !root.revealed
    // The room between the band's chips and the parts beside them.
    readonly property real gap: chromeStyle.space.s5

    // Whether to draw our own min/max/close controls. Set false whenever the NATIVE frame is shown,
    // on any OS, because its server-side controls would otherwise be duplicated -- that is exactly
    // show_title_bar, and the owner (Main.qml) drives this from the controller's titleBarVisible.
    //
    // Not a platform test: it was one (`Qt.platform.os !== "osx"`), from when macOS kept its native
    // frame unconditionally, but Main.qml has overridden it unconditionally ever since, so the check
    // decided nothing. What macOS actually wants is macOS-shaped controls, which is
    // window_control_style's job, not this flag's.
    property bool useCustomWindowControls: true

    // Leading space to leave clear for the OPERATING SYSTEM's own window controls, where it paints
    // them over our chrome rather than in a bar of its own above it -- macOS, whose
    // full-size-content window keeps drawing the real traffic lights while the content view extends
    // underneath them. Zero everywhere else.
    //
    // The number comes from the frame adapter (WindowController.nativeControlsInset), NOT from our
    // chrome tokens: AppKit's buttons are a fixed size whatever font Contour is set in, so composing
    // it from windowControlInset/trafficLightDotSize would be wrong by whatever the style says --
    // badly wrong under `ui_style: terminal`, where those are counted in character cells. It is rounded
    // up to whole cells there, so the tabs after it stay on the cell grid.
    property real nativeControlsInset: 0

    // The whole bar's height, and so the window's chrome height (Main.qml declares it to the
    // WindowController, which feeds the WM size hints): the band token, 38 under the Pill shape, one
    // character row in the terminal style, and the historical 34px on the other native rows.
    implicitHeight: chromeStyle.bandHeight

    // A hidden "reveal" bar fades under Reduce Motion (and slides under full motion, see slideOffset).
    opacity: root.isConcealed && !chromeStyle.motion.animates ? 0 : 1
    Behavior on opacity {
        enabled: root.presence === "reveal"
        NumberAnimation { duration: chromeStyle.motion.standard }
    }

    HoverHandler {
        id: bandHover
    }

    // Live OS palette handle: re-emits paletteChanged on an OS dark/light switch so the bar recolors
    // in realtime.
    SystemPalette {
        id: systemPalette
        colorGroup: SystemPalette.Active
    }

    // Opaque background so the transparent window doesn't show the desktop through the bar. The
    // ApplicationWindow itself is transparent for terminal see-through, so the bar must paint a fully
    // opaque fill of its own — sourced from the OS window color so it matches the system theme (the
    // palette roles are opaque QColors).
    Rectangle {
        anchors.fill: parent
        color: systemPalette.window
    }

    // The window controls, wherever they go. One definition, instantiated by whichever of the two
    // Loaders below the configured side turns on; the other is held at zero width, so the corner it
    // is not using costs the bar nothing.
    Component {
        id: windowControlsComponent

        WindowControls {
            window: root.window
            controller: root.controller
        }
    }

    // What the bar's parts before and after the tabs take, from their preferred widths, which do not
    // depend on the tabs: the tabs are left the rest, less the drag reserve.
    readonly property real leadingPartsWidth: leadingSlot.Layout.preferredWidth
                                              + hostChipSlot.Layout.preferredWidth
                                              + hostChipSlot.Layout.leftMargin + hostChipSlot.Layout.rightMargin
    readonly property real trailingPartsWidth: driftSlotItem.Layout.preferredWidth + driftSlotItem.Layout.rightMargin
                                               + newTabSlot.Layout.preferredWidth + newTabSlot.Layout.rightMargin
                                               + trailingSlot.Layout.preferredWidth
    readonly property real fixedPartsWidth: root.leadingPartsWidth + root.trailingPartsWidth

    // The room the centred title has: the bar less twice the wider of its two ends, so it stays centred
    // on the bar without running under either end.
    readonly property real titleRoom: Math.max(0, root.width - 2 * Math.max(root.leadingPartsWidth,
                                                                            root.trailingPartsWidth))

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // Leading slot: macOS puts its traffic lights here, before the tabs.
        Loader {
            id: leadingSlot
            objectName: "leadingWindowControls"
            Layout.fillHeight: true
            active: root.isBand && root.useCustomWindowControls && windowControls.side === "leading"
            // Zero while inactive, stated rather than inherited: a Loader only recomputes its
            // implicit size while it HAS an item, so one that has ever been filled keeps the width
            // of three controls forever -- and after a live style change that would leave the
            // vacated corner reserving space for buttons that moved to the other one.
            // @see the trailing slot below.
            // No side test: nativeControlsInset is already zero unless the OS is drawing its own
            // controls over this corner. Asking where OUR hidden controls would have sat is a
            // different question, and answers it wrong when the two disagree.
            Layout.preferredWidth: active ? implicitWidth
                                 : root.isBand ? chromeStyle.snapToCells(root.nativeControlsInset) : 0
            sourceComponent: windowControlsComponent
        }

        // The host a remote window is attached to. The band's only, and absent for a local window.
        Loader {
            id: hostChipSlot
            active: root.isBand
            readonly property bool shown: item !== null && item.visible
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: shown ? item.implicitWidth : 0
            Layout.leftMargin: shown ? root.gap : 0
            Layout.rightMargin: shown ? root.gap : 0
            sourceComponent: HostChip {
                // `?? ""` keeps a lightweight mock controller (offscreen tests) without the property local.
                hostLabel: root.controller ? (root.controller.hostLabel ?? "") : ""
                connection: root.controller ? (root.controller.hostChipState ?? 0) : 0
            }
        }

        // The tabs, while the middle holds them. A Loader, so a band showing the title holds no second
        // strip (with its own tab menu) beside the tabs bar's.
        Loader {
            id: tabStripLoader
            active: root.centre === "tabs"
            Layout.fillHeight: true
            // As wide as the tabs need: the strip caps itself at availableWidth, so the drag region below
            // keeps at least its reserve.
            Layout.preferredWidth: active ? implicitWidth : 0
            sourceComponent: TabStrip {
                id: tabStrip
                objectName: "tabStrip"
                // What the tabs may take: the bar less its other parts, the drag reserve and the strip's
                // own buttons. Read from preferred widths, which do not depend on the strip.
                availableWidth: Math.max(0, root.width - root.fixedPartsWidth
                                            - chromeStyle.space.dragReserve - tabStrip.fixedWidth)
                controller: root.controller
                // Threaded down so a closed inline rename can hand keyboard focus back to the terminal.
                window: root.window
            }
        }

        // Draggable empty region: move the window, double-click to toggle maximize, right-click for the
        // window's own context menu.
        Item {
            id: dragRegion
            objectName: "dragRegion"
            Layout.fillWidth: true
            Layout.fillHeight: true
            // The band that always stays draggable, however many tabs there are.
            Layout.minimumWidth: chromeStyle.space.dragReserve

            // Right-click opens the title bar menu. It does not fight the handlers below: both of those
            // take the LEFT button only, which the DragHandler now says explicitly rather than leaving
            // to a default that a future edit could quietly change.
            TapHandler {
                acceptedButtons: Qt.RightButton
                // ReleaseWithinBounds rather than the default DragThreshold: a little jitter while the
                // button is down should still open the menu, not swallow the click.
                gesturePolicy: TapHandler.ReleaseWithinBounds
                onTapped: if (root.controller) root.controller.openTitleBarContextMenu()
            }

            TapHandler {
                // Routed through the controller (the only window-geometry mutator): direct
                // QWindow show-mode calls skip its size-increment protocol.
                onTapped: (eventPoint, button) => {
                    if (tapCount === 2)
                        root.controller.toggleMaximized()
                }
            }
            DragHandler {
                objectName: "titleBarDragHandler" // findChild() handle for the GUI test
                target: null
                acceptedButtons: Qt.LeftButton
                // The default grabPermissions minus CanTakeOverFromItems, for the same reason as the tab
                // drag handler in TabItem (issue #1997): the resize border's top edge zone covers the top
                // six pixels of this region, and taking its grab asked for a window MOVE on top of the
                // resize already under way. See TabItem.qml for why the rule belongs on the handler.
                grabPermissions: PointerHandler.CanTakeOverFromHandlersOfDifferentType
                                 | PointerHandler.ApprovesTakeOverByHandlersOfDifferentType
                                 | PointerHandler.ApprovesTakeOverByItems
                                 | PointerHandler.ApprovesCancellation
                onActiveChanged: if (active) root.window.startSystemMove()
            }
        }

        // The drift chip's place in the band's trailing area, before the "+" (Pill) or the window controls
        // (Flat). Main.qml parents the chip here while the band is shown; it is the band's only.
        Item {
            id: driftSlotItem
            objectName: "bandDriftSlot"
            readonly property Item chip: children.length > 0 ? children[0] : null
            readonly property bool shown: root.isBand && chip !== null && chip.visible
            Layout.fillHeight: true
            Layout.preferredWidth: shown ? chip.width : 0
            Layout.rightMargin: shown ? root.gap : 0
        }

        // The band's new-tab button, under the Pill shape (Flat draws its own in the strip). A secondary
        // click or a long press opens the profiles, as the macOS "+" does (layout.profileMenu).
        Loader {
            id: newTabSlot
            active: root.isBand && root.isPill
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: active ? implicitWidth : 0
            Layout.rightMargin: active ? chromeStyle.space.s4 : 0
            sourceComponent: ToolbarButton {
                id: newTabButton
                objectName: "newTabButton"
                glyph: chromeStyle.newTabGlyph
                accessibleName: qsTr("New tab")
                accessibleDescription: qsTr("Right-click or press and hold for a new tab with a profile")
                onClicked: if (root.controller) root.controller.createNewTab()
                onSecondaryClicked: {
                    if (root.controller)
                        root.controller.openNewTabMenu(newTabButton.mapToItem(null, 0, newTabButton.height))
                }
            }
        }

        // Trailing slot: where Windows and Breeze put theirs, after the drag region.
        Loader {
            id: trailingSlot
            objectName: "trailingWindowControls"
            Layout.fillHeight: true
            active: root.isBand && root.useCustomWindowControls && windowControls.side === "trailing"
            // Zero while inactive, for the reason the leading slot states.
            Layout.preferredWidth: active ? implicitWidth : 0
            sourceComponent: windowControlsComponent
        }
    }

    // The active session's title, centred on the band while its middle holds no tabs: one tab on macOS.
    // Read from the active row's title role, so it is the label its tab would show.
    Text {
        id: bandTitle
        objectName: "bandTitle"
        visible: root.isBand && root.centre === "title"
        anchors.centerIn: parent
        width: Math.min(implicitWidth, root.titleRoom)
        text: titleRows.activeTitle
        font: root.titleFont
        color: chromeStyle.colors.tabText
        elide: Text.ElideMiddle
    }

    // The title in the body type, medium: a copy, since the token's font is shared.
    readonly property font titleFont: {
        let font = chromeStyle.type.body;
        font.weight = Font.Medium;
        return font;
    }

    // One object per tab, carrying its title and whether it is active, while the band shows a title.
    Instantiator {
        id: titleRows
        active: bandTitle.visible
        model: root.controller
        delegate: QtObject {
            required property string title
            required property bool isActive
        }

        readonly property string activeTitle: {
            for (let index = 0; index < titleRows.count; ++index) {
                const row = titleRows.objectAt(index);
                if (row && row.isActive)
                    return row.title;
            }
            return "";
        }
    }

    // The new-tab profile menu, the band's only: opened below the button that asked for it -- the band's
    // "+" under the Pill shape, the strip's "▾" on Flat.
    Loader {
        id: newTabMenuLoader
        active: root.isBand
        sourceComponent: ActionContextMenu {
            objectName: "newTabMenu"
            entries: root.controller ? root.controller.newTabMenuModel : []
            onPicked: (actionId) => { if (root.controller) root.controller.triggerNewTabMenuAction(actionId) }
        }
    }

    Connections {
        target: root.controller
        enabled: root.isBand
        function onNewTabMenuRequested() {
            const menu = newTabMenuLoader.item;
            if (!menu)
                return;
            const anchor = root.isPill ? newTabSlot.item
                         : tabStripLoader.item ? tabStripLoader.item.newTabMenuAnchor : null;
            if (anchor)
                menu.popup(anchor, 0, anchor.height);
            else
                menu.popup();
        }
    }
}
