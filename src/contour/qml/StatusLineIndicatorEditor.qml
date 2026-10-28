// vim:syntax=qml
// Visual builder for the indicator status line.
//
// The indicator is configured as three template strings — left, middle and right — of the form
// "{InputMode:Bold,Color=#FFFF00}{Tabs:ActiveColor=#FFFF00,Left= │ }". This editor shows each segment as a
// row of chips, one per placeholder; the status line page above it shows the finished status line, live.
// Clicking a chip opens its full style sheet: every cell flag the template grammar accepts, both colours,
// the side adornments, and whatever else that particular placeholder carries.
//
// Three things worth knowing before changing anything here:
//
// * The controller is the single source of truth. The item lists are derived from it on every draft change
//   rather than cached locally, so there is no second copy to drift and no re-entry guard to get wrong.
//   `_revision` exists only to give those bindings something to depend on, because indicatorSegment() is
//   an invokable and a binding cannot see through a function call by itself.
// * The placeholder catalog and the flag list come from C++ (SettingsController::indicatorPlaceholders /
//   indicatorFlags, both reading vtbackend::StatusLineDefinitions). Listing them here as well is what
//   previously let the picker offer a set of placeholders that disagreed with the parser's.
// * Every mutation goes through commit(), which serializes the whole segment. `editable` is checked there
//   as well as on each control, because a disabled control is a courtesy and the single write path is the
//   guarantee (the controller refuses a read-only profile too).
//
// The chips, the segment rows and the colour fields live in their own files. That is not only tidiness:
// an inline component is not usable as a Repeater delegate here — it silently instantiates nothing, with
// no warning, which rendered every segment empty.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root

    property var controller: null
    property bool editable: true

    /// Bumped on every draft change, purely so the item-list bindings below have a dependency to
    /// re-evaluate on. See the note above.
    property int _revision: 0

    spacing: chromeStyle.space.s5

    Connections {
        target: root.controller
        function onDraftChanged() { root._revision++ }
    }

    // {{{ Derived state

    /// The parsed items of one segment.
    function itemsOf(segmentIndex) {
        // _revision has to be *used*, not merely mentioned: a bare `root._revision` statement has no
        // effect, so the JS engine drops it and the binding is left with no dependency on it at all —
        // which left the chips showing whatever the segments held when the page was built, forever.
        if (root._revision < 0 || !root.controller)
            return []
        return root.controller.parseIndicatorSegment(root.controller.indicatorSegment(segmentIndex))
    }

    /// One segment's raw template, as the config file holds it.
    function rawOf(segmentIndex) {
        if (root._revision < 0 || !root.controller)
            return ""
        return root.controller.indicatorSegment(segmentIndex)
    }

    readonly property var leftItems: root.itemsOf(0)
    readonly property var middleItems: root.itemsOf(1)
    readonly property var rightItems: root.itemsOf(2)

    function segmentItems(segmentIndex) {
        return segmentIndex === 0 ? root.leftItems
             : segmentIndex === 1 ? root.middleItems
                                  : root.rightItems
    }

    /// Every placeholder the editor may offer, from C++.
    readonly property var placeholders: root.controller ? root.controller.indicatorPlaceholders() : []
    /// Every cell flag an item may carry, from C++, as [{ key, label }].
    readonly property var flagCatalog: root.controller ? root.controller.indicatorFlags() : []
    /// Just the flag keys, for the chips' style badges.
    readonly property var flagKeys: {
        var out = []
        for (var i = 0; i < root.flagCatalog.length; ++i)
            out.push(root.flagCatalog[i].key)
        return out
    }

    // }}}
    // {{{ Mutation

    /// Serializes @p items into segment @p segmentIndex. The single write path.
    function commit(segmentIndex, items) {
        if (!root.controller || !root.editable)
            return
        root.controller.setIndicatorSegment(segmentIndex, root.controller.serializeIndicatorSegment(items))
    }

    /// A shallow copy of a segment's items, safe to mutate before committing.
    function copyOf(segmentIndex) {
        var out = []
        var items = root.segmentItems(segmentIndex)
        for (var i = 0; i < items.length; ++i)
            out.push(items[i])
        return out
    }

    function addPlaceholder(segmentIndex, entry) {
        var items = root.copyOf(segmentIndex)
        // Only the keys that differ from the C++ defaults need setting: serializeIndicatorSegment reads a
        // missing key as absent/false.
        var added = {
            "type": entry.type,
            "label": entry.label,
            "displayName": entry.label,
            "sample": entry.sample,
            "flags": ({}),
            "hasColor": false, "color": "",
            "hasBgColor": false, "bgColor": "",
            "textLeft": "", "textRight": ""
        }
        if (entry.type === "Text")
            added.text = qsTr("text")
        if (entry.type === "Command")
            added.command = ""
        if (entry.type === "Tabs") {
            added.hasActiveColor = false
            added.activeColor = ""
            added.hasActiveBackground = false
            added.activeBackground = ""
            added.separator = ""
        }
        items.push(added)
        root.commit(segmentIndex, items)
    }

    function removeItem(segmentIndex, index) {
        var items = root.copyOf(segmentIndex)
        if (index < 0 || index >= items.length)
            return
        items.splice(index, 1)
        root.commit(segmentIndex, items)
    }

    /// Moves the item at @p index by @p delta places. A move past either end is dropped.
    function moveItem(segmentIndex, index, delta) {
        var items = root.copyOf(segmentIndex)
        var target = index + delta
        if (index < 0 || index >= items.length || target < 0 || target >= items.length)
            return
        var moved = items.splice(index, 1)[0]
        items.splice(target, 0, moved)
        root.commit(segmentIndex, items)
    }

    function replaceItem(segmentIndex, index, item) {
        var items = root.copyOf(segmentIndex)
        if (index < 0 || index >= items.length)
            return
        items[index] = item
        root.commit(segmentIndex, items)
    }

    function setRaw(segmentIndex, text) {
        if (!root.controller || !root.editable)
            return
        root.controller.setIndicatorSegment(segmentIndex, text)
    }

    // }}}

    // {{{ The three segment rows
    //
    // Written out rather than repeated over a three-element model: left, middle and right are a fixed,
    // individually named set, not a collection that varies, and each row wants its own index baked into
    // its handlers anyway.
    StatusLineSegmentRow {
        objectName: "indicatorSegmentLeft"
        Layout.fillWidth: true
        segmentLabel: qsTr("Left")
        items: root.leftItems
        placeholders: root.placeholders
        flagKeys: root.flagKeys
        rawTemplate: root.rawOf(0)
        editable: root.editable
        onAddRequested: (entry) => root.addPlaceholder(0, entry)
        onRemoveRequested: (index) => root.removeItem(0, index)
        onRawCommitted: (text) => root.setRaw(0, text)
        onEditRequested: (index) => stylePopup.edit(0, index, root.leftItems.length, root.leftItems[index])
    }

    StatusLineSegmentRow {
        objectName: "indicatorSegmentMiddle"
        Layout.fillWidth: true
        segmentLabel: qsTr("Middle")
        items: root.middleItems
        placeholders: root.placeholders
        flagKeys: root.flagKeys
        rawTemplate: root.rawOf(1)
        editable: root.editable
        onAddRequested: (entry) => root.addPlaceholder(1, entry)
        onRemoveRequested: (index) => root.removeItem(1, index)
        onRawCommitted: (text) => root.setRaw(1, text)
        onEditRequested: (index) => stylePopup.edit(1, index, root.middleItems.length, root.middleItems[index])
    }

    StatusLineSegmentRow {
        objectName: "indicatorSegmentRight"
        Layout.fillWidth: true
        segmentLabel: qsTr("Right")
        items: root.rightItems
        placeholders: root.placeholders
        flagKeys: root.flagKeys
        rawTemplate: root.rawOf(2)
        editable: root.editable
        onAddRequested: (entry) => root.addPlaceholder(2, entry)
        onRemoveRequested: (index) => root.removeItem(2, index)
        onRawCommitted: (text) => root.setRaw(2, text)
        onEditRequested: (index) => stylePopup.edit(2, index, root.rightItems.length, root.rightItems[index])
    }
    // }}}

    // {{{ Style-editing dialog
    StatusLineItemDialog {
        id: stylePopup
        flagCatalog: root.flagCatalog
        colorRoles: root.controller ? root.controller.indicatorColorRoles() : []
        onApplied: (segmentIndex, itemIndex, item) => root.replaceItem(segmentIndex, itemIndex, item)
        onRemoved: (segmentIndex, itemIndex) => root.removeItem(segmentIndex, itemIndex)
        onMoved: (segmentIndex, itemIndex, delta) => root.moveItem(segmentIndex, itemIndex, delta)
    }
    // }}}
}
