// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include <vtworkspace/ModelEvents.h>
#include <vtworkspace/SessionModel.h>

using namespace vtworkspace;

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
    void paneOrientationChanged(TabId t, PaneId splitNode, SplitState newState) override
    {
        log.push_back(
            std::format("orientation:{}:{}:{}", t.value, splitNode.value, static_cast<int>(newState)));
        lastOrientation = newState;
    }
    void paneSwapped(TabId t, PaneId a, PaneId b) override
    {
        log.push_back(std::format("swapped:{}:{}:{}", t.value, a.value, b.value));
    }
    void paneZoomChanged(TabId t, std::optional<PaneId> zoomedLeaf) override
    {
        // Log the leaf, not just "on/off": zoom follows focus, so it can move between leaves while
        // staying on, and a host has to re-read WHICH pane is on screen.
        log.push_back(std::format(
            "zoom:{}:{}", t.value, zoomedLeaf.has_value() ? std::to_string(zoomedLeaf->value) : "-"));
        lastZoomedLeaf = zoomedLeaf;
    }
    void paneTreeRestructured(TabId t) override { log.push_back(std::format("restructured:{}", t.value)); }
    void tabAboutToBeMovedToWindow(WindowId from, int fromIdx, WindowId to, int toIdx) override
    {
        log.push_back(std::format("tabAboutToMoveWin:{}:{}>{}:{}", from.value, fromIdx, to.value, toIdx));
    }
    void tabMovedToWindow(WindowId from, TabId t, int fromIdx, WindowId to, int toIdx) override
    {
        log.push_back(
            std::format("tabMovedWin:{}:{}:{}>{}:{}", from.value, t.value, fromIdx, to.value, toIdx));
    }

    // Last-seen values for direct assertions (the log strings are for ordering/count).
    std::optional<PaneId> lastSplitNode;
    std::optional<PaneId> lastNewLeaf;
    std::optional<PaneId> lastClosedSurvivor;
    std::optional<PaneId> lastActivePane;
    std::optional<double> lastRatio;
    std::optional<SplitState> lastOrientation;
    std::optional<PaneId> lastZoomedLeaf;

    /// Count how many log entries start with @p prefix (for exact-once assertions).
    [[nodiscard]] long countPrefix(std::string const& prefix) const
    {
        return std::count_if(log.begin(), log.end(), [&](auto const& e) { return e.starts_with(prefix); });
    }
    void tabTitleChanged(TabId t) override { log.push_back(std::format("title:{}", t.value)); }
    void tabColorChanged(TabId t) override { log.push_back(std::format("color:{}", t.value)); }

    [[nodiscard]] bool sawPrefix(std::string const& prefix) const
    {
        for (auto const& e: log)
            if (e.starts_with(prefix))
                return true;
        return false;
    }
};

struct Fixture
{
    RecordingEvents events;
    uint64_t nextSession = 1000;
    SessionModel model { events, [this] { return SessionId { nextSession++ }; } };
};
} // namespace

TEST_CASE("SessionModel: creating tabs notifies and tracks the active tab", "[vtworkspace][model][tab]")
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
          "[vtworkspace][model][tab]")
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

TEST_CASE("SessionModel: structural tab events bracket the mutation", "[vtworkspace][model][tab]")
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

TEST_CASE("SessionModel: activateTab and closeTab keep the active index sane", "[vtworkspace][model][tab]")
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

TEST_CASE("SessionModel: closeTabAt keeps the active index on the same tab", "[vtworkspace][model][tab]")
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

TEST_CASE("SessionModel: closeOtherTabs and closeTabsToRight", "[vtworkspace][model][tab]")
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

TEST_CASE("SessionModel: moveTab on an empty or unknown tab is a safe no-op", "[vtworkspace][model][tab]")
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
          "[vtworkspace][model][tab]")
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
        f.model.moveTab(win->id(), c->id(), 0);
        CHECK(win->tabAt(0) == c);
        CHECK(win->tabAt(1) == a);
        CHECK(win->tabAt(2) == b);
    }
}

TEST_CASE("SessionModel: moveTab reorders tabs regardless of per-tab pane counts", "[vtworkspace][model][tab]")
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

TEST_CASE("SessionModel: moveTab preserves the active tab across reorder", "[vtworkspace][model][tab]")
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

TEST_CASE("SessionModel: moveTab fires activeTabChanged when the active index shifts", "[vtworkspace][model][tab]")
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

TEST_CASE("SessionModel: splitting a tab's pane fires the expected events", "[vtworkspace][model][split]")
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

TEST_CASE("SessionModel: pane ops target the named tab, not the active one", "[vtworkspace][model][split]")
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

