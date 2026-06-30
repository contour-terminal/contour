// SPDX-License-Identifier: Apache-2.0
//
// Contract tests for the tab strip's QAbstractListModel projection.
//
// TerminalSessionManager projects a Qt-free vtmux::SessionModel into a QAbstractListModel whose rows
// are the active window's TABS (a tab with several split panes is still one row). The manager itself
// cannot be instantiated headlessly — it needs a full ContourGuiApp and spawns real PTYs — so these
// tests drive a thin TabListModel that performs the EXACT SAME projection and the EXACT SAME
// ModelEvents -> begin/end* mapping the manager uses, wrapped in a QAbstractItemModelTester. The
// tester machine-verifies the QAbstractListModel contract (rowCount consistency and balanced
// begin/end insert/remove/move pairs) on every model mutation, which is precisely what the
// row-indexing findings are about:
//   * rows track tabs, not sessions/panes (a split must not grow the row count);
//   * structural changes use the proper begin/end* notifications.

#include <contour/TabLabel.h>
#include <contour/TerminalSessionManager.h> // for the production Roles enum the static_asserts pin to

#include <QtCore/QAbstractListModel>
#include <QtGui/QColor>
#include <QtGui/QGuiApplication>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>

#include <QtTest/QAbstractItemModelTester>
#include <QtTest/QSignalSpy>
#include <vtmux/ModelEvents.h>
#include <vtmux/SessionModel.h>

using namespace vtmux;

namespace
{

/// A faithful stand-in for TerminalSessionManager's QAbstractListModel surface: rows are the active
/// window's tabs, and the vtmux ModelEvents are mapped to begin/end* exactly as the manager does.
/// It deliberately does NOT back rows with a per-pane session vector, so a split adds no row.
class TabListModel: public QAbstractListModel, public ModelEvents
{
  public:
    enum Roles : int
    {
        TitleRole = Qt::UserRole + 1,
        ColorRole,
        IsActiveRole,
        PaneCountRole,
        SessionIdRole,
        RawTitleRole,
    };

    // This model's projection is a hand-copy of TerminalSessionManager's (the manager is not headless-
    // constructible, so the contract is tested through this stand-in). Pin each role to the production
    // value so a reorder/insert in TerminalSessionManager::Roles is a COMPILE error here rather than a
    // silently-passing stale test. If a role is added, add it here AND to data()/roleNames() below.
    static_assert(static_cast<int>(TitleRole) == static_cast<int>(contour::TerminalSessionManager::TitleRole));
    static_assert(static_cast<int>(ColorRole) == static_cast<int>(contour::TerminalSessionManager::ColorRole));
    static_assert(
        static_cast<int>(IsActiveRole) == static_cast<int>(contour::TerminalSessionManager::IsActiveRole));
    static_assert(
        static_cast<int>(PaneCountRole) == static_cast<int>(contour::TerminalSessionManager::PaneCountRole));
    static_assert(
        static_cast<int>(SessionIdRole) == static_cast<int>(contour::TerminalSessionManager::SessionIdRole));
    static_assert(
        static_cast<int>(RawTitleRole) == static_cast<int>(contour::TerminalSessionManager::RawTitleRole));

    TabListModel():
        _model { *this,
                 [this] {
                     return SessionId { _nextSession++ };
                 } },
        _window { _model.createWindow() }
    {
        // Mirror TerminalSessionManager's resolver: map a SessionId to the session's own title. Here
        // titles are seeded per session via setSessionTitle() so a test can control {WindowTitle}.
        _model.setSessionTitleResolver([this](SessionId id) -> std::string {
            auto const it = _sessionTitles.find(id.value);
            return it != _sessionTitles.end() ? it->second : std::string {};
        });
    }

    [[nodiscard]] SessionModel& model() noexcept { return _model; }
    [[nodiscard]] Window& window() noexcept { return *_window; }

    /// Sets the program-driven title used for {WindowTitle} expansion of @p session.
    void setSessionTitle(SessionId session, std::string title)
    {
        _sessionTitles[session.value] = std::move(title);
    }

    /// Sets the per-profile tab-label template the model applies (mirrors profile().tabLabel).
    void setTabLabelTemplate(std::string tmpl) { _tabLabelTemplate = std::move(tmpl); }

    // {{{ QAbstractListModel — mirror of TerminalSessionManager
    [[nodiscard]] int rowCount(QModelIndex const& parent = QModelIndex()) const override
    {
        if (parent.isValid())
            return 0;
        return _window != nullptr ? _window->tabCount() : 0;
    }

