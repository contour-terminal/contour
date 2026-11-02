// vim:syntax=qml
// The title band: the window's own bar, holding its controls, the host chip, the tabs or the active
// session's title, a draggable region, the secure-input chip, the daemon drift chip and (under the Pill shape)
// the new-tab button.
//
// Because the window is frameless, this bar owns everything the native decoration used to: the tabs, an
// empty draggable region (drag to move the window, double-click to maximize/restore), and the
// min/max/close controls. Which END of the bar those controls sit at is the configured
// window_control_style's to decide, and under `linux` the desktop's button layout's -- either end or both
// (`windowControls.leadingButtons`, `trailingButtons`) -- which is why there is a slot for them at each
// edge and never an anchor pinning them to one.
//
// Under the Unified arrangement (the ContourLinux row) the band is the Linux header bar: the window buttons
// at whichever ends the desktop's layout names, a separator between them and the header's items (new tab
// leading; search and the main menu trailing), the tabs in the middle. Which item takes which end, and
// where a separator stands, is UiStyleProvider::headerBar's table (window::HeaderBarRules); this file only
// lays the slots out.
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
    /// Where the bar rests, in the window: Main.qml's to say, since it places the bar. A full-screen band
    /// rests below the menu bar AppKit reveals over the window's top (WindowController.titleBandRevealTop).
    property real restingTop: 0
    /// How far a hidden "reveal" bar is slid above its resting place: wholly out of the window -- its height
    /// and its resting top -- under full motion, and 0 under Reduce Motion, where it fades in place instead.
    /// Main.qml adds it to `restingTop`.
    readonly property real slideOffset: root.isConcealed && chromeStyle.motion.animates
                                        ? -(root.restingTop + root.height) : 0
    /// Whether the pointer is over the bar.
    readonly property bool hovered: bandHover.hovered
    /// Whether the bar has a popup or an editor of its own open -- the new-tab menu, the tab menu, a
    /// tab's colour flyout or rename -- which a HoverHandler cannot see, since popups live in the overlay.
    readonly property bool engaged: (newTabMenuLoader.item !== null && newTabMenuLoader.item.visible)
                                    || (tabStripLoader.item !== null && tabStripLoader.item.engaged)
    /// Whether the bar's tab strip can show a tab's lock: false without a strip, or while it shows icons alone.
    readonly property bool tabsHostLock: tabStripLoader.hostsLock
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
    /// The tab strip, while the middle holds the tabs; null otherwise.
    readonly property Item tabStrip: tabStripLoader.item
    /// The band's toolbar items that are on screen, in visual order: the host chip, the drift chip, then the
    /// new-tab button. What the toolbar's keyboard focus walks (WindowController.focusedToolbarIndex).
    readonly property var toolbarItems: {
        const items = [hostChipSlot.item, driftSlotItem.chip, newTabSlot.item]
        return items.filter((item) => item !== null && item.visible)
    }
    /// Whether the window's keyboard focus is in the toolbar: an item that also holds active focus rings.
    /// `?? -1` keeps a mock controller without the property local.
    readonly property bool toolbarHasFocus: !!root.controller && (root.controller.focusedToolbarIndex ?? -1) >= 0

    readonly property bool isBand: root.role === "band"
    // Whether the band is the Linux header bar (the row's arrangement is Unified).
    readonly property bool isHeader: root.isBand && chromeStyle.layout.headerBar === chromeStyle.layout.unified
    // The header bar's slots and separators for this frame and the window buttons on either side.
    readonly property var header: chromeStyle.headerBar(!root.useCustomWindowControls,
                                                        windowControls.leadingButtons.length,
                                                        windowControls.trailingButtons.length)
    // The header-bar items the menus pop below, by their row's `menu` ("newTab", "main"); empty off the
    // header bar. Filled by the items as they are created, read only when a menu opens.
    property var headerMenuAnchors: ({})
    readonly property bool isPill: chromeStyle.layout.isPill
    readonly property bool isJoined: chromeStyle.layout.isJoined
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

    // The radius of the bar's two top corners: the window's own while the bar is the window's top edge
    // under client-side decorations (WindowController.windowCornerRadius), 0 otherwise. The bottom corners
    // stay square: rounding them would mask the terminal's surface every frame.
    property real cornerRadius: 0

    /// The band's height as the window measured it (WindowController.titleBandHeight): the token grown to
    /// the native title bar it covers, on macOS. 0, as on a bar that measures nothing, keeps the token.
    property real measuredHeight: 0

    /// What the bar paints behind its parts. The band's is its surface (UiStyleProvider.bandFill, which
    /// Main.qml assigns); a bar holding only the tabs carries no traffic lights and keeps the OS window
    /// colour.
    property color fill: systemPalette.window

    // The whole bar's height, and so the window's chrome height (Main.qml declares it to the
    // WindowController, which feeds the WM size hints): the measured height where there is one, else the
    // band token -- 38 under the Pill shape, one character row in the terminal style, and the historical
    // 34px on the other native rows; a band whose middle is empty (Minimal, one tab) is the bare band,
    // as WindowController::refreshTitleBand measures it from the same token.
    readonly property real tokenHeight: root.isBand && root.centre === "empty" ? chromeStyle.size.bareBand
                                                                              : chromeStyle.bandHeight
    implicitHeight: root.measuredHeight > 0 ? root.measuredHeight : root.tokenHeight

    // A hidden "reveal" bar fades under Reduce Motion (and slides under full motion, see slideOffset).
    opacity: root.isConcealed && !chromeStyle.motion.animates ? 0 : 1
    Behavior on opacity {
        enabled: root.presence === "reveal"
        NumberAnimation { duration: chromeStyle.motion.standard }
    }

    HoverHandler {
        id: bandHover
    }

    // The keys of the toolbar, offered to the controller while one of its items holds keyboard focus: an item
    // that uses a key (the button's Return and Space) accepts it first, the rest arrive here.
    Keys.onPressed: (event) => {
        if (root.controller && root.controller.chromeKey)
            event.accepted = root.controller.chromeKey(event.key, event.modifiers)
    }

    // Live OS palette handle: re-emits paletteChanged on an OS dark/light switch so the bar recolors
    // in realtime.
    SystemPalette {
        id: systemPalette
        colorGroup: SystemPalette.Active
    }

    // The bar's own fill, so the transparent window doesn't show the desktop through it: the
    // ApplicationWindow itself is transparent for terminal see-through. Opaque unless the surface is
    // cleared for a material behind it (`fill`); the palette roles and the band's tint are opaque QColors.
    Rectangle {
        objectName: root.isBand ? "bandBackground" : "tabsBarBackground"
        anchors.fill: parent
        color: root.fill
        topLeftRadius: root.cornerRadius
        topRightRadius: root.cornerRadius
    }

    // The window controls, one group per side: each of the two Loaders below is on while its side's
    // list has a button; one that is off is held at zero width, so a corner with no buttons costs the
    // bar nothing.
    Component {
        id: leadingControlsComponent

        WindowControls {
            side: "leading"
            window: root.window
            controller: root.controller
        }
    }
    Component {
        id: trailingControlsComponent

        WindowControls {
            side: "trailing"
            window: root.window
            controller: root.controller
        }
    }

    // One header-bar item: a toolbar-sized button showing the item's freedesktop icon (the bundled one where
    // the theme lacks it), which asks the controller to run the item -- the main menu pops below it.
    Component {
        id: headerItemComponent

        ToolButton {
            id: headerButton

            required property var modelData

            objectName: headerButton.modelData.objectName
            implicitWidth: chromeStyle.size.toolbarButton
            implicitHeight: chromeStyle.size.toolbarButton
            padding: 0
            focusPolicy: Qt.NoFocus
            display: AbstractButton.IconOnly
            icon.name: headerButton.modelData.iconName
            icon.source: headerButton.modelData.bundledIcon
            // The window buttons' ink, so the items and the buttons beside them read as one set.
            icon.color: root.window && root.window.active ? chromeStyle.colors.windowButtonGlyph
                                                          : chromeStyle.colors.captionGlyphInactive
            icon.width: chromeStyle.size.headerIcon
            icon.height: chromeStyle.size.headerIcon
            contentItem: SymbolicIcon {
                objectName: headerButton.modelData.objectName + "Icon"
                icon: headerButton.icon
            }
            Accessible.role: Accessible.Button
            Accessible.name: headerButton.modelData.accessibleName
            onClicked: {
                if (root.controller)
                    root.controller.activateHeaderBarItem(headerButton.modelData.item,
                                                          headerButton.mapToItem(null, 0, headerButton.height));
            }
            background: Rectangle {
                radius: chromeStyle.radii.control
                color: headerButton.down ? chromeStyle.colors.controlPressedFill
                     : headerButton.hovered ? chromeStyle.colors.tabHoverFill
                     : "transparent"
            }

            KitToolTipHost {
                text: headerButton.modelData.accessibleName
                shortcutCommand: headerButton.modelData.command
                toolTipName: headerButton.modelData.objectName + "ToolTip"
            }

            // The new-tab item opens the profiles on a secondary click, as the band's "+" does.
            readonly property bool isNewTab: headerButton.modelData.menu === "newTab"
            TapHandler {
                enabled: headerButton.isNewTab
                acceptedButtons: Qt.RightButton
                gesturePolicy: TapHandler.ReleaseWithinBounds
                onTapped: {
                    if (root.controller)
                        root.controller.openNewTabMenu(headerButton.mapToItem(null, 0, headerButton.height))
                }
            }
            Component.onCompleted: if (headerButton.modelData.menu !== "")
                root.headerMenuAnchors[headerButton.modelData.menu] = headerButton
        }
    }

    // The header bar's items at one end, in the table's order; empty off the header bar.
    // @param slot "leading" or "trailing".
    function headerItemsAt(slot) {
        if (!root.isHeader)
            return [];
        return chromeStyle.headerBarItems.filter(row => root.header.slots[row.item] === slot);
    }

    // What the bar's parts before and after the tabs take, from their preferred widths, which do not
    // depend on the tabs: the tabs are left the rest, less the drag reserve.
    readonly property real leadingPartsWidth: barRow.anchors.leftMargin + leadingSlot.Layout.preferredWidth
                                              + leadingSeparator.Layout.preferredWidth
                                              + leadingSeparator.Layout.leftMargin + leadingSeparator.Layout.rightMargin
                                              + leadingItems.Layout.preferredWidth + leadingItems.Layout.rightMargin
                                              + hostChipSlot.Layout.preferredWidth
                                              + hostChipSlot.Layout.leftMargin + hostChipSlot.Layout.rightMargin
    readonly property real trailingPartsWidth: secureChipSlot.Layout.preferredWidth + secureChipSlot.Layout.rightMargin
                                               + driftSlotItem.Layout.preferredWidth + driftSlotItem.Layout.rightMargin
                                               + newTabSlot.Layout.preferredWidth + newTabSlot.Layout.rightMargin
                                               + trailingItems.Layout.preferredWidth + trailingItems.Layout.leftMargin
                                               + trailingSeparator.Layout.preferredWidth
                                               + trailingSeparator.Layout.leftMargin + trailingSeparator.Layout.rightMargin
                                               + trailingSlot.Layout.preferredWidth + barRow.anchors.rightMargin
    readonly property real fixedPartsWidth: root.leadingPartsWidth + root.trailingPartsWidth
    // What the tabs may take with the secure-input chip's slot left out of the other parts.
    readonly property real tabRoomWithoutChip: Math.max(0, root.width - root.leadingPartsWidth
                                                        - driftSlotItem.Layout.preferredWidth - driftSlotItem.Layout.rightMargin
                                                        - newTabSlot.Layout.preferredWidth - newTabSlot.Layout.rightMargin
                                                        - trailingSlot.Layout.preferredWidth
                                                        - chromeStyle.space.dragReserve)

    // The room the centred title has: the bar less twice the wider of its two ends, so it stays centred
    // on the bar without running under either end.
    readonly property real titleRoom: Math.max(0, root.width - 2 * Math.max(root.leadingPartsWidth,
                                                                            root.trailingPartsWidth))

    RowLayout {
        id: barRow
        anchors.fill: parent
        // The header bar is inset from the window's edges; the band sits flush.
        anchors.leftMargin: root.isHeader ? chromeStyle.space.s4 : 0
        anchors.rightMargin: root.isHeader ? chromeStyle.space.s4 : 0
        spacing: 0

        // Leading slot: macOS puts its traffic lights here, before the tabs.
        Loader {
            id: leadingSlot
            objectName: "leadingWindowControls"
            Layout.fillHeight: true
            active: root.isBand && root.useCustomWindowControls && !!root.controller
                    && windowControls.leadingButtons.length > 0
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
            sourceComponent: leadingControlsComponent
        }

        // The header bar's separator after the leading window buttons.
        Rectangle {
            id: leadingSeparator
            objectName: "leadingControlsSeparator"
            readonly property bool shown: root.isHeader && root.header.leadingSeparator
            visible: leadingSeparator.shown
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: leadingSeparator.shown ? chromeStyle.borderWidth : 0
            Layout.preferredHeight: chromeStyle.size.controlsSeparator
            Layout.leftMargin: leadingSeparator.shown ? chromeStyle.space.s3 : 0
            Layout.rightMargin: leadingSeparator.shown ? chromeStyle.space.s3 : 0
            color: chromeStyle.colors.headerSeparator
        }

        // The header bar's leading items (the new-tab button).
        RowLayout {
            id: leadingItems
            readonly property var rows: root.headerItemsAt("leading")
            visible: leadingItems.rows.length > 0
            spacing: chromeStyle.space.s3
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: leadingItems.visible ? leadingItems.implicitWidth : 0
            Layout.rightMargin: leadingItems.visible ? chromeStyle.space.s3 : 0
            Repeater {
                model: leadingItems.rows
                delegate: headerItemComponent
            }
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
                focusRingShown: activeFocus && root.toolbarHasFocus
            }
        }

        // The tabs, while the middle holds them. A Loader, so a band showing the title holds no second
        // strip (with its own tab menu) beside the tabs bar's.
        Loader {
            id: tabStripLoader
            // Whether the strip in it can show a tab's lock; false without a strip (Main.qml.tabsOnScreen).
            // Asked of the room the tabs have without the lock's own chip: the chip shows only where the tabs
            // cannot hold the lock, so counting its width here would make the answer depend on itself.
            readonly property bool hostsLock: item !== null
                && item.hostsLockWithin(root.tabRoomWithoutChip - item.fixedWidth)
            active: root.centre === "tabs"
            Layout.fillHeight: true
            // As wide as the tabs need: the strip caps itself at availableWidth, so the drag region below
            // keeps at least its reserve.
            Layout.preferredWidth: active ? implicitWidth : 0
            // Joined tabs are the title bar: they start the kit's inset in from the band's leading edge.
            Layout.leftMargin: active && root.isJoined ? chromeStyle.space.tabStripLeading : 0
            sourceComponent: TabStrip {
                id: tabStrip
                objectName: "tabStrip"
                // What the tabs may take: the bar less its other parts, the drag reserve and the strip's
                // own buttons. Read from preferred widths, which do not depend on the strip.
                availableWidth: Math.max(0, root.width - root.fixedPartsWidth - tabStripLoader.Layout.leftMargin
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

        // The lock, as a band chip, while the active tab's pane is at a password prompt and neither a pane
        // header nor the tab well is there to show it: one tab, one pane (window::SecureIndicatorRules).
        Item {
            id: secureChipSlot
            // The panes' headers are not known here, and may be hidden (density, a short pane): the chip is
            // shown whatever they do, so that two indicators beat none.
            readonly property int headerSpot: chromeStyle.secureInputs.spotHidden
            // The tabs on screen now (Main.qml): in a strip, or in a band that rests on screen.
            readonly property int wellSpot: root.window && root.window.tabsOnScreen
                                            ? chromeStyle.secureInputs.spotShown : chromeStyle.secureInputs.spotHidden
            // A band that only appears on demand (full screen) is not one the lock can rely on.
            readonly property int bandSpot: root.presence === "shown" || root.presence === "pinned"
                                            ? chromeStyle.secureInputs.spotShown : chromeStyle.secureInputs.spotHidden
            readonly property bool shown: root.isBand && titleRows.activeLocked
                && chromeStyle.secureIndicatorSpots(headerSpot, wellSpot, bandSpot).band === true
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: shown ? secureChip.implicitWidth : 0
            Layout.preferredHeight: secureChip.implicitHeight
            Layout.rightMargin: shown ? root.gap : 0

            Item {
                id: secureChip
                objectName: "secureInputChip"
                visible: secureChipSlot.shown
                readonly property bool bracketed: chromeStyle.chipOpenGlyph !== ""
                // The chip's padding at each end, unless its cells bracket it.
                readonly property real padding: bracketed ? 0 : chromeStyle.space.toolbarChipPadding
                implicitWidth: chipRow.implicitWidth + padding + padding
                implicitHeight: chromeStyle.size.toolbarChip
                width: implicitWidth
                height: implicitHeight
                Accessible.role: Accessible.Indicator
                Accessible.name: chromeStyle.secureInputText(titleRows.activeIndicator)

                Rectangle {
                    anchors.fill: parent
                    radius: chromeStyle.radii.toolbarChip
                    color: chromeStyle.colors.chipFill
                }

                Row {
                    id: chipRow
                    anchors.centerIn: parent
                    spacing: chromeStyle.space.s3

                    SecureInputLock {
                        id: chipLock
                        indicator: titleRows.activeIndicator
                        anchors.verticalCenter: parent.verticalCenter
                        color: chromeStyle.colors.chipText
                        Accessible.ignored: true
                    }

                    // The words beside the icon; the Terminal row's cell text already says it.
                    Text {
                        objectName: "secureInputChipText"
                        visible: !chipLock.drawsText
                        anchors.verticalCenter: parent.verticalCenter
                        text: chromeStyle.secureInputText(titleRows.activeIndicator)
                        font: chromeStyle.type.footnote
                        color: chromeStyle.colors.chipText
                        Accessible.ignored: true
                    }
                }
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
        // click or a long press opens the profiles, as the macOS "+" does (layout.profileMenu). Not on a
        // bare band (Minimal, one tab): it holds the traffic lights only.
        Loader {
            id: newTabSlot
            // The header bar draws its own new-tab item instead.
            active: root.isBand && root.isPill && !root.isHeader && root.centre !== "empty"
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: active ? implicitWidth : 0
            Layout.rightMargin: active ? chromeStyle.space.s4 : 0
            sourceComponent: ToolbarButton {
                id: newTabButton
                objectName: "newTabButton"
                glyph: chromeStyle.newTabGlyph
                accessibleName: qsTr("New tab")
                toolTipCommand: "CreateNewTab"
                accessibleDescription: qsTr("Right-click or press and hold for a new tab with a profile")
                focusRingShown: activeFocus && root.toolbarHasFocus
                onClicked: if (root.controller) root.controller.createNewTab()
                onSecondaryClicked: {
                    if (root.controller)
                        root.controller.openNewTabMenu(newTabButton.mapToItem(null, 0, newTabButton.height))
                }
            }
        }

        // The header bar's trailing items (search, the main menu).
        RowLayout {
            id: trailingItems
            readonly property var rows: root.headerItemsAt("trailing")
            visible: trailingItems.rows.length > 0
            spacing: chromeStyle.space.s3
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: trailingItems.visible ? trailingItems.implicitWidth : 0
            Layout.leftMargin: trailingItems.visible ? chromeStyle.space.s3 : 0
            Repeater {
                model: trailingItems.rows
                delegate: headerItemComponent
            }
        }

        // The header bar's separator before the trailing window buttons.
        Rectangle {
            id: trailingSeparator
            objectName: "trailingControlsSeparator"
            readonly property bool shown: root.isHeader && root.header.trailingSeparator
            visible: trailingSeparator.shown
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: trailingSeparator.shown ? chromeStyle.borderWidth : 0
            Layout.preferredHeight: chromeStyle.size.controlsSeparator
            Layout.leftMargin: trailingSeparator.shown ? chromeStyle.space.s3 : 0
            Layout.rightMargin: trailingSeparator.shown ? chromeStyle.space.s3 : 0
            color: chromeStyle.colors.headerSeparator
        }

        // Trailing slot: where Windows and Breeze put theirs, after the drag region.
        Loader {
            id: trailingSlot
            objectName: "trailingWindowControls"
            Layout.fillHeight: true
            active: root.isBand && root.useCustomWindowControls && !!root.controller
                    && windowControls.trailingButtons.length > 0
            // Zero while inactive, for the reason the leading slot states.
            Layout.preferredWidth: active ? implicitWidth : 0
            sourceComponent: trailingControlsComponent
        }
    }

    // The active session's title, centred on the band while its middle holds no tabs: one tab on macOS.
    // Read from the active row's title role, so it is the label its tab would show.
    //
    // While the active session's sticky command header lives in the band (window::StickyHeaderHost::TitleBand:
    // the tab's only pane, with no header of its own), the title is that header, `✓ cmake --build · 2m 14s`,
    // with a link after it that jumps to the command.
    readonly property var sticky: root.controller && root.controller.activeStickyHeader !== undefined
                                  ? root.controller.activeStickyHeader : ({})
    readonly property bool stickyShown: root.sticky.command !== undefined && root.sticky.command !== ""
        && root.sticky.host === chromeStyle.blocks.hostTitleBand
    readonly property string stickyText: root.stickyShown
        ? root.sticky.glyph + " " + root.sticky.command
          + (root.sticky.detail !== "" ? qsTr(" · ") + root.sticky.detail : "")
        : ""

    /// What a screen reader says for the sticky header: the text without its duration, which ticks every second
    /// while the block runs. Announced when the command or its outcome's glyph changes, not at every tick.
    readonly property string stickyName: root.stickyShown ? root.sticky.glyph + " " + root.sticky.command : ""

    Row {
        id: bandTitleRow
        objectName: "bandTitleRow"
        visible: root.isBand && root.centre === "title"
        anchors.centerIn: parent
        spacing: chromeStyle.space.s3

        Text {
            id: bandTitle
            objectName: "bandTitle"
            // The jump link keeps its room; the title gives way to it.
            width: Math.min(implicitWidth, Math.max(0, root.titleRoom - (bandJump.visible ? bandJump.width + bandTitleRow.spacing : 0)))
            text: root.stickyShown ? root.stickyText : titleRows.activeTitle
            Accessible.role: Accessible.StaticText
            Accessible.name: root.stickyShown ? root.stickyName : text
            font: chromeStyle.type.bodyMedium
            color: chromeStyle.colors.tabText
            elide: Text.ElideMiddle
        }

        Text {
            id: bandJump
            objectName: "bandJump"
            visible: root.stickyShown
            anchors.verticalCenter: bandTitle.verticalCenter
            text: root.stickyShown ? root.sticky.jumpLabel : ""
            font: chromeStyle.type.footnote
            color: chromeStyle.paletteRevision < 0 ? "transparent"
                 : chromeStyle.statusTextOn(chromeStyle.statusRunning, root.fill)
            Accessible.role: Accessible.Button
            Accessible.name: text
            Accessible.onPressAction: if (root.controller) root.controller.jumpToActiveStickyHeader()

            TapHandler {
                gesturePolicy: TapHandler.ReleaseWithinBounds
                onTapped: if (root.controller) root.controller.jumpToActiveStickyHeader()
            }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
        }
    }

    // One object per tab, carrying its title and whether it is active, while the band shows a title.
    Instantiator {
        id: titleRows
        // Also while the band may hold the secure-input chip: it reads whether the active tab is locked.
        active: bandTitle.visible || root.isBand
        model: root.controller
        delegate: QtObject {
            required property string title
            required property bool isActive
            // Through the model object, as TabStrip reads it: a model without the role locks nothing.
            required property var model
            readonly property int secureState: model.tabSecure ?? 0
            readonly property bool locked: secureState !== 0
        }

        // What the active tab's lock says (a window::SecureIndicator's value), none without one.
        readonly property int activeIndicator: {
            for (let index = 0; index < titleRows.count; ++index) {
                const row = titleRows.objectAt(index);
                if (row && row.isActive)
                    return row.secureState;
            }
            return chromeStyle.secureInputs.none;
        }

        // Whether the active tab has a pane at a password prompt.
        readonly property bool activeLocked: {
            for (let index = 0; index < titleRows.count; ++index) {
                const row = titleRows.objectAt(index);
                if (row && row.isActive)
                    return row.locked;
            }
            return false;
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

    // The main menu, the header bar's only: the menu bar's menus as submenus, popped below its button.
    Loader {
        id: mainMenuLoader
        active: root.isHeader
        sourceComponent: ActionContextMenu {
            objectName: "mainMenu"
            entries: root.controller ? (root.controller.mainMenuModel ?? []) : []
            onPicked: (actionId) => { if (root.controller) root.controller.triggerMainMenuAction(actionId) }
        }
    }

    Connections {
        target: root.controller
        enabled: root.isHeader
        ignoreUnknownSignals: true
        function onMainMenuRequested() {
            const menu = mainMenuLoader.item;
            if (!menu)
                return;
            const anchor = root.headerMenuAnchors["main"];
            if (anchor)
                menu.popup(anchor, 0, anchor.height);
            else
                menu.popup();
        }
    }

    Connections {
        target: root.controller
        enabled: root.isBand
        function onNewTabMenuRequested() {
            const menu = newTabMenuLoader.item;
            if (!menu)
                return;
            const anchor = root.isHeader ? root.headerMenuAnchors["newTab"]
                         : root.isPill ? newTabSlot.item
                         : tabStripLoader.item ? tabStripLoader.item.newTabMenuAnchor : null;
            if (anchor)
                menu.popup(anchor, 0, anchor.height);
            else
                menu.popup();
        }
    }
}
