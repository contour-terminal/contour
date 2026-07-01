// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Color.h>

#include <memory>
#include <span>
#include <vector>

#include <vtmux/ModelEvents.h>
#include <vtmux/Pane.h>
#include <vtmux/Primitives.h>
#include <vtmux/Tab.h>

namespace vtmux
{

/// A logical window: an ordered list of tabs with one active tab.
///
/// In single-process GUI mode there is one Window per OS window; in daemon mode a Window is a
/// server-side layout that one or more clients may attach to.
class Window
{
  public:
    explicit Window(WindowId id) noexcept: _id { id } {}

    Window(Window const&) = delete;
    Window& operator=(Window const&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;
    ~Window() = default;

    [[nodiscard]] WindowId id() const noexcept { return _id; }
    [[nodiscard]] int tabCount() const noexcept { return static_cast<int>(_tabs.size()); }
    [[nodiscard]] bool empty() const noexcept { return _tabs.empty(); }

    /// The active tab, or nullptr if the window has no tabs.
    [[nodiscard]] Tab* activeTab() const noexcept;
    [[nodiscard]] int activeTabIndex() const noexcept { return _activeTabIndex; }

    /// The tab at @p index, or nullptr if out of range.
    [[nodiscard]] Tab* tabAt(int index) const noexcept;

    /// The index of @p tab, or -1 if not found.
    [[nodiscard]] int indexOf(TabId tab) const noexcept;
    [[nodiscard]] int indexOf(Tab const* tab) const noexcept;

  private:
    friend class SessionModel;

    WindowId _id;
    std::vector<std::unique_ptr<Tab>> _tabs;
    int _activeTabIndex = -1;
};

/// The single source of truth for all windows, tabs and panes.
///
/// SessionModel is deliberately free of Qt and of any transport. Every mutation is performed through
/// one of its methods, which updates the tree and notifies the host through ModelEvents. The Qt GUI
/// supplies a ModelEvents implementation that turns each callback into a Qt signal; a future daemon
/// supplies one that fans each callback out to subscribed network clients. The model is the same in
/// both worlds, which is what lets a daemon own it and many clients observe it.
///
/// Session lifetime is *not* owned here: the model refers to a session only by SessionId. Creating
/// the underlying terminal/PTY for a new SessionId, and tearing it down for a closed one, is the
/// host's responsibility (it learns of both through the SessionAllocator and ModelEvents).
class SessionModel
{
  public:
    /// Allocates a fresh session for a new pane and returns its id. The host creates the backing
    /// terminal/PTY for that id. Invoked when a tab or split needs a new session.
    using SessionAllocator = std::function<SessionId()>;

    /// @param events    Observer notified of every change. Must outlive the model.
    /// @param allocator Supplies a new SessionId (and its backing terminal) on demand.
    SessionModel(ModelEvents& events, SessionAllocator allocator);

    SessionModel(SessionModel const&) = delete;
    SessionModel& operator=(SessionModel const&) = delete;
    SessionModel(SessionModel&&) = delete;
    SessionModel& operator=(SessionModel&&) = delete;
    ~SessionModel() = default;

    // {{{ Windows

    /// Creates a new, empty window and returns it.
    Window* createWindow();

    /// Removes a window and everything in it. Each contained session is reported closed.
    void removeWindow(WindowId window);

    [[nodiscard]] Window* window(WindowId id) const noexcept;

    // }}}
    // {{{ Tabs

    /// Appends a new tab (with a single fresh-session pane) to @p window and makes it active.
    Tab* createTab(WindowId window);

    /// Closes the tab @p tab in @p window, reporting each of its sessions closed. If it was the
    /// last tab, the window becomes empty (the host decides whether to close the OS window).
    void closeTab(WindowId window, TabId tab);

    /// Closes every tab in @p window except @p keep.
    void closeOtherTabs(WindowId window, TabId keep);

    /// Closes every tab positioned after @p anchor in @p window.
    void closeTabsToRight(WindowId window, TabId anchor);

    /// Makes @p tab the active tab of @p window.
    void activateTab(WindowId window, TabId tab);

    /// Moves @p tab to @p toIndex within @p window.
    void moveTab(WindowId window, TabId tab, int toIndex);

    // }}}
    // {{{ Panes

    /// Splits @p tab's active pane along @p direction, allocating a new session for the new pane.
    /// Returns the new (now active) leaf.
    Pane* splitActivePane(TabId tab, SplitState direction, double ratio = 0.5);

    /// Closes pane @p leaf in @p tab. If it was the tab's last pane, the tab (and possibly the
    /// window) is closed instead. Reports the closed session(s).
    void closePane(WindowId window, TabId tab, PaneId leaf);

    /// Sets the active pane of @p tab.
    void setActivePane(TabId tab, PaneId leaf);

    /// Moves focus within @p tab in @p direction.
    void focusDirection(TabId tab, FocusDirection direction);

    /// Updates the split ratio of the internal node @p splitNode in @p tab.
    void setPaneRatio(TabId tab, PaneId splitNode, double ratio);

    // }}}
    // {{{ Title & color (authoritative state lives here, below the GUI)

    void setTabTitle(TabId tab, std::string title);
    void resetTabTitle(TabId tab);
    void setTabColor(TabId tab, vtbackend::RGBColor color);
    void resetTabColor(TabId tab);

    /// The session-title resolver the host installs so tabs can derive their title from the active
    /// pane's program title.
    void setSessionTitleResolver(Tab::SessionTitleResolver resolver) { _titleResolver = std::move(resolver); }

    [[nodiscard]] Tab::SessionTitleResolver const& sessionTitleResolver() const noexcept
    {
        return _titleResolver;
    }

    // }}}
    // {{{ Lookups & palette

    [[nodiscard]] Tab* findTab(TabId tab) const noexcept;

    /// The predefined tab-color palette offered to the user (a grid of swatches, WT-style). Both the
    /// GUI and a future daemon expose the same set so all clients see identical choices. Backed by a
    /// single constexpr table with static storage duration, so this hands back a view over it — no
    /// per-instance copy or allocation.
    [[nodiscard]] std::span<vtbackend::RGBColor const> colorPalette() const noexcept;

    // }}}

  private:
    /// Locates the window owning @p tab. Returns {window, index-of-tab} or {nullptr, -1}.
    [[nodiscard]] std::pair<Window*, int> locateTab(TabId tab) const noexcept;

    void closeTabAt(Window& window, int index);

    ModelEvents& _events;
    SessionAllocator _allocateSession;
    Tab::SessionTitleResolver _titleResolver;

    std::vector<std::unique_ptr<Window>> _windows;

    WindowId _nextWindowId { 1 };
    TabId _nextTabId { 1 };
    PaneId _nextPaneId { 1 };
};

} // namespace vtmux