TEST_CASE("SessionModel: a split assigns the new leaf's session before notifying", "[vtworkspace][model][split]")
{
    // The manager registers the backing session for the new leaf's id BEFORE calling
    // splitActivePane, because the model fires activePaneChanged synchronously and the GUI binds the
    // new pane to sessionForId(newLeaf.session()) right then. This pins the contract that makes the
    // ordering correct: the new leaf already carries its (allocator-issued) session id when
    // activePaneChanged fires, so a pre-registered session resolves and the pane is not bound to
    // null. (Before the manager fix the session was registered only after the split, binding null.)
    SplitTimingEvents events;
    uint64_t nextSession = 5000;
    SessionModel model { events, [&] { return SessionId { nextSession++ }; } };
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

TEST_CASE("SessionModel: closing the last pane closes the tab", "[vtworkspace][model][close]")
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

TEST_CASE("SessionModel: closing a non-last pane keeps the tab and absorbs", "[vtworkspace][model][close]")
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

TEST_CASE("SessionModel: closing every leaf of a multi-pane tab closes the tab", "[vtworkspace][model][close]")
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

TEST_CASE("SessionModel: removeWindow tears the whole window down", "[vtworkspace][model][window]")
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

TEST_CASE("SessionModel: setPaneRatio updates the split node", "[vtworkspace][model][split]")
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

TEST_CASE("SessionModel: tab title and color mutations notify", "[vtworkspace][model][title][color]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* tab = f.model.createTab(win->id());

    f.model.setTabTitle(tab->id(), "build");
    CHECK(tab->runtimeTitle() == "build");
    CHECK(f.events.sawPrefix(std::format("title:{}", tab->id().value)));

    f.model.setTabColor(tab->id(), TabColorSource::User, vtbackend::RGBColor { 0x33, 0xCC, 0x33 });
    REQUIRE(tab->color().has_value());
    CHECK(tab->color()->green == 0xCC);
    CHECK(f.events.sawPrefix(std::format("color:{}", tab->id().value)));

    f.model.resetTabColor(tab->id(), TabColorSource::User);
    CHECK_FALSE(tab->color().has_value());
}

TEST_CASE("SessionModel: a tab color is tracked per source, and both changes are announced",
          "[vtworkspace][model][color]")
{
    // Both sources reach the same tab and both fire tabColorChanged, but they write different slots, so
    // an application's DECAC color and a color the user picked never overwrite one another.
    Fixture f;
    auto* win = f.model.createWindow();
    auto* tab = f.model.createTab(win->id());

    auto constexpr AppColor = vtbackend::RGBColor { 0x11, 0x22, 0x33 };
    auto constexpr UserColor = vtbackend::RGBColor { 0xAA, 0xBB, 0xCC };

    f.model.setTabColor(tab->id(), TabColorSource::Application, AppColor);
    CHECK(tab->color() == AppColor);
    CHECK(f.events.sawPrefix(std::format("color:{}", tab->id().value)));

    f.model.setTabColor(tab->id(), TabColorSource::User, UserColor);
    CHECK(tab->color() == UserColor);

    f.events.log.clear();
    f.model.resetTabColor(tab->id(), TabColorSource::Application);
    CHECK(tab->color() == UserColor); // the user's choice survives an application reset
    CHECK(f.events.sawPrefix(std::format("color:{}", tab->id().value)));

    f.model.resetTabColor(tab->id(), TabColorSource::User);
    CHECK_FALSE(tab->color().has_value());
}

TEST_CASE("SessionModel: an empty or blank tab title resets to the default template", "[vtworkspace][model][title]")
{
    // The GUI's inline rename editor opens pre-filled with the RAW override — empty for a
    // never-renamed tab — and commits verbatim on click-away. Without normalization that stored ""
    // as a runtime override, which takes precedence over the '{WindowTitle}' default and left the
    // tab permanently unlabeled with no UI to recover.
    Fixture f;
    auto* win = f.model.createWindow();
    auto* tab = f.model.createTab(win->id());

    f.model.setTabTitle(tab->id(), "");
    CHECK_FALSE(tab->runtimeTitle().has_value());
    CHECK(f.events.sawPrefix(std::format("title:{}", tab->id().value)));

    f.model.setTabTitle(tab->id(), "build");
    REQUIRE(tab->runtimeTitle().has_value());

    // A whitespace-only rename is visually indistinguishable from a blank label: reset as well.
    f.model.setTabTitle(tab->id(), "   ");
    CHECK_FALSE(tab->runtimeTitle().has_value());
}

TEST_CASE("SessionModel: color palette is non-empty and shared", "[vtworkspace][model][color]")
{
    Fixture const f;
    CHECK_FALSE(f.model.colorPalette().empty());
    CHECK(f.model.colorPalette().size() >= 8);

    // colorPalette() is a view over a single static constexpr table (no per-instance copy/allocation):
    // repeated calls, and calls on distinct models, must all view the SAME backing storage.
    auto const first = f.model.colorPalette();
    auto const again = f.model.colorPalette();
    CHECK(first.data() == again.data());
    CHECK(first.size() == again.size());

    Fixture const other;
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
          "[vtworkspace][model][focus]")
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
          "[vtworkspace][model][fold]")
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
            std::ranges::find_if(f.events.log, [](auto const& e) { return e.starts_with("paneClosed"); });
        auto const activeIt =
            std::ranges::find_if(f.events.log, [](auto const& e) { return e.starts_with("activePane"); });
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
          "[vtworkspace][model][resize]")
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
          "[vtworkspace][model][focus]")
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
          "[vtworkspace][model][split]")
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
        std::ranges::find_if(f.events.log, [](auto const& e) { return e.starts_with("paneSplit"); });
    auto const activeIt =
        std::ranges::find_if(f.events.log, [](auto const& e) { return e.starts_with("activePane"); });
    auto const titleIt =
        std::ranges::find_if(f.events.log, [](auto const& e) { return e.starts_with("title"); });
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

TEST_CASE("SessionModel: closePane guard and last-pane paths", "[vtworkspace][model][fold]")
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

TEST_CASE("SessionModel: a second split preserves every earlier leaf's id and session (no theft)",
          "[vtworkspace][model][split]")
{
    // Regression for "the first pane goes black after the second split": at the MODEL layer, splitting the
    // active (2nd) pane to make a 3rd must leave the FIRST pane's leaf id AND session completely untouched —
    // Pane::split only mutates the active-leaf subtree. (The GUI-thread session-STEAL that actually blacked
    // out the first pane was in TerminalSessionManager::FocusOnDisplay/tryFindSessionForDisplayOrClose, fixed
    // separately; this pins the model invariant those relied on: sibling sessions are stable across a split.)
    Fixture f;
    auto const [win, tab, rootLeaf] = makeTab(f.model);

    // Split #1: splits the original leaf -> [ firstLeaf(orig session) | secondLeaf ].
    auto* second = f.model.splitActivePane(tab, SplitState::Vertical);
    REQUIRE(second != nullptr);
    auto* firstLeaf = f.model.findTab(tab)->rootPane()->first();
    auto const firstId = firstLeaf->id();
    auto const firstSession = firstLeaf->session();
    auto const secondId = second->id();
    auto const secondSession = second->session();

    // Split #2: splits the active (second) leaf -> [ first | [ second | third ] ]. The first pane is a
    // sibling of the subtree being split and must not be touched.
    auto* third = f.model.splitActivePane(tab, SplitState::Vertical);
    REQUIRE(third != nullptr);
    REQUIRE(f.model.findTab(tab)->paneCount() == 3);

    // The first pane's leaf still resolves by its ORIGINAL id to its ORIGINAL, non-empty session.
    auto* firstAfter = f.model.findTab(tab)->rootPane()->findPane(firstId);
    REQUIRE(firstAfter != nullptr);
    CHECK(firstAfter->isLeaf());
    CHECK(firstAfter->session().value == firstSession.value);
    CHECK(firstSession.value != 0); // a real allocated session, not the empty SessionId{}

    // All three leaves carry distinct, non-empty sessions.
    std::vector<uint64_t> leafSessions;
    f.model.findTab(tab)->rootPane()->walkTree([&](Pane& p) {
        if (p.isLeaf())
            leafSessions.push_back(p.session().value);
    });
    REQUIRE(leafSessions.size() == 3);
    std::ranges::sort(leafSessions);
    CHECK(std::ranges::adjacent_find(leafSessions) == leafSessions.end()); // all distinct
    CHECK(std::ranges::none_of(leafSessions, [](uint64_t s) { return s == 0; }));
    // The second pane's session also survived the split that turned its leaf into a split node's child.
    CHECK(std::ranges::find(leafSessions, secondSession.value) != leafSessions.end());
    (void) secondId;
}

