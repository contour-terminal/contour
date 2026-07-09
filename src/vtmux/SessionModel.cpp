// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cassert>
#include <cctype>
#include <ranges>

#include <vtmux/SessionModel.h>

namespace vtmux
{

// {{{ Window

Tab* Window::activeTab() const noexcept
{
    return tabAt(_activeTabIndex);
}

Tab* Window::previousActiveTab() const noexcept
{
    return tabAt(_previousActiveTabIndex);
}

Tab* Window::tabAt(int index) const noexcept
{
    if (index < 0 || index >= static_cast<int>(_tabs.size()))
        return nullptr;
    return _tabs[static_cast<size_t>(index)].get();
}

int Window::indexOf(TabId tab) const noexcept
{
    auto const it = std::ranges::find_if(_tabs, [tab](auto const& t) { return t->id() == tab; });
    return it != _tabs.end() ? static_cast<int>(std::distance(_tabs.begin(), it)) : -1;
}

int Window::indexOf(Tab const* tab) const noexcept
{
    auto const it = std::ranges::find_if(_tabs, [tab](auto const& t) { return t.get() == tab; });
    return it != _tabs.end() ? static_cast<int>(std::distance(_tabs.begin(), it)) : -1;
}

// }}}

namespace
{
    /// The default predefined tab-color palette (a WT-style grid of swatches): the eight standard
    /// ANSI colors plus their bright variants. Data-driven so the set can be tuned in one place.
    constexpr vtbackend::RGBColor DefaultPalette[] = {
        vtbackend::RGBColor { 0xCC, 0x33, 0x33 }, // red
        vtbackend::RGBColor { 0xCC, 0x77, 0x33 }, // orange
        vtbackend::RGBColor { 0xCC, 0xCC, 0x33 }, // yellow
        vtbackend::RGBColor { 0x33, 0xCC, 0x33 }, // green
        vtbackend::RGBColor { 0x33, 0xCC, 0xCC }, // cyan
        vtbackend::RGBColor { 0x33, 0x77, 0xCC }, // blue
        vtbackend::RGBColor { 0x77, 0x33, 0xCC }, // purple
        vtbackend::RGBColor { 0xCC, 0x33, 0xCC }, // magenta
        vtbackend::RGBColor { 0xEE, 0x66, 0x66 }, // bright red
        vtbackend::RGBColor { 0xEE, 0xAA, 0x66 }, // bright orange
        vtbackend::RGBColor { 0xEE, 0xEE, 0x66 }, // bright yellow
        vtbackend::RGBColor { 0x66, 0xEE, 0x66 }, // bright green
        vtbackend::RGBColor { 0x66, 0xEE, 0xEE }, // bright cyan
        vtbackend::RGBColor { 0x66, 0xAA, 0xEE }, // bright blue
        vtbackend::RGBColor { 0xAA, 0x66, 0xEE }, // bright purple
        vtbackend::RGBColor { 0xEE, 0x66, 0xEE }, // bright magenta
    };
} // namespace

// {{{ SessionModel ctor

SessionModel::SessionModel(ModelEvents& events, SessionAllocator allocator):
    _events { events }, _allocateSession { std::move(allocator) }
{
}

std::span<vtbackend::RGBColor const> SessionModel::colorPalette() const noexcept
{
    // View over the single constexpr table (static storage duration) — no per-instance copy/allocation.
    return DefaultPalette;
}

// }}}
// {{{ Windows

Window* SessionModel::createWindow()
{
    auto window = std::make_unique<Window>(_nextWindowId++);
    _windows.push_back(std::move(window));
    return _windows.back().get();
}

void SessionModel::removeWindow(WindowId windowId)
{
    auto* win = window(windowId);
    if (win == nullptr)
        return;

    // Close every tab (reporting sessions closed) from the back so indices stay valid.
    while (!win->_tabs.empty())
        closeTabAt(*win, static_cast<int>(win->_tabs.size()) - 1);

    std::erase_if(_windows, [windowId](auto const& w) { return w->id() == windowId; });
}

Window* SessionModel::window(WindowId id) const noexcept
{
    auto const it = std::ranges::find_if(_windows, [id](auto const& w) { return w->id() == id; });
    return it != _windows.end() ? it->get() : nullptr;
}

// }}}
// {{{ Tabs

Tab* SessionModel::createTab(WindowId windowId)
{
    auto* win = window(windowId);
    assert(win != nullptr);

    auto const tabId = _nextTabId++;
    auto const rootPaneId = _nextPaneId++;
    auto const session = _allocateSession();

    // Bracket the insertion so a Qt host's beginInsertRows() runs BEFORE the tab set grows (its contract
    // requires rowCount() to still report the old count at that point) and endInsertRows() after.
    auto const index = static_cast<int>(win->_tabs.size());
    _events.tabAboutToBeAdded(windowId, index);
    win->_tabs.push_back(std::make_unique<Tab>(tabId, rootPaneId, session));
    _events.tabAdded(windowId, tabId, index);

    // A new tab is a genuine activation: remember the outgoing active slot as the previous tab.
    win->_previousActiveTabIndex = win->_activeTabIndex;
    win->_activeTabIndex = index;
    _events.activeTabChanged(windowId, tabId, index);

    return win->_tabs[static_cast<size_t>(index)].get();
}

void SessionModel::closeTabAt(Window& win, int index)
{
    assert(index >= 0 && index < static_cast<int>(win._tabs.size()));
    auto const tabId = win._tabs[static_cast<size_t>(index)]->id();

    auto const previousActiveIndex = win._activeTabIndex;
    // Bracket the removal: a Qt host's beginRemoveRows() must run while the tab is still present (old
    // rowCount), endRemoveRows() after the erase.
    _events.tabAboutToBeRemoved(win.id(), index);
    win._tabs.erase(win._tabs.begin() + index);
    _events.tabClosed(win.id(), tabId, index);

    // Translate/invalidate the "previous active tab" slot the same way as the active slot below: it must
    // never dangle at a wrong tab. If the closed tab WAS the previous one, there is no valid previous
    // anymore (-1); if it was before the previous slot, that slot shifted down by one.
    if (win._previousActiveTabIndex == index)
        win._previousActiveTabIndex = -1;
    else if (index < win._previousActiveTabIndex)
        --win._previousActiveTabIndex;

    // Keep the active index pointing at the *same* surviving tab, and notify only if it changed.
    if (win._tabs.empty())
    {
        win._activeTabIndex = -1;
        win._previousActiveTabIndex = -1;
        return;
    }

    // Erasing at @p index shifts every tab after it down by one slot. There are three cases:
    //  - the erased tab was after the active one  -> the active tab kept its slot (no change);
    //  - the erased tab was before the active one  -> the active tab shifted down by one (decrement);
    //  - the active tab itself was erased          -> adopt the tab now occupying that slot, clamped
    //                                                 to the new last index.
    auto newActive = win._activeTabIndex;
    if (index < win._activeTabIndex)
        --newActive;
    else if (index == win._activeTabIndex)
        newActive = std::min(newActive, static_cast<int>(win._tabs.size()) - 1);
    newActive = std::max(newActive, 0);

    win._activeTabIndex = newActive;
    if (newActive != previousActiveIndex || index == previousActiveIndex)
        _events.activeTabChanged(win.id(), win._tabs[static_cast<size_t>(newActive)]->id(), newActive);
}

void SessionModel::closeTab(WindowId windowId, TabId tabId)
{
    auto* win = window(windowId);
    if (win == nullptr)
        return;
    auto const index = win->indexOf(tabId);
    if (index < 0)
        return;
    closeTabAt(*win, index);
}

void SessionModel::closeOtherTabs(WindowId windowId, TabId keep)
{
    auto* win = window(windowId);
    if (win == nullptr)
        return;
    // Iterate back-to-front so each erasure leaves the lower indices valid.
    for (auto const i: std::views::iota(0, static_cast<int>(win->_tabs.size())) | std::views::reverse)
        if (win->_tabs[static_cast<size_t>(i)]->id() != keep)
            closeTabAt(*win, i);
}

void SessionModel::closeTabsToRight(WindowId windowId, TabId anchor)
{
    auto* win = window(windowId);
    if (win == nullptr)
        return;
    auto const anchorIndex = win->indexOf(anchor);
    if (anchorIndex < 0)
        return;
    // Iterate back-to-front so each erasure leaves the lower indices valid.
    for (auto const i:
         std::views::iota(anchorIndex + 1, static_cast<int>(win->_tabs.size())) | std::views::reverse)
        closeTabAt(*win, i);
}

void SessionModel::activateTab(WindowId windowId, TabId tabId)
{
    auto* win = window(windowId);
    if (win == nullptr)
        return;
    auto const index = win->indexOf(tabId);
    if (index < 0 || index == win->_activeTabIndex)
        return;
    // Switching tabs is the canonical activation: the outgoing tab becomes the "previous" one, so a
    // subsequent switch-to-previous toggles back to it.
    win->_previousActiveTabIndex = win->_activeTabIndex;
    win->_activeTabIndex = index;
    _events.activeTabChanged(windowId, tabId, index);
}

void SessionModel::moveTab(WindowId windowId, TabId tabId, int toIndex)
{
    auto* win = window(windowId);
    if (win == nullptr)
        return;
    auto const from = win->indexOf(tabId);
    // Guard before clamping: on an empty/unknown-tab list indexOf returns -1, and clamping with a
    // hi bound of size()-1 == -1 would call std::clamp with lo > hi (undefined behavior; aborts
    // under hardened/_GLIBCXX_ASSERTIONS or libc++ debug builds).
    if (from < 0)
        return;
    auto const to = std::clamp(toIndex, 0, static_cast<int>(win->_tabs.size()) - 1);
    if (from == to)
        return;

    auto* activeTab = win->activeTab();
    auto const previousActiveIndex = win->_activeTabIndex;
    // Track the "previous active tab" by object too, so the reorder preserves which tab it names rather
    // than a now-stale slot (mirrors the active-tab handling below).
    auto* previousTab = win->previousActiveTab();

    // Bracket the reorder: a Qt host's beginMoveRows() must run before the vector is mutated (it reads
    // the pre-move layout to validate the destination), endMoveRows() after.
    _events.tabAboutToBeMoved(windowId, from, to);
    auto held = std::move(win->_tabs[static_cast<size_t>(from)]);
    win->_tabs.erase(win->_tabs.begin() + from);
    win->_tabs.insert(win->_tabs.begin() + to, std::move(held));

    // Preserve which *tab* is active (and which is the previous) across the reorder.
    if (activeTab != nullptr)
        win->_activeTabIndex = win->indexOf(activeTab);
    win->_previousActiveTabIndex = previousTab != nullptr ? win->indexOf(previousTab) : -1;

    _events.tabMoved(windowId, tabId, from, to);

    // The active tab object is unchanged, but the reorder can shift its index (e.g. moving a tab
    // across the active one). Observers that track the active tab by index — not by the tabMoved
    // delta — need an explicit activeTabChanged to avoid a stale active-tab index.
    if (activeTab != nullptr && win->_activeTabIndex != previousActiveIndex)
        _events.activeTabChanged(windowId, activeTab->id(), win->_activeTabIndex);
}

void SessionModel::moveTabToWindow(WindowId fromId, TabId tabId, WindowId toId, int toIndex)
{
    // Same-window request degenerates to a plain reorder — reuse the tested single-window path so the
    // active-index translation and beginMoveRows bracketing stay in one place.
    if (fromId == toId)
    {
        moveTab(fromId, tabId, toIndex);
        return;
    }

    auto* from = window(fromId);
    auto* to = window(toId);
    if (from == nullptr || to == nullptr)
        return;
    auto const fromIndex = from->indexOf(tabId);
    if (fromIndex < 0)
        return; // tab not in the source window

    // Snapshot the source's active/previous tab OBJECTS so their indices can be re-derived after the
    // erase (mirrors closeTabAt); the destination gains the tab as its new active.
    auto* srcActiveTab = from->activeTab();
    auto* srcPreviousTab = from->previousActiveTab();
    auto const dstIndex = std::clamp(toIndex, 0, static_cast<int>(to->_tabs.size()));

    _events.tabAboutToBeMovedToWindow(fromId, fromIndex, toId, dstIndex);

    // Transplant the Tab intact — its pane subtree and the sessions it carries travel with it; nothing
    // is torn down (unlike closeTab/removeWindow, which report sessions closed).
    auto held = std::move(from->_tabs[static_cast<size_t>(fromIndex)]);
    from->_tabs.erase(from->_tabs.begin() + fromIndex);
    to->_tabs.insert(to->_tabs.begin() + dstIndex, std::move(held));
    assert(from->indexOf(tabId) < 0 && "the moved tab must have left the source window");

    // Fix the SOURCE window's active/previous indices exactly as closeTabAt does after an erase.
    if (from->_previousActiveTabIndex == fromIndex)
        from->_previousActiveTabIndex = -1;
    else if (fromIndex < from->_previousActiveTabIndex)
        --from->_previousActiveTabIndex;

    bool const srcActiveWasMoved = (srcActiveTab == nullptr) || (from->indexOf(srcActiveTab) < 0);
    if (from->_tabs.empty())
    {
        from->_activeTabIndex = -1;
        from->_previousActiveTabIndex = -1;
    }
    else
    {
        // Re-derive the surviving active tab's slot; if the active tab itself was the one moved out,
        // adopt the tab now occupying its old slot (clamped), matching closeTabAt.
        if (srcActiveWasMoved)
            from->_activeTabIndex = std::min(from->_activeTabIndex, static_cast<int>(from->_tabs.size()) - 1);
        else
            from->_activeTabIndex = from->indexOf(srcActiveTab);
        from->_activeTabIndex = std::max(from->_activeTabIndex, 0);
        from->_previousActiveTabIndex = srcPreviousTab != nullptr ? from->indexOf(srcPreviousTab) : -1;
    }

    // The DESTINATION adopts the moved tab as its active tab (a genuine activation): remember the old
    // active slot as the previous tab, then point at the inserted slot.
    to->_previousActiveTabIndex = to->_activeTabIndex;
    to->_activeTabIndex = dstIndex;

    _events.tabMovedToWindow(fromId, tabId, fromIndex, toId, dstIndex);
    _events.activeTabChanged(toId, tabId, dstIndex);
    if (!from->_tabs.empty() && srcActiveWasMoved)
        _events.activeTabChanged(
            fromId, from->_tabs[static_cast<size_t>(from->_activeTabIndex)]->id(), from->_activeTabIndex);
}

// }}}
// {{{ Panes

std::pair<Window*, int> SessionModel::locateTab(TabId tabId) const noexcept
{
    for (auto const& win: _windows)
        if (auto const index = win->indexOf(tabId); index >= 0)
            return { win.get(), index };
    return { nullptr, -1 };
}

Tab* SessionModel::findTab(TabId tabId) const noexcept
{
    auto const [win, index] = locateTab(tabId);
    return win != nullptr ? win->tabAt(index) : nullptr;
}

WindowId SessionModel::windowOfTab(TabId tabId) const noexcept
{
    auto const [win, index] = locateTab(tabId);
    return win != nullptr ? win->id() : WindowId {};
}

Pane* SessionModel::splitActivePane(TabId tabId, SplitState direction, double ratio)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return nullptr;

