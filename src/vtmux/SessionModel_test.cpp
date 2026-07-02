// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include <vtmux/ModelEvents.h>
#include <vtmux/SessionModel.h>

using namespace vtmux;

namespace
{
/// Records every model event so tests can assert what fired.
struct RecordingEvents: ModelEvents
{
    std::vector<std::string> log;

    void tabAboutToBeAdded(WindowId, int index) override
    {
        log.push_back(std::format("tabAboutToBeAdded:{}", index));
    }
    void tabAdded(WindowId, TabId t, int index) override
    {
        log.push_back(std::format("tabAdded:{}:{}", t.value, index));
    }
    void tabAboutToBeRemoved(WindowId, int index) override
    {
        log.push_back(std::format("tabAboutToBeRemoved:{}", index));
    }
    void tabClosed(WindowId, TabId t, int index) override
    {
        log.push_back(std::format("tabClosed:{}:{}", t.value, index));
    }
    void tabAboutToBeMoved(WindowId, int from, int to) override
    {
        log.push_back(std::format("tabAboutToBeMoved:{}>{}", from, to));
    }
    void tabMoved(WindowId, TabId t, int from, int to) override
    {
        log.push_back(std::format("tabMoved:{}:{}>{}", t.value, from, to));
    }
    void activeTabChanged(WindowId, TabId t, int index) override
    {
        log.push_back(std::format("activeTab:{}:{}", t.value, index));
    }
    void paneSplit(TabId t, PaneId splitNode, PaneId newLeaf) override
    {
        log.push_back(std::format("paneSplit:{}:{}", t.value, newLeaf.value));
        lastSplitNode = splitNode;
        lastNewLeaf = newLeaf;
    }
    void paneClosed(TabId t, PaneId closed, PaneId survivor) override
    {
        // Log the survivor too, so the collapse/fold contract (which leaf absorbs) can be asserted.
        log.push_back(std::format("paneClosed:{}:{}:{}", t.value, closed.value, survivor.value));
        lastClosedSurvivor = survivor;
    }
    void activePaneChanged(TabId t, PaneId leaf) override
    {
        log.push_back(std::format("activePane:{}:{}", t.value, leaf.value));
        lastActivePane = leaf;
    }
    void paneRatioChanged(TabId, PaneId node, double ratio) override
    {
        log.push_back(std::format("ratio:{}:{}", node.value, ratio));
        lastRatio = ratio;
    }

    // Last-seen values for direct assertions (the log strings are for ordering/count).
    std::optional<PaneId> lastSplitNode;
    std::optional<PaneId> lastNewLeaf;
    std::optional<PaneId> lastClosedSurvivor;
    std::optional<PaneId> lastActivePane;
    std::optional<double> lastRatio;

    /// Count how many log entries start with @p prefix (for exact-once assertions).
    [[nodiscard]] long countPrefix(std::string const& prefix) const
    {
        return std::count_if(log.begin(), log.end(), [&](auto const& e) { return e.rfind(prefix, 0) == 0; });
    }
    void tabTitleChanged(TabId t) override { log.push_back(std::format("title:{}", t.value)); }
    void tabColorChanged(TabId t) override { log.push_back(std::format("color:{}", t.value)); }

    [[nodiscard]] bool sawPrefix(std::string const& prefix) const
    {
        for (auto const& e: log)
            if (e.rfind(prefix, 0) == 0)
                return true;
        return false;
    }
};

struct Fixture
{
    RecordingEvents events;
    uint64_t nextSession = 1000;
    SessionModel model { events, [this] {
                            return SessionId { nextSession++ };
                        } };
};
} // namespace

TEST_CASE("SessionModel: creating tabs notifies and tracks the active tab", "[vtmux][model][tab]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    REQUIRE(win != nullptr);
    CHECK(win->empty());

    auto* tab1 = f.model.createTab(win->id());
    auto* tab2 = f.model.createTab(win->id());

    CHECK(win->tabCount() == 2);
    CHECK(win->activeTab() == tab2); // newest tab becomes active
    CHECK(f.events.sawPrefix(std::format("tabAdded:{}", tab1->id().value)));
    CHECK(f.events.sawPrefix(std::format("tabAdded:{}", tab2->id().value)));
}

TEST_CASE("SessionModel: createTab fires tabAdded immediately followed by activeTabChanged",
          "[vtmux][model][tab]")
{
    // The manager relies on this exact ordering to avoid double work: tabAdded does only the Qt row
    // insert and activeTabChanged does the active-index signal + PaneProxy rebuild. If createTab ever
    // stopped firing activeTabChanged right after tabAdded, the new tab's proxies would never build.
    Fixture f;
    auto* win = f.model.createWindow();

    auto* tab = f.model.createTab(win->id());
    auto const added = std::format("tabAdded:{}:0", tab->id().value);
    auto const active = std::format("activeTab:{}:0", tab->id().value);

    auto const addedIt = std::ranges::find(f.events.log, added);
    auto const activeIt = std::ranges::find(f.events.log, active);
    REQUIRE(addedIt != f.events.log.end());
    REQUIRE(activeIt != f.events.log.end());
    // activeTabChanged for the new tab comes strictly after its tabAdded.
    CHECK(addedIt < activeIt);
    CHECK(std::next(addedIt) == activeIt); // immediately after, nothing in between
}