// {{{ Multi-window contract (safety net for the per-OS-window refactor)
//
// The GUI is moving from one shared vtworkspace::Window (all OS windows rendered the same tabs) to one Window
// per OS window. These tests lock the model's per-window isolation and teardown contract BEFORE the
// TerminalSessionManager/WindowController start depending on it for N windows. All assertions read the
// model directly (each Window's own tabCount/activeTab), so they do not depend on the shared event log.

TEST_CASE("SessionModel: windows are created with distinct ids and are independent containers",
          "[vtworkspace][model][window]")
{
    Fixture f;
    auto* a = f.model.createWindow();
    auto* b = f.model.createWindow();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    // Distinct identities, distinct objects, both empty to start.
    CHECK(a->id().value != b->id().value);
    CHECK(a != b);
    CHECK(a->empty());
    CHECK(b->empty());
    // window(id) resolves each back to its own object.
    CHECK(f.model.window(a->id()) == a);
    CHECK(f.model.window(b->id()) == b);
    // An unknown id resolves to nullptr.
    CHECK(f.model.window(WindowId { 999999 }) == nullptr);
}

TEST_CASE("SessionModel: tabs and panes added to one window do not appear in another",
          "[vtworkspace][model][window]")
{
    Fixture f;
    auto* a = f.model.createWindow();
    auto* b = f.model.createWindow();

    // Two tabs into A, one into B.
    auto* a1 = f.model.createTab(a->id());
    f.model.createTab(a->id());
    auto* b1 = f.model.createTab(b->id());
    REQUIRE(a1 != nullptr);
    REQUIRE(b1 != nullptr);

    CHECK(a->tabCount() == 2);
    CHECK(b->tabCount() == 1);
    // Each window's tabs are its own; A's tab is not reachable as one of B's.
    CHECK(b->indexOf(a1->id()) == -1);
    CHECK(a->indexOf(b1->id()) == -1);
    // findTab is model-global (it locates across windows) — but the tab belongs to exactly one window.
    CHECK(f.model.findTab(a1->id()) == a1);
    CHECK(f.model.findTab(b1->id()) == b1);
    // windowOfTab resolves each tab to its owning window; an unknown tab maps to the empty WindowId.
    CHECK(f.model.windowOfTab(a1->id()) == a->id());
    CHECK(f.model.windowOfTab(b1->id()) == b->id());
    CHECK(f.model.windowOfTab(TabId { 999999 }) == WindowId {});

    // Splitting A's active pane must not change B's pane/tab counts.
    auto const bTabsBefore = b->tabCount();
    f.model.splitActivePane(a->activeTab()->id(), SplitState::Vertical);
    CHECK(a->activeTab()->paneCount() == 2);
    CHECK(b->tabCount() == bTabsBefore);
    CHECK(b1->paneCount() == 1);
}

TEST_CASE("SessionModel: active-tab state is tracked per window", "[vtworkspace][model][window]")
{
    Fixture f;
    auto* a = f.model.createWindow();
    auto* b = f.model.createWindow();

    auto* a1 = f.model.createTab(a->id());
    f.model.createTab(a->id()); // a2 becomes A's active
    auto* b1 = f.model.createTab(b->id());

    // Newest tab is active in each window, independently.
    CHECK(a->activeTabIndex() == 1);
    CHECK(b->activeTabIndex() == 0);

    // Activating a tab in A leaves B's active tab untouched.
    f.model.activateTab(a->id(), a1->id());
    CHECK(a->activeTab() == a1);
    CHECK(a->activeTabIndex() == 0);
    CHECK(b->activeTab() == b1);
    CHECK(b->activeTabIndex() == 0);
}

TEST_CASE("SessionModel: removeWindow tears down only that window and leaves others intact",
          "[vtworkspace][model][window]")
{
    Fixture f;
    auto* a = f.model.createWindow();
    auto* b = f.model.createWindow();

    f.model.createTab(a->id());
    f.model.createTab(a->id());
    auto* b1 = f.model.createTab(b->id());
    auto const bId = b->id();
    auto const aId = a->id();

    // Every tabClosed reported during removeWindow(A) names window A only, never B.
    f.events.log.clear();
    f.model.removeWindow(aId);

    // A is gone; B and its tab survive with the same identity.
    CHECK(f.model.window(aId) == nullptr);
    REQUIRE(f.model.window(bId) == b);
    CHECK(b->tabCount() == 1);
    CHECK(f.model.findTab(b1->id()) == b1);
    // A's tabs were reported closed; B fired no close.
    CHECK(f.events.sawPrefix("tabClosed"));
    CHECK(f.events.countPrefix("tabClosed") == 2); // exactly A's two tabs

    // Removing an unknown window is a silent no-op (does not disturb B).
    f.events.log.clear();
    f.model.removeWindow(WindowId { 999999 });
    CHECK(f.events.log.empty());
    CHECK(f.model.window(bId) == b);
}
// }}}

// {{{ Previous-active-tab (model-level "switch to previous tab" memory)
//
// Replaces DisplayState::previousSession as the source for switchToPreviousTab. Lives on vtworkspace::Window
// so it is per-window, Qt-free, and unit-testable. Must never dangle at a wrong slot after a close/move.

TEST_CASE("SessionModel: previousActiveTab follows the last activation and toggles", "[vtworkspace][model][tab]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* t0 = f.model.createTab(win->id()); // active=0, prev=-1
    CHECK(win->previousActiveTabIndex() == -1);

    auto* t1 = f.model.createTab(win->id()); // active=1, prev=0
    CHECK(win->activeTab() == t1);
    CHECK(win->previousActiveTab() == t0);

    f.model.createTab(win->id()); // active=2, prev=1
    CHECK(win->previousActiveTabIndex() == 1);

    // An explicit activation records the outgoing tab as previous.
    f.model.activateTab(win->id(), t0->id()); // active=0, prev=2
    CHECK(win->activeTab() == t0);
    CHECK(win->previousActiveTabIndex() == 2);

    // Re-activating the CURRENT tab is a no-op and does not disturb the previous memory.
    f.model.activateTab(win->id(), t0->id());
    CHECK(win->previousActiveTabIndex() == 2);
}