    auto const splitNodeId = _nextPaneId++;
    auto const newLeafId = _nextPaneId++;
    auto const newSession = _allocateSession();

    auto* newLeaf = tab->splitActivePane(direction, splitNodeId, newLeafId, newSession, ratio);
    _events.paneSplit(tabId, splitNodeId, newLeafId);
    _events.activePaneChanged(tabId, newLeaf->id());
    _events.tabTitleChanged(tabId); // single-pane -> "Multiple panes" possibly
    return newLeaf;
}

void SessionModel::closePane(WindowId windowId, TabId tabId, PaneId leafId)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return;
    auto* leaf = tab->rootPane()->findPane(leafId);
    if (leaf == nullptr || !leaf->isLeaf())
        return;

    if (tab->isLastPane(leaf))
    {
        // Closing the only pane closes the whole tab.
        closeTab(windowId, tabId);
        return;
    }

    auto* parent = leaf->parent();
    auto* survivor = (parent->first() == leaf) ? parent->second() : parent->first();
    auto const survivorId = survivor->id();

    tab->closePane(leaf);

    _events.paneClosed(tabId, leafId, survivorId);
    _events.activePaneChanged(tabId, tab->activePane()->id());
    _events.tabTitleChanged(tabId);
}

void SessionModel::setActivePane(TabId tabId, PaneId leafId)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return;
    auto* leaf = tab->rootPane()->findPane(leafId);
    if (leaf == nullptr || !leaf->isLeaf() || leaf == tab->activePane())
        return;
    tab->setActivePane(leaf);
    _events.activePaneChanged(tabId, leafId);
    // NB: no tabTitleChanged here. The resolved title depends on the active pane only for a
    // single-pane tab (Tab::title precedence: runtime > "Multiple panes" > active session), and a
    // single-pane tab's active pane cannot change — the root leaf is already active, so the
    // early-return above fires. A conditional emit for that case was provably dead code.
}

