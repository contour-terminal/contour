// SPDX-License-Identifier: Apache-2.0
#include <contour/display/ShortcutOverride.hpp>
#include <contour/platform/QtInvoke.hpp>
#include <contour/window/AppMenuBar.hpp>

#include <QtCore/QChar>
#include <QtCore/QCoreApplication>
#include <QtCore/QString>
#include <QtCore/QTranslator>
#include <QtGui/QKeySequence>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace contour::window
{

using command::ContextMenuEntryKind;
using command::MenuBarMenu;
using command::MenuItemRole;
using command::MenuNode;
using command::MenuTree;
using command::RowAvailability;
using command::RowCheck;
using vtbackend::Key;
using vtbackend::Modifier;

namespace
{
    /// A menu row role and the Qt role it is given in every menu.
    struct RoleRow
    {
        MenuItemRole role;
        QAction::MenuRole qtRole;
    };

    /// The rows with a role of their own; MenuItemRole::None depends on the menu, see qtMenuRoleFor().
    constexpr auto RoleTable = std::array {
        RoleRow { .role = MenuItemRole::About, .qtRole = QAction::AboutRole },
        RoleRow { .role = MenuItemRole::Preferences, .qtRole = QAction::PreferencesRole },
        RoleRow { .role = MenuItemRole::Quit, .qtRole = QAction::QuitRole },
        // Qt has no role for it, and it must not move: it stays the Window menu's first row.
        RoleRow { .role = MenuItemRole::Minimize, .qtRole = QAction::NoRole },
    };

    /// A string Qt's cocoa plugin titles a macOS application menu item with, and what the bar has it say.
    struct ApplicationMenuText
    {
        std::string_view context; ///< The translation context Qt looks the string up in.
        std::string_view source;  ///< Qt's own string, as its source text.
        std::string_view text;    ///< What the bar has the item say.
    };

    /// The application menu strings the bar retitles; every other one stays Qt's.
    ///
    /// Qt titles a merged role row from these strings rather than from the action's text (see
    /// QCocoaMenuItem::mergeText() and qt_mac_applicationmenu_string(), Qt 6.11), so a translation is the one
    /// way to retitle one. macOS 13 and later call the Preferences item "Settings…".
    constexpr auto ApplicationMenuTexts = std::array {
        ApplicationMenuText {
            .context = "MAC_APPLICATION_MENU", .source = "Preferences...", .text = "Settings…" },
    };

    /// A translator of ApplicationMenuTexts alone: any other string is left to the next translator, or to
    /// its source text.
    class ApplicationMenuTranslator final: public QTranslator
    {
      public:
        [[nodiscard]] QString translate(char const* context,
                                        char const* sourceText,
                                        char const* /*disambiguation*/,
                                        int /*n*/) const override
        {
            if (context == nullptr || sourceText == nullptr)
                return {};
            auto const row = std::ranges::find_if(ApplicationMenuTexts, [&](ApplicationMenuText const& text) {
                return text.context == context && text.source == sourceText;
            });
            if (row == ApplicationMenuTexts.end())
                return {};
            return QString::fromUtf8(row->text.data(), static_cast<qsizetype>(row->text.size()));
        }

        /// QTranslator's own signature; it holds strings, so it is never empty.
        [[nodiscard]] bool isEmpty() const override { return false; }
    };

    /// A new ApplicationMenuTranslator, installed into the application; it uninstalls itself as it dies.
    [[nodiscard]] std::unique_ptr<QTranslator> installApplicationMenuText()
    {
        auto translator = std::make_unique<ApplicationMenuTranslator>();
        QCoreApplication::installTranslator(translator.get());
        return translator;
    }

    /// What opening a menu bar menu may do to its rows.
    enum class MenuOnOpen : std::uint8_t
    {
        Refill = 0,       ///< Only this bar holds its rows: a changed shape refills the menu.
        RefreshInPlace,   ///< Qt merged its role rows into the macOS application menu: the actions stay.
        RefreshAndRelist, ///< AppKit adopted it and lists the windows after its rows: the actions stay, and
                          ///< the tab list is refilled ahead of the marker that ends the rows.
    };

    /// A menu bar menu and what opening it may do.
    struct MenuBehaviour
    {
        MenuBarMenu menu;
        MenuOnOpen onOpen;
    };

    /// One row per menu bar menu, in MenuBarMenu order.
    constexpr auto MenuBehaviourTable = std::array {
        MenuBehaviour { .menu = MenuBarMenu::Application, .onOpen = MenuOnOpen::RefreshInPlace },
        MenuBehaviour { .menu = MenuBarMenu::Shell, .onOpen = MenuOnOpen::Refill },
        MenuBehaviour { .menu = MenuBarMenu::Edit, .onOpen = MenuOnOpen::Refill },
        MenuBehaviour { .menu = MenuBarMenu::View, .onOpen = MenuOnOpen::Refill },
        MenuBehaviour { .menu = MenuBarMenu::Window, .onOpen = MenuOnOpen::RefreshAndRelist },
        MenuBehaviour { .menu = MenuBarMenu::Help, .onOpen = MenuOnOpen::Refill },
    };
    static_assert(MenuBehaviourTable.size() == command::MenuBarMenuCount,
                  "every menu bar menu needs a row saying what opening it may do");
    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, command::MenuBarMenuCount),
                                      [](std::size_t index) {
                                          return MenuBehaviourTable[index].menu
                                                 == static_cast<MenuBarMenu>(index);
                                      }),
                  "MenuBehaviourTable must list the menus in MenuBarMenu order");
    static_assert(std::ranges::count(MenuBehaviourTable, MenuOnOpen::RefreshAndRelist, &MenuBehaviour::onOpen)
                      == 1,
                  "the bar keeps one tab list and one end-of-rows marker: those of the menu AppKit lists the "
                  "windows in");

    /// What opening @p menu may do to its rows.
    [[nodiscard]] constexpr MenuOnOpen onOpenOf(MenuBarMenu menu) noexcept
    {
        return MenuBehaviourTable[static_cast<std::size_t>(menu)].onOpen;
    }

    /// A chord modifier and the Qt modifier it is, under Qt::AA_MacDontSwapCtrlAndMeta: the inverse of
    /// input/KeyMapping.cpp's makeModifiers().
    struct ModifierRow
    {
        Modifier modifier;
        Qt::KeyboardModifier qtModifier;
    };

    constexpr auto ModifierTable = std::array {
        ModifierRow { .modifier = Modifier::Control, .qtModifier = Qt::ControlModifier },
        ModifierRow { .modifier = Modifier::Alt, .qtModifier = Qt::AltModifier },
        ModifierRow { .modifier = Modifier::Shift, .qtModifier = Qt::ShiftModifier },
        ModifierRow { .modifier = Modifier::Super, .qtModifier = Qt::MetaModifier },
    };

    /// The modifiers Qt has no modifier for: a chord holding one has no Qt key.
    constexpr auto UnmappedModifiers = std::array { Modifier::Hyper, Modifier::Meta };

    /// The modifiers that make a chord a key equivalent.
    constexpr auto KeyEquivalentModifiers = std::array { Modifier::Control, Modifier::Super };

    /// A named key and its Qt key: the inverse of session/SessionInput.cpp's KeyMappings.
    struct KeyRow
    {
        Key key;
        Qt::Key qtKey;
    };

    // clang-format off
    constexpr auto KeyTable = std::array {
        KeyRow { Key::Escape, Qt::Key_Escape },       KeyRow { Key::Enter, Qt::Key_Return },
        KeyRow { Key::Tab, Qt::Key_Tab },             KeyRow { Key::Backspace, Qt::Key_Backspace },
        KeyRow { Key::Insert, Qt::Key_Insert },       KeyRow { Key::Delete, Qt::Key_Delete },
        KeyRow { Key::RightArrow, Qt::Key_Right },    KeyRow { Key::LeftArrow, Qt::Key_Left },
        KeyRow { Key::DownArrow, Qt::Key_Down },      KeyRow { Key::UpArrow, Qt::Key_Up },
        KeyRow { Key::PageDown, Qt::Key_PageDown },   KeyRow { Key::PageUp, Qt::Key_PageUp },
        KeyRow { Key::Home, Qt::Key_Home },           KeyRow { Key::End, Qt::Key_End },
        KeyRow { Key::F1, Qt::Key_F1 },               KeyRow { Key::F2, Qt::Key_F2 },
        KeyRow { Key::F3, Qt::Key_F3 },               KeyRow { Key::F4, Qt::Key_F4 },
        KeyRow { Key::F5, Qt::Key_F5 },               KeyRow { Key::F6, Qt::Key_F6 },
        KeyRow { Key::F7, Qt::Key_F7 },               KeyRow { Key::F8, Qt::Key_F8 },
        KeyRow { Key::F9, Qt::Key_F9 },               KeyRow { Key::F10, Qt::Key_F10 },
        KeyRow { Key::F11, Qt::Key_F11 },             KeyRow { Key::F12, Qt::Key_F12 },
        KeyRow { Key::F13, Qt::Key_F13 },             KeyRow { Key::F14, Qt::Key_F14 },
        KeyRow { Key::F15, Qt::Key_F15 },             KeyRow { Key::F16, Qt::Key_F16 },
        KeyRow { Key::F17, Qt::Key_F17 },             KeyRow { Key::F18, Qt::Key_F18 },
        KeyRow { Key::F19, Qt::Key_F19 },             KeyRow { Key::F20, Qt::Key_F20 },
        KeyRow { Key::F21, Qt::Key_F21 },             KeyRow { Key::F22, Qt::Key_F22 },
        KeyRow { Key::F23, Qt::Key_F23 },             KeyRow { Key::F24, Qt::Key_F24 },
        KeyRow { Key::F25, Qt::Key_F25 },             KeyRow { Key::F26, Qt::Key_F26 },
        KeyRow { Key::F27, Qt::Key_F27 },             KeyRow { Key::F28, Qt::Key_F28 },
        KeyRow { Key::F29, Qt::Key_F29 },             KeyRow { Key::F30, Qt::Key_F30 },
        KeyRow { Key::F31, Qt::Key_F31 },             KeyRow { Key::F32, Qt::Key_F32 },
        KeyRow { Key::F33, Qt::Key_F33 },             KeyRow { Key::F34, Qt::Key_F34 },
        KeyRow { Key::F35, Qt::Key_F35 },
        KeyRow { Key::MediaPlay, Qt::Key_MediaPlay }, KeyRow { Key::MediaStop, Qt::Key_MediaStop },
        KeyRow { Key::MediaPrevious, Qt::Key_MediaPrevious },
        KeyRow { Key::MediaNext, Qt::Key_MediaNext }, KeyRow { Key::MediaPause, Qt::Key_MediaPause },
        KeyRow { Key::MediaTogglePlayPause, Qt::Key_MediaTogglePlayPause },
        KeyRow { Key::VolumeUp, Qt::Key_VolumeUp },   KeyRow { Key::VolumeDown, Qt::Key_VolumeDown },
        KeyRow { Key::VolumeMute, Qt::Key_VolumeMute },
        KeyRow { Key::CapsLock, Qt::Key_CapsLock },   KeyRow { Key::ScrollLock, Qt::Key_ScrollLock },
        KeyRow { Key::NumLock, Qt::Key_NumLock },     KeyRow { Key::PrintScreen, Qt::Key_Print },
        KeyRow { Key::Pause, Qt::Key_Pause },         KeyRow { Key::Menu, Qt::Key_Menu },
    };
    // clang-format on

    /// The Qt key of a named key.
    [[nodiscard]] std::optional<Qt::Key> qtKeyOf(Key key) noexcept
    {
        auto const row = std::ranges::find(KeyTable, key, &KeyRow::key);
        if (row == KeyTable.end())
            return std::nullopt;
        return row->qtKey;
    }

    /// The Qt key of a character: Qt names a letter by its upper case.
    [[nodiscard]] std::optional<Qt::Key> qtKeyOf(char32_t ch) noexcept
    {
        if (ch == 0 || ch > 0xFFFF)
            return std::nullopt;
        return static_cast<Qt::Key>(QChar(static_cast<char16_t>(ch)).toUpper().unicode());
    }

    /// @return @p title as a QAction text: an ampersand is literal, not a mnemonic.
    [[nodiscard]] QString actionText(std::string const& title)
    {
        return QString::fromStdString(title).replace(QLatin1Char('&'), QStringLiteral("&&"));
    }

    /// The index of the first row of the Window menu's tab list in @p nodes, with the separator ahead of it,
    /// or @c nodes.size() when there is no tab list. A tab row is one that switches to a tab by position.
    [[nodiscard]] std::size_t tabListStart(std::span<MenuNode const> nodes, MenuTree const& tree)
    {
        auto const first = std::ranges::find_if(nodes, [&](MenuNode const& node) {
            return node.kind == ContextMenuEntryKind::Command
                   && std::holds_alternative<actions::SwitchToTab>(tree.actions[node.actionIndex]);
        });
        auto start = static_cast<std::size_t>(std::distance(nodes.begin(), first));
        if (start != nodes.size() && start > 0 && nodes[start - 1].kind == ContextMenuEntryKind::Separator)
            --start;
        return start;
    }

    /// Sets @p action's enabled and checked state from @p node.
    void applyState(QAction& action, MenuNode const& node)
    {
        action.setEnabled(node.availability == RowAvailability::Enabled);
        action.setCheckable(node.check != RowCheck::None);
        action.setChecked(node.check == RowCheck::Checked);
    }

    /// Refreshes the state of @p actions from @p nodes, row by row, descending into submenus. A row whose
    /// title or kind differs from its node's is left as it is: the menu's shape changes on rebuild only.
    void refreshActions(QList<QAction*> const& actions, std::span<MenuNode const> nodes)
    {
        for (auto const& [action, node]: std::views::zip(actions, nodes))
        {
            if (action->isSeparator() != (node.kind == ContextMenuEntryKind::Separator)
                || action->text() != actionText(node.title))
                continue;
            switch (node.kind)
            {
                case ContextMenuEntryKind::Separator: break;
                case ContextMenuEntryKind::Command: applyState(*action, node); break;
                case ContextMenuEntryKind::Submenu:
                    action->setEnabled(node.availability == RowAvailability::Enabled);
                    if (auto const* submenu = action->menu())
                        refreshActions(submenu->actions(), node.children);
                    break;
            }
        }
    }

    /// Whether @p actions are the rows @p nodes would build: the same kinds and titles, submenus alike.
    [[nodiscard]] bool hasShapeOf(QList<QAction*> const& actions, std::span<MenuNode const> nodes)
    {
        if (static_cast<std::size_t>(actions.size()) != nodes.size())
            return false;
        return std::ranges::all_of(std::views::zip(actions, nodes), [](auto const& pair) {
            auto const& [action, node] = pair;
            auto const* const submenu = action->menu();
            if (action->isSeparator() != (node.kind == ContextMenuEntryKind::Separator)
                || (submenu != nullptr) != (node.kind == ContextMenuEntryKind::Submenu)
                || (!action->isSeparator() && action->text() != actionText(node.title)))
                return false;
            return submenu == nullptr || hasShapeOf(submenu->actions(), node.children);
        });
    }

    /// Removes every row of @p menu but @p keep, their submenus with them.
    void clearMenu(QMenu& menu, QAction const* keep = nullptr)
    {
        for (auto* const action: menu.actions())
        {
            if (action == keep)
                continue;
            menu.removeAction(action);
            // A submenu takes its own menuAction with it; a row the menu made is the menu's to delete.
            auto* const submenu = action->menu();
            if (submenu == nullptr && action->parent() == &menu)
                delete action;
            delete submenu;
        }
    }

    /// Adds one empty menu per menu bar menu to @p bar, in MenuBarMenu order.
    [[nodiscard]] std::array<QMenu*, command::MenuBarMenuCount> addMenus(QMenuBar& bar)
    {
        auto menus = std::array<QMenu*, command::MenuBarMenuCount> {};
        for (auto& menu: menus)
            menu = bar.addMenu(QString {});
        return menus;
    }

    /// Adds a hidden separator to @p menu, to keep a place in it.
    [[nodiscard]] QAction* addHiddenMarker(QMenu& menu)
    {
        auto* const marker = menu.addSeparator();
        marker->setVisible(false);
        return marker;
    }
} // namespace