TEST_CASE("SessionModel: closing the previous tab invalidates the previous-tab memory", "[vtworkspace][model][tab]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* t0 = f.model.createTab(win->id());
    auto* t1 = f.model.createTab(win->id());  // active=1, prev=0(t0)
    f.model.activateTab(win->id(), t1->id()); // no-op (already active), prev stays 0
    REQUIRE(win->previousActiveTab() == t0);

    // Close t0 (the previous tab): there is no valid previous anymore.
    f.model.closeTab(win->id(), t0->id());
    CHECK(win->previousActiveTabIndex() == -1);
    CHECK(win->previousActiveTab() == nullptr);
    CHECK(win->activeTab() == t1); // active survivor unchanged
}

TEST_CASE("SessionModel: closing a lower-indexed tab shifts the previous-tab slot down",
          "[vtworkspace][model][tab]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* t0 = f.model.createTab(win->id());
    auto* t1 = f.model.createTab(win->id());
    auto* t2 = f.model.createTab(win->id());  // active=2(t2), prev=1(t1)
    f.model.activateTab(win->id(), t2->id()); // no-op; keep active=2, prev=1
    REQUIRE(win->previousActiveTab() == t1);

    // Close t0 (index 0, below both active and previous): both shift down by one, still naming the
    // same tabs.
    f.model.closeTab(win->id(), t0->id());
    CHECK(win->activeTab() == t2);
    CHECK(win->previousActiveTab() == t1);
    CHECK(win->previousActiveTabIndex() == 0); // t1 is now slot 0
}

TEST_CASE("SessionModel: moving a tab keeps the previous-tab pointer on the same tab", "[vtworkspace][model][tab]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* t0 = f.model.createTab(win->id());
    auto* t1 = f.model.createTab(win->id());
    auto* t2 = f.model.createTab(win->id()); // active=2(t2), prev=1(t1)
    REQUIRE(win->previousActiveTab() == t1);

    // Move t1 (the previous tab) from index 1 to the front: the previous pointer must still name t1.
    f.model.moveTab(win->id(), t1->id(), 0);
    CHECK(win->previousActiveTab() == t1);
    CHECK(win->activeTab() == t2);
    (void) t0;
}
// }}}

// {{{ Guard no-ops (unknown ids / no-op requests never fire events or mutate state)

TEST_CASE("SessionModel: tab operations on an unknown window are safe no-ops", "[vtworkspace][model][tab]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* tab = f.model.createTab(win->id());
    auto const unknownWindow = WindowId { 999999 };
    f.events.log.clear();

    f.model.closeTab(unknownWindow, tab->id());
    f.model.closeOtherTabs(unknownWindow, tab->id());
    f.model.closeTabsToRight(unknownWindow, tab->id());
    f.model.activateTab(unknownWindow, tab->id());
    f.model.moveTab(unknownWindow, tab->id(), 0);

    // Nothing fired and the real window's state is untouched.
    CHECK(f.events.log.empty());
    CHECK(win->tabCount() == 1);
    CHECK(win->activeTab() == tab);
}

TEST_CASE("SessionModel: closeTab and closeTabsToRight with an unknown tab id are safe no-ops",
          "[vtworkspace][model][tab]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* a = f.model.createTab(win->id());
    auto* b = f.model.createTab(win->id());
    f.events.log.clear();

    f.model.closeTab(win->id(), TabId { 999999 });         // unknown tab in a known window
    f.model.closeTabsToRight(win->id(), TabId { 999999 }); // unknown anchor: nothing to the right of it

    CHECK(f.events.log.empty());
    CHECK(win->tabCount() == 2);
    CHECK(win->tabAt(0) == a);
    CHECK(win->tabAt(1) == b);
}

TEST_CASE("SessionModel: moveTab to the tab's current index is a silent no-op", "[vtworkspace][model][tab]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* a = f.model.createTab(win->id());
    auto* b = f.model.createTab(win->id());
    f.events.log.clear();

    // Destination == current index: no bracket events, no reorder.
    f.model.moveTab(win->id(), b->id(), 1);
    CHECK(f.events.log.empty());
    CHECK(win->tabAt(0) == a);
    CHECK(win->tabAt(1) == b);

    // An out-of-range destination clamps to the last index — where b already sits: still a no-op.
    f.model.moveTab(win->id(), b->id(), 99);
    CHECK(f.events.log.empty());
    CHECK(win->tabAt(1) == b);
}

TEST_CASE("SessionModel: title and color mutations on an unknown tab are safe no-ops",
          "[vtworkspace][model][title][color]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* tab = f.model.createTab(win->id());
    f.model.setTabTitle(tab->id(), "kept");
    f.model.setTabColor(tab->id(), TabColorSource::User, vtbackend::RGBColor { 0x11, 0x22, 0x33 });
    f.events.log.clear();

    auto const unknown = TabId { 999999 };
    f.model.setTabTitle(unknown, "ignored");
    f.model.resetTabTitle(unknown);
    f.model.setTabColor(unknown, TabColorSource::User, vtbackend::RGBColor { 0xAA, 0xBB, 0xCC });
    f.model.resetTabColor(unknown, TabColorSource::User);

    CHECK(f.events.log.empty());
    // The real tab's overrides are untouched.
    CHECK(tab->runtimeTitle() == "kept");
    REQUIRE(tab->color().has_value());
    CHECK(tab->color()->red == 0x11);
}
// }}}

// {{{ Default ModelEvents "about to" implementations

namespace
{
/// A host that overrides ONLY the pure-virtual "completed change" callbacks and inherits every
/// default no-op "about to" implementation — the non-Qt host ModelEvents documents (e.g. a daemon
/// that only reacts to completed changes). Exercises the default bodies of tabAboutToBeAdded(),
/// tabAboutToBeRemoved(), and tabAboutToBeMoved().
struct CompletedChangesOnlyEvents: ModelEvents
{
    int added = 0;
    int closed = 0;
    int moved = 0;

    void tabAdded(WindowId, TabId, int) override { ++added; }
    void tabClosed(WindowId, TabId, int) override { ++closed; }
    void tabMoved(WindowId, TabId, int, int) override { ++moved; }
    void activeTabChanged(WindowId, TabId, int) override {}
    void paneSplit(TabId, PaneId, PaneId) override {}
    void paneClosed(TabId, PaneId, PaneId) override {}
    void activePaneChanged(TabId, PaneId) override {}
    void paneRatioChanged(TabId, PaneId, double) override {}
    void tabTitleChanged(TabId) override {}
    void tabColorChanged(TabId) override {}
};
} // namespace

