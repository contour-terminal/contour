// vim:syntax=qml
// Right-click context menu for a terminal pane.
//
// The menu's STRUCTURE is not written here. It arrives as data from C++ (WindowController.contextMenuModel,
// produced by the pure table in ContextMenu.h), and this file only knows how to turn one ROW into one menu
// entry. That is what keeps "what is in the menu, and when is each entry shown or grayed out" in a single
// headlessly testable place, and keeps the QML from growing a second copy of the command list that could
// drift from the first.
import QtQuick
import QtQuick.Controls

Menu {
    id: root

    // Force the in-scene (item) popup rather than a native OS menu. Since Qt 6.8 a Controls Menu defaults
    // to a native platform menu where one exists, and a native menu cannot host the sub-menus this file
    // creates at runtime — the same trap that made TabContextMenu come up EMPTY on Windows. Fusion (pinned
    // app-wide in ContourGuiApp) then draws an opaque, themed surface on every platform.
    popupType: Popup.Item

    // The WindowController.
    required property var controller

    // The menu, as data. One row is
    //   { kind: "command" | "separator" | "submenu",
    //     title, enabled, checkable, checked, actionId, children: [rows] }
    property var entries: []

    // Objects this file created, so a rebuild can destroy exactly what it made. A sub-menu owns its own
    // children, so only the top-level ones are tracked here.
    property var _created: []

    // Rebuilt whenever the model changes, and the model is republished on every right-click — so the menu
    // that pops is the menu for the state the user actually clicked in. NOT rebuilt in an "about to show"
    // hook: doing it on model change instead means the rows exist as soon as the component is complete,
    // which is what lets QmlComponents_test assert them offscreen, where a popup has no overlay to open
    // into and popup() can never be called.
    Component.onCompleted: root.rebuild()
    onEntriesChanged: root.rebuild()

    Component {
        id: commandEntry
        MenuItem {
            property int actionId: -1
            onTriggered: root.controller.triggerContextMenuAction(actionId)
        }
    }

    Component {
        id: separatorEntry
        MenuSeparator {}
    }

    Component {
        id: submenuEntry
        // popupType is per-Popup, so a sub-menu needs the same in-scene treatment as the root does.
        Menu {
            popupType: Popup.Item
        }
    }

    // Appends `rows` to `menu`, recursing into sub-menus. Returns the objects it created, so the caller
    // can destroy them on the next rebuild.
    //
    // MenuItems and MenuSeparators are created parented to `menu.contentItem`, NOT to `menu`: a Menu is a
    // Popup and therefore not itself a graphical parent, and creating a QQuickItem under one earns a
    // "Created graphical object was not placed in the graphics scene" warning (which the test harness
    // treats as a failure, rightly). addItem() then re-parents them where they belong. A sub-menu is
    // itself a Popup rather than an Item, so it is created under `menu` and goes in via addMenu() —
    // addItem() cannot take one.
    function fillMenu(menu, rows) {
        let created = [];
        if (!rows)
            return created;

        for (let i = 0; i < rows.length; ++i) {
            const row = rows[i];

            if (row.kind === "separator") {
                const sep = separatorEntry.createObject(menu.contentItem);
                menu.addItem(sep);
                created.push(sep);
            } else if (row.kind === "submenu") {
                const sub = submenuEntry.createObject(menu, { "title": row.title });
                root.fillMenu(sub, row.children);
                menu.addMenu(sub);
                created.push(sub);
            } else {
                const item = commandEntry.createObject(menu.contentItem, {
                    "text": row.title,
                    "enabled": row.enabled,
                    "checkable": row.checkable,
                    "checked": row.checked,
                    "actionId": row.actionId
                });
                menu.addItem(item);
                created.push(item);
            }
        }

        return created;
    }

    function rebuild() {
        if (!root.controller)
            return;

        while (root.count > 0) {
            // A sub-menu does not sit in the content model itself — a Menu is a Popup, not an Item. Qt
            // represents it there with a proxy MenuItem that addMenu() creates and Qt owns. takeItem()
            // hands that proxy back WITHOUT destroying it, and it is not among the objects this file
            // created, so nothing would ever destroy it: two fully-built controls leaked on every single
            // right-click, for as long as the window lives. takeMenu() is the call that disposes of it.
            if (root.menuAt(0))
                root.takeMenu(0);
            else
                root.takeItem(0);
        }

        for (let i = 0; i < root._created.length; ++i)
            root._created[i].destroy();

        root._created = root.fillMenu(root, root.entries);
    }
}