    [[nodiscard]] QVariant data(QModelIndex const& index, int role) const override
    {
        auto const row = index.row();
        if (row < 0 || row >= rowCount())
            return {};
        auto* tab = _window != nullptr ? _window->tabAt(row) : nullptr;
        switch (role)
        {
            case Qt::DisplayRole:
            case SessionIdRole:
                return tab != nullptr ? QVariant(static_cast<qulonglong>(tab->activePane()->session().value))
                                      : QVariant {};
            case TitleRole: return resolvedTabLabel(tab, row);
            case RawTitleRole:
                return tab != nullptr ? QString::fromStdString(tab->runtimeTitle().value_or("")) : QString {};
            case ColorRole:
                if (tab != nullptr && tab->color().has_value())
                {
                    auto const c = *tab->color();
                    return QColor(c.red, c.green, c.blue);
                }
                return QColor(Qt::transparent);
            case IsActiveRole: return _window != nullptr && row == _window->activeTabIndex();
            case PaneCountRole: return tab != nullptr ? tab->paneCount() : 1;
            default: return {};
        }
    }

    [[nodiscard]] QHash<int, QByteArray> roleNames() const override
    {
        return { { Qt::DisplayRole, "display" }, { TitleRole, "title" },
                 { ColorRole, "accentColor" },   { IsActiveRole, "isActive" },
                 { PaneCountRole, "paneCount" }, { SessionIdRole, "sessionId" },
                 { RawTitleRole, "rawTitle" } };
    }
    // }}}

    // {{{ vtmux::ModelEvents — identical begin/end* mapping to TerminalSessionManager
    void tabAdded(WindowId, TabId, int index) override
    {
        beginInsertRows(QModelIndex(), index, index);
        endInsertRows();
        refreshAllTabTitles();
    }
    void tabClosed(WindowId, TabId, int index) override
    {
        beginRemoveRows(QModelIndex(), index, index);
        endRemoveRows();
        refreshAllTabTitles();
    }
    void tabMoved(WindowId, TabId, int fromIndex, int toIndex) override
    {
        // Qt's moveRows: when moving DOWN, the destination row is one past the target.
        auto const dest = toIndex > fromIndex ? toIndex + 1 : toIndex;
        beginMoveRows(QModelIndex(), fromIndex, fromIndex, QModelIndex(), dest);
        endMoveRows();
        refreshAllTabTitles();
    }
    void activeTabChanged(WindowId, TabId, int index) override
    {
        ++activeTabChangedCount;
        lastActiveTabRow = index;
        // Mirror TerminalSessionManager::activeTabChanged: invalidate IsActiveRole across all rows so
        // the previously-active and newly-active rows both re-read the tab-strip highlight.
        if (auto const rows = rowCount(); rows > 0)
            emit dataChanged(this->index(0), this->index(rows - 1), { IsActiveRole });
    }

    int activeTabChangedCount = 0;
    int lastActiveTabRow = -1;
    // Counts active-session notifications. The manager emits activeSessionChanged() whenever the
    // active-pane session may change — on a pane-focus change (here) as well as a tab/tree change — so
    // window-level bindings (the window title) follow the focused pane. Mirror that so a test can
    // assert a pane-focus change notifies active-session consumers.
    int activeSessionChangedCount = 0;
    void paneSplit(TabId tab, PaneId, PaneId) override { notifyRow(tab, { TitleRole, PaneCountRole }); }
    void paneClosed(TabId tab, PaneId, PaneId) override { notifyRow(tab, { TitleRole, PaneCountRole }); }
    void activePaneChanged(TabId tab, PaneId) override
    {
        notifyRow(tab, { TitleRole });
        ++activeSessionChangedCount; // mirror: manager emits activeSessionChanged() here
    }
    void paneRatioChanged(TabId, PaneId splitNode, double ratio) override
    {
        // Mirrors TerminalSessionManager::paneRatioChanged: look the split node's proxy up by PaneId
        // and notify it. Here we just record the routing key/value so the test can assert a
        // model-driven ratio change is delivered to the per-PaneId proxy slot.
        lastRatioPaneId = splitNode.value;
        lastRatio = ratio;
        ++ratioChangedCount;
    }

    uint64_t lastRatioPaneId = 0;
    double lastRatio = 0.0;
    int ratioChangedCount = 0;
    // Counts how often the indicator status line would be republished. The manager refreshes the
    // status line on BOTH a color change and a title change (the {Tabs} entry is built from the
    // titles); mirror that here so a test can assert a rename republishes the status line, not only
    // the QML tab-strip row.
    int statusLineUpdateCount = 0;
    void tabTitleChanged(TabId tab) override
    {
        notifyRow(tab, { TitleRole, RawTitleRole });
        ++statusLineUpdateCount; // mirror: manager calls updateStatusLine() here too
    }
    void tabColorChanged(TabId tab) override
    {
        notifyRow(tab, { ColorRole });
        ++statusLineUpdateCount; // mirror: manager calls updateStatusLine() here
    }
    // }}}