TEST_CASE("SessionModel: hosts observing only completed changes may inherit the no-op bracket defaults",
          "[vtworkspace][model][events]")
{
    CompletedChangesOnlyEvents events;
    uint64_t nextSession = 1;
    SessionModel model { events, [&] { return SessionId { nextSession++ }; } };

    auto* win = model.createWindow();
    auto* a = model.createTab(win->id()); // runs the default tabAboutToBeAdded() no-op
    auto* b = model.createTab(win->id());
    CHECK(events.added == 2);

    model.moveTab(win->id(), a->id(), 1); // runs the default tabAboutToBeMoved() no-op
    CHECK(events.moved == 1);
    CHECK(win->tabAt(0) == b);
    CHECK(win->tabAt(1) == a);

    model.closeTab(win->id(), a->id()); // runs the default tabAboutToBeRemoved() no-op
    CHECK(events.closed == 1);
    CHECK(win->tabCount() == 1);
    CHECK(win->activeTab() == b);

    // Pane and title/color operations have no "about to" half at all; a completed-changes-only host
    // drives them through the same interface without further overrides.
    auto* newLeaf = model.splitActivePane(b->id(), SplitState::Vertical);
    REQUIRE(newLeaf != nullptr);
    model.setPaneRatio(b->id(), b->rootPane()->id(), 0.3);
    model.setTabColor(b->id(), TabColorSource::User, vtbackend::RGBColor { 0x33, 0xCC, 0x33 });
    model.closePane(win->id(), b->id(), newLeaf->id());
    CHECK(b->paneCount() == 1);
    CHECK(win->tabCount() == 1);
}

TEST_CASE("SessionModel: resizeActivePane nudges the ancestor split ratio in the pressed direction",
          "[vtworkspace][model][resize]")
{
    Fixture f;
    auto const [win, tab, rootLeaf] = makeTab(f.model);
    // Vertical split (side-by-side): left | right, ratio 0.5. `right` is active.
    auto* right = f.model.splitActivePane(tab, SplitState::Vertical);
    REQUIRE(right != nullptr);
    auto const splitId = f.model.findTab(tab)->rootPane()->id();

    SECTION("pressing Right raises the first child's ratio; the emitted value is the stored one")
    {
        f.events.log.clear();
        f.model.resizeActivePane(tab, FocusDirection::Right, 0.1);
        REQUIRE(f.events.lastRatio.has_value());
        CHECK(*f.events.lastRatio == Catch::Approx(0.6));
        CHECK(f.events.sawPrefix(std::format("ratio:{}", splitId.value)));
    }

    SECTION("pressing Left lowers the first child's ratio")
    {
        f.model.resizeActivePane(tab, FocusDirection::Left, 0.1);
        REQUIRE(f.events.lastRatio.has_value());
        CHECK(*f.events.lastRatio == Catch::Approx(0.4));
    }

    SECTION("a cross-axis direction with no matching ancestor is a silent no-op")
    {
        f.events.log.clear();
        f.model.resizeActivePane(tab, FocusDirection::Up, 0.1); // no Horizontal ancestor
        CHECK_FALSE(f.events.sawPrefix("ratio:"));
    }

    SECTION("unknown tab is a no-op")
    {
        f.events.log.clear();
        f.model.resizeActivePane(TabId { 9999 }, FocusDirection::Right, 0.1);
        CHECK_FALSE(f.events.sawPrefix("ratio:"));
    }
}

TEST_CASE("SessionModel: toggleActivePaneOrientation flips the parent split and emits the new state",
          "[vtworkspace][model][orientation]")
{
    Fixture f;
    auto const [win, tab, rootLeaf] = makeTab(f.model);

    SECTION("single-pane tab: no split, no event")
    {
        f.model.toggleActivePaneOrientation(tab);
        CHECK_FALSE(f.events.sawPrefix("orientation:"));
    }

    SECTION("split tab: fires paneOrientationChanged with the flipped axis, and no ratio event")
    {
        f.model.splitActivePane(tab, SplitState::Vertical);
        auto const splitId = f.model.findTab(tab)->rootPane()->id();
        f.events.log.clear();
        f.model.toggleActivePaneOrientation(tab);
        CHECK(f.events.sawPrefix(std::format("orientation:{}:{}", tab.value, splitId.value)));
        CHECK(f.events.lastOrientation == SplitState::Horizontal);
        CHECK_FALSE(f.events.sawPrefix("ratio:")); // orientation change is not a ratio change
    }
}

TEST_CASE("SessionModel: swapActivePane swaps sessions and fires paneSwapped + activePaneChanged",
          "[vtworkspace][model][swap]")
{
    Fixture f;
    auto const [win, tab, rootLeaf] = makeTab(f.model);
    auto* right = f.model.splitActivePane(tab, SplitState::Vertical);
    REQUIRE(right != nullptr);
    auto* left = f.model.findTab(tab)->rootPane()->first();
    auto const leftId = left->id();
    auto const rightSession = right->session();

    SECTION("swapping left fires the swap + active-change events and moves the session")
    {
        f.events.log.clear();
        f.model.swapActivePane(tab, FocusDirection::Left);
        CHECK(f.events.sawPrefix(std::format("swapped:{}", tab.value)));
        CHECK(f.events.sawPrefix(std::format("activePane:{}", tab.value)));
        // The active session moved into the left slot; the active pane is now the left leaf.
        CHECK(f.model.findTab(tab)->rootPane()->first()->session() == rightSession);
        CHECK(f.model.findTab(tab)->activePane()->id() == leftId);
    }

    SECTION("no neighbor is a silent no-op")
    {
        f.events.log.clear();
        f.model.swapActivePane(tab, FocusDirection::Right); // right pane, nothing further right
        CHECK_FALSE(f.events.sawPrefix("swapped:"));
    }
}