TEST_CASE("SessionModel: structural tab events bracket the mutation", "[vtmux][model][tab]")
{
    // Every structural change fires a paired about-to/after event so a Qt QAbstractItemModel host can map
    // them to beginInsertRows/endInsertRows (etc.), whose contract requires the "begin" call while the
    // model still reports the OLD row count. Assert both the pairing and the ordering the host relies on.
    Fixture f;
    auto* win = f.model.createWindow();

    // Insert: tabAboutToBeAdded must precede tabAdded, at the append index.
    auto* a = f.model.createTab(win->id());
    auto* b = f.model.createTab(win->id());
    auto const aboutAdd = std::ranges::find(f.events.log, "tabAboutToBeAdded:0");
    auto const added = std::ranges::find(f.events.log, std::format("tabAdded:{}:0", a->id().value));
    REQUIRE(aboutAdd != f.events.log.end());
    REQUIRE(added != f.events.log.end());
    CHECK(aboutAdd < added);
    CHECK(std::next(aboutAdd) == added); // strictly bracketing, nothing between

    // Move: tabAboutToBeMoved must precede tabMoved.
    f.model.moveTab(win->id(), a->id(), 1);
    auto const aboutMove = std::ranges::find(f.events.log, "tabAboutToBeMoved:0>1");
    auto const moved = std::ranges::find(f.events.log, std::format("tabMoved:{}:0>1", a->id().value));
    REQUIRE(aboutMove != f.events.log.end());
    REQUIRE(moved != f.events.log.end());
    CHECK(aboutMove < moved);
    CHECK(std::next(aboutMove) == moved);

    // Remove: tabAboutToBeRemoved must precede tabClosed. `b` now sits at row 0 after the move above.
    // Capture b's id and row BEFORE the close destroys the Tab (reading them after would be a dangling
    // read, the very bug Tab::closePane was hardened against).
    auto const bId = b->id().value;
    auto const bRow = win->indexOf(b->id());
    f.model.closeTab(win->id(), b->id());
    auto const aboutRemove = std::ranges::find(f.events.log, std::format("tabAboutToBeRemoved:{}", bRow));
    auto const closed = std::ranges::find(f.events.log, std::format("tabClosed:{}:{}", bId, bRow));
    REQUIRE(aboutRemove != f.events.log.end());
    REQUIRE(closed != f.events.log.end());
    CHECK(aboutRemove < closed);
    CHECK(std::next(aboutRemove) == closed);
}

TEST_CASE("SessionModel: activateTab and closeTab keep the active index sane", "[vtmux][model][tab]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* a = f.model.createTab(win->id());
    auto* b = f.model.createTab(win->id());
    auto* c = f.model.createTab(win->id());
    REQUIRE(win->activeTab() == c);

    f.model.activateTab(win->id(), a->id());
    CHECK(win->activeTab() == a);

    // Close the active first tab -> active moves to a surviving tab.
    f.model.closeTab(win->id(), a->id());
    CHECK(win->tabCount() == 2);
    CHECK(win->activeTab() != nullptr);
    CHECK((win->activeTab() == b || win->activeTab() == c));
}

TEST_CASE("SessionModel: closeTabAt keeps the active index on the same tab", "[vtmux][model][tab]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* a = f.model.createTab(win->id());
    auto* b = f.model.createTab(win->id());
    auto* c = f.model.createTab(win->id());

    SECTION("closing a tab LEFT of the active one decrements the active index onto the same tab")
    {
        f.model.activateTab(win->id(), b->id()); // active b at index 1
        REQUIRE(win->activeTabIndex() == 1);
        f.events.log.clear();

        f.model.closeTab(win->id(), a->id()); // erase index 0 -> [b, c]
        CHECK(win->activeTab() == b);         // must still be b, not c
        CHECK(win->activeTabIndex() == 0);    // b shifted down to index 0
        // The active *index* changed (1 -> 0) so observers tracking it by index must be told.
        CHECK(f.events.sawPrefix(std::format("activeTab:{}:0", b->id().value)));
    }

    SECTION("closing a tab RIGHT of the active one leaves the active index untouched and silent")
    {
        f.model.activateTab(win->id(), b->id()); // active b at index 1
        REQUIRE(win->activeTabIndex() == 1);
        f.events.log.clear();

        f.model.closeTab(win->id(), c->id()); // erase index 2 -> [a, b]
        CHECK(win->activeTab() == b);
        CHECK(win->activeTabIndex() == 1);             // unchanged
        CHECK_FALSE(f.events.sawPrefix("activeTab:")); // active tab object and index both unchanged
    }

    SECTION("closing the active tab adopts the tab now occupying that slot")
    {
        f.model.activateTab(win->id(), b->id()); // active b at index 1
        REQUIRE(win->activeTabIndex() == 1);
        f.events.log.clear();

        f.model.closeTab(win->id(), b->id()); // erase index 1 -> [a, c], slot 1 now holds c
        CHECK(win->activeTab() == c);
        CHECK(win->activeTabIndex() == 1);
        // Index value is unchanged but the active *tab object* changed, so we must still notify.
        CHECK(f.events.sawPrefix(std::format("activeTab:{}:1", c->id().value)));
    }

    SECTION("closing the active last tab clamps the active index down")
    {
        f.model.activateTab(win->id(), c->id()); // active c at index 2
        REQUIRE(win->activeTabIndex() == 2);
        f.events.log.clear();

        f.model.closeTab(win->id(), c->id()); // erase index 2 -> [a, b], must clamp to index 1
        CHECK(win->activeTab() == b);
        CHECK(win->activeTabIndex() == 1);
        CHECK(f.events.sawPrefix(std::format("activeTab:{}:1", b->id().value)));
    }
}