  private:
    void notifyRow(TabId tab, QList<int> const& roles)
    {
        if (_window == nullptr)
            return;
        if (auto const row = _window->indexOf(tab); row >= 0)
            emit dataChanged(index(row), index(row), roles);
    }

    // Mirror of TerminalSessionManager::refreshAllTabTitles.
    void refreshAllTabTitles()
    {
        if (auto const rows = rowCount(); rows > 0)
            emit dataChanged(index(0), index(rows - 1), { TitleRole });
    }

    // Mirror of TerminalSessionManager::resolvedTabLabel: pick a template by precedence (rename >
    // multi-pane sentinel > profile template), then expand it once with the tab's 1-based position.
    [[nodiscard]] QString resolvedTabLabel(Tab* tab, int row) const
    {
        if (tab == nullptr)
            return {};
        auto const windowTitle = _model.sessionTitleResolver()(tab->activePane()->session());
        auto templ = std::string_view {};
        if (auto const& renamed = tab->runtimeTitle(); renamed.has_value())
            templ = *renamed;
        else if (tab->hasMultiplePanes())
            templ = Tab::MultiplePanesLabel;
        else
            templ = _tabLabelTemplate;
        return QString::fromStdString(contour::expandTabLabel(
            templ, contour::TabLabelContext { .position = row + 1, .windowTitle = windowTitle }));
    }

    uint64_t _nextSession = 1;
    SessionModel _model;
    Window* _window;
    std::string _tabLabelTemplate = "{WindowTitle}";
    std::map<uint64_t, std::string> _sessionTitles;
};

} // namespace

TEST_CASE("TabListModel: rows track tabs and a split does not add a row", "[contour][gui][model]")
{
    // The Catch2 main builds a QGuiApplication for us (see test_main.cpp).
    TabListModel m;
    // Attaching the tester makes every subsequent model mutation contract-checked.
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Warning);

    CHECK(m.rowCount() == 0);

    auto* t1 = m.model().createTab(m.window().id());
    auto* t2 = m.model().createTab(m.window().id());
    CHECK(m.rowCount() == 2); // two tabs -> two rows

    // Splitting t2's pane adds a pane WITHIN the tab; the row count must stay at 2 (no phantom row).
    auto* leaf = m.model().splitActivePane(t2->id(), SplitState::Vertical);
    REQUIRE(leaf != nullptr);
    CHECK(m.rowCount() == 2);

    // The split tab now reports two panes via PaneCountRole; the other tab still one.
    CHECK(m.data(m.index(0), TabListModel::PaneCountRole).toInt() == 1);
    CHECK(m.data(m.index(1), TabListModel::PaneCountRole).toInt() == 2);

    // SessionIdRole resolves to the tab's active leaf, not a row-indexed session vector.
    CHECK(m.data(m.index(1), TabListModel::SessionIdRole).toULongLong() == t2->activePane()->session().value);
    (void) t1;
}

TEST_CASE("TabListModel: closing a tab removes exactly one row via the remove contract",
          "[contour][gui][model]")
{
    TabListModel m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Warning);

    auto* a = m.model().createTab(m.window().id());
    auto* b = m.model().createTab(m.window().id());
    m.model().createTab(m.window().id());
    CHECK(m.rowCount() == 3);

    // Closing the middle tab drops the row count by exactly one; the tester verifies rowCount was 3
    // until endRemoveRows and 2 after (the off-by-one ordering finding).
    m.model().closeTab(m.window().id(), b->id());
    CHECK(m.rowCount() == 2);
    (void) a;
}

TEST_CASE("TabListModel: closing one pane of a split tab keeps the tab row", "[contour][gui][model]")
{
    // The core split-close finding: a tab hosting two panes is ONE row. Closing one of its panes
    // must absorb the sibling and keep the row — it must NOT remove the tab row (which would orphan
    // the surviving pane's shell). Closing the now-single tab's last pane removes the row.
    TabListModel m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Warning);

    auto* keep = m.model().createTab(m.window().id());  // row 0, single pane
    auto* split = m.model().createTab(m.window().id()); // row 1, will be split
    auto* newLeaf = m.model().splitActivePane(split->id(), SplitState::Vertical);
    REQUIRE(newLeaf != nullptr);
    REQUIRE(m.rowCount() == 2);
    REQUIRE(split->paneCount() == 2);

    // Close ONE pane of the split tab -> tab stays, sibling absorbed, row count unchanged.
    m.model().closePane(m.window().id(), split->id(), newLeaf->id());
    CHECK(m.rowCount() == 2);       // still two tabs/rows
    CHECK(split->paneCount() == 1); // absorbed back to a single pane
    CHECK(m.data(m.index(1), TabListModel::PaneCountRole).toInt() == 1);

    // Closing the last pane of that tab now removes exactly that one row.
    m.model().closePane(m.window().id(), split->id(), split->rootPane()->id());
    CHECK(m.rowCount() == 1);
    CHECK(m.window().tabAt(0) == keep);
}

