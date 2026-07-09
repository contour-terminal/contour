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

    /// The tab that was active before the current one (the target of a "switch to previous tab"
    /// toggle), or nullptr if there is no valid previous tab (never switched, or the previous tab was
    /// closed). Tracked here — the authoritative, Qt-free model — so switch-to-previous-tab is a pure
    /// model query and lives per window automatically.
    [[nodiscard]] Tab* previousActiveTab() const noexcept;
    [[nodiscard]] int previousActiveTabIndex() const noexcept { return _previousActiveTabIndex; }

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
    /// The slot that was active before _activeTabIndex, or -1 if none. Maintained by SessionModel: set on
    /// a genuine activation (createTab/activateTab), and translated/invalidated (never left dangling) by
    /// the reindexing operations (moveTab/closeTabAt).
    int _previousActiveTabIndex = -1;
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

    /// Moves @p tab out of window @p from and into window @p to at @p toIndex, transplanting its whole
    /// pane subtree (and the sessions it carries — none are closed). Fixes the active/previous-tab
    /// bookkeeping on both windows. No-op if either window is unknown or @p tab is not in @p from.
    ///
    /// This is the model half of dragging a tab between windows (or tearing it into a fresh window):
    /// the Tab object moves intact, so its sessions survive and the host only re-binds them to the
    /// destination window's displays.
    /// @param from    The window currently holding @p tab.
    /// @param tab     The tab to move.
    /// @param to      The destination window (may equal @p from, in which case this reorders).
    /// @param toIndex The destination position, clamped into the destination's tab range.
    void moveTabToWindow(WindowId from, TabId tab, WindowId to, int toIndex);

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

    /// Grows or shrinks @p tab's active pane along @p direction by @p fraction, by nudging the ratio
    /// of the nearest ancestor split on the matching axis. No-op if the active pane has no such
    /// ancestor (a single pane, or only cross-axis splits above it).
    /// @param tab       The tab whose active pane resizes.
    /// @param direction The side the active pane grows toward.
    /// @param fraction  The ratio delta magnitude in (0, 1); the sign is derived from @p direction.
    void resizeActivePane(TabId tab, FocusDirection direction, double fraction);

    /// Flips the orientation of @p tab's active pane's parent split (Horizontal<->Vertical). No-op if
    /// the active pane is the tab's only pane.
    void toggleActivePaneOrientation(TabId tab);

    /// Swaps @p tab's active pane with its neighbor in @p direction (the two terminals trade slots),
    /// keeping the moved session focused. No-op if there is no neighbor.
    void swapActivePane(TabId tab, FocusDirection direction);

    /// Moves @p tab's active pane across its neighbor in @p direction, re-parenting it in the tree
    /// (see Tab::moveActivePane). No-op if there is no neighbor.
    void moveActivePane(TabId tab, FocusDirection direction);

    // }}}
    // {{{ Title & color (authoritative state lives here, below the GUI)

    void setTabTitle(TabId tab, std::string title);
    void resetTabTitle(TabId tab);

    /// Assigns @p tab's color on behalf of @p source. Sources are independent and ranked, so a user's
    /// choice and an application's DECAC color coexist; see TabColorSource.
    /// @param tab The tab to color.
    /// @param source The assigning source.
    /// @param color The color to assign.
    void setTabColor(TabId tab, TabColorSource source, vtbackend::RGBColor color);

    /// Clears @p source's color for @p tab, falling back to the next-highest source that has one.
    /// @param tab The tab to un-color.
    /// @param source The source whose color to clear.
    void resetTabColor(TabId tab, TabColorSource source);

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

    /// The id of the window owning @p tab, or a default WindowId{} (value 0) if @p tab is unknown. Lets
    /// the Qt host route a pane event (which carries only a TabId) to the owning window's controller.
    [[nodiscard]] WindowId windowOfTab(TabId tab) const noexcept;

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