TEST_CASE("SessionModel: closeOtherTabs and closeTabsToRight", "[vtmux][model][tab]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* a = f.model.createTab(win->id());
    auto* b = f.model.createTab(win->id());
    auto* c = f.model.createTab(win->id());
    auto const aId = a->id();
    auto const bId = b->id();
    auto const cId = c->id();

    SECTION("closeOtherTabs keeps only the anchor and closes the rest")
    {
        f.model.closeOtherTabs(win->id(), bId);
        CHECK(win->tabCount() == 1);
        CHECK(win->activeTab() == b);
        CHECK(win->indexOf(bId) == 0);
        // The two non-anchor tabs are closed; the anchor is not.
        CHECK(f.events.sawPrefix(std::format("tabClosed:{}", aId.value)));
        CHECK(f.events.sawPrefix(std::format("tabClosed:{}", cId.value)));
        CHECK_FALSE(f.events.sawPrefix(std::format("tabClosed:{}", bId.value)));
    }

    SECTION("closeOtherTabs with the first tab as anchor")
    {
        f.model.closeOtherTabs(win->id(), aId);
        CHECK(win->tabCount() == 1);
        CHECK(win->activeTab() == a);
    }

    SECTION("closeOtherTabs with the last tab as anchor")
    {
        f.model.closeOtherTabs(win->id(), cId);
        CHECK(win->tabCount() == 1);
        CHECK(win->activeTab() == c);
    }

    SECTION("closeTabsToRight closes everything after the anchor")
    {
        f.model.closeTabsToRight(win->id(), bId);
        CHECK(win->tabCount() == 2); // a and b remain
        CHECK(win->tabAt(0) == a);
        CHECK(win->tabAt(1) == b);
        CHECK(f.events.sawPrefix(std::format("tabClosed:{}", cId.value)));
        CHECK_FALSE(f.events.sawPrefix(std::format("tabClosed:{}", aId.value)));
        CHECK_FALSE(f.events.sawPrefix(std::format("tabClosed:{}", bId.value)));
    }

    SECTION("closeTabsToRight on the last tab is a no-op")
    {
        f.model.closeTabsToRight(win->id(), cId);
        CHECK(win->tabCount() == 3); // nothing to the right of c
        CHECK_FALSE(f.events.sawPrefix("tabClosed:"));
    }
}

TEST_CASE("SessionModel: moveTab on an empty or unknown tab is a safe no-op", "[vtmux][model][tab]")
{
    Fixture f;
    auto* win = f.model.createWindow();

    SECTION("empty window: moveTab must not evaluate std::clamp with an inverted range")
    {
        // win->_tabs is empty, so size()-1 == -1. The buggy version called
        // std::clamp(toIndex, 0, -1) (lo > hi == undefined behavior) before the from<0 guard;
        // under this build's -D_GLIBCXX_ASSERTIONS that traps. This must simply do nothing.
        f.model.moveTab(win->id(), TabId { 123 }, 0);
        CHECK(win->tabCount() == 0);
        CHECK(win->activeTabIndex() == -1);
        CHECK(f.events.log.empty());
    }

    SECTION("unknown tab id in a non-empty window is a no-op")
    {
        f.model.createTab(win->id());
        f.model.createTab(win->id());
        f.events.log.clear();

        f.model.moveTab(win->id(), TabId { 9999 }, 0); // no such tab
        CHECK(win->tabCount() == 2);
        CHECK(f.events.log.empty());
    }
}

TEST_CASE("SessionModel: moveTab reorders tabs the way the manager's MoveTab* actions route",
          "[vtmux][model][tab]")
{
    // The TerminalSessionManager keyboard actions delegate to SessionModel::moveTab:
    //   MoveTabTo(position)  -> moveTab(tab, position - 1)   (position is 1-based)
    //   MoveTabToLeft        -> moveTab(tab, row - 1)
    //   MoveTabToRight       -> moveTab(tab, row + 1)
    // This pins that contract at the model layer; before the fix those actions swapped a legacy
    // vector and never reached the model, so the visible (model-driven) tab order never changed.
    Fixture f;
    auto* win = f.model.createWindow();
    auto* a = f.model.createTab(win->id());
    auto* b = f.model.createTab(win->id());
    auto* c = f.model.createTab(win->id()); // order: [a, b, c]

    SECTION("MoveTabToRight on b (row 1) lands it at row 2")
    {
        f.model.moveTab(win->id(), b->id(), 1 + 1);
        CHECK(win->tabAt(0) == a);
        CHECK(win->tabAt(1) == c);
        CHECK(win->tabAt(2) == b);
    }

    SECTION("MoveTabToLeft on c (row 2) lands it at row 1")
    {
        f.model.moveTab(win->id(), c->id(), 2 - 1);
        CHECK(win->tabAt(0) == a);
        CHECK(win->tabAt(1) == c);
        CHECK(win->tabAt(2) == b);
    }

    SECTION("MoveTabTo(position=1) sends c to the front")
    {
        f.model.moveTab(win->id(), c->id(), 1 - 1);
        CHECK(win->tabAt(0) == c);
        CHECK(win->tabAt(1) == a);
        CHECK(win->tabAt(2) == b);
    }
}