TEST_CASE("TabListModel: activating a tab by row updates the model's active tab", "[contour][gui][model]")
{
    // Mirrors TerminalSessionManager::activateModelTabByRow: selecting a tab row must make that tab
    // the model's active tab (so the rendered split tree and later split/close act on it) and fire
    // activeTabChanged with the new row — the "tab switch leaves the model active tab stale" finding.
    TabListModel m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Warning);

    auto* a = m.model().createTab(m.window().id()); // row 0
    auto* b = m.model().createTab(m.window().id()); // row 1
    auto* c = m.model().createTab(m.window().id()); // row 2 (active: newest)
    REQUIRE(m.window().activeTab() == c);

    auto const baseline = m.activeTabChangedCount;

    // Activate row 0 (tab a), as activateModelTabByRow(0) would via _model->activateTab.
    m.model().activateTab(m.window().id(), m.window().tabAt(0)->id());
    CHECK(m.window().activeTab() == a);
    CHECK(m.window().activeTabIndex() == 0);
    CHECK(m.data(m.index(0), TabListModel::IsActiveRole).toBool());
    CHECK_FALSE(m.data(m.index(2), TabListModel::IsActiveRole).toBool());
    CHECK(m.activeTabChangedCount == baseline + 1);
    CHECK(m.lastActiveTabRow == 0);

    // Re-activating the already-active tab is a no-op (no spurious activeTabChanged).
    auto const afterFirst = m.activeTabChangedCount;
    m.model().activateTab(m.window().id(), m.window().tabAt(0)->id());
    CHECK(m.activeTabChangedCount == afterFirst);
    (void) b;
}

TEST_CASE("TabListModel: switching the active tab repaints the old and new rows' IsActiveRole",
          "[contour][gui][model]")
{
    // Regression for the tab-strip highlight bug: IsActiveRole is per-row (`row == activeTabIndex()`)
    // and the QML highlight is painted from that role (not from ListView.currentIndex). When the
    // active tab switches, activeTabChanged must emit a dataChanged whose range covers BOTH the
    // previously-active row and the newly-active row with IsActiveRole in the roles list — otherwise
    // no delegate re-reads isActive and the highlight stays on the old tab. The earlier "activating a
    // tab by row" test only checks the role VALUES; this one asserts the dataChanged SIGNAL.
    TabListModel m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Warning);

    m.model().createTab(m.window().id());           // row 0
    m.model().createTab(m.window().id());           // row 1
    auto* c = m.model().createTab(m.window().id()); // row 2, active (newest)
    REQUIRE(m.window().activeTab() == c);
    REQUIRE(m.window().activeTabIndex() == 2);

    // Spy AFTER the initial tabs exist so creation's activeTabChanged emissions aren't counted.
    QSignalSpy spy(&m, &QAbstractItemModel::dataChanged);
    REQUIRE(spy.isValid());

    // Activate row 0; the active tab moves 2 -> 0.
    m.model().activateTab(m.window().id(), m.window().tabAt(0)->id());
    REQUIRE(m.window().activeTabIndex() == 0);

    // Some dataChanged emission must span both the old row (2) and the new row (0) with IsActiveRole
    // in its roles list. The full-range emit (top-left row 0 .. bottom-right row 2) satisfies this; an
    // emission touching only one row, or omitting IsActiveRole, would fail. An empty roles list is
    // treated as "all roles" per Qt's dataChanged contract.
    auto const isActiveRole = static_cast<int>(TabListModel::IsActiveRole);
    auto const covered = std::ranges::any_of(spy, [&](QList<QVariant> const& args) {
        auto const top = args.at(0).value<QModelIndex>().row();
        auto const bottom = args.at(1).value<QModelIndex>().row();
        auto const roles = args.at(2).value<QList<int>>();
        auto const spansBoth = top <= 0 && bottom >= 2;
        auto const hasRole = roles.isEmpty() || roles.contains(isActiveRole);
        return spansBoth && hasRole;
    });
    CHECK(covered);

    // And the projected role values flipped, as the highlight binding reads them.
    CHECK(m.data(m.index(0), TabListModel::IsActiveRole).toBool());
    CHECK_FALSE(m.data(m.index(2), TabListModel::IsActiveRole).toBool());
}