TEST_CASE("SessionModel: moveActivePane re-parents and fires paneTreeRestructured + activePaneChanged",
          "[vtworkspace][model][move]")
{
    Fixture f;
    auto const [win, tab, rootLeaf] = makeTab(f.model);
    // left | right(top/bottom); bottom active.
    auto* right = f.model.splitActivePane(tab, SplitState::Vertical);
    f.model.setActivePane(tab, right->id());
    auto* bottom = f.model.splitActivePane(tab, SplitState::Horizontal);
    auto const movedId = bottom->id();
    REQUIRE(f.model.findTab(tab)->paneCount() == 3);

    SECTION("moving Left re-parents; the tree is restructured and the moved pane stays active")
    {
        f.events.log.clear();
        f.model.moveActivePane(tab, FocusDirection::Left);
        CHECK(f.events.sawPrefix(std::format("restructured:{}", tab.value)));
        CHECK(f.events.sawPrefix(std::format("activePane:{}", tab.value)));
        CHECK(f.model.findTab(tab)->paneCount() == 3); // no pane lost
        CHECK(f.model.findTab(tab)->activePane()->id() == movedId);
    }

    SECTION("no neighbor is a silent no-op")
    {
        f.events.log.clear();
        f.model.moveActivePane(tab, FocusDirection::Down); // bottom of the split, nothing below
        CHECK_FALSE(f.events.sawPrefix("restructured:"));
    }
}

TEST_CASE("SessionModel: moveTabToWindow transplants a tab between windows, sessions intact",
          "[vtworkspace][model][window][move]")
{
    Fixture f;
    auto* a = f.model.createWindow();
    auto* b = f.model.createWindow();
    auto* t0 = f.model.createTab(a->id());
    auto* t1 = f.model.createTab(a->id()); // a has [t0, t1], t1 active
    auto* b0 = f.model.createTab(b->id()); // b has [b0], b0 active
    (void) t0;
    (void) b0;

    SECTION("the tab leaves the source and appears in the destination at the requested index")
    {
        f.events.log.clear();
        f.model.moveTabToWindow(a->id(), t1->id(), b->id(), 0);

        CHECK(a->tabCount() == 1);
        CHECK(a->indexOf(t1->id()) < 0); // gone from source
        CHECK(b->tabCount() == 2);
        CHECK(b->tabAt(0) == t1);    // inserted at index 0 in dest
        CHECK(b->activeTab() == t1); // dest adopts the moved tab as active

        // The bracketing + completion events fired with both window ids.
        CHECK(f.events.sawPrefix(std::format("tabAboutToMoveWin:{}", a->id().value)));
        CHECK(f.events.sawPrefix(std::format("tabMovedWin:{}:{}", a->id().value, t1->id().value)));
    }

    SECTION("moving a window's LAST tab empties the source (indices reset), dest gains it")
    {
        // Give A a single tab by closing t1 first.
        f.model.closeTab(a->id(), t1->id());
        REQUIRE(a->tabCount() == 1);
        f.model.moveTabToWindow(a->id(), t0->id(), b->id(), 1);
        CHECK(a->tabCount() == 0);
        CHECK(a->activeTabIndex() == -1);
        CHECK(b->tabCount() == 2);
        CHECK(b->indexOf(t0->id()) == 1);
    }

    SECTION("same-window move degenerates to a reorder")
    {
        f.model.moveTabToWindow(a->id(), t0->id(), a->id(), 1);
        CHECK(a->tabAt(1) == t0);
        CHECK(a->tabCount() == 2);
    }

    SECTION("unknown source/destination window or unknown tab is a safe no-op")
    {
        f.model.moveTabToWindow(WindowId { 9999 }, t1->id(), b->id(), 0);
        f.model.moveTabToWindow(a->id(), TabId { 9999 }, b->id(), 0);
        f.model.moveTabToWindow(a->id(), t1->id(), WindowId { 9999 }, 0);
        CHECK(a->tabCount() == 2);
        CHECK(b->tabCount() == 1);
    }
}
// }}}

// {{{ Zoom
//
// The event contract: paneZoomChanged fires exactly when the ZOOMED LEAF changes — on entering and
// leaving zoom, and when zoom follows focus to a different leaf — and never for an unzoomed tab.

TEST_CASE("SessionModel: toggling pane zoom announces the zoomed leaf, then its clearing",
          "[vtworkspace][model][zoom]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* tab = f.model.createTab(win->id());
    auto* newLeaf = f.model.splitActivePane(tab->id(), SplitState::Vertical);
    REQUIRE(newLeaf != nullptr);
    auto const leafId = newLeaf->id();
    f.events.log.clear();

    f.model.toggleActivePaneZoom(tab->id());
    CHECK(f.events.countPrefix("zoom:") == 1);
    CHECK(f.events.lastZoomedLeaf == leafId);
    CHECK(tab->isZoomed());
    // The tab is now titled after the pane on screen rather than "Multiple panes".
    CHECK(f.events.countPrefix("title:") == 1);

    f.model.toggleActivePaneZoom(tab->id());
    CHECK(f.events.countPrefix("zoom:") == 2);
    CHECK_FALSE(f.events.lastZoomedLeaf.has_value());
    CHECK_FALSE(tab->isZoomed());
}

TEST_CASE("SessionModel: zooming a single-pane tab announces nothing", "[vtworkspace][model][zoom]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* tab = f.model.createTab(win->id());
    f.events.log.clear();

    // Nothing to hide, so nothing changed, so nothing is announced — a host must not be told to
    // re-lay-out a tab that did not move.
    f.model.toggleActivePaneZoom(tab->id());
    CHECK(f.events.countPrefix("zoom:") == 0);
    CHECK(f.events.log.empty());
    CHECK_FALSE(tab->isZoomed());
}

TEST_CASE("SessionModel: a focus move while zoomed re-announces the new leaf", "[vtworkspace][model][zoom]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* tab = f.model.createTab(win->id());
    auto* right = f.model.splitActivePane(tab->id(), SplitState::Vertical);
    REQUIRE(right != nullptr);
    auto const leftId = tab->rootPane()->first()->id();

    f.model.toggleActivePaneZoom(tab->id());
    REQUIRE(tab->isZoomed());
    f.events.log.clear();

    // Zoom follows focus: the tab STAYS zoomed but a different pane is now on screen. Without an
    // event carrying the new leaf, the host would keep rendering the old one.
    f.model.focusDirection(tab->id(), FocusDirection::Left);
    CHECK(tab->isZoomed());
    CHECK(f.events.countPrefix("zoom:") == 1);
    CHECK(f.events.lastZoomedLeaf == leftId);
    CHECK(tab->layoutRoot()->id() == leftId);
    // The pane on screen changed, so the tab's title did too.
    CHECK(f.events.countPrefix("title:") == 1);
}

TEST_CASE("SessionModel: a focus move on an unzoomed tab announces no zoom change", "[vtworkspace][model][zoom]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* tab = f.model.createTab(win->id());
    REQUIRE(f.model.splitActivePane(tab->id(), SplitState::Vertical) != nullptr);
    f.events.log.clear();

    f.model.focusDirection(tab->id(), FocusDirection::Left);
    CHECK(f.events.countPrefix("activePane:") == 1);
    // The layout root did not move (it is still the tree root), so there is nothing to re-root.
    CHECK(f.events.countPrefix("zoom:") == 0);
    // ...and an unzoomed multi-pane tab is titled "Multiple panes" regardless of which pane is active.
    CHECK(f.events.countPrefix("title:") == 0);
}