QAction::MenuRole qtMenuRoleFor(MenuItemRole role, MenuBarMenu menu) noexcept
{
    if (auto const row = std::ranges::find(RoleTable, role, &RoleRow::role); row != RoleTable.end())
        return row->qtRole;
    return menu == MenuBarMenu::Application ? QAction::ApplicationSpecificRole : QAction::NoRole;
}

std::optional<QKeyCombination> qtKeyFor(command::ShortcutChord const& chord)
{
    if (std::ranges::any_of(UnmappedModifiers, [&](Modifier m) { return chord.modifiers.test(m); }))
        return std::nullopt;

    auto const key = std::visit([](auto input) { return qtKeyOf(input); }, chord.input);
    if (!key)
        return std::nullopt;

    auto modifiers = Qt::KeyboardModifiers {};
    for (auto const& row: ModifierTable)
        if (chord.modifiers.test(row.modifier))
            modifiers |= row.qtModifier;
    return QKeyCombination(modifiers, *key);
}

bool carriesKeyEquivalent(command::ShortcutChord const& chord) noexcept
{
    return std::ranges::any_of(KeyEquivalentModifiers, [&](Modifier m) { return chord.modifiers.test(m); });
}

AppMenuBar::AppMenuBar(AppMenuBarSources sources, platform::IMenuBarIntegration& integration):
    _sources { std::move(sources) },
    _applicationMenuText { installApplicationMenuText() },
    // Parentless, so that on macOS it is the one bar every window shares.
    _menuBar { std::make_unique<QMenuBar>() },
    _menus { addMenus(*_menuBar) },
    _windowRowsEnd { addHiddenMarker(*_menus[static_cast<std::size_t>(MenuBarMenu::Window)]) }
{
    for (auto const index: std::views::iota(std::size_t { 0 }, command::MenuBarMenuCount))
    {
        auto const which = static_cast<MenuBarMenu>(index);
        // Up to date as the menu opens, see refresh().
        QObject::connect(
            _menus[index], &QMenu::aboutToShow, _menuBar.get(), [this, which] { refresh(which); });
    }

    rebuild();
    integration.adopt(menu(MenuBarMenu::Window), menu(MenuBarMenu::Help));

    // The Application menu is deliberately NOT hidden, though on macOS Qt moves every row of it into the
    // application menu: a hidden QAction is a disabled one, Qt disables the menu's platform menu with it,
    // and every row it merged would then validate as disabled -- Quit, Settings… and About alike, so that
    // ⌘Q and ⌘, fell through to the terminal. Qt hides the native menu itself once nothing visible is left
    // in it (QCocoaMenuBar::syncMenu_helper, Qt 6.11.1), so no second, empty "Contour" shows.
}