TEST_CASE("SessionModel: moveTab reorders tabs regardless of per-tab pane counts", "[vtmux][model][tab]")
{
    // The QML drag path (TerminalSessionManager::moveTab) used to reorder a pane-space _sessions
    // vector by tab-row indices, which scrambled once a tab had multiple panes. Reordering through
    // the model is tab-space, so splitting a tab (raising its pane count above 1) must not perturb
    // how a tab-row move reorders the tabs.
    Fixture f;
    auto* win = f.model.createWindow();
    auto* t0 = f.model.createTab(win->id());
    auto* t1 = f.model.createTab(win->id());
    auto* t2 = f.model.createTab(win->id()); // [t0, t1, t2]

    // Split t0 twice so it hosts 3 panes; the others stay single-pane. Pane count now diverges from
    // tab count (5 panes across 3 tabs).
    f.model.splitActivePane(t0->id(), SplitState::Vertical);
    f.model.splitActivePane(t0->id(), SplitState::Horizontal);
    REQUIRE(t0->paneCount() == 3);

    // Drag the last tab (row 2) to the front (row 0).
    f.model.moveTab(win->id(), t2->id(), 0);
    CHECK(win->tabAt(0) == t2);
    CHECK(win->tabAt(1) == t0);
    CHECK(win->tabAt(2) == t1);
    // The multi-pane tab is intact after the move.
    CHECK(t0->paneCount() == 3);
}

TEST_CASE("SessionModel: moveTab preserves the active tab across reorder", "[vtmux][model][tab]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* a = f.model.createTab(win->id());
    auto* b = f.model.createTab(win->id());
    auto* c = f.model.createTab(win->id());
    f.model.activateTab(win->id(), a->id());

    f.model.moveTab(win->id(), a->id(), 2); // move a to the end
    CHECK(win->tabAt(2) == a);
    CHECK(win->activeTab() == a); // still active after the move
    CHECK(win->tabAt(0) == b);
    CHECK(win->tabAt(1) == c);
}

TEST_CASE("SessionModel: moveTab fires activeTabChanged when the active index shifts", "[vtmux][model][tab]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* a = f.model.createTab(win->id());
    auto* b = f.model.createTab(win->id());
    auto* c = f.model.createTab(win->id());

    SECTION("moving the active tab to a new index notifies with the new index")
    {
        f.model.activateTab(win->id(), c->id());
        REQUIRE(win->activeTabIndex() == 2);
        f.events.log.clear();

        f.model.moveTab(win->id(), c->id(), 0); // [c, a, b], c still active at index 0
        CHECK(win->activeTab() == c);
        CHECK(win->activeTabIndex() == 0);
        CHECK(f.events.sawPrefix("tabMoved:"));
        CHECK(f.events.sawPrefix(std::format("activeTab:{}:0", c->id().value)));
    }

    SECTION("moving a non-active tab across the active one shifts and notifies the active index")
    {
        f.model.activateTab(win->id(), b->id()); // active b at index 1
        REQUIRE(win->activeTabIndex() == 1);
        f.events.log.clear();

        f.model.moveTab(win->id(), a->id(), 2); // [b, c, a], b now at index 0
        CHECK(win->activeTab() == b);
        CHECK(win->activeTabIndex() == 0);
        CHECK(f.events.sawPrefix(std::format("activeTab:{}:0", b->id().value)));
    }

    SECTION("a move that leaves the active index unchanged does not notify activeTabChanged")
    {
        f.model.activateTab(win->id(), a->id()); // active a at index 0
        REQUIRE(win->activeTabIndex() == 0);
        f.events.log.clear();

        f.model.moveTab(win->id(), b->id(), 2); // [a, c, b], a stays at index 0
        CHECK(win->activeTab() == a);
        CHECK(win->activeTabIndex() == 0);
        CHECK(f.events.sawPrefix("tabMoved:"));
        CHECK_FALSE(f.events.sawPrefix("activeTab:"));
    }
}

TEST_CASE("SessionModel: splitting a tab's pane fires the expected events", "[vtmux][model][split]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* tab = f.model.createTab(win->id());

    // Observe only the split's events: createTab above legitimately fired tabAdded.
    f.events.log.clear();
    auto* newLeaf = f.model.splitActivePane(tab->id(), SplitState::Vertical);
    REQUIRE(newLeaf != nullptr);
    CHECK(tab->paneCount() == 2);
    CHECK(tab->activePane() == newLeaf);
    // A split adds a pane WITHIN the tab; it must NOT add a tab (the tab strip keeps one row). This
    // is the invariant the Qt list-model's rowCount() relies on to avoid a phantom tab row.
    CHECK(win->tabCount() == 1);
    CHECK_FALSE(f.events.sawPrefix("tabAdded:"));
    CHECK(f.events.sawPrefix(std::format("paneSplit:{}", tab->id().value)));
    CHECK(f.events.sawPrefix(std::format("title:{}", tab->id().value)));
}

TEST_CASE("SessionModel: pane ops target the named tab, not the active one", "[vtmux][model][split]")
{
    // The manager's pane actions (split/close/focus) now pass the *acting* session's tab id, which
    // can differ from the model's active tab. The model already keys every pane op on an explicit
    // TabId, so a split must land in the named tab even while another tab is active — this is the
    // behavior the manager fix depends on (it must hand the right tab id to these methods).
    Fixture f;
    auto* win = f.model.createWindow();
    auto* target = f.model.createTab(win->id());
    auto* other = f.model.createTab(win->id()); // 'other' is created last, so it is active
    REQUIRE(win->activeTab() == other);
    REQUIRE(target->paneCount() == 1);
    REQUIRE(other->paneCount() == 1);

    // Split the NON-active tab.
    auto* newLeaf = f.model.splitActivePane(target->id(), SplitState::Vertical);
    REQUIRE(newLeaf != nullptr);

    // Only the named tab gained a pane; the active tab is untouched.
    CHECK(target->paneCount() == 2);
    CHECK(other->paneCount() == 1);
    CHECK(f.events.sawPrefix(std::format("paneSplit:{}", target->id().value)));
    CHECK_FALSE(f.events.sawPrefix(std::format("paneSplit:{}", other->id().value)));
}