TEST_CASE("SessionModel: restructuring a zoomed tab announces the zoom clearing exactly once",
          "[vtworkspace][model][zoom]")
{
    auto const zoomedTab = [](Fixture& f) {
        auto* win = f.model.createWindow();
        auto* tab = f.model.createTab(win->id());
        REQUIRE(f.model.splitActivePane(tab->id(), SplitState::Vertical) != nullptr);
        REQUIRE(f.model.splitActivePane(tab->id(), SplitState::Horizontal) != nullptr);
        f.model.toggleActivePaneZoom(tab->id());
        REQUIRE(tab->isZoomed());
        f.events.log.clear();
        return tab;
    };

    SECTION("splitting")
    {
        Fixture f;
        auto* tab = zoomedTab(f);
        f.model.splitActivePane(tab->id(), SplitState::Vertical);
        CHECK_FALSE(tab->isZoomed());
        CHECK(f.events.countPrefix("zoom:") == 1);
        CHECK_FALSE(f.events.lastZoomedLeaf.has_value());
    }

    SECTION("closing a pane")
    {
        Fixture f;
        auto* tab = zoomedTab(f);
        auto const win = f.model.windowOfTab(tab->id());
        f.model.closePane(win, tab->id(), tab->activePane()->id());
        CHECK_FALSE(tab->isZoomed());
        CHECK(f.events.countPrefix("zoom:") == 1);
        CHECK_FALSE(f.events.lastZoomedLeaf.has_value());
    }

    // Unlike split/close, the three below emit no tabTitleChanged of their own — so they are exactly the
    // paths where a hand-written "retitle if zoomed" gets forgotten. Clearing the zoom renames the tab
    // from the zoomed pane's own title back to "Multiple panes", and a host that is never told keeps
    // showing a stale name. announceZoomChange() emits the retitle WITH the zoom change; these pin it.
    SECTION("flipping the split orientation")
    {
        Fixture f;
        auto* tab = zoomedTab(f);
        f.model.toggleActivePaneOrientation(tab->id());
        CHECK_FALSE(tab->isZoomed());
        CHECK(f.events.countPrefix("zoom:") == 1);
        CHECK(f.events.countPrefix("title:") == 1);
    }

    SECTION("swapping panes")
    {
        Fixture f;
        auto* tab = zoomedTab(f);
        f.model.swapActivePane(tab->id(), FocusDirection::Up);
        CHECK_FALSE(tab->isZoomed());
        CHECK(f.events.countPrefix("zoom:") == 1);
        CHECK(f.events.countPrefix("title:") == 1);
    }

    SECTION("moving a pane")
    {
        Fixture f;
        auto* tab = zoomedTab(f);
        f.model.moveActivePane(tab->id(), FocusDirection::Left);
        CHECK_FALSE(tab->isZoomed());
        CHECK(f.events.countPrefix("zoom:") == 1);
        CHECK(f.events.countPrefix("title:") == 1);
    }
}

TEST_CASE("SessionModel: an unzoomed restructure does not retitle a multi-pane tab", "[vtworkspace][model][zoom]")
{
    // The mirror of the sections above: with no zoom to clear, orientation/swap/move leave the resolved
    // title ("Multiple panes") untouched, so they must stay silent. This keeps announceZoomChange()'s
    // retitle honest — tied to the zoom actually changing, not a blanket emit on every mutation.
    Fixture f;
    auto* win = f.model.createWindow();
    auto* tab = f.model.createTab(win->id());
    REQUIRE(f.model.splitActivePane(tab->id(), SplitState::Vertical) != nullptr);
    REQUIRE(f.model.splitActivePane(tab->id(), SplitState::Horizontal) != nullptr);
    f.events.log.clear();

    f.model.toggleActivePaneOrientation(tab->id());
    f.model.swapActivePane(tab->id(), FocusDirection::Up);
    f.model.moveActivePane(tab->id(), FocusDirection::Left);

    CHECK(f.events.countPrefix("zoom:") == 0);
    CHECK(f.events.countPrefix("title:") == 0);
}

TEST_CASE("SessionModel: mutating an unzoomed tab announces no zoom change at all", "[vtworkspace][model][zoom]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* tab = f.model.createTab(win->id());
    REQUIRE(f.model.splitActivePane(tab->id(), SplitState::Vertical) != nullptr);
    REQUIRE(f.model.splitActivePane(tab->id(), SplitState::Horizontal) != nullptr);

    // Every pane mutator is funneled through the same zoom-announcing helper, so guard against it
    // becoming chatty: an unzoomed tab must never emit a zoom event, whatever is done to it.
    f.events.log.clear();
    f.model.toggleActivePaneOrientation(tab->id());
    f.model.swapActivePane(tab->id(), FocusDirection::Up);
    f.model.focusDirection(tab->id(), FocusDirection::Down);
    f.model.moveActivePane(tab->id(), FocusDirection::Left);
    f.model.resizeActivePane(tab->id(), FocusDirection::Left, 0.1);
    f.model.closePane(win->id(), tab->id(), tab->activePane()->id());

    CHECK(f.events.countPrefix("zoom:") == 0);
}

TEST_CASE("SessionModel: closing the last pane of a zoomed tab closes the tab", "[vtworkspace][model][zoom]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* tab = f.model.createTab(win->id());
    auto const tabId = tab->id();
    REQUIRE(f.model.splitActivePane(tabId, SplitState::Vertical) != nullptr);
    f.model.toggleActivePaneZoom(tabId);
    REQUIRE(tab->isZoomed());

    // Close down to one pane (which clears the zoom), then close that one: the tab goes away. The
    // zoom-announcing helper must not touch the tab after this last close destroyed it.
    f.model.closePane(win->id(), tabId, tab->activePane()->id());
    REQUIRE(f.model.findTab(tabId) != nullptr);
    f.events.log.clear();

    f.model.closePane(win->id(), tabId, tab->activePane()->id());
    CHECK(f.model.findTab(tabId) == nullptr);
    CHECK(f.events.sawPrefix("tabClosed:"));
    CHECK(f.events.countPrefix("zoom:") == 0);
}
// }}}