AppMenuBar::~AppMenuBar() = default;

MenuTree AppMenuBar::resolve() const
{
    return command::buildMenuTree(
        command::buildMenuBar(_sources.state()), _sources.chords(), command::platformShortcutStyle());
}

QMenu& AppMenuBar::menu(MenuBarMenu which) const
{
    return *_menus[static_cast<std::size_t>(which)];
}

void AppMenuBar::rebuild()
{
    auto const tree = resolve();
    for (auto const index: std::views::iota(std::size_t { 0 }, command::MenuBarMenuCount))
    {
        auto const which = static_cast<MenuBarMenu>(index);
        auto& top = menu(which);
        auto const& node = tree.nodes[index];
        top.setTitle(actionText(node.title));
        auto const rows = std::span<MenuNode const> { node.children };

        switch (onOpenOf(which))
        {
            case MenuOnOpen::Refill:
            case MenuOnOpen::RefreshInPlace:
                clearMenu(top);
                fill(top, which, rows, tree, nullptr);
                break;
            case MenuOnOpen::RefreshAndRelist: {
                // Every row goes ahead of the marker: Qt appends a row that has nothing to go ahead of
                // after the window list AppKit appended to this menu.
                clearMenu(top, _windowRowsEnd);
                auto const tabs = tabListStart(rows, tree);
                fill(top, which, rows.first(tabs), tree, _windowRowsEnd);
                _tabList = fill(top, which, rows.subspan(tabs), tree, _windowRowsEnd);
                break;
            }
        }
    }
}