TEST_CASE("TabListModel: reordering a tab uses the row-move contract", "[contour][gui][model]")
{
    // The tab-move finding: a reorder must be projected as beginMoveRows/endMoveRows, not a
    // dataChanged over the range. The QAbstractItemModelTester validates the move's destination
    // index convention (toIndex+1 for a downward move) and the begin/end pairing; an in-place
    // dataChanged that lied about a move would leave the row count/order inconsistent here.
    TabListModel m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Warning);

    auto* a = m.model().createTab(m.window().id());
    auto* b = m.model().createTab(m.window().id());
    auto* c = m.model().createTab(m.window().id());
    REQUIRE(m.rowCount() == 3);

    SECTION("downward move (front to back)")
    {
        m.model().moveTab(m.window().id(), a->id(), 2); // [b, c, a]
        CHECK(m.rowCount() == 3);
        CHECK(m.window().tabAt(0) == b);
        CHECK(m.window().tabAt(1) == c);
        CHECK(m.window().tabAt(2) == a);
    }

    SECTION("upward move (back to front)")
    {
        m.model().moveTab(m.window().id(), c->id(), 0); // [c, a, b]
        CHECK(m.rowCount() == 3);
        CHECK(m.window().tabAt(0) == c);
        CHECK(m.window().tabAt(1) == a);
        CHECK(m.window().tabAt(2) == b);
    }
}

TEST_CASE("TabListModel: a model-driven ratio change routes to the split node", "[contour][gui][model]")
{
    // The empty-paneRatioChanged finding: a model-driven setPaneRatio (not the QML drag) must reach
    // the split node's proxy slot, keyed by the split node's PaneId, so the divider re-binds.
    TabListModel m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Warning);

    auto* tab = m.model().createTab(m.window().id());
    m.model().splitActivePane(tab->id(), SplitState::Vertical, 0.5);
    auto const splitNodeId = tab->rootPane()->id().value;

    auto const baseline = m.ratioChangedCount;
    m.model().setPaneRatio(tab->id(), tab->rootPane()->id(), 0.3);

    CHECK(m.ratioChangedCount == baseline + 1);
    CHECK(m.lastRatioPaneId == splitNodeId); // routed by the split node's PaneId, the proxy key
    CHECK(m.lastRatio == 0.3);
    CHECK(tab->rootPane()->ratio() == 0.3);
}

TEST_CASE("TabListModel: focusing another pane notifies active-session consumers", "[contour][gui][model]")
{
    // Regression for the "window title stale on pane focus" finding: focusing a different pane within a
    // split tab does not rebuild the tree (no activeTabRootPaneChanged), so the manager emits
    // activeSessionChanged() from activePaneChanged to keep window-level bindings (the window title)
    // following the focused pane. Assert that a pane-focus change does fire that notification.
    TabListModel m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Warning);

    auto* tab = m.model().createTab(m.window().id());
    auto* newLeaf = m.model().splitActivePane(tab->id(), SplitState::Vertical, 0.5);
    REQUIRE(newLeaf != nullptr);
    // The new leaf becomes active on split; focus the OTHER (original) leaf to force a real change.
    auto* firstLeaf = tab->rootPane()->first();
    REQUIRE(firstLeaf != nullptr);
    REQUIRE(firstLeaf->isLeaf());
    REQUIRE(tab->activePane() != firstLeaf);

    auto const before = m.activeSessionChangedCount;
    m.model().setActivePane(tab->id(), firstLeaf->id());

    CHECK(tab->activePane() == firstLeaf);            // focus actually moved
    CHECK(m.activeSessionChangedCount == before + 1); // active-session consumers were notified
}

TEST_CASE("TabListModel: closeOtherTabs/closeTabsToRight keep a split tab intact", "[contour][gui][model]")
{
    // F13: the manager now delegates to the tab-based SessionModel close methods instead of indexing
    // a per-pane vector. A kept tab that is split must survive with BOTH panes — the pane-space
    // indexing it replaced would have mismatched rows and panes.
    TabListModel m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Warning);

    auto* a = m.model().createTab(m.window().id());
    auto* keep = m.model().createTab(m.window().id()); // will be split
    auto* c = m.model().createTab(m.window().id());
    m.model().splitActivePane(keep->id(), SplitState::Vertical); // keep now has 2 panes
    REQUIRE(m.rowCount() == 3);
    REQUIRE(keep->paneCount() == 2);

    SECTION("closeOtherTabs keeps only the split tab, with both its panes")
    {
        m.model().closeOtherTabs(m.window().id(), keep->id());
        CHECK(m.rowCount() == 1);
        CHECK(m.window().tabAt(0) == keep);
        CHECK(keep->paneCount() == 2); // the split survives intact
    }

    SECTION("closeTabsToRight from the split tab drops only the tab(s) to its right")
    {
        m.model().closeTabsToRight(m.window().id(), keep->id());
        CHECK(m.rowCount() == 2); // a and keep remain; c dropped
        CHECK(m.window().tabAt(0) == a);
        CHECK(m.window().tabAt(1) == keep);
        CHECK(keep->paneCount() == 2);
    }
    (void) c;
}