namespace
{
/// Records, inside the activePaneChanged callback, the session id the model has already assigned to
/// the just-split leaf. Used to prove the new pane's session id is final by the time the model
/// notifies — the moment the manager's proxy rebuild reads sessionForId(newLeaf.session()).
struct SplitTimingEvents: RecordingEvents
{
    SessionModel* model = nullptr;
    std::optional<uint64_t> leafSessionAtActiveChange;

    void activePaneChanged(TabId t, PaneId leaf) override
    {
        RecordingEvents::activePaneChanged(t, leaf);
        if (model != nullptr)
            if (auto* tab = model->findTab(t); tab != nullptr)
                if (auto* pane = tab->rootPane()->findPane(leaf); pane != nullptr && pane->isLeaf())
                    leafSessionAtActiveChange = pane->session().value;
    }
};
} // namespace

TEST_CASE("SessionModel: a split assigns the new leaf's session before notifying", "[vtmux][model][split]")
{
    // The manager registers the backing session for the new leaf's id BEFORE calling
    // splitActivePane, because the model fires activePaneChanged synchronously and the GUI binds the
    // new pane to sessionForId(newLeaf.session()) right then. This pins the contract that makes the
    // ordering correct: the new leaf already carries its (allocator-issued) session id when
    // activePaneChanged fires, so a pre-registered session resolves and the pane is not bound to
    // null. (Before the manager fix the session was registered only after the split, binding null.)
    SplitTimingEvents events;
    uint64_t nextSession = 5000;
    SessionModel model { events, [&] {
                            return SessionId { nextSession++ };
                        } };
    events.model = &model;

    auto* win = model.createWindow();
    auto* tab = model.createTab(win->id());
    auto const rootSession = tab->activePane()->session().value;
    events.log.clear();
    events.leafSessionAtActiveChange.reset();

    auto* newLeaf = model.splitActivePane(tab->id(), SplitState::Vertical);
    REQUIRE(newLeaf != nullptr);

    // The new leaf carries a freshly allocated, non-zero session id distinct from the root pane's.
    CHECK(newLeaf->session().value != 0);
    CHECK(newLeaf->session().value != rootSession);
    // And that id was already assigned when activePaneChanged fired (what the GUI binding reads).
    REQUIRE(events.leafSessionAtActiveChange.has_value());
    CHECK(*events.leafSessionAtActiveChange == newLeaf->session().value);
}

TEST_CASE("SessionModel: closing the last pane closes the tab", "[vtmux][model][close]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* tab = f.model.createTab(win->id());
    auto const rootPaneId = tab->rootPane()->id();
    auto const tabId = tab->id(); // capture before the tab is destroyed by the close

    f.model.closePane(win->id(), tabId, rootPaneId);
    CHECK(win->tabCount() == 0);
    CHECK(f.events.sawPrefix(std::format("tabClosed:{}", tabId.value)));
}

TEST_CASE("SessionModel: closing a non-last pane keeps the tab and absorbs", "[vtmux][model][close]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* tab = f.model.createTab(win->id());
    auto* newLeaf = f.model.splitActivePane(tab->id(), SplitState::Horizontal);
    auto const newLeafId = newLeaf->id();

    f.model.closePane(win->id(), tab->id(), newLeafId);
    CHECK(win->tabCount() == 1);
    CHECK(tab->paneCount() == 1);
    CHECK(f.events.sawPrefix(std::format("paneClosed:{}", tab->id().value)));
}

TEST_CASE("SessionModel: closing every leaf of a multi-pane tab closes the tab", "[vtmux][model][close]")
{
    // This is the whole-tab close path the manager's CloseTab action now uses (close each of a
    // tab's pane sessions): the model must keep the tab alive while panes survive and tear it down
    // only with the last pane. Contrast with closing a single pane, which must keep the tab.
    Fixture f;
    auto* win = f.model.createWindow();
    auto* tab = f.model.createTab(win->id());
    f.model.splitActivePane(tab->id(), SplitState::Vertical);
    f.model.splitActivePane(tab->id(), SplitState::Horizontal);
    auto const tabId = tab->id();
    REQUIRE(tab->paneCount() == 3);
    REQUIRE(win->tabCount() == 1);

    SECTION("closing one leaf keeps the tab open (CloseTab must not stop here)")
    {
        auto const firstLeafId = tab->activePane()->id();
        f.model.closePane(win->id(), tabId, firstLeafId);
        CHECK(win->tabCount() == 1); // tab survives
        CHECK(tab->paneCount() == 2);
    }

    SECTION("closing all leaves tears the tab down")
    {
        // Collect every current leaf id, then close them one by one (as closeTab/closeTabAtIndex do
        // by terminating each pane session). Re-collect each round since ids/structure change.
        while (win->tabCount() == 1)
        {
            auto* survivingTab = win->tabAt(0);
            std::vector<PaneId> leaves;
            survivingTab->rootPane()->walkTree([&](Pane& p) {
                if (p.isLeaf())
                    leaves.push_back(p.id());
            });
            REQUIRE_FALSE(leaves.empty());
            f.model.closePane(win->id(), survivingTab->id(), leaves.front());
        }
        CHECK(win->tabCount() == 0);
        CHECK(f.events.sawPrefix(std::format("tabClosed:{}", tabId.value)));
    }
}