TEST_CASE("SessionModel: resizing a zoomed pane announces the zoom clearing", "[vtworkspace][model][zoom]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* tab = f.model.createTab(win->id());
    REQUIRE(f.model.splitActivePane(tab->id(), SplitState::Vertical) != nullptr);
    f.model.toggleActivePaneZoom(tab->id());
    REQUIRE(tab->isZoomed());
    f.events.log.clear();

    // Regression: resizeActivePane was the one pane mutator outside the zoom contract, so this used to
    // nudge an off-screen ratio and announce only paneRatioChanged.
    f.model.resizeActivePane(tab->id(), FocusDirection::Left, 0.05);
    CHECK_FALSE(tab->isZoomed());
    CHECK(f.events.countPrefix("zoom:") == 1);
    CHECK(f.events.countPrefix("ratio:") == 1);
}

TEST_CASE("SessionModel: splitting or closing a zoomed tab retitles it exactly once",
          "[vtworkspace][model][zoom][title]")
{
    // Both mutators retitle for their OWN reason (the pane count crossing 1<->many) and would retitle
    // again for the zoom they cleared. The host's tabTitleChanged fan-out rebuilds and republishes the
    // status line to every session in the window, so a double emit runs that whole sweep twice per key.
    SECTION("splitting")
    {
        Fixture f;
        auto* win = f.model.createWindow();
        auto* tab = f.model.createTab(win->id());
        REQUIRE(f.model.splitActivePane(tab->id(), SplitState::Vertical) != nullptr);
        f.model.toggleActivePaneZoom(tab->id());
        REQUIRE(tab->isZoomed());
        f.events.log.clear();

        f.model.splitActivePane(tab->id(), SplitState::Horizontal);
        CHECK(f.events.countPrefix("zoom:") == 1);
        CHECK(f.events.countPrefix("title:") == 1);
    }

    SECTION("closing a pane")
    {
        Fixture f;
        auto* win = f.model.createWindow();
        auto* tab = f.model.createTab(win->id());
        REQUIRE(f.model.splitActivePane(tab->id(), SplitState::Vertical) != nullptr);
        REQUIRE(f.model.splitActivePane(tab->id(), SplitState::Horizontal) != nullptr);
        f.model.toggleActivePaneZoom(tab->id());
        REQUIRE(tab->isZoomed());
        f.events.log.clear();

        f.model.closePane(win->id(), tab->id(), tab->activePane()->id());
        CHECK(f.events.countPrefix("zoom:") == 1);
        CHECK(f.events.countPrefix("title:") == 1);
    }

    SECTION("an unzoomed split still retitles once")
    {
        Fixture f;
        auto* win = f.model.createWindow();
        auto* tab = f.model.createTab(win->id());
        f.events.log.clear();

        // No zoom to clear, so the split's own retitle (single-pane -> "Multiple panes") must still fire.
        f.model.splitActivePane(tab->id(), SplitState::Vertical);
        CHECK(f.events.countPrefix("zoom:") == 0);
        CHECK(f.events.countPrefix("title:") == 1);
    }
}

TEST_CASE("SessionModel: zoom is per-tab state that survives switching away and back", "[vtworkspace][model][zoom]")
{
    Fixture f;
    auto* win = f.model.createWindow();
    auto* zoomed = f.model.createTab(win->id());
    REQUIRE(f.model.splitActivePane(zoomed->id(), SplitState::Vertical) != nullptr);
    f.model.toggleActivePaneZoom(zoomed->id());
    auto const zoomedLeaf = zoomed->zoomedLeafId();
    REQUIRE(zoomedLeaf.has_value());

    auto* other = f.model.createTab(win->id());
    REQUIRE(win->activeTab() == other);
    CHECK_FALSE(other->isZoomed()); // a fresh tab is not zoomed just because its neighbor is

    // Zoom lives on the Tab, so switching away cannot disturb it: come back and the same leaf is still
    // the one filling the tab.
    f.model.activateTab(win->id(), zoomed->id());
    CHECK(zoomed->isZoomed());
    CHECK(zoomed->zoomedLeafId() == zoomedLeaf);
    CHECK(zoomed->layoutRoot() == zoomed->activePane());
}

TEST_CASE("SessionModel: zoom travels with a tab moved to another window", "[vtworkspace][model][zoom]")
{
    Fixture f;
    auto* a = f.model.createWindow();
    auto* b = f.model.createWindow();
    auto* tab = f.model.createTab(a->id());
    REQUIRE(f.model.splitActivePane(tab->id(), SplitState::Vertical) != nullptr);
    f.model.toggleActivePaneZoom(tab->id());
    auto const zoomedLeaf = tab->zoomedLeafId();
    REQUIRE(zoomedLeaf.has_value());

    // moveTabToWindow transplants the Tab object intact, and the zoom is Tab state — so it rides along
    // rather than being silently dropped on the destination window's first rebuild.
    f.model.moveTabToWindow(a->id(), tab->id(), b->id(), 0);

    REQUIRE(f.model.windowOfTab(tab->id()) == b->id());
    CHECK(tab->isZoomed());
    CHECK(tab->zoomedLeafId() == zoomedLeaf);
    CHECK(tab->layoutRoot() == tab->activePane());
}

TEST_CASE("SessionModel: findSessionLeaf locates a session in any window", "[vtworkspace][model][pane]")
{
    Fixture f;
    auto* win1 = f.model.createWindow();
    auto* win2 = f.model.createWindow();
    auto* tab1 = f.model.createTab(win1->id());                                  // session 1000
    auto* tab2 = f.model.createTab(win2->id());                                  // session 1001
    auto* splitLeaf = f.model.splitActivePane(tab2->id(), SplitState::Vertical); // session 1002
    REQUIRE(splitLeaf != nullptr);

    // A session in the primary window resolves to its only leaf.
    auto const [w1, t1, l1] = f.model.findSessionLeaf(SessionId { 1000 });
    CHECK(w1 == win1);
    CHECK(t1 == tab1);
    CHECK(l1 == tab1->rootPane());

    // A session in a SECONDARY window, behind a split, resolves just the same
    // (the daemon's session-exit pruning and output mapping depend on it).
    auto const [w2, t2, l2] = f.model.findSessionLeaf(SessionId { 1002 });
    CHECK(w2 == win2);
    CHECK(t2 == tab2);
    CHECK(l2 == splitLeaf);

    // An unknown session finds nothing.
    auto const [w3, t3, l3] = f.model.findSessionLeaf(SessionId { 4242 });
    CHECK(w3 == nullptr);
    CHECK(t3 == nullptr);
    CHECK(l3 == nullptr);
}