TEST_CASE("TabListModel: out-of-range and parented indices yield empty data", "[contour][gui][model]")
{
    TabListModel m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Warning);
    m.model().createTab(m.window().id());

    CHECK_FALSE(m.data(m.index(5), TabListModel::TitleRole).isValid());
    CHECK(m.rowCount(m.index(0)) == 0); // list model: rows only under the root
}

TEST_CASE("TabListModel: the default tab-label template shows the session title verbatim",
          "[contour][gui][model][tablabel]")
{
    // With the default "{WindowTitle}" template, TitleRole reproduces the pre-template behavior:
    // the tab shows its active leaf session's own title, with no position prefix.
    TabListModel m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Warning);

    auto* a = m.model().createTab(m.window().id());
    auto* b = m.model().createTab(m.window().id());
    m.setSessionTitle(a->activePane()->session(), "vim");
    m.setSessionTitle(b->activePane()->session(), "bash");

    CHECK(m.data(m.index(0), TabListModel::TitleRole).toString() == "vim");
    CHECK(m.data(m.index(1), TabListModel::TitleRole).toString() == "bash");
}

TEST_CASE("TabListModel: a positional template prefixes the 1-based tab position",
          "[contour][gui][model][tablabel]")
{
    TabListModel m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Warning);
    m.setTabLabelTemplate("{TabPosition}: {WindowTitle}");

    auto* a = m.model().createTab(m.window().id());
    auto* b = m.model().createTab(m.window().id());
    m.setSessionTitle(a->activePane()->session(), "vim");
    m.setSessionTitle(b->activePane()->session(), "bash");

    CHECK(m.data(m.index(0), TabListModel::TitleRole).toString() == "1: vim");
    CHECK(m.data(m.index(1), TabListModel::TitleRole).toString() == "2: bash");
}

TEST_CASE("TabListModel: a user rename overrides the profile template", "[contour][gui][model][tablabel]")
{
    // A rename replaces the profile's tab-label template for that one tab. A plain rename (no
    // placeholders) therefore shows verbatim, without the position prefix the profile template adds.
    TabListModel m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Warning);
    m.setTabLabelTemplate("{TabPosition}: {WindowTitle}");

    auto* a = m.model().createTab(m.window().id());
    m.setSessionTitle(a->activePane()->session(), "vim");
    m.model().setTabTitle(a->id(), "deploy");

    CHECK(m.data(m.index(0), TabListModel::TitleRole).toString() == "deploy");

    // Clearing the rename restores the profile-templated label.
    m.model().resetTabTitle(a->id());
    CHECK(m.data(m.index(0), TabListModel::TitleRole).toString() == "1: vim");
}

TEST_CASE("TabListModel: a rename containing placeholders is itself expanded",
          "[contour][gui][model][tablabel]")
{
    // The rename acts as the template for its tab: a rename of "{WindowTitle}" tracks the live title,
    // and a rename combining placeholders and literals expands all of them. This is the fix for the
    // reported "{WindowTitle} printed literally when set as the tab name" behavior.
    TabListModel m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Warning);
    m.setTabLabelTemplate("plain"); // profile template differs, to prove the rename drives expansion

    auto* a = m.model().createTab(m.window().id());
    auto* b = m.model().createTab(m.window().id());
    m.setSessionTitle(a->activePane()->session(), "vim");
    m.setSessionTitle(b->activePane()->session(), "bash");

    m.model().setTabTitle(a->id(), "{WindowTitle}");
    CHECK(m.data(m.index(0), TabListModel::TitleRole).toString() == "vim");

    m.model().setTabTitle(b->id(), "[{TabPosition}] {WindowTitle}");
    CHECK(m.data(m.index(1), TabListModel::TitleRole).toString() == "[2] bash");
}

TEST_CASE("TabListModel: a split tab shows the multi-pane sentinel, not the template",
          "[contour][gui][model][tablabel]")
{
    TabListModel m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Warning);
    m.setTabLabelTemplate("{TabPosition}: {WindowTitle}");

    auto* a = m.model().createTab(m.window().id());
    m.setSessionTitle(a->activePane()->session(), "vim");
    auto* leaf = m.model().splitActivePane(a->id(), SplitState::Vertical);
    REQUIRE(leaf != nullptr);

    CHECK(m.data(m.index(0), TabListModel::TitleRole).toString() == "Multiple panes");
}