QList<QAction*> AppMenuBar::fill(
    QMenu& menu, MenuBarMenu which, std::span<MenuNode const> nodes, MenuTree const& tree, QAction* before)
{
    auto added = QList<QAction*> {};
    for (auto const& node: nodes)
    {
        switch (node.kind)
        {
            case ContextMenuEntryKind::Separator: {
                added.push_back(menu.insertSeparator(before));
                break;
            }
            case ContextMenuEntryKind::Submenu: {
                auto* const submenu = new QMenu(actionText(node.title), &menu);
                auto* const action = menu.insertMenu(before, submenu);
                action->setMenuRole(qtMenuRoleFor(node.role, which));
                action->setEnabled(node.availability == RowAvailability::Enabled);
                fill(*submenu, which, node.children, tree, nullptr);
                added.push_back(action);
                break;
            }
            case ContextMenuEntryKind::Command: {
                auto* const action = new QAction(actionText(node.title), &menu);
                action->setMenuRole(qtMenuRoleFor(node.role, which));
                applyState(*action, node);
                // A role row answers to its menu-owned key, never to the binding's chord.
                if (auto const owned = display::menuKeyFor(node.role))
                    action->setShortcut(*owned);
                else if (node.chord && carriesKeyEquivalent(*node.chord))
                    if (auto const key = qtKeyFor(*node.chord))
                        action->setShortcut(QKeySequence(*key));
                // Posted, with the action captured now: a rebuild while the post is queued cannot change
                // what runs, and the row may be deleted by what it runs.
                QObject::connect(action,
                                 &QAction::triggered,
                                 _menuBar.get(),
                                 [this, picked = tree.actions[node.actionIndex], scope = node.scope] {
                                     platform::postToObject(_menuBar.get(), [this, picked, scope] {
                                         _sources.run(picked, scope);
                                     });
                                 });
                menu.insertAction(before, action);
                added.push_back(action);
                break;
            }
        }
    }
    return added;
}