void SessionModel::focusDirection(TabId tabId, FocusDirection direction)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return;
    if (auto* target = tab->focusDirection(direction); target != nullptr)
        _events.activePaneChanged(tabId, target->id());
}

void SessionModel::setPaneRatio(TabId tabId, PaneId splitNodeId, double ratio)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return;
    auto* node = tab->rootPane()->findPane(splitNodeId);
    if (node == nullptr || node->isLeaf())
        return;
    node->setRatio(ratio);
    // Emit the CLAMPED value the node actually stored (setRatio() clamps to [MinimumRatio, 1-MinimumRatio]),
    // not the raw request — otherwise the GUI divider and the model disagree at the extremes (a drag to the
    // edge would echo back an out-of-range ratio the model never adopted).
    _events.paneRatioChanged(tabId, splitNodeId, node->ratio());
}

void SessionModel::resizeActivePane(TabId tabId, FocusDirection direction, double fraction)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return;
    auto const axis = crossingSplitFor(direction);
    auto* split = Pane::ancestorSplitOnAxis(tab->activePane(), axis);
    if (split == nullptr)
        return; // single pane, or only cross-axis splits above the active pane

    // `_ratio` is the FIRST child's share of the split. The user presses a direction to move the shared
    // divider that way: pressing toward the second child (Right/Down) enlarges the first child's share
    // (+fraction); pressing toward the first child (Left/Up) shrinks it (-fraction). This "move the
    // boundary in this direction" model matches Windows Terminal and is independent of which side the
    // active pane sits on. setRatio() clamps; emit the value actually stored.
    double const delta = pointsTowardSecondChild(direction) ? fraction : -fraction;
    split->setRatio(split->ratio() + delta);
    _events.paneRatioChanged(tabId, split->id(), split->ratio());
}

