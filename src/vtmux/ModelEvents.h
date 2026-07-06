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

    /// The tab @p tab is about to move from window @p from (at @p fromIndex) into window @p to (at
    /// @p toIndex), before the transplant. Paired with tabMovedToWindow().
    ///
    /// A cross-window move is modeled by a Qt host as a remove on @p from and an insert on @p to (Qt's
    /// beginMoveRows is single-model), so this "before" half lets the host bracket a beginRemoveRows on
    /// the source and a beginInsertRows on the destination. Defaulted to a no-op so single-window and
    /// completed-only hosts can ignore it.
    /// @param from      The window losing the tab.
    /// @param fromIndex The tab's current position in @p from.
    /// @param to        The window gaining the tab.
    /// @param toIndex   The position the tab will occupy in @p to.
    virtual void tabAboutToBeMovedToWindow(WindowId from, int fromIndex, WindowId to, int toIndex)
    {
        (void) from, (void) fromIndex, (void) to, (void) toIndex;
    }

    /// The tab @p tab moved from window @p from (it was at @p fromIndex) into window @p to at
    /// @p toIndex. Paired with tabAboutToBeMovedToWindow(). Its sessions travel with it — none are
    /// closed. Defaulted to a no-op.
    virtual void tabMovedToWindow(WindowId from, TabId tab, int fromIndex, WindowId to, int toIndex)
    {
        (void) from, (void) tab, (void) fromIndex, (void) to, (void) toIndex;
    }

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

    /// The internal node @p splitNode of @p tab flipped orientation to @p newState (its children and
    /// ratio are unchanged; only the layout axis differs).
    ///
    /// Defaulted to a no-op so hosts that only react to completed *structural* changes (a daemon
    /// replaying to network clients) can ignore a pure re-layout. The Qt GUI overrides it to re-read
    /// the affected pane's orientation.
    virtual void paneOrientationChanged(TabId tab, PaneId splitNode, SplitState newState)
    {
        (void) tab, (void) splitNode, (void) newState;
    }

    /// Two leaves of @p tab swapped their sessions in place (their PaneIds are unchanged); the leaf
    /// @p a and the leaf @p b now host each other's former session.
    ///
    /// Defaulted to a no-op (see paneOrientationChanged). The Qt GUI overrides it to re-bind the two
    /// affected panes to their new sessions.
    virtual void paneSwapped(TabId tab, PaneId a, PaneId b) { (void) tab, (void) a, (void) b; }

    /// The pane tree of @p tab was restructured in a way that reshapes topology unpredictably (a pane
    /// was moved/re-parented). Unlike the targeted events above, the host should re-read the whole
    /// tab's tree rather than a single node.
    ///
    /// Defaulted to a no-op (see paneOrientationChanged). The Qt GUI overrides it to rebuild the
    /// tab's proxy tree wholesale.
    virtual void paneTreeRestructured(TabId tab) { (void) tab; }

    /// The resolved title of @p tab changed (runtime rename, active-pane change, or session title).
    virtual void tabTitleChanged(TabId tab) = 0;

    /// The color of @p tab changed (set or reset).
    virtual void tabColorChanged(TabId tab) = 0;
};

} // namespace vtmux