TEST_CASE("SessionModel: removeWindow tears the whole window down", "[vtmux][model][window]")
{
    // The manager's closeWindow() delegates the structural teardown to removeWindow and then clears
    // its own registries. removeWindow must close every tab (firing tabClosed so the Qt rows are
    // removed) regardless of how many panes each tab has, and drop the window itself.
    Fixture f;
    auto* win = f.model.createWindow();
    auto const windowId = win->id();
    auto* t0 = f.model.createTab(win->id());
    auto* t1 = f.model.createTab(win->id());
    auto const t0Id = t0->id();
    auto const t1Id = t1->id();
    f.model.splitActivePane(t1->id(), SplitState::Vertical); // t1 hosts 2 panes
    REQUIRE(win->tabCount() == 2);
    f.events.log.clear();

    f.model.removeWindow(windowId);

    // Every tab reported closed, and the window no longer resolves.
    CHECK(f.events.sawPrefix(std::format("tabClosed:{}", t0Id.value)));
    CHECK(f.events.sawPrefix(std::format("tabClosed:{}", t1Id.value)));
    CHECK(f.model.findTab(t0Id) == nullptr);
    CHECK(f.model.findTab(t1Id) == nullptr);

    // Removing a window that is already gone is a safe no-op.
    f.events.log.clear();
    f.model.removeWindow(windowId);
    CHECK(f.events.log.empty());
}

TEST_CASE("SessionModel: setPaneRatio updates the split node", "[vtmux][model][split]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* tab = f.model.createTab(win->id());
    f.model.splitActivePane(tab->id(), SplitState::Vertical, 0.5);
    auto const splitNodeId = tab->rootPane()->id();

    f.model.setPaneRatio(tab->id(), splitNodeId, 0.3);
    CHECK(tab->rootPane()->ratio() == 0.3);
    CHECK(f.events.sawPrefix(std::format("ratio:{}", splitNodeId.value)));
}

TEST_CASE("SessionModel: tab title and color mutations notify", "[vtmux][model][title][color]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* tab = f.model.createTab(win->id());

    f.model.setTabTitle(tab->id(), "build");
    CHECK(tab->runtimeTitle() == "build");
    CHECK(f.events.sawPrefix(std::format("title:{}", tab->id().value)));

    f.model.setTabColor(tab->id(), vtbackend::RGBColor { 0x33, 0xCC, 0x33 });
    REQUIRE(tab->color().has_value());
    CHECK(tab->color()->green == 0xCC);
    CHECK(f.events.sawPrefix(std::format("color:{}", tab->id().value)));

    f.model.resetTabColor(tab->id());
    CHECK_FALSE(tab->color().has_value());
}

TEST_CASE("SessionModel: color palette is non-empty and shared", "[vtmux][model][color]")
{
    Fixture f;
    CHECK_FALSE(f.model.colorPalette().empty());
    CHECK(f.model.colorPalette().size() >= 8);

    // colorPalette() is a view over a single static constexpr table (no per-instance copy/allocation):
    // repeated calls, and calls on distinct models, must all view the SAME backing storage.
    auto const first = f.model.colorPalette();
    auto const again = f.model.colorPalette();
    CHECK(first.data() == again.data());
    CHECK(first.size() == again.size());

    Fixture other;
    CHECK(other.model.colorPalette().data() == first.data());
}

// ============================================================================================
// Pane-operation coverage: focus / fold(collapse) / resize / activation event contracts. These pin the
// model-layer behaviour behind the split GUI so a focus/collapse/ratio regression fails here (headless,
// deterministic) rather than shipping — Qt keyboard focus does not resolve offscreen, so the model is the
// only reliable oracle for focus correctness.
// ============================================================================================

namespace
{
/// Builds a window + single-pane tab and returns (windowId, tabId, rootLeafId).
struct TabSetup
{
    WindowId window;
    TabId tab;
    PaneId rootLeaf;
};

[[nodiscard]] TabSetup makeTab(SessionModel& model)
{
    auto* win = model.createWindow();
    auto* tab = model.createTab(win->id());
    return { win->id(), tab->id(), tab->rootPane()->id() };
}
} // namespace

TEST_CASE("SessionModel: focusDirection moves the active pane across a nested split and names the neighbor",
          "[vtmux][model][focus]")
{
    // focusDirection had ZERO coverage. Build a nested tree and assert each direction activates the
    // geometrically-correct neighbor, a no-neighbor direction is a silent no-op, and bad ids do nothing.
    // This is the model-layer oracle for the "move focus across nested splits" behaviour the goal calls out.
    Fixture f;
    auto const [win, tab, rootLeaf] = makeTab(f.model);

    // Vertical split (side-by-side): left | right. New leaf (right) is active.
    auto* right = f.model.splitActivePane(tab, SplitState::Vertical);
    REQUIRE(right != nullptr);
    auto const rightId = right->id();
    // `left` is the original session's leaf (rootLeaf migrated into the first child on split).
    auto* leftPane = f.model.findTab(tab)->rootPane()->first();
    REQUIRE(leftPane != nullptr);
    auto const leftId = leftPane->first() ? leftPane->first()->id() : leftPane->id();

    f.events.log.clear();

    // Move focus Left: from the active right leaf to the left leaf.
    f.model.focusDirection(tab, FocusDirection::Left);
    REQUIRE(f.events.lastActivePane.has_value());
    CHECK(f.events.lastActivePane->value == leftId.value);
    CHECK(f.model.findTab(tab)->activePane()->id().value == leftId.value);
    CHECK(f.events.countPrefix("activePane") == 1);

    // Move Right: back to the right leaf.
    f.events.log.clear();
    f.model.focusDirection(tab, FocusDirection::Right);
    REQUIRE(f.events.lastActivePane.has_value());
    CHECK(f.events.lastActivePane->value == rightId.value);
    CHECK(f.events.countPrefix("activePane") == 1);

    // No vertical neighbor above/below in a purely-vertical split: Up/Down are silent no-ops.
    f.events.log.clear();
    auto const activeBefore = f.model.findTab(tab)->activePane()->id().value;
    f.model.focusDirection(tab, FocusDirection::Up);
    f.model.focusDirection(tab, FocusDirection::Down);
    CHECK(f.events.countPrefix("activePane") == 0);
    CHECK(f.model.findTab(tab)->activePane()->id().value == activeBefore);

    // Unknown tab id: no-op, no events.
    f.events.log.clear();
    f.model.focusDirection(TabId { 999999 }, FocusDirection::Left);
    CHECK(f.events.log.empty());
}

