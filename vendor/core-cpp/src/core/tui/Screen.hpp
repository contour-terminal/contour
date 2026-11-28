// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Clock.hpp>
#include <core/Defines.hpp>
#include <core/tui/Box.hpp>
#include <core/tui/Buffer.hpp>
#include <core/tui/Component.hpp>
#include <core/tui/CursorShape.hpp>
#include <core/tui/FrameDiff.hpp>
#include <core/tui/HoverState.hpp>
#include <core/tui/Theme.hpp>
#include <core/tui/Tooltip.hpp>
#include <core/tui/TuiCoreExport.hpp>

#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace core::tui
{

class ScreenSurface;

/// Viewport mode for the screen.
enum class Viewport : uint8_t
{
    Fullscreen, ///< Uses entire terminal, clears on each frame.
    Inline,     ///< Renders at current cursor position, grows downward.
    Fixed,      ///< Fixed region of the terminal.
};

/// Render mode for performance testing.
enum class RenderMode : uint8_t
{
    Diff, ///< Only write changed cells (default).
    Full, ///< Always write all cells (for benchmarking).
};

/// Unscroll mode for inline viewport.
///
/// Controls whether to use the Kitty unscroll extension (CSI Ps + T) to
/// restore scrollback content when inline content shrinks (e.g., popup hides).
/// Supported by: Kitty, Contour, mintty.
enum class UnscrollMode : uint8_t
{
    Auto,     ///< Auto-detect terminal support (default).
    Enabled,  ///< Always use unscroll (for known-supported terminals).
    Disabled, ///< Never use unscroll (fallback to clearing rows).
};

/// Which of the terminal's screen buffers a Screen draws on.
enum class ScreenBuffer : uint8_t
{
    Primary, ///< The buffer the shell was on, scrollback and all (default).
    /// The alternate screen buffer (DEC private mode 1049), entered for the lifetime of the Screen
    /// and left on destruction, which restores the primary buffer and the user's scrollback.
    /// Intended for full-screen apps with @c Viewport::Fullscreen.
    Alternate,
};

/// Configuration for the Screen.
///
/// Every enumeration here is zero for its default, so a zero-initialized configuration is the
/// default one.
struct ScreenConfig
{
    Viewport viewport = Viewport::Fullscreen;
    Rect fixedArea {};                              ///< For Viewport::Fixed.
    int inlineMaxHeight = 0;                        ///< For Viewport::Inline (0 = no limit).
    UnscrollMode unscrollMode = UnscrollMode::Auto; ///< Unscroll behavior for inline mode.
    /// Whether rendered lines turn text reflow (DEC 2028) off: @c DiffReflow::Inhibit writes the
    /// reset at the start of each row the inline viewport draws. The frame diff's own option.
    DiffReflow reflow = DiffReflow::Keep;
    ScreenBuffer screenBuffer = ScreenBuffer::Primary; ///< The terminal buffer drawn on.
    BorderStyle tooltipBorder = BorderStyle::Single;   ///< The frame of the hover tooltip the screen shows.
};

/// Cursor movement calculations for inline rendering.
///
/// This struct captures the cursor movements needed when transitioning
/// between renders with different content heights in inline viewport mode.
struct InlineCursorMovement
{
    int moveUpToStart = 0;       ///< Move up from previous cursor position to row 0.
    int newLinesToEmit = 0;      ///< Newlines to emit when content grows.
    int moveUpAfterNewlines = 0; ///< Move up after emitting newlines (equals newLinesToEmit).
    int rowsToClear = 0;         ///< Rows to clear when content shrinks.
};

/// Calculates cursor movements for inline rendering transitions.
///
/// @param previousContentHeight Height of content from previous render.
/// @param previousCursorRow Cursor row at end of previous render.
/// @param newContentHeight Height of content for current render.
/// @return Cursor movement instructions.
[[nodiscard]] CORE_TUI_CORE_API InlineCursorMovement calculateInlineCursorMovement(int previousContentHeight,
                                                                                   int previousCursorRow,
                                                                                   int newContentHeight);

/// Central coordinator for the TUI component system.
///
/// Screen manages the component tree, rendering, event dispatch, and focus.
/// It owns the root component and coordinates all rendering through a
/// double-buffered diff system for efficient terminal updates.
///
/// Usage:
/// @code
/// core::tui::Terminal terminal;
/// core::tui::Screen screen(terminal);
///
/// // Add components to root
/// MyComponent myComp;
/// screen.root().addChild(myComp, {.area = {0, 0, 40, 10}});
///
/// // Or let the layout pass size it: the root is the viewport, laid out as a row,
/// // and a child with a flex item that grows fills it on every resize.
/// screen.root().addChild(myComp, {.flex = layout::Style {.grow = 1}});
///
/// // Main loop
/// while (running) {
///     screen.draw();  // Render and flush
///     for (auto& event : terminal.poll())
///         screen.dispatchEvent(event);
/// }
/// @endcode
class CORE_TUI_CORE_API Screen
{
  public:
    /// Constructs a Screen that draws on the given surface.
    /// @param surface What the screen draws on (a @c Terminal on a real terminal); must outlive it.
    /// @param config Optional configuration.
    /// @param clock The clock hover delays are measured on (not owned; must outlive the screen).
    explicit Screen(ScreenSurface& surface,
                    ScreenConfig config = {},
                    IClock const& clock CORE_LIFETIMEBOUND = defaultSteadyClock());

    /// A clock is kept, not copied: a temporary would dangle, so an rvalue clock is refused.
    Screen(ScreenSurface& surface, ScreenConfig config, IClock const&& clock) = delete;

    ~Screen();

    // Prevent copying
    Screen(Screen const&) = delete;
    Screen& operator=(Screen const&) = delete;

    // --- Root Component ---

    /// Returns the root component. Add top-level components as children.
    [[nodiscard]] Component& root() noexcept;
    [[nodiscard]] Component const& root() const noexcept;

    // --- Rendering ---

    /// Renders the component tree to the terminal.
    /// Performs diff-based update (or full update if RenderMode::Full).
    void draw();

    /// Marks the entire screen as needing a full redraw.
    void invalidate();

    /// Marks a specific component as needing a redraw.
    void invalidate(Component& component);

    /// Releases cursor tracking state after external output.
    ///
    /// Call this when external processes have written to the terminal (e.g., after
    /// running shell commands). The cursor position is no longer known, so the next
    /// draw() in inline mode will start fresh instead of trying to move relative
    /// to where it thinks the cursor is.
    void releaseCursor();

    /// Clears content from the tracked cursor position and releases cursor tracking.
    ///
    /// Moves up to the top of the inline content region, clears from there to the
    /// end of the display, and calls releaseCursor(). Use this instead of manual
    /// moveUp() + clear + releaseCursor() sequences to avoid cursor drift.
    void clearAndRelease();

    // --- Render Mode ---

    /// Sets the render mode (Diff or Full).
    void setRenderMode(RenderMode mode) { _renderMode = mode; }

    /// Returns the current render mode.
    [[nodiscard]] RenderMode renderMode() const noexcept { return _renderMode; }

    // --- Theme ---

    /// Sets the theme for all components.
    void setTheme(Theme theme);

    /// Returns the current theme.
    [[nodiscard]] Theme const& theme() const noexcept { return _theme; }

    // --- Dimensions ---

    /// Returns the number of rows available for rendering.
    [[nodiscard]] int rows() const noexcept;

    /// Returns the number of columns available for rendering.
    [[nodiscard]] int cols() const noexcept;

    /// Returns the size of the rendering area.
    [[nodiscard]] Size size() const noexcept;

    // --- Viewport ---

    /// Sets the viewport mode.
    void setViewport(Viewport viewport);

    /// Sets a fixed viewport area.
    void setViewport(Rect fixedArea);

    /// Returns the current viewport mode.
    [[nodiscard]] Viewport viewport() const noexcept { return _config.viewport; }

    /// Returns the current viewport area in terminal coordinates.
    [[nodiscard]] Rect viewportArea() const noexcept;

    // --- Event Dispatch ---

    /// Dispatches an event through the component tree.
    /// For mouse events: a move or release goes to the pointer capture target if there is one (see
    /// pointerCapture()); anything else hit tests to find the target (overlays first, see
    /// componentAt()) and bubbles up, and a handled press makes its handler the capture target. A press
    /// also hides the tooltip and resets the hover state, so no tooltip opens during the drag it starts.
    /// For keyboard events: sends to focused component, then bubbles up.
    /// @param event The event to dispatch.
    /// @return The result of event handling.
    [[nodiscard]] EventResult dispatchEvent(InputEvent const& event);

    // --- Focus Groups ---

    /// Sets the active focus group.
    /// Only the active group receives keyboard events.
    void setActiveGroup(FocusGroupId const& group);

    /// Returns the active focus group.
    [[nodiscard]] FocusGroupId const& activeGroup() const noexcept { return _activeGroup; }

    /// Sets the focused component within a focus group.
    void setFocus(Component* component);

    /// Sets the focused component within a specific focus group.
    void setFocus(FocusGroupId const& group, Component* component);

    /// Returns the focused component in the active group.
    [[nodiscard]] Component* focusedComponent() const noexcept;

    /// Returns the focused component in a specific group.
    [[nodiscard]] Component* focusedComponent(FocusGroupId const& group) const noexcept;

    /// Moves focus to the next focusable component in the active group.
    void focusNext();

    /// Moves focus to the previous focusable component in the active group.
    void focusPrev();

    // --- Surface Access ---

    /// Returns the surface this screen draws on.
    [[nodiscard]] ScreenSurface& surface() noexcept { return _surface; }

    [[nodiscard]] ScreenSurface const& surface() const noexcept { return _surface; }

    // --- Overlay System ---

    /// Shows an overlay component at the given absolute screen position.
    ///
    /// Overlays are rendered above all regular components and are not clipped
    /// by their logical parent's bounds. Use this for popups, tooltips, menus, etc.
    ///
    /// @param overlay The overlay component to show.
    /// @param position Absolute screen position (top-left corner of overlay).
    void showOverlay(Component& overlay, Point position);

    /// Hides/removes an overlay from the screen.
    /// @param overlay The overlay to hide.
    void hideOverlay(Component& overlay);

    /// Updates an overlay's position.
    /// @param overlay The overlay to reposition.
    /// @param position New absolute screen position.
    void positionOverlay(Component& overlay, Point position);

    /// Returns true if the overlay is currently visible.
    [[nodiscard]] bool isOverlayVisible(Component const& overlay) const noexcept;

    // --- Hit Testing ---

    /// @brief Returns the top-most visible component at a cell, as the last draw() laid the screen out.
    ///
    /// Overlays come first, in the order they are drawn, so the one shown last is on top; then the
    /// tree, siblings in reverse z-order and the deepest descendant that contains the cell. The
    /// screen's own tooltip is never returned. Mouse events are routed by the same lookup; a drag
    /// source asks it, at the release, which component lies under the pointer.
    /// @param row 0-based row, in the coordinates of Component::screenBounds() (rows from the top of
    ///            the inline content in Viewport::Inline).
    /// @param col 0-based column, in the same coordinates.
    /// @return The component, or nullptr when nothing but the root covers the cell.
    [[nodiscard]] Component* componentAt(int row, int col) const;

    // --- Pointer Capture ---

    /// @brief Returns the component the pointer is captured by, or nullptr.
    ///
    /// A press makes the component that handled it (after bubbling) the capture target. Until the
    /// capture ends, every move and the next release go to it directly -- no hit test, no bubbling --
    /// in its own coordinates, which are below 1 or beyond its size once the pointer has left it. The
    /// press itself bubbles as any mouse event does, so an ancestor that takes the capture sees the
    /// press in the coordinates of the component under the pointer, and the moves and the release in
    /// its own. The release ends the capture, as do the next press, releasePointer(), and the target
    /// leaving this screen: destroyed, removed from the tree (an ancestor's removal included, and a
    /// move to another parent, which removes it on the way) or hidden as an overlay. Scroll events are
    /// never captured, and a captured move does not update the hover state.
    /// @return The capture target, or nullptr when the pointer is not captured.
    [[nodiscard]] Component* pointerCapture() const noexcept { return _pointerCapture; }

    /// @brief Ends the pointer capture, if any: the next move or release is hit-tested again.
    ///
    /// A press handler that calls it and handles the press declines the capture its press would take.
    /// One that calls it and ignores the press lets the press bubble on, as any ignored event does, and
    /// the ancestor that handles it takes the capture.
    void releasePointer() noexcept { _pointerCapture = nullptr; }

    // --- Hover and Tooltip System ---

    /// Returns the hover state manager.
    [[nodiscard]] HoverState& hoverState() noexcept { return _hoverState; }

    [[nodiscard]] HoverState const& hoverState() const noexcept { return _hoverState; }

    /// Returns the recommended poll timeout in ms, considering hover state.
    ///
    /// Returns -1 if no timeout needed (block indefinitely).
    /// Returns 0 if immediate processing needed.
    /// Returns >0 for hover delay timeout.
    [[nodiscard]] int pollTimeoutMs() const;

    /// Processes hover timer tick. Call this when poll times out with no events.
    void tickHover();

    /// Shows a tooltip at the specified position.
    /// @param text Tooltip content.
    /// @param position Absolute screen position for tooltip.
    /// @param contentType Content type (plain text or markdown).
    void showTooltip(std::string_view text,
                     Point position,
                     TooltipContentType contentType = TooltipContentType::PlainText);

    /// Hides the current tooltip.
    void hideTooltip();

    /// Returns true if a tooltip is currently visible.
    [[nodiscard]] bool isTooltipVisible() const noexcept { return _tooltipVisible; }

    /// Returns the last-rendered buffer (for testing). Valid after draw().
    [[nodiscard]] Buffer const& renderedBuffer() const noexcept { return _previous; }

    /// The most frames in a row whose layout sweep only the previous frame's sweep asked for. Settling takes
    /// a frame per level of Responsive whose hints its own size moves; a chain longer than this is
    /// breakpoints whose choices flip each other (see Responsive), which would swap once per frame forever.
    static constexpr int MaxChainedLayoutSweeps = 8;

    /// Whether the layout stopped re-laying a chain of sweeps past MaxChainedLayoutSweeps: the tree is left
    /// as the last sweep put it. A resize, or a hint changed from outside a sweep, takes it up again.
    /// @return True from the frame the chain was abandoned until the next such change.
    [[nodiscard]] bool layoutAbandoned() const noexcept
    {
        return _chainedLayoutSweeps > MaxChainedLayoutSweeps;
    }

  private:
    friend class Component;

    ScreenSurface& _surface;
    ScreenConfig _config;
    Theme _theme;
    RenderMode _renderMode = RenderMode::Diff;

    /// The component the pointer is captured by, or nullptr.
    Component* _pointerCapture = nullptr;

    /// The component dispatchPress() is waiting on, cleared if it leaves this screen meanwhile.
    Component const* _pressTarget = nullptr;

    std::unique_ptr<RootComponent> _root;
    Buffer _current;
    Buffer _previous;

    bool _needsFullRedraw = true;
    bool _enteredAlternateScreen = false; ///< Whether we switched to the alternate screen buffer.
    std::unordered_set<Component*> _dirtyComponents;
    int _previousContentHeight = 0;
    int _previousCursorRow = 0;         ///< Cursor row at end of last inline render (for positioning)
    int _inlineContentStartRow = -1;    ///< Terminal row (0-based) where inline content starts (-1 = unknown)
    int _mainContentHeight = 0;         ///< Content height before overlays (for mouse coordinate translation)
    int _previousMainContentHeight = 0; ///< Previous main content height (for detecting real content growth)
    int _peakContentHeight = 0;         ///< High water mark - max content height we've allocated space for
    int _totalNewlinesEmitted = 0;      ///< Total newlines emitted since start (for tracking content shift)
    // NOTE: Unscroll feature is disabled until properly implemented for inline mode.
    // The _scrolledLinesCount and _unscrollSupported fields are kept for future use.
    // int _scrolledLinesCount = 0;   ///< Lines scrolled into scrollback (for unscroll)
    // bool _unscrollSupported = false; ///< Resolved unscroll support (from config + detection)

    // Focus state
    FocusGroupId _activeGroup = std::string(DefaultFocusGroup);
    std::unordered_map<FocusGroupId, Component*> _focusedComponents;
    CursorShape _currentCursorShape = CursorShape::Default; ///< Last applied cursor shape

    // Overlay state
    struct OverlayEntry
    {
        Component* component = nullptr;
        Point position;
    };

    std::vector<OverlayEntry> _overlays;

    // Deferred blurs. A component that leaves this screen focused hears onBlur() -- but not from inside
    // the detach: the detach runs in loops over a parent's children (setScreen(), clearChildren(), a
    // layout pass), where a handler that adds or removes a sibling would reallocate the vector being
    // walked. Every such loop holds a TreeChange open; the components that lost their focus wait in
    // _pendingBlurs, and the outermost TreeChange to close calls them, then destroys what was retired.

    /// Holds a tree change on @p screen open for its lifetime; the outermost one to close delivers the
    /// deferred blurs. A null screen holds nothing.
    class TreeChange
    {
      public:
        explicit TreeChange(Screen* screen) noexcept: _screen(screen)
        {
            if (_screen)
                ++_screen->_treeChanges;
        }
        ~TreeChange()
        {
            if (_screen)
                _screen->endTreeChange();
        }
        TreeChange(TreeChange const&) = delete;
        TreeChange& operator=(TreeChange const&) = delete;
        TreeChange(TreeChange&&) = delete;
        TreeChange& operator=(TreeChange&&) = delete;

      private:
        Screen* _screen;
    };

    /// Closes one tree change; the outermost calls onBlur() of each component in _pendingBlurs, in the
    /// order they left, and then destroys what retire() was handed -- until neither has anything left.
    void endTreeChange();

    /// Forgets a component waiting for its onBlur(): it is being destroyed, or is being focused again
    /// and hears it now.
    /// @param component The component waiting.
    void forgetQueuedBlur(Component& component) noexcept;

    /// Keeps a component that left the tree alive until the open tree change closes, so that a blur
    /// queued for it, or for one of its children, reaches a living object. Destroys it at once when no
    /// change is open.
    /// @param component The component, already removed from its parent.
    void retire(std::unique_ptr<Component> component);

    int _treeChanges = 0;                             ///< How many TreeChange are open.
    std::vector<Component*> _pendingBlurs;            ///< Left focused, onBlur() not yet called.
    std::vector<std::unique_ptr<Component>> _retired; ///< Destroyed when the tree change closes.

    // Hover and Tooltip state
    HoverState _hoverState;
    Tooltip _tooltip;
    bool _tooltipVisible = false;

    /// Sizes the root to the viewport, which lays out the children added with a flex item, then lays out
    /// again the children of every component a child's hintsChanged() marked.
    void layoutRoot();

    /// Called by a component whose child's hints changed: the next layoutRoot() sweeps the tree for it.
    /// A mark made while a sweep runs is that sweep's consequence, and is counted as such.
    void layoutRequested() noexcept
    {
        _layoutRequested =
            _layoutSweep == LayoutSweep::Running ? LayoutRequest::Chained : LayoutRequest::Pending;
    }

    /// Lays out again, depth first, the children of each component in @p component's subtree marked by a
    /// child's hintsChanged().
    static void relayoutMarked(Component& component);

    /// Whether some component on this screen was marked by a child's hintsChanged(), and by what.
    enum class LayoutRequest : std::uint8_t
    {
        None,    ///< Nothing marked.
        Pending, ///< Marked from outside a sweep: a resize, a component changing its own hints.
        Chained, ///< Marked by the previous frame's sweep alone, as a Responsive it resized swapped.
    };
    LayoutRequest _layoutRequested = LayoutRequest::None;

    /// Whether layoutRoot() is sweeping the tree, so that a mark made now is known as the sweep's own.
    enum class LayoutSweep : std::uint8_t
    {
        Idle,
        Running,
    };
    LayoutSweep _layoutSweep = LayoutSweep::Idle;

    /// How many frames in a row have swept for a Chained request alone; past MaxChainedLayoutSweeps, the
    /// chain was abandoned, until a resize or a mark from outside a sweep sets it back to zero.
    int _chainedLayoutSweeps = 0;

    // Rendering phases
    void beginFrame();
    void renderTree();
    void renderComponent(Component& component, Rect parentBounds);
    void renderOverlays();
    void endFrame();
    void flush();
    void flushFullscreen();
    void flushInline();
    void flushFixed();

    /// @brief Repaints @p area by absolute cursor motion, cell by cell.
    ///
    /// The shared body of flushFullscreen() and flushFixed(), which differ only in the rectangle
    /// they cover. flushInline() deliberately does not come through here: it moves the cursor
    /// sequentially, erases to end of line and maintains scroll bookkeeping, so it shares the loop
    /// shape but almost none of the emission.
    ///
    /// @param area Rectangle to repaint, in buffer coordinates. Clipped to the buffer.
    void flushArea(Rect area);
    void applyCursorShape(); ///< Applies cursor shape based on focused component.

    // Hit testing
    /// componentAt() without its last step: an empty cell answers the root, which is what mouse
    /// dispatch and the hover state expect.
    /// @param row 0-based row, as for componentAt().
    /// @param col 0-based column, as for componentAt().
    /// @return The top-most visible component at the cell, the root, or nullptr outside the root.
    [[nodiscard]] Component* hitTest(int row, int col) const;
    [[nodiscard]] Component* componentAtRecursive(Component& component, int row, int col) const;

    // Event dispatch helpers
    [[nodiscard]] static EventResult bubbleEvent(Component* target, InputEvent const& event);
    [[nodiscard]] EventResult dispatchKeyEvent(InputEvent const& event);

    /// @brief Routes a mouse event: to the capture target, or to the component under the pointer.
    ///
    /// A move or release while the pointer is captured goes to the capture target alone, in its
    /// coordinates. Anything else is hit-tested and bubbles from the component under the pointer, in
    /// that component's coordinates; a press goes through dispatchPress(), so the component that
    /// handles it takes the capture, and first ends any capture left over and the hover.
    /// @param mouse The event, in 1-based terminal coordinates.
    /// @return The result of the component that handled it, or Ignored.
    [[nodiscard]] EventResult dispatchMouseEvent(MouseEvent const& mouse);

    /// @brief Delivers a press from @p target up through its ancestors, making each the capture target
    /// while it decides, so that the one that handles it keeps the capture.
    /// @param target The component under the pointer.
    /// @param event The press, in @p target's coordinates.
    /// @return The first result that is not Ignored, or Ignored.
    [[nodiscard]] EventResult dispatchPress(Component* target, InputEvent const& event);

    /// @brief Called by a component that leaves this screen -- removed from the tree, directly or with
    /// an ancestor, or hidden as an overlay -- so that nothing here outlives its place on the screen: the
    /// pointer capture, the press target, the focus of every group it held and the hover on it. A focused
    /// one is queued for its onBlur(), delivered when the open tree change closes (endTreeChange()).
    ///
    /// Not noexcept: queueing the blur may allocate. It queues before it forgets anything, so a throw
    /// leaves the screen and the component as they were (Component::setScreen() then leaves _screen as
    /// it was, too).
    /// @param component The component leaving.
    void componentDetached(Component& component);

    /// @brief What componentDetached() does but queue a blur: forgets every reference this screen holds
    /// to @p component and clears its focus. Allocates nothing, so a destructor may call it.
    /// @param component The component leaving or being destroyed.
    void forgetComponent(Component& component) noexcept;

    /// @brief Called by a component this screen still knows when it is destroyed: what
    /// componentDetached() does, and an overlay that was never hidden leaves the overlay list, so
    /// nothing draws, hit-tests or detaches it after it is gone.
    /// @param component The component being destroyed.
    void componentDestroyed(Component& component) noexcept;

    // Focus helpers
    static void updateFocus(Component* oldFocus, Component* newFocus);
    [[nodiscard]] std::vector<Component*> collectFocusableComponents(FocusGroupId const& group) const;
    void collectFocusableRecursive(Component& component,
                                   FocusGroupId const& group,
                                   std::vector<Component*>& out) const;
};

} // namespace core::tui