TEST_CASE("TabListModel: closing a tab renumbers positional labels and re-emits TitleRole",
          "[contour][gui][model][tablabel]")
{
    // The stale-position fix: with a positional template, closing a tab shifts the {TabPosition} of
    // every later tab. tabClosed must re-emit TitleRole across the surviving rows so the strip
    // renumbers; without it, later tabs would keep their old numbers until some other event.
    TabListModel m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Warning);
    m.setTabLabelTemplate("{TabPosition}: {WindowTitle}");

    auto* a = m.model().createTab(m.window().id());
    auto* b = m.model().createTab(m.window().id());
    auto* c = m.model().createTab(m.window().id());
    m.setSessionTitle(a->activePane()->session(), "a");
    m.setSessionTitle(b->activePane()->session(), "b");
    m.setSessionTitle(c->activePane()->session(), "c");
    REQUIRE(m.data(m.index(2), TabListModel::TitleRole).toString() == "3: c");

    QSignalSpy spy(&m, &QAbstractItemModel::dataChanged);
    REQUIRE(spy.isValid());

    // Close the first tab; b and c shift from positions 2,3 to 1,2.
    m.model().closeTab(m.window().id(), a->id());
    REQUIRE(m.rowCount() == 2);

    // The projected labels renumbered...
    CHECK(m.data(m.index(0), TabListModel::TitleRole).toString() == "1: b");
    CHECK(m.data(m.index(1), TabListModel::TitleRole).toString() == "2: c");

    // ...and some dataChanged emission covered the surviving rows with TitleRole (an empty roles list
    // means "all roles" per Qt's contract, which also satisfies the renumber refresh).
    auto const titleRole = static_cast<int>(TabListModel::TitleRole);
    auto const renumbered = std::ranges::any_of(spy, [&](QList<QVariant> const& args) {
        auto const top = args.at(0).value<QModelIndex>().row();
        auto const bottom = args.at(1).value<QModelIndex>().row();
        auto const roles = args.at(2).value<QList<int>>();
        auto const spansSurvivors = top <= 0 && bottom >= 1;
        auto const hasRole = roles.isEmpty() || roles.contains(titleRole);
        return spansSurvivors && hasRole;
    });
    CHECK(renumbered);
}

// {{{ Setting tab titles: raw template vs. expanded label (RawTitleRole)
//
// These cover the rename round-trip and the displayed-vs-edited split. Note on scope: the headless
// stand-in sources {WindowTitle} from its seeded sessionTitleResolver (it has no real Terminal), so
// the production "raw window-title source" fix (TerminalSession::resolvedWindowTitle) is validated by
// the expandTabLabel unit tests + manual checks; here we exercise that a rename is stored verbatim,
// exposed un-expanded via RawTitleRole for the editor, and expanded for the displayed TitleRole.

TEST_CASE("TabListModel: setting a tab title exposes the raw template and the expanded label separately",
          "[contour][gui][model][tablabel]")
{
    // The reported bug: setting "a: {WindowTitle}" must show "a: <title>" on the tab, while the rename
    // editor (RawTitleRole) must still show the un-expanded "a: {WindowTitle}" so editing keeps it.
    TabListModel m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Warning);
    m.setTabLabelTemplate("{TabPosition}: {WindowTitle}");

    auto* a = m.model().createTab(m.window().id());
    m.setSessionTitle(a->activePane()->session(), "vim");

    m.model().setTabTitle(a->id(), "a: {WindowTitle}");

    CHECK(m.data(m.index(0), TabListModel::RawTitleRole).toString() == "a: {WindowTitle}"); // editor
    CHECK(m.data(m.index(0), TabListModel::TitleRole).toString() == "a: vim");              // displayed
}

TEST_CASE("TabListModel: a plain rename round-trips verbatim in both roles",
          "[contour][gui][model][tablabel]")
{
    TabListModel m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Warning);
    m.setTabLabelTemplate("{TabPosition}: {WindowTitle}");

    auto* a = m.model().createTab(m.window().id());
    m.setSessionTitle(a->activePane()->session(), "vim");
    m.model().setTabTitle(a->id(), "deploy");

    CHECK(m.data(m.index(0), TabListModel::RawTitleRole).toString() == "deploy");
    CHECK(m.data(m.index(0), TabListModel::TitleRole).toString() == "deploy");
}

TEST_CASE("TabListModel: a never-renamed tab has an empty raw title", "[contour][gui][model][tablabel]")
{
    // The editor pre-fill: a tab that was never renamed opens the rename field blank, so accepting an
    // empty edit does not freeze the displayed expansion as a literal name.
    TabListModel m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Warning);
    m.setTabLabelTemplate("{WindowTitle}");

    auto* a = m.model().createTab(m.window().id());
    m.setSessionTitle(a->activePane()->session(), "vim");

    CHECK(m.data(m.index(0), TabListModel::RawTitleRole).toString().isEmpty());
    CHECK(m.data(m.index(0), TabListModel::TitleRole).toString() == "vim"); // label still resolves
}

