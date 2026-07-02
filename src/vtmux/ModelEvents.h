// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtmux/Primitives.h>

namespace vtmux
{

/// Observer interface for changes to the session/layout model.
///
/// This mirrors the spirit of vtbackend::Terminal::Events: the model never knows what its host is,
/// it only calls out through this interface when something changes. The Qt GUI implements it and
/// translates each callback into a Qt signal / model-reset; a future daemon implements it and fans
/// each callback out to subscribed network clients (see docs/drafts/daemon-mode.md). The model
/// itself stays free of Qt and of any transport.
///
/// All callbacks are invoked on the thread that performed the mutation. The host is responsible for
/// any cross-thread marshalling it needs (e.g. the Qt GUI posts to its GUI thread).
class ModelEvents
{
  public:
    ModelEvents() = default;
    ModelEvents(ModelEvents const&) = default;
    ModelEvents(ModelEvents&&) = default;
    ModelEvents& operator=(ModelEvents const&) = default;
    ModelEvents& operator=(ModelEvents&&) = default;
    virtual ~ModelEvents() = default;

    /// A new tab is about to be appended to @p window at position @p index, before it is inserted.
    ///
    /// Paired with tabAdded(): this "before" half fires while the model still holds the OLD tab set, the
    /// "after" half once the tab exists. A Qt QAbstractItemModel host maps this pair to
    /// beginInsertRows()/endInsertRows(), whose contract requires the begin call while rowCount() still
    /// reports the old count. The default is a no-op so non-Qt hosts (e.g. a daemon that only reacts to
    /// completed changes) can ignore it.
    /// @param window The window receiving the tab.
    /// @param index  The position the new tab will occupy.
    virtual void tabAboutToBeAdded(WindowId window, int index) { (void) window, (void) index; }

    /// A new tab was appended to @p window at position @p index. Paired with tabAboutToBeAdded().
    virtual void tabAdded(WindowId window, TabId tab, int index) = 0;

    /// The tab @p tab at @p index is about to be removed from @p window, before it is erased.
    ///
    /// Paired with tabClosed(); maps to beginRemoveRows()/endRemoveRows() on a Qt host. Default no-op.
    /// @param window The window losing the tab.
    /// @param index  The position of the tab about to be removed.
    virtual void tabAboutToBeRemoved(WindowId window, int index) { (void) window, (void) index; }

    /// The tab @p tab was removed from @p window (it was at @p index before removal). Paired with
    /// tabAboutToBeRemoved().
    virtual void tabClosed(WindowId window, TabId tab, int index) = 0;

    /// A tab is about to move within @p window from @p fromIndex to @p toIndex, before the reorder.
    ///
    /// Paired with tabMoved(); maps to beginMoveRows()/endMoveRows() on a Qt host. Default no-op.
    /// @param window    The window whose tab moves.
    /// @param fromIndex The tab's current position.
    /// @param toIndex   The tab's destination position.
    virtual void tabAboutToBeMoved(WindowId window, int fromIndex, int toIndex)
    {
        (void) window, (void) fromIndex, (void) toIndex;
    }

    /// A tab moved within its window from @p fromIndex to @p toIndex. Paired with tabAboutToBeMoved().
    virtual void tabMoved(WindowId window, TabId tab, int fromIndex, int toIndex) = 0;

    /// The active tab of @p window changed to @p tab (at @p index).
    virtual void activeTabChanged(WindowId window, TabId tab, int index) = 0;

    /// A leaf @p leaf was split, producing the new leaf @p newLeaf. After this call @p leaf is an
    /// internal split node whose first child holds the original session and whose second child is
    /// @p newLeaf.
    virtual void paneSplit(TabId tab, PaneId splitNode, PaneId newLeaf) = 0;

    /// The pane @p closed was closed; its parent absorbed the surviving sibling, which now lives at
    /// @p survivor. (If the whole tab was closed because the last pane went away, tabClosed fires
    /// instead and this is not called.)
    virtual void paneClosed(TabId tab, PaneId closed, PaneId survivor) = 0;

    /// The active leaf of @p tab changed to @p leaf.
    virtual void activePaneChanged(TabId tab, PaneId leaf) = 0;

    /// The split ratio of the internal node @p splitNode changed.
    virtual void paneRatioChanged(TabId tab, PaneId splitNode, double ratio) = 0;

    /// The resolved title of @p tab changed (runtime rename, active-pane change, or session title).
    virtual void tabTitleChanged(TabId tab) = 0;

    /// The color of @p tab changed (set or reset).
    virtual void tabColorChanged(TabId tab) = 0;
};

} // namespace vtmux