TEST_CASE("SessionModel: closePane reports the correct survivor and reactivates it (both children)",
          "[vtmux][model][fold]")
{
    // The collapse/fold contract: closing one leaf of a 2-pane tab must report the sibling as survivor and
    // make it the active pane, and the tab survives with one pane. Cover closing BOTH the original and the
    // new leaf — a wrong-survivor / wrong-active-pane bug is a top split-bug source.
    auto check = [](bool closeOriginal) {
        Fixture f;
        auto const [win, tab, rootLeaf] = makeTab(f.model);
        auto* newLeaf = f.model.splitActivePane(tab, SplitState::Vertical);
        REQUIRE(newLeaf != nullptr);
        auto const newLeafId = newLeaf->id();
        // The original session migrated into the first child on split; find that leaf id.
        auto* firstChild = f.model.findTab(tab)->rootPane()->first();
        auto const originalLeafId = firstChild->id();

        auto const toClose = closeOriginal ? originalLeafId : newLeafId;
        auto const survivor = closeOriginal ? newLeafId : originalLeafId;

        f.events.log.clear();
        f.model.closePane(win, tab, toClose);

        // paneClosed carries the correct survivor, and the survivor becomes active.
        REQUIRE(f.events.lastClosedSurvivor.has_value());
        CHECK(f.events.lastClosedSurvivor->value == survivor.value);
        REQUIRE(f.events.lastActivePane.has_value());
        CHECK(f.events.lastActivePane->value == survivor.value);

        auto* t = f.model.findTab(tab);
        REQUIRE(t != nullptr);
        CHECK(t->paneCount() == 1);
        CHECK(t->activePane()->id().value == survivor.value);
        CHECK_FALSE(t->hasMultiplePanes());

        // Event order: paneClosed -> activePane -> title, contiguous.
        auto const closedIt =
            std::ranges::find_if(f.events.log, [](auto const& e) { return e.rfind("paneClosed", 0) == 0; });
        auto const activeIt =
            std::ranges::find_if(f.events.log, [](auto const& e) { return e.rfind("activePane", 0) == 0; });
        REQUIRE(closedIt != f.events.log.end());
        REQUIRE(activeIt != f.events.log.end());
        CHECK(std::next(closedIt) == activeIt);
    };

    SECTION("closing the original (first) leaf")
    {
        check(/*closeOriginal*/ true);
    }
    SECTION("closing the new (second) leaf")
    {
        check(/*closeOriginal*/ false);
    }
}

TEST_CASE("SessionModel: setPaneRatio clamps stored ratio AND emitted value, no-op on leaf/unknown",
          "[vtmux][model][resize]")
{
    // Confirmed bug fix: the model clamped the stored ratio but emitted the RAW request, so the GUI divider
    // and the model disagreed at the extremes. Assert both the stored node ratio and the emitted event value
    // are clamped, and that the operation is a guarded no-op on a leaf / unknown ids.
    Fixture f;
    auto const [win, tab, rootLeaf] = makeTab(f.model);
    auto* newLeaf = f.model.splitActivePane(tab, SplitState::Vertical);
    REQUIRE(newLeaf != nullptr);
    auto* splitNode = f.model.findTab(tab)->rootPane(); // the root became a split node
    REQUIRE_FALSE(splitNode->isLeaf());
    auto const splitNodeId = splitNode->id();

    constexpr double MinRatio = 0.05;

    // Over-large: clamp to 1 - MinRatio in BOTH the stored value and the emitted event.
    f.events.log.clear();
    f.model.setPaneRatio(tab, splitNodeId, 1.0);
    CHECK(splitNode->ratio() == Catch::Approx(1.0 - MinRatio));
    REQUIRE(f.events.lastRatio.has_value());
    CHECK(*f.events.lastRatio == Catch::Approx(1.0 - MinRatio));

    // Too-small: clamp to MinRatio.
    f.events.log.clear();
    f.model.setPaneRatio(tab, splitNodeId, 0.0);
    CHECK(splitNode->ratio() == Catch::Approx(MinRatio));
    REQUIRE(f.events.lastRatio.has_value());
    CHECK(*f.events.lastRatio == Catch::Approx(MinRatio));

    // In-range passes through unchanged.
    f.events.log.clear();
    f.model.setPaneRatio(tab, splitNodeId, 0.3);
    CHECK(splitNode->ratio() == Catch::Approx(0.3));
    CHECK(*f.events.lastRatio == Catch::Approx(0.3));

    // No-op: on a LEAF id.
    f.events.log.clear();
    f.model.setPaneRatio(tab, newLeaf->id(), 0.4);
    CHECK(f.events.log.empty());
    // No-op: unknown pane id and unknown tab id.
    f.model.setPaneRatio(tab, PaneId { 999999 }, 0.4);
    f.model.setPaneRatio(TabId { 999999 }, splitNodeId, 0.4);
    CHECK(f.events.log.empty());
}