TEST_CASE("TabListModel: resetting a tab title clears the raw role and restores the template",
          "[contour][gui][model][tablabel]")
{
    TabListModel m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Warning);
    m.setTabLabelTemplate("{TabPosition}: {WindowTitle}");

    auto* a = m.model().createTab(m.window().id());
    m.setSessionTitle(a->activePane()->session(), "vim");
    m.model().setTabTitle(a->id(), "renamed");
    REQUIRE(m.data(m.index(0), TabListModel::RawTitleRole).toString() == "renamed");

    m.model().resetTabTitle(a->id());
    CHECK(m.data(m.index(0), TabListModel::RawTitleRole).toString().isEmpty());
    CHECK(m.data(m.index(0), TabListModel::TitleRole).toString() == "1: vim");
}

TEST_CASE("TabListModel: setting and clearing a title emits dataChanged covering RawTitleRole",
          "[contour][gui][model][tablabel]")
{
    // The editor binds to RawTitleRole, so a rename/reset must invalidate that role or a re-opened
    // editor would show a stale template.
    TabListModel m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Warning);

    auto* a = m.model().createTab(m.window().id());
    m.setSessionTitle(a->activePane()->session(), "vim");

    auto const rawRole = static_cast<int>(TabListModel::RawTitleRole);
    auto coversRawTitleForRow0 = [&](QSignalSpy const& spy) {
        return std::ranges::any_of(spy, [&](QList<QVariant> const& args) {
            auto const top = args.at(0).value<QModelIndex>().row();
            auto const bottom = args.at(1).value<QModelIndex>().row();
            auto const roles = args.at(2).value<QList<int>>();
            auto const spansRow0 = top <= 0 && bottom >= 0;
            auto const hasRole = roles.isEmpty() || roles.contains(rawRole);
            return spansRow0 && hasRole;
        });
    };

    {
        QSignalSpy spy(&m, &QAbstractItemModel::dataChanged);
        REQUIRE(spy.isValid());
        m.model().setTabTitle(a->id(), "x");
        CHECK(coversRawTitleForRow0(spy));
    }
    {
        QSignalSpy spy(&m, &QAbstractItemModel::dataChanged);
        REQUIRE(spy.isValid());
        m.model().resetTabTitle(a->id());
        CHECK(coversRawTitleForRow0(spy));
    }
}

TEST_CASE("TabListModel: a templated rename's raw role survives a positional renumber",
          "[contour][gui][model][tablabel]")
{
    // Proves the displayed/edit split holds across structural change: closing a sibling renumbers the
    // displayed {TabPosition} but must NOT rewrite the stored rename template the editor reads.
    TabListModel m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Warning);
    m.setTabLabelTemplate("plain");

    auto* a = m.model().createTab(m.window().id());
    auto* b = m.model().createTab(m.window().id());
    m.setSessionTitle(b->activePane()->session(), "bash");
    m.model().setTabTitle(b->id(), "{TabPosition}: {WindowTitle}");
    REQUIRE(m.data(m.index(1), TabListModel::TitleRole).toString() == "2: bash");

    m.model().closeTab(m.window().id(), a->id()); // b moves from row 1 to row 0

    CHECK(m.data(m.index(0), TabListModel::RawTitleRole).toString() == "{TabPosition}: {WindowTitle}");
    CHECK(m.data(m.index(0), TabListModel::TitleRole).toString() == "1: bash"); // renumbered
}

TEST_CASE("TabListModel: a tab rename republishes the indicator status line",
          "[contour][gui][model][tablabel]")
{
    // Regression for the "status line {Tabs} goes stale on rename" finding: the indicator status
    // line's {Tabs} entry is built from the tab titles, so a rename must republish it — exactly as a
    // color change does. Before the fix tabTitleChanged() refreshed only the QML tab-strip row and
    // skipped updateStatusLine(), so the status line kept the old name until an unrelated event.
    TabListModel m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Warning);
    auto* a = m.model().createTab(m.window().id());

    SECTION("a rename refreshes the status line, like a color change does")
    {
        auto const before = m.statusLineUpdateCount;
        m.model().setTabTitle(a->id(), "deploy");
        CHECK(m.statusLineUpdateCount == before + 1);
    }

    SECTION("a color change refreshes the status line (the established contract this mirrors)")
    {
        auto const before = m.statusLineUpdateCount;
        m.model().setTabColor(a->id(), vtbackend::RGBColor { 10, 20, 30 });
        CHECK(m.statusLineUpdateCount == before + 1);
    }

    SECTION("resetting a rename also refreshes the status line")
    {
        m.model().setTabTitle(a->id(), "deploy");
        auto const before = m.statusLineUpdateCount;
        m.model().resetTabTitle(a->id());
        CHECK(m.statusLineUpdateCount == before + 1);
    }
}
// }}}
