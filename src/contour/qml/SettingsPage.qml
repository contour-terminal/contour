// vim:syntax=qml
// The Settings window's content: a sidebar of the sections, the profiles and "Edit contour.yml…" beside the
// page the sidebar selects, under the page header and the banners.
//
// The page binds to the SettingsController its `host` (the SettingsWindowHost) owns. Everything applies as it
// is changed, and "Revert Changes" puts back what this session changed; the page never writes contour.yml.
// What the layout looks like -- where the title sits, whether the rows are one card or each its own, how a
// stepper's arrows sit -- is `chromeStyle.settingsTraits`, and every extent, colour and type size comes from
// the chrome tokens, so the page never learns which style is active.
//
// Pages are named: "section:<id>", "profile:<name>", and under a profile "profile:<name>:schemes", ":links" and
// ":statusline", and "host:<name>". `currentPage` is the page shown; `openPage` opens one.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    objectName: "settingsPage"

    /// The SettingsWindowHost: the controller the page edits, the Settings window's close, and "Edit
    /// contour.yml…".
    property var host: null
    /// The SettingsController. Null-guarded throughout: the controllers are torn down before the QML tree
    /// on window close, and bindings re-evaluate once against null during teardown.
    property var controller: host ? host.controller : null
    /// The page shown, as a host remembers it.
    property string currentPage: "section:general"
    /// What a frameless window's header row works its window through, as WindowControls asks of a
    /// controller (SettingsWindow.qml); null while the OS draws the frame. Set, the header row is the
    /// window's title bar: dragged, it moves the window, and double-clicked, it maximises it.
    property var windowController: null
    /// The window controls a frameless window draws at the header row's ends; null draws none.
    property Component leadingControls: null
    property Component trailingControls: null

    // The kinds of page, taken from the page's id.
    readonly property var _parts: root.currentPage.split(":")
    readonly property string _kind: root._parts[0]
    readonly property string _subpage: root._kind === "profile" && root._parts.length > 2 ? root._parts[2] : ""
    readonly property string _profileName: root._kind === "profile" ? root._parts[1] : ""
    readonly property bool _isProfilePage: root._kind === "profile"
    readonly property string _hostName: root._kind === "host" ? root._parts.slice(1).join(":") : ""
    readonly property bool _isHostPage: root._kind === "host"
    readonly property var _sections: root.controller ? root.controller.sections : []
    readonly property var _traits: chromeStyle.settingsTraits
    readonly property bool _splitHeader: root._traits.header === root._traits.splitHeaderBar
    // The page's pages keep a margin at each side.
    readonly property real _pageMargins: 2 * chromeStyle.space.s8
    readonly property bool _locked: root.controller !== null && root.controller.locked
    // Where an id remembered by an older release now lives.
    readonly property var _legacyPages: ({ "globals": "section:general", "keybindings": "section:keyboard" })

    /// The title of the page shown.
    readonly property string pageTitle: {
        if (root._isProfilePage) {
            const subtitle = root._subpageTitles[root._subpage];
            if (root._traits.header === root._traits.breadcrumb) {
                const trail = qsTr("Profiles") + " › " + root._profileName;
                return subtitle ? trail + " › " + subtitle : trail;
            }
            return subtitle ? subtitle : root._profileName;
        }
        if (root._isHostPage)
            return root._traits.header === root._traits.breadcrumb ? qsTr("Hosts") + " › " + root._hostName
                                                                   : root._hostName;
        const section = root._sections.find(row => row.id === root._parts[1]);
        return section ? section.title : "";
    }
    readonly property var _subpageTitles: ({
        "schemes": qsTr("Colors"),
        "links": qsTr("Links"),
        "statusline": qsTr("Status Line")
    })

    /// Opens the page @p id names.
    /// @return Whether it opened it: a page or a profile this release does not know leaves the page as it is.
    function openPage(id) {
        const wanted = root._legacyPages[id] !== undefined ? root._legacyPages[id] : id;
        const parts = wanted.split(":");
        if (!root.controller)
            return false;
        if (parts[0] === "section") {
            if (!root._sections.some(row => row.id === parts[1]))
                return false;
        } else if (parts[0] === "profile") {
            if (!root.controller.profiles.some(row => row.name === parts[1]))
                return false;
            if (parts.length > 2 && root._subpageTitles[parts[2]] === undefined)
                return false;
            // The profile the controller edits is the one shown, whichever way it was opened.
            if (root.controller.editingProfile !== parts[1])
                root.controller.editProfile(parts[1]);
        } else if (parts[0] === "host") {
            const name = parts.slice(1).join(":");
            if (!root.controller.hosts.some(row => row.name === name))
                return false;
            // The host the controller edits is the one shown, whichever way it was opened.
            if (root.controller.editingHost !== name)
                root.controller.editHost(name);
        } else {
            return false;
        }
        root.currentPage = wanted;
        return true;
    }

    // Why the controller last refused what the page asked of it -- a value the configuration would reject, a
    // name already taken, a file it could not write -- kept until the next change it saves, or until the page
    // or the draft it was about changes. The controller reports a refusal only through errorOccurred, and
    // every change it saves (and every reopening of the page) through changed().
    property string errorMessage: ""
    // The row a search result asked to be shown, once its page is.
    property string _pendingReveal: ""

    Connections {
        target: root.controller
        function onErrorOccurred(message) {
            root.errorMessage = message;
            // Said as well as shown: a screen reader hears the edited value revert, and otherwise not why.
            errorBanner.Accessible.announce(message);
        }
        function onChanged() {
            root.errorMessage = "";
            root.followProfile();
            root.followHost();
        }
        function onHostRenamed(oldName, newName) {
            if (root._isHostPage && root._hostName === oldName)
                root.currentPage = "host:" + newName;
        }
        function onHostDraftChanged() { root.errorMessage = "" }
        function onProfileRenamed(oldName, newName) {
            // The page of the renamed profile stays open, on the same subpage, under its new name.
            if (root._isProfilePage && root._profileName === oldName)
                root.currentPage = ["profile", newName].concat(root._parts.slice(2)).join(":");
        }
        function onDraftChanged() { root.errorMessage = "" }
        function onGlobalFieldsChanged() { root.errorMessage = "" }
        function onSchemeDraftChanged() { root.errorMessage = "" }
        function onEditingSchemeChanged() { root.errorMessage = "" }
    }
    onCurrentPageChanged: {
        root.errorMessage = "";
        pageFade.restart();
    }

    /// A profile page whose profile is gone falls back to the Profiles page.
    function followProfile() {
        // A profile staged and not yet written is not in the list, but is the one the controller edits.
        if (root._isProfilePage && root.controller && root.controller.editingProfile !== root._profileName
            && !root.controller.profiles.some(row => row.name === root._profileName))
            root.currentPage = "section:profiles";
    }

    /// A host page whose host is gone falls back to the Hosts page.
    function followHost() {
        if (root._isHostPage && root.controller && !root.controller.hosts.some(row => row.name === root._hostName))
            root.currentPage = "section:hosts";
    }

    /// Opens the page of the host the controller now edits: after a host was added or duplicated.
    function showEditedHost() {
        if (root.controller && root.controller.editingHost.length > 0)
            root.currentPage = "host:" + root.controller.editingHost;
    }

    /// Opens the page of the profile the controller now edits: after a profile was added or duplicated.
    function showEditedProfile() {
        if (root.controller && root.controller.editingProfile.length > 0)
            root.currentPage = "profile:" + root.controller.editingProfile;
    }

    /// Opens the page a section's link, or a search result for one, names.
    /// @param target A link's target, as `controller.sections` and the search results spell it.
    function openLink(target) {
        if (target === "default-profile-status-line")
            root.openPage("profile:" + root.controller.defaultProfile + ":statusline");
    }

    /// Opens a search result's page and shows its row.
    function openResult(result) {
        if (result.target !== undefined) {
            root.openLink(result.target);
            return;
        }
        if (result.scope === "profile") {
            if (root.controller.editingProfile.length === 0)
                root.controller.editProfile(root.controller.defaultProfile);
            root.currentPage = "profile:" + root.controller.editingProfile;
        } else if (result.scope === "host") {
            // A host's rows are on a host's page: the open one, else the first; with none, the Hosts page.
            if (root.controller.editingHost.length === 0 && root.controller.hosts.length > 0)
                root.controller.editHost(root.controller.hosts[0].name);
            root.currentPage = root.controller.editingHost.length > 0 ? "host:" + root.controller.editingHost
                                                                      : "section:hosts";
        } else {
            root.currentPage = "section:" + result.section;
        }
        root._pendingReveal = result.key;
        Qt.callLater(root.revealPending);
    }

    /// Shows the row a search result asked for, now its page is up.
    function revealPending() {
        const page = pageLoader.item;
        if (root._pendingReveal.length > 0 && page && page.revealRow)
            page.revealRow(root._pendingReveal);
        root._pendingReveal = "";
    }

    // Delete is confirmed through a modal dialog; whatever asked stashes its target here first.
    property string pendingDeleteKind: "" // "profile" | "scheme" | "host" | "linkPattern:<row>"
    property string pendingDeleteName: ""

    function requestDelete(kind, name) {
        root.pendingDeleteKind = kind;
        root.pendingDeleteName = name;
        deleteDialog.open();
    }

    color: chromeStyle.colors.settingsPageFill

    ConfirmDialog {
        id: deleteDialog
        objectName: "deleteDialog"
        readonly property bool isLinkPattern: root.pendingDeleteKind.startsWith("linkPattern:")
        heading: qsTr("Delete “%1”").arg(root.pendingDeleteName)
        confirmText: qsTr("Delete")
        message: root.pendingDeleteKind === "profile"
                 ? qsTr("Delete the profile “%1”? This removes its GUI side file and cannot be undone.").arg(root.pendingDeleteName)
                 : root.pendingDeleteKind === "host"
                 ? qsTr("Delete the host “%1”? This removes its GUI side file and cannot be undone.").arg(root.pendingDeleteName)
                 : isLinkPattern
                   ? qsTr("Remove the link pattern “%1” from this profile?").arg(root.pendingDeleteName)
                   : qsTr("Delete the color scheme “%1”? This removes its GUI side file and cannot be undone.").arg(root.pendingDeleteName)
        onConfirmed: {
            if (!root.controller)
                return;
            if (root.pendingDeleteKind === "profile")
                root.controller.requestDeleteProfile(root.pendingDeleteName);
            else if (root.pendingDeleteKind === "host")
                root.controller.requestDeleteHost(root.pendingDeleteName);
            else if (isLinkPattern)
                root.controller.removeLinkPattern(parseInt(root.pendingDeleteKind.split(":")[1]));
            else
                root.controller.requestDeleteScheme(root.pendingDeleteName);
        }
    }

    // Renaming a profile, or a host, asks for the new name.
    KitDialog {
        id: renameDialog
        objectName: "renameProfileDialog"
        // The host's page renames the host; any other the profile shown.
        readonly property string _target: root._isHostPage ? root._hostName : root._profileName
        heading: qsTr("Rename “%1”").arg(renameDialog._target)
        acceptText: qsTr("Rename")
        acceptEnabled: renameField.text.trim().length > 0
        onOpened: {
            renameField.text = renameDialog._target;
            renameField.selectAll();
            renameField.forceActiveFocus();
        }
        onAccepted: {
            if (!root.controller)
                return;
            if (root._isHostPage)
                root.controller.requestRenameHost(root._hostName, renameField.text.trim());
            else
                root.controller.requestRenameProfile(root._profileName, renameField.text.trim());
        }
        bodyData: [
            TextField {
                id: renameField
                objectName: "renameProfileField"
                Layout.fillWidth: true
                Accessible.name: qsTr("New name")
                onAccepted: renameDialog.pressDefault()
            }
        ]
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        SettingsSidebar {
            id: sidebar
            Layout.preferredWidth: chromeStyle.settings.sidebarWidth
            Layout.fillHeight: true
            controller: root.controller
            currentPage: root.currentPage
            searchInHeader: root._splitHeader
            windowController: root.windowController
            editable: !root._locked
            leadingControls: root.leadingControls
            onOpenRequested: page => root.openPage(page)
            onResultRequested: result => root.openResult(result)
            onEditConfigurationRequested: if (root.host) root.host.editConfigurationFile()
            onAddProfileRequested: {
                root.controller.addProfile();
                root.showEditedProfile();
            }
            onRemoveProfileRequested: root.requestDelete("profile", sidebar.selectedProfile)
            onProfileActionRequested: action => {
                if (action === "duplicate") {
                    root.controller.duplicate(sidebar.selectedProfile);
                    root.showEditedProfile();
                } else if (action === "default") {
                    root.controller.requestDefaultProfile(sidebar.selectedProfile);
                } else {
                    renameDialog.open();
                }
            }
            onAddHostRequested: {
                root.controller.addHost();
                root.showEditedHost();
            }
            onRemoveHostRequested: root.requestDelete("host", sidebar.selectedHost)
            onHostActionRequested: action => {
                if (action === "duplicate") {
                    root.controller.requestDuplicateHost(sidebar.selectedHost);
                    root.showEditedHost();
                } else if (action === "attach") {
                    root.controller.requestAttachHost(sidebar.selectedHost);
                } else {
                    renameDialog.open();
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // The page header: the title, and "Revert Changes".
            Rectangle {
                id: header
                objectName: "settingsHeader"
                Layout.fillWidth: true
                Layout.preferredHeight: chromeStyle.settings.headerBar
                color: root._splitHeader ? chromeStyle.colors.settingsSidebarFill : chromeStyle.colors.settingsPageFill

                // A frameless window's title bar, as the terminal window's (TitleBar.qml).
                TitleBarGestures {
                    objectName: "settingsHeaderGestures"
                    anchors.fill: parent
                    windowController: root.windowController
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: chromeStyle.space.s8
                    anchors.rightMargin: chromeStyle.space.s8
                    spacing: chromeStyle.space.s6

                    Label {
                        objectName: "settingsPageTitle"
                        Layout.fillWidth: true
                        // Beside a kind, the title takes its own width and the kind the rest.
                        Layout.maximumWidth: pageKind.visible ? implicitWidth : Number.POSITIVE_INFINITY
                        text: root.pageTitle
                        font: chromeStyle.settings.pageTitleType > 0
                              ? Qt.font({ family: chromeStyle.type.title.family,
                                          weight: chromeStyle.type.title.weight,
                                          pixelSize: chromeStyle.settings.pageTitleType })
                              : chromeStyle.type.title
                        color: chromeStyle.colors.settingsLabel
                        elide: Text.ElideRight
                        textFormat: Text.PlainText

                        Accessible.role: Accessible.Heading
                        Accessible.name: text
                    }

                    // What the page is about, beside its name: "Host". A breadcrumb already says so.
                    Label {
                        id: pageKind
                        objectName: "settingsPageKind"
                        Layout.fillWidth: true
                        visible: root._isHostPage && root._traits.header !== root._traits.breadcrumb
                        text: qsTr("Host")
                        font: chromeStyle.type.footnote
                        color: chromeStyle.colors.settingsHelp
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                    }

                    // A host's page opens a window attached to it, as Attach to Host does.
                    Button {
                        objectName: "attachHostButton"
                        visible: root._isHostPage
                        enabled: root.controller !== null && root.controller.hostAttachable
                        text: qsTr("Attach")
                        font: chromeStyle.type.body
                        Accessible.name: qsTr("Attach to %1").arg(root._hostName)
                        onClicked: root.controller.requestAttachHost(root._hostName)
                    }

                    // Every change applies as it is made; this puts back what the session changed.
                    Button {
                        objectName: "revertChangesButton"
                        visible: !root._locked
                        enabled: root.controller !== null && root.controller.hasChanges
                        text: qsTr("Revert Changes")
                        font: chromeStyle.type.body
                        Accessible.name: text
                        onClicked: root.controller.revertChanges()
                    }

                    Loader {
                        objectName: "trailingWindowControls"
                        // The header's full height, as the title bar's slot: the controls span their bar, and
                        // a style whose buttons state no height of their own would otherwise collapse to zero.
                        Layout.fillHeight: true
                        active: root.trailingControls !== null
                        visible: active
                        sourceComponent: root.trailingControls
                    }
                }
            }

            Banner {
                objectName: "lockedBanner"
                Layout.fillWidth: true
                dismissible: false
                glyph: "⚿"
                text: root._locked ? qsTr("Settings are locked: gui_config_locked is set in contour.yml.") : ""
            }

            // A profile that contour.yml defines, or that is built in, is read-only; a copy is editable.
            Banner {
                id: readOnlyBanner
                objectName: "readOnlyBanner"
                Layout.fillWidth: true
                readonly property var _origin: root.controller && root._isProfilePage
                                               ? root.controller.profiles.find(row => row.name === root._profileName)
                                               : null
                readonly property bool _shown: root.controller !== null && !root._locked
                                               && ((root._isProfilePage && root.controller.editingReadOnly)
                                                   || (root._isHostPage && root.controller.editingHostReadOnly))
                dismissible: false
                actionName: "duplicateToEditButton"
                actionText: qsTr("Duplicate to Edit")
                glyph: "⚿"
                text: !_shown ? "" : (_origin && _origin.origin === "builtin")
                                     ? qsTr("Built in · Duplicate to Edit")
                                     : qsTr("Defined in contour.yml · Duplicate to Edit")
                onActionTriggered: {
                    if (root._isHostPage) {
                        root.controller.requestDuplicateHost(root._hostName);
                        root.showEditedHost();
                        return;
                    }
                    root.controller.duplicate(root._profileName);
                    root.showEditedProfile();
                }
            }

            // The reason for the last refusal: a refused value leaves its row showing the stored value
            // again, so this is the only place the page says why.
            Banner {
                id: errorBanner
                objectName: "settingsErrorBanner"
                Layout.fillWidth: true
                text: root.errorMessage
                onDismissed: root.errorMessage = ""
            }

            Loader {
                id: pageLoader
                objectName: "settingsPageLoader"
                Layout.fillWidth: true
                Layout.fillHeight: true
                sourceComponent: root._kind === "section" ? sectionPage
                                 : root._isHostPage ? hostPage
                                 : root._subpage === "schemes" ? schemesPage
                                 : root._subpage === "links" ? linksPage
                                 : root._subpage === "statusline" ? statusLinePage
                                 : root._isProfilePage ? profilePage
                                 : sectionPage

                // Page change follows the style's motion: a fade, none under Reduce Motion.
                NumberAnimation {
                    id: pageFade
                    target: pageLoader
                    property: "opacity"
                    from: 0
                    to: 1
                    duration: chromeStyle.motion.animates ? chromeStyle.motion.standard : 0
                }
            }
        }
    }

    Component {
        id: sectionPage
        SettingsSectionPage {
            controller: root.controller
            sectionId: root._kind === "section" ? root._parts[1] : "general"
            onProfileRequested: name => root.openPage("profile:" + name)
            onLinkRequested: target => root.openLink(target)
            onHostRequested: name => root.openPage("host:" + name)
            onAddHostRequested: {
                root.controller.addHost();
                root.showEditedHost();
            }
        }
    }

    Component {
        id: hostPage
        HostPage {
            controller: root.controller
            hostName: root._hostName
        }
    }

    Component {
        id: profilePage
        ProfilePage {
            controller: root.controller
            preview: root.host ? root.host.preview : null
            profileName: root._profileName
            onSubpageRequested: kind => root.openPage("profile:" + root._profileName + ":" + kind)
            onDuplicateRequested: {
                root.controller.duplicate(root._profileName);
                root.showEditedProfile();
            }
            onOpenInNewTabRequested: if (root.host) root.host.openProfileInNewTab(root._profileName)
        }
    }

    // {{{ The profile's sub-pages: what is not a scalar setting.
    Component {
        id: schemesPage
        ColorSchemesPage {
            controller: root.controller
            onDeleteRequested: name => root.requestDelete("scheme", name)
        }
    }

    Component {
        id: linksPage
        LinkPatternsPage {
            controller: root.controller
            onDeleteRequested: (index, name) => root.requestDelete("linkPattern:" + index, name)
        }
    }

    Component {
        id: statusLinePage
        StatusLinePage {
            controller: root.controller
            preview: root.host ? root.host.preview : null
            profileName: root._profileName
        }
    }
    // }}}
}