void SessionModel::toggleActivePaneOrientation(TabId tabId)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return;
    if (auto* split = tab->toggleActivePaneOrientation(); split != nullptr)
        _events.paneOrientationChanged(tabId, split->id(), split->splitState());
}

void SessionModel::swapActivePane(TabId tabId, FocusDirection direction)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return;
    auto const [a, b] = tab->swapActivePane(direction);
    if (a == nullptr)
        return; // no neighbor
    _events.paneSwapped(tabId, a->id(), b->id());
    _events.activePaneChanged(tabId, tab->activePane()->id());
}

void SessionModel::moveActivePane(TabId tabId, FocusDirection direction)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return;
    if (tab->moveActivePane(direction, _nextPaneId))
    {
        // The re-parent path consumes a fresh split id; the swap-degenerate path does not, but always
        // advancing keeps ids monotonic and unique whichever branch ran.
        ++_nextPaneId;
        _events.paneTreeRestructured(tabId);
        _events.activePaneChanged(tabId, tab->activePane()->id());
    }
}

// }}}
// {{{ Title & color

void SessionModel::setTabTitle(TabId tabId, std::string title)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return;
    // An empty (or all-whitespace) rename means "reset to the default template", never "blank the
    // label": an empty runtime override takes precedence over the '{WindowTitle}' default and would
    // leave the tab permanently unlabeled — the GUI's inline rename editor opens pre-filled with the
    // raw override (empty for a never-renamed tab) and commits verbatim on click-away. Normalizing
    // here keeps the invariant "no blank runtime titles" for every adapter.
    if (std::ranges::all_of(title, [](unsigned char c) { return std::isspace(c) != 0; }))
    {
        resetTabTitle(tabId);
        return;
    }
    tab->setRuntimeTitle(std::move(title));
    _events.tabTitleChanged(tabId);
}

void SessionModel::resetTabTitle(TabId tabId)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return;
    tab->setRuntimeTitle(std::nullopt);
    _events.tabTitleChanged(tabId);
}

void SessionModel::setTabColor(TabId tabId, TabColorSource source, vtbackend::RGBColor color)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return;
    tab->setColor(source, color);
    _events.tabColorChanged(tabId);
}

void SessionModel::resetTabColor(TabId tabId, TabColorSource source)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return;
    tab->resetColor(source);
    _events.tabColorChanged(tabId);
}

// }}}

} // namespace vtmux