void AppMenuBar::refreshAll()
{
    for (auto const index: std::views::iota(std::size_t { 0 }, command::MenuBarMenuCount))
        refresh(static_cast<MenuBarMenu>(index));
}

void AppMenuBar::refresh(MenuBarMenu which)
{
    auto const tree = resolve();
    auto const rows = std::span<MenuNode const> { tree.nodes[static_cast<std::size_t>(which)].children };
    auto& top = menu(which);
    auto actions = top.actions();

    switch (onOpenOf(which))
    {
        case MenuOnOpen::Refill:
            // A row shown only in some states (Edit's Speech) comes and goes here, not on the next reload.
            if (!hasShapeOf(actions, rows))
            {
                clearMenu(top);
                fill(top, which, rows, tree, nullptr);
                return;
            }
            refreshActions(actions, rows);
            return;
        case MenuOnOpen::RefreshInPlace: refreshActions(actions, rows); return;
        case MenuOnOpen::RefreshAndRelist: {
            auto const tabs = tabListStart(rows, tree);
            auto const firstTabRow = _tabList.isEmpty() ? _windowRowsEnd : _tabList.front();
            actions.resize(actions.indexOf(firstTabRow));
            refreshActions(actions, rows.first(tabs));
            refillTabList(rows.subspan(tabs), tree);
            return;
        }
    }
}

void AppMenuBar::refillTabList(std::span<MenuNode const> nodes, MenuTree const& tree)
{
    for (auto* const action: std::exchange(_tabList, {}))
    {
        menu(MenuBarMenu::Window).removeAction(action);
        delete action;
    }
    _tabList = fill(menu(MenuBarMenu::Window), MenuBarMenu::Window, nodes, tree, _windowRowsEnd);
}

} // namespace contour::window