TEST_CASE("SessionModel: setActivePane fires activePaneChanged, no-op on already-active/internal/unknown",
          "[vtmux][model][focus]")
{
    // setActivePane (click-to-focus routing) had ZERO coverage. Assert it activates a leaf, is a no-op on the
    // already-active leaf, and rejects internal split-node ids and unknown ids (the spurious-event/bad-id
    // guards).
    Fixture f;
    auto const [win, tab, rootLeaf] = makeTab(f.model);
    auto* newLeaf = f.model.splitActivePane(tab, SplitState::Vertical); // new leaf active
    REQUIRE(newLeaf != nullptr);
    auto* firstChild = f.model.findTab(tab)->rootPane()->first();
    auto const originalLeafId = firstChild->id();
    auto const splitNodeId = f.model.findTab(tab)->rootPane()->id();

    // Activate the non-active (original) leaf -> exactly one activePaneChanged.
    f.events.log.clear();
    f.model.setActivePane(tab, originalLeafId);
    CHECK(f.events.countPrefix("activePane") == 1);
    CHECK(f.model.findTab(tab)->activePane()->id().value == originalLeafId.value);

    // Re-selecting the already-active leaf -> NO event.
    f.events.log.clear();
    f.model.setActivePane(tab, originalLeafId);
    CHECK(f.events.countPrefix("activePane") == 0);

    // Internal split-node id (not a leaf) -> NO event.
    f.events.log.clear();
    f.model.setActivePane(tab, splitNodeId);
    CHECK(f.events.countPrefix("activePane") == 0);

    // Unknown ids -> no-op.
    f.model.setActivePane(tab, PaneId { 999999 });
    f.model.setActivePane(TabId { 999999 }, originalLeafId);
    CHECK(f.events.countPrefix("activePane") == 0);
}

TEST_CASE("SessionModel: splitActivePane fires paneSplit->activePaneChanged->tabTitleChanged, once, in order",
          "[vtmux][model][split]")
{
    // The manager/GUI depend on this exact ordering and multiplicity; today only an order-agnostic prefix
    // check exists, so a duplicate/reordered emission would slip through.
    Fixture f;
    auto const [win, tab, rootLeaf] = makeTab(f.model);

    f.events.log.clear();
    auto* newLeaf = f.model.splitActivePane(tab, SplitState::Vertical);
    REQUIRE(newLeaf != nullptr);

    CHECK(f.events.countPrefix("paneSplit") == 1);
    CHECK(f.events.countPrefix("activePane") == 1);
    CHECK(f.events.countPrefix("title") == 1);

    auto const splitIt =
        std::ranges::find_if(f.events.log, [](auto const& e) { return e.rfind("paneSplit", 0) == 0; });
    auto const activeIt =
        std::ranges::find_if(f.events.log, [](auto const& e) { return e.rfind("activePane", 0) == 0; });
    auto const titleIt =
        std::ranges::find_if(f.events.log, [](auto const& e) { return e.rfind("title", 0) == 0; });
    REQUIRE(splitIt != f.events.log.end());
    REQUIRE(activeIt != f.events.log.end());
    REQUIRE(titleIt != f.events.log.end());
    CHECK(std::next(splitIt) == activeIt);
    CHECK(std::next(activeIt) == titleIt);
    // The activated pane is the new leaf.
    REQUIRE(f.events.lastActivePane.has_value());
    CHECK(f.events.lastActivePane->value == newLeaf->id().value);

    // Unknown tab id -> zero events.
    f.events.log.clear();
    CHECK(f.model.splitActivePane(TabId { 999999 }, SplitState::Vertical) == nullptr);
    CHECK(f.events.log.empty());
}

TEST_CASE("SessionModel: closePane guard and last-pane paths", "[vtmux][model][fold]")
{
    Fixture f;
    auto const [win, tab, rootLeaf] = makeTab(f.model);
    auto* newLeaf = f.model.splitActivePane(tab, SplitState::Vertical);
    REQUIRE(newLeaf != nullptr);
    auto const splitNodeId = f.model.findTab(tab)->rootPane()->id();

    // Guards: internal-node id, unknown leaf id, unknown tab -> zero events.
    f.events.log.clear();
    f.model.closePane(win, tab, splitNodeId);       // not a leaf
    f.model.closePane(win, tab, PaneId { 999999 }); // unknown
    f.model.closePane(win, TabId { 999999 }, newLeaf->id());
    CHECK(f.events.log.empty());

    // Close one pane -> collapse (paneClosed, tab survives).
    f.model.closePane(win, tab, newLeaf->id());
    CHECK(f.events.countPrefix("paneClosed") == 1);
    REQUIRE(f.model.findTab(tab) != nullptr);
    CHECK(f.model.findTab(tab)->paneCount() == 1);

    // Closing the LAST remaining pane routes to closeTab (tabClosed bracket), not paneClosed.
    auto const lastLeafId = f.model.findTab(tab)->rootPane()->id();
    f.events.log.clear();
    f.model.closePane(win, tab, lastLeafId);
    CHECK(f.events.countPrefix("paneClosed") == 0);
    CHECK(f.events.sawPrefix("tabAboutToBeRemoved"));
    CHECK(f.events.sawPrefix("tabClosed"));
    CHECK(f.model.findTab(tab) == nullptr); // tab is gone
}
