// SPDX-License-Identifier: Apache-2.0
#include <contour/display/ShortcutOverride.hpp>
#include <contour/platform/QtInvoke.hpp>
#include <contour/window/AppMenuBar.hpp>

#include <QtCore/QChar>
#include <QtCore/QString>
#include <QtGui/QKeySequence>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <string>
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
    };

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

    /// Removes every row of @p menu, its submenus with them.
    void clearMenu(QMenu& menu)
    {
        menu.clear();
        // clear() deletes the actions the menu owns, but a submenu is a child widget, not an action.
        for (auto* const submenu: menu.findChildren<QMenu*>(Qt::FindDirectChildrenOnly))
            delete submenu;
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
    _sources { std::move(sources) }, _menuBar { std::make_unique<QMenuBar>() }
{
    // Parentless, so that on macOS it is the one bar every window shares.
    for (auto const index: std::views::iota(std::size_t { 0 }, command::MenuBarMenuCount))
    {
        auto* const top = _menuBar->addMenu(QString {});
        _menus[index] = top;
        auto const which = static_cast<MenuBarMenu>(index);
        // The state as the menu opens; the rows themselves stay, see refresh().
        QObject::connect(top, &QMenu::aboutToShow, _menuBar.get(), [this, which] { refresh(which); });
    }

    rebuild();
    integration.adopt(menu(MenuBarMenu::Window), menu(MenuBarMenu::Help));

#ifdef __APPLE__
    // Its rows all carry a role, so Qt moves every one into the application menu macOS names after the
    // app; the menu itself would be a second, empty "Contour" beside it.
    menu(MenuBarMenu::Application).menuAction()->setVisible(false);
#endif
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
        clearMenu(top);
        top.setTitle(actionText(node.title));

        auto const rows = std::span<MenuNode const> { node.children };
        if (which != MenuBarMenu::Window)
        {
            fill(top, which, rows, tree, nullptr);
            continue;
        }

        // The Window menu's tab list changes as tabs come and go, so it goes ahead of a hidden marker that
        // keeps its place: past the marker, AppKit lists the application's windows.
        auto const tabs = tabListStart(rows, tree);
        fill(top, which, rows.first(tabs), tree, nullptr);
        _tabListEnd = top.addSeparator();
        _tabListEnd->setVisible(false);
        _tabList = fill(top, which, rows.subspan(tabs), tree, _tabListEnd);
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
                // A role row answers to its platform's standard key, never to the binding's chord.
                if (auto const standard = display::standardKeyFor(node.role))
                    action->setShortcut(QKeySequence(*standard));
                else if (node.chord && carriesKeyEquivalent(*node.chord))
                    if (auto const key = qtKeyFor(*node.chord))
                        action->setShortcut(QKeySequence(*key));
                // Posted, with the action captured now: a rebuild while the post is queued cannot change
                // what runs, and the row may be deleted by what it runs.
                QObject::connect(action,
                                 &QAction::triggered,
                                 _menuBar.get(),
                                 [this, picked = tree.actions[node.actionIndex]] {
                                     platform::postToObject(_menuBar.get(),
                                                            [this, picked] { _sources.run(picked); });
                                 });
                menu.insertAction(before, action);
                added.push_back(action);
                break;
            }
        }
    }
    return added;
}

void AppMenuBar::refresh(MenuBarMenu which)
{
    auto const tree = resolve();
    auto const rows = std::span<MenuNode const> { tree.nodes[static_cast<std::size_t>(which)].children };
    auto actions = menu(which).actions();
    if (which != MenuBarMenu::Window)
    {
        refreshActions(actions, rows);
        return;
    }

    auto const tabs = tabListStart(rows, tree);
    auto const firstTabRow = _tabList.isEmpty() ? _tabListEnd : _tabList.front();
    actions.resize(actions.indexOf(firstTabRow));
    refreshActions(actions, rows.first(tabs));
    refillTabList(rows.subspan(tabs), tree);
}

void AppMenuBar::refillTabList(std::span<MenuNode const> nodes, MenuTree const& tree)
{
    for (auto* const action: std::exchange(_tabList, {}))
    {
        menu(MenuBarMenu::Window).removeAction(action);
        delete action;
    }
    _tabList = fill(menu(MenuBarMenu::Window), MenuBarMenu::Window, nodes, tree, _tabListEnd);
}

} // namespace contour::window
