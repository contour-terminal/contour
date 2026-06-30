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

    /// A new tab was appended to @p window at position @p index.
    virtual void tabAdded(WindowId window, TabId tab, int index) = 0;

    /// The tab @p tab was removed from @p window (it was at @p index before removal).
    virtual void tabClosed(WindowId window, TabId tab, int index) = 0;

    /// A tab moved within its window from @p fromIndex to @p toIndex.
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
