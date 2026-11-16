// SPDX-License-Identifier: Apache-2.0
#include <core/tui/Screen.hpp>

#include <core/tui/Canvas.hpp>
#include <core/tui/FrameDiff.hpp>
#include <core/tui/ScreenSurface.hpp>

#include <algorithm>
#include <chrono>
#include <ranges>
#include <utility>
#include <vector>

namespace core::tui
{

InlineCursorMovement calculateInlineCursorMovement(int previousContentHeight,
                                                   int previousCursorRow,
                                                   int newContentHeight)
{
    InlineCursorMovement result;

    // Move from previous cursor position to row 0
    if (previousContentHeight > 0 && previousCursorRow > 0)
        result.moveUpToStart = previousCursorRow;

    // Calculate newlines needed when content grows
    if (newContentHeight > previousContentHeight)
    {
        result.newLinesToEmit = newContentHeight - previousContentHeight;
        // Move back up by the number of newlines emitted (NOT contentHeight!)
        result.moveUpAfterNewlines = result.newLinesToEmit;
    }

    // Calculate rows to clear when content shrinks
    if (previousContentHeight > newContentHeight && newContentHeight > 0)
        result.rowsToClear = previousContentHeight - newContentHeight;

    return result;
}

namespace
{

    /// @brief The number of rows from the top of @p frame down to its last row holding anything
    /// but blanks: how much of an inline frame there is to show.
    [[nodiscard]] int contentHeightOf(Buffer const& frame) noexcept
    {
        auto const blank = [](Cell const* cell) {
            return cell == nullptr || cell->grapheme.empty() || cell->grapheme == " ";
        };
        for (auto const row: std::views::iota(0, frame.rows()) | std::views::reverse)
            for (auto const col: std::views::iota(0, frame.cols()))
                if (!blank(frame.cellAt({ .x = col, .y = row })))
                    return row + 1;
        return 0;
    }

} // namespace

Screen::Screen(ScreenSurface& surface, ScreenConfig config, IClock const& clock):
    _surface(surface),
    _config(config),
    _theme(currentTheme()),
    _root(std::make_unique<RootComponent>()),
    _hoverState(HoverState::DefaultHoverDelay, clock),
    _tooltip(_config.tooltipBorder)
{
    _root->setScreen(this);

    // Initialize buffers based on terminal size
    auto const termRows = _surface.rows();
    auto const termCols = _surface.columns();
    _current.resize(termRows, termCols);
    _previous.resize(termRows, termCols);

    // NOTE: Unscroll feature (CSI n + T) is currently disabled for inline mode because
    // the sequence shifts the ENTIRE screen down, which doesn't work well when rendering
    // at the bottom of the terminal. The feature needs a different approach for inline
    // rendering, possibly using scroll regions.
    // The UnscrollMode config is preserved for future implementation.
    (void) _config.unscrollMode; // Suppress unused warning

    // Wire hover callbacks: translate hover position to component-relative coordinates
    // and call the component's onHover() virtual method.
    _hoverState.setOnHoverConfirmed([this](HoverInfo const& hover) {
        auto* target = hover.target;
        // Fall back to focused component when hit-testing fails (e.g., in inline mode
        // before first render or after releaseCursor() resets _inlineContentStartRow).
        if (!target)
            target = focusedComponent();
        if (!target)
            return;
        auto const bounds = target->screenBounds();
        int const relX = hover.x - 1 - bounds.x;
        int const relY = hover.y - 1 - bounds.y;
        if (auto result = target->onHover(relX, relY))
        {
            auto const absPos =
                Point { .x = bounds.x + result->position.x, .y = bounds.y + result->position.y };
            showTooltip(result->text, absPos, result->contentType);
        }
    });

    _hoverState.setOnHoverLeave([this]() { hideTooltip(); });

    // Switch to the alternate screen buffer (DEC private mode 1049) so the app paints on a
    // fresh screen and the user's scrollback is preserved; restored in the destructor.
    if (_config.screenBuffer == ScreenBuffer::Alternate)
    {
        _surface.output().writeRaw("\033[?1049h");
        _surface.output().flush();
        _enteredAlternateScreen = true;
    }
}

Screen::~Screen()
{
    // Overlays and the tree's components belong to the caller and may outlive this screen. Each
    // forgets it here, while every member is alive, so that no teardown -- theirs later, or the root's
    // among this screen's members -- reaches a destroyed screen.
    // Every component that leaves focused hears onBlur() when this change closes, while every member
    // is still alive; each has forgotten this screen by then. An onBlur() handler must not re-attach a
    // component to this screen during that final flush (showOverlay() on it, say):
    // the screen is being destroyed, and what it re-attached would point at a dead screen.
    {
        auto const change = TreeChange { this };
        for (auto const& entry: std::exchange(_overlays, {}))
            entry.component->setScreen(nullptr);
        _root->setScreen(nullptr);
    }
    // (_root is declared before _overlays and the other containers, so it is destroyed after them;
    // still attached, ~RootComponent would have called back into those destroyed members.)

    if (_enteredAlternateScreen)
    {
        auto& out = _surface.output();
        out.showCursor();
        out.writeRaw("\033[?1049l"); // leave the alternate screen, restoring the primary buffer
        out.flush();
    }
}

Component& Screen::root() noexcept
{
    return *_root;
}

Component const& Screen::root() const noexcept
{
    return *_root;
}

void Screen::draw()
{
    beginFrame();
    renderTree();

    // Calculate main content height BEFORE overlays for mouse coordinate translation
    if (_config.viewport == Viewport::Inline)
    {
        _previousMainContentHeight = _mainContentHeight;
        _mainContentHeight = contentHeightOf(_current);
    }

    renderOverlays();
    endFrame();
    flush();
}

void Screen::invalidate()
{
    _needsFullRedraw = true;
}

void Screen::invalidate([[maybe_unused]] Component& component)
{
    // For now, just mark full redraw needed.
    // Future optimization: track dirty regions per component.
    _needsFullRedraw = true;
}

void Screen::releaseCursor()
{
    // Reset cursor tracking for inline mode.
    // After external output (e.g., shell command output), the cursor position
    // is no longer where we think it is. Reset to 0 so next draw() starts fresh.
    _previousContentHeight = 0;
    _previousCursorRow = 0;
    _peakContentHeight = 0;
    _inlineContentStartRow = -1;
    _totalNewlinesEmitted = 0;
    _needsFullRedraw = true;
    // Reset cursor shape tracking so we re-apply it on next flush
    _currentCursorShape = CursorShape::Default;
}

void Screen::clearAndRelease()
{
    auto& out = _surface.output();

    // Move up from the tracked cursor row to the top of the content region.
    if (_previousCursorRow > 0)
        out.moveUp(_previousCursorRow);

    // Clear from here to end of display and flush.
    out.carriageReturn();
    out.clearToEndOfDisplay();
    out.enableReflow();
    out.saveCursor(); // DECSC: anchor exact cursor position for next Screen
    out.flush();

    // Communicate to the next Screen that room already exists on the terminal,
    // so its first flushInline() render can skip the LF room reservation that
    // would otherwise cause +1 cursor drift per shell→agent transition.
    out.setInlineRoomReserved(_peakContentHeight);

    releaseCursor();
}

void Screen::setTheme(Theme theme)
{
    _theme = theme;
    invalidate();
}

int Screen::rows() const noexcept
{
    switch (_config.viewport)
    {
        case Viewport::Fullscreen: return _surface.rows();
        case Viewport::Inline: return _config.inlineMaxHeight > 0 ? _config.inlineMaxHeight : _surface.rows();
        case Viewport::Fixed: return _config.fixedArea.height;
    }
    return _surface.rows();
}

int Screen::cols() const noexcept
{
    switch (_config.viewport)
    {
        case Viewport::Fullscreen: return _surface.columns();
        case Viewport::Inline: return _surface.columns();
        case Viewport::Fixed: return _config.fixedArea.width;
    }
    return _surface.columns();
}

Size Screen::size() const noexcept
{
    return { .width = cols(), .height = rows() };
}

void Screen::setViewport(Viewport viewport)
{
    _config.viewport = viewport;
    invalidate();
}

void Screen::setViewport(Rect fixedArea)
{
    _config.viewport = Viewport::Fixed;
    _config.fixedArea = fixedArea;
    invalidate();
}

Rect Screen::viewportArea() const noexcept
{
    switch (_config.viewport)
    {
        case Viewport::Fullscreen:
            return { .x = 0, .y = 0, .width = _surface.columns(), .height = _surface.rows() };
        case Viewport::Inline: {
            // For inline mode, the viewport area is the maximum area available for rendering.
            // We use inlineMaxHeight if set, otherwise the full terminal height.
            // The actual content height (_previousContentHeight) is only used for flush positioning.
            int const maxHeight = _config.inlineMaxHeight > 0 ? _config.inlineMaxHeight : _surface.rows();
            return { .x = 0, .y = 0, .width = _surface.columns(), .height = maxHeight };
        }
        case Viewport::Fixed: return _config.fixedArea;
    }
    return { .x = 0, .y = 0, .width = _surface.columns(), .height = _surface.rows() };
}

EventResult Screen::dispatchEvent(InputEvent const& event)
{
    // Handle resize events specially
    if (std::holds_alternative<ResizeEvent>(event))
    {
        auto const& resize = std::get<ResizeEvent>(event);
        _current.resize(resize.rows, resize.columns);
        _previous.resize(resize.rows, resize.columns);
        layoutRoot();
        invalidate();
        return EventResult::Handled;
    }

    // Handle mouse events with hit testing
    if (auto const* mouse = std::get_if<MouseEvent>(&event))
    {
        return dispatchMouseEvent(*mouse);
    }

    // Handle keyboard events - send to focused component
    return dispatchKeyEvent(event);
}

void Screen::setActiveGroup(FocusGroupId const& group)
{
    _activeGroup = group;
}

void Screen::setFocus(Component* component)
{
    if (component)
        setFocus(component->focusGroup(), component);
    else
        setFocus(_activeGroup, nullptr);
}

void Screen::setFocus(FocusGroupId const& group, Component* component)
{
    Component* oldFocus = _focusedComponents[group];
    if (oldFocus != component)
    {
        _focusedComponents[group] = component;
        if (group == _activeGroup)
            updateFocus(oldFocus, component);
    }
}

Component* Screen::focusedComponent() const noexcept
{
    return focusedComponent(_activeGroup);
}

Component* Screen::focusedComponent(FocusGroupId const& group) const noexcept
{
    auto it = _focusedComponents.find(group);
    return it != _focusedComponents.end() ? it->second : nullptr;
}

void Screen::focusNext()
{
    auto focusable = collectFocusableComponents(_activeGroup);
    if (focusable.empty())
        return;

    Component* current = focusedComponent();
    if (!current)
    {
        setFocus(focusable.front());
        return;
    }

    auto it = std::ranges::find(focusable, current);
    if (it != focusable.end())
        ++it;
    if (it == focusable.end())
        setFocus(focusable.front()); // Wrap around
    else
        setFocus(*it);
}

void Screen::focusPrev()
{
    auto focusable = collectFocusableComponents(_activeGroup);
    if (focusable.empty())
        return;

    Component* current = focusedComponent();
    if (!current)
    {
        setFocus(focusable.back());
        return;
    }

    auto it = std::ranges::find(focusable, current);
    if (it == focusable.end() || it == focusable.begin())
        setFocus(focusable.back()); // Wrap around
    else
        setFocus(*--it);
}

// --- Private Methods ---

void Screen::beginFrame()
{
    // Resize buffers if terminal size changed
    auto const termRows = _surface.rows();
    auto const termCols = _surface.columns();

    if (_current.rows() != termRows || _current.cols() != termCols)
    {
        _current.resize(termRows, termCols);
        _previous.resize(termRows, termCols);
        _needsFullRedraw = true;
    }

    layoutRoot();

    // Clear current buffer
    _current.clear(_theme.textNormal);
}

void Screen::layoutRoot()
{
    // One tree change for the whole pass: a blur a swap queues is delivered after it, not inside a loop.
    auto const change = TreeChange { this };

    // The root's area is the viewport's size at the origin: its children's areas are relative to the
    // viewport, which renderTree() offsets them by. setArea() lays the tree out only when the size changed.
    // A resize takes up a chain that was abandoned.
    auto const viewport = viewportArea();
    auto const rootArea = Rect { .x = 0, .y = 0, .width = viewport.width, .height = viewport.height };
    if (_root->area() != rootArea)
        _chainedLayoutSweeps = 0;
    _root->setArea(rootArea);

    // One sweep: a mark made during it (a Responsive the sweep resized swapping its layout) is the next
    // frame's, so a layout whose hints chase its size settles a frame at a time rather than looping here.
    switch (_layoutRequested)
    {
        case LayoutRequest::None:
            if (!layoutAbandoned())
                _chainedLayoutSweeps = 0;
            return;
        case LayoutRequest::Pending: _chainedLayoutSweeps = 0; break;
        case LayoutRequest::Chained:
            // Past the bound the chain does not settle: the marks stay where they are, and the layout as
            // it is, until something from outside -- a resize, a hint changed -- asks again.
            if (++_chainedLayoutSweeps > MaxChainedLayoutSweeps)
            {
                _layoutRequested = LayoutRequest::None;
                return;
            }
            break;
    }
    _layoutRequested = LayoutRequest::None;
    _layoutSweep = LayoutSweep::Running;
    relayoutMarked(*_root);
    _layoutSweep = LayoutSweep::Idle;
}

void Screen::relayoutMarked(Component& component)
{
    if (component.layoutPending())
        component.layoutChildren();
    // A copy: laying a child's children out may rebuild them (Responsive), but not this list.
    auto const children = std::vector<Component*>(component.children().begin(), component.children().end());
    for (Component* child: children)
        relayoutMarked(*child);
}

namespace
{

    /// Returns a component's children in paint order: ascending z-index, children with the same z-index
    /// in the order they were added. A stable sort makes the last-added of a tie the one drawn on top, and
    /// hit-testing walks this order backwards so that it finds what the user sees.
    std::vector<Component*> paintOrder(Component const& component)
    {
        auto const children = component.children();
        std::vector<Component*> ordered(children.begin(), children.end());
        std::ranges::stable_sort(ordered,
                                 [](Component* a, Component* b) { return a->zIndex() < b->zIndex(); });
        return ordered;
    }

} // namespace

void Screen::renderTree()
{
    // Calculate root bounds based on viewport
    Rect const rootBounds = viewportArea();

    // Set root's screen bounds
    _root->setScreenBounds(rootBounds);

    // Render root's children sorted by z-index
    for (Component* child: paintOrder(*_root))
    {
        if (child->visible())
            renderComponent(*child, rootBounds);
    }
}

void Screen::renderComponent(Component& component, Rect parentBounds)
{
    // Calculate component's screen bounds
    Rect const localArea = component.area();
    Rect screenBounds = localArea.offset(parentBounds.x, parentBounds.y);

    // Clip to parent bounds
    screenBounds = screenBounds.intersect(parentBounds);
    if (screenBounds.empty())
        return;

    component.setScreenBounds(screenBounds);

    // Create canvas for this component
    Canvas canvas(_current, screenBounds, _theme);

    // Render the component
    component.render(canvas);

    // Render children sorted by z-index
    for (Component* child: paintOrder(component))
    {
        if (child->visible())
            renderComponent(*child, screenBounds);
    }
}

void Screen::renderOverlays()
{
    // Render overlays on top of the main tree, without clipping to parent bounds
    for (auto const& entry: _overlays)
    {
        if (!entry.component || !entry.component->visible())
            continue;

        // Calculate overlay bounds at absolute position
        Size const overlaySize = entry.component->preferredSize();
        Rect overlayBounds { .x = entry.position.x,
                             .y = entry.position.y,
                             .width = overlaySize.width,
                             .height = overlaySize.height };

        // Clip only to screen bounds, not to any parent
        Rect const screenBounds = viewportArea();
        overlayBounds = overlayBounds.intersect(screenBounds);
        if (overlayBounds.empty())
            continue;

        entry.component->setScreenBounds(overlayBounds);

        // Create canvas for overlay (clipped to screen only)
        Canvas canvas(_current, overlayBounds, _theme);
        entry.component->render(canvas);

        // Render overlay's children (if any)
        for (Component* child: paintOrder(*entry.component))
        {
            if (child->visible())
                renderComponent(*child, overlayBounds);
        }
    }
}

void Screen::showOverlay(Component& overlay, Point position)
{
    // Check if already visible
    auto it = std::ranges::find_if(_overlays, [&](auto const& e) { return e.component == &overlay; });

    if (it != _overlays.end())
    {
        // Update position
        it->position = position;
    }
    else
    {
        // Add new overlay
        _overlays.push_back({ .component = &overlay, .position = position });
        overlay.setScreen(this);
    }
    invalidate();
}

void Screen::hideOverlay(Component& overlay)
{
    auto it = std::ranges::find_if(_overlays, [&](auto const& e) { return e.component == &overlay; });

    if (it != _overlays.end())
    {
        _overlays.erase(it);
        invalidate();
        overlay.setScreen(nullptr);
    }
}

void Screen::positionOverlay(Component& overlay, Point position)
{
    auto it = std::ranges::find_if(_overlays, [&](auto const& e) { return e.component == &overlay; });

    if (it != _overlays.end())
    {
        it->position = position;
        invalidate();
    }
}

bool Screen::isOverlayVisible(Component const& overlay) const noexcept
{
    return std::ranges::find_if(_overlays, [&](auto const& e) { return e.component == &overlay; })
           != _overlays.end();
}

void Screen::endFrame()
{
    // Swap buffers for next frame
    // Note: we swap after flush, so _previous holds the last rendered state
}

void Screen::flush()
{
    if (_config.viewport == Viewport::Inline && _peakContentHeight == 0)
    {
        // First inline render: skip SyncGuard — no previous content to tear against,
        // and avoids SIGCHLD-interrupted write leaving the terminal in sync-buffer mode.
        _surface.output().flush();
        flushInline();
        applyCursorShape();
        _surface.output().flush();
    }
    else
    {
        // Use synchronized output to prevent tearing
        auto syncGuard = _surface.output().syncGuard();

        switch (_config.viewport)
        {
            case Viewport::Fullscreen: flushFullscreen(); break;
            case Viewport::Inline: flushInline(); break;
            case Viewport::Fixed: flushFixed(); break;
        }

        // Apply cursor shape based on focused component
        applyCursorShape();
    }

    // Swap buffers
    std::swap(_current, _previous);
    _needsFullRedraw = false;
    _dirtyComponents.clear();
}

void Screen::flushArea(Rect area)
{
    auto& out = _surface.output();
    bool const useDiff = (_renderMode == RenderMode::Diff) && !_needsFullRedraw;

    // Hide cursor during update
    out.hideCursor();

    writeFrameDiff(out,
                   _previous,
                   _current,
                   DiffOptions { .addressing = DiffAddressing::Absolute,
                                 .repaint = useDiff ? DiffRepaint::Changed : DiffRepaint::All,
                                 .area = area,
                                 .hyperlinks = DiffHyperlinks::Emit });

    // Restore cursor
    if (_current.cursorVisible())
    {
        Point const cursor = _current.cursor();
        out.moveTo(cursor.y + 1, cursor.x + 1);
        out.showCursor();
    }

    out.flush();
}

void Screen::flushFullscreen()
{
    flushArea(Rect { .x = 0, .y = 0, .width = _current.cols(), .height = _current.rows() });
}

void Screen::flushInline()
{
    auto& out = _surface.output();

    int const contentHeight = contentHeightOf(_current);

    // Move cursor to start of content region (row 0)
    // The cursor is currently at _previousCursorRow from the last render
    if (_previousContentHeight > 0)
    {
        // Move from previous cursor position to row 0
        if (_previousCursorRow > 0)
            out.moveUp(_previousCursorRow);
    }

    // If content grew beyond our peak, we need to emit newlines to make room
    // Only emit newlines when growing BEYOND the peak, not when re-showing an overlay
    // that we already made room for.
    if (contentHeight > _peakContentHeight)
    {
        if (_peakContentHeight == 0)
        {
            // First render: emit newlines to ensure room at the bottom of the terminal.
            // Without this, rendering near the last row causes \n to scroll the header
            // into scrollback, making it invisible until the next draw.
            //
            // However, if a previous Screen already reserved room via clearAndRelease(),
            // the space already exists on the terminal. Emitting LFs again would push the
            // cursor down by +1 per transition (the Ctrl+T drift bug).
            auto const reservedRoom = out.consumeInlineRoom();
            if (reservedRoom > 0)
            {
                // A previous Screen saved the cursor position — restore it exactly.
                // This eliminates any tracking drift from relative cursor movements.
                out.restoreCursor(); // DECRC: anchor to exact saved position

                // Query cursor position after DECRC to know where content starts.
                auto const cursor = _surface.queryCursorPosition();
                auto const startRow =
                    (cursor && cursor->first > 0) ? (cursor->first - 1) : (_surface.rows() - 1);

                if (reservedRoom >= contentHeight)
                {
                    // Room already exists — skip LF emission, just adopt the peak.
                    _peakContentHeight = contentHeight;
                }
                else
                {
                    // Room exists but insufficient — emit additional LFs for the extra rows.
                    auto const newLines = contentHeight - reservedRoom;
                    for ([[maybe_unused]] auto _: std::views::iota(0, newLines))
                        out.linefeed();
                    if (newLines > 0)
                        out.moveUp(newLines);
                    _totalNewlinesEmitted += newLines;
                    _peakContentHeight = contentHeight;
                }

                // Content start: min(cursor row, bottom-aligned position).
                _inlineContentStartRow = std::min(startRow, _surface.rows() - contentHeight);
            }
            else
            {
                // Very first render (no previous Screen) — reserve room with LFs.
                // Query actual cursor position to compute _inlineContentStartRow accurately.
                auto const cursor = _surface.queryCursorPosition();
                auto const startRow =
                    (cursor && cursor->first > 0) ? (cursor->first - 1) : (_surface.rows() - 1);

                // Emit contentHeight-1 newlines (enough to ensure room without overshooting).
                auto const newLines = std::max(0, contentHeight - 1);
                for ([[maybe_unused]] auto _: std::views::iota(0, newLines))
                    out.linefeed();
                if (newLines > 0)
                    out.moveUp(newLines);
                _totalNewlinesEmitted += newLines;
                _peakContentHeight = contentHeight;

                // Content start: min(cursor row, bottom-aligned position).
                // Cursor near top (row 5, rows=50, height=5): start = min(5, 45) = 5
                // Cursor at bottom (row 49, rows=50, height=5): start = min(49, 45) = 45
                _inlineContentStartRow = std::min(startRow, _surface.rows() - contentHeight);
            }
        }
        else
        {
            // Content grew — emit LFs for the growth and adjust start row only for actual scrolls.
            auto const growth = contentHeight - _peakContentHeight;
            auto const scrollsNeeded = std::max(0, _inlineContentStartRow + contentHeight - _surface.rows());

            for ([[maybe_unused]] auto _: std::views::iota(0, growth))
                out.linefeed();
            if (growth > 0)
                out.moveUp(growth);

            _peakContentHeight = contentHeight;
            _totalNewlinesEmitted += growth;
            _inlineContentStartRow -= scrollsNeeded;
        }
    }
    else if (_inlineContentStartRow < 0)
    {
        // Haven't set start row yet but peak is already set (shouldn't happen, but handle it)
        _inlineContentStartRow = _surface.rows() - _mainContentHeight - _totalNewlinesEmitted;
    }

    // Track how many rows to clear if content shrank
    int const rowsToClear = (_previousContentHeight > contentHeight && contentHeight > 0)
                                ? (_previousContentHeight - contentHeight)
                                : 0;

    _previousContentHeight = contentHeight;

    // Now render each line, from row 0 of the content, where the cursor is.
    bool const useDiff = (_renderMode == RenderMode::Diff) && !_needsFullRedraw;

    out.hideCursor();

    writeFrameDiff(
        out,
        _previous,
        _current,
        DiffOptions { .addressing = DiffAddressing::Relative,
                      .repaint = useDiff ? DiffRepaint::Changed : DiffRepaint::All,
                      .area = Rect { .x = 0, .y = 0, .width = _current.cols(), .height = contentHeight },
                      .hyperlinks = DiffHyperlinks::Emit,
                      .images = DiffImages::Emit,
                      .rowMarks = DiffRowMarks::Emit,
                      .reflow = _config.reflow });

    // Clear excess rows when content shrank (to avoid leaving garbage on screen)
    if (rowsToClear > 0)
    {
        // We're currently at row contentHeight-1, need to clear rows below
        for ([[maybe_unused]] auto _: std::views::iota(0, rowsToClear))
        {
            out.linefeed();
            out.carriageReturn();
            out.clearToEndOfLine();
        }
        // Move back up to where we were (row contentHeight-1)
        out.moveUp(rowsToClear);
    }

    // Position cursor and track its row for next render
    Point const cursor = _current.cursor();
    _previousCursorRow = cursor.y;

    if (_current.cursorVisible())
    {
        // Move to cursor position (we're currently at row contentHeight-1)
        int const rowsToMoveUp = contentHeight - 1 - cursor.y;
        if (rowsToMoveUp > 0)
            out.moveUp(rowsToMoveUp);
        else if (rowsToMoveUp < 0)
            out.moveDown(-rowsToMoveUp);

        out.carriageReturn();
        if (cursor.x > 0)
            out.moveRight(cursor.x);

        out.showCursor();
    }

    out.flush();
}

void Screen::flushFixed()
{
    // Fullscreen's repaint, restricted to the configured area.
    flushArea(_config.fixedArea);
}

void Screen::applyCursorShape()
{
    CursorShape desiredShape = CursorShape::Default;
    if (Component* focused = focusedComponent())
        desiredShape = focused->cursorShape();

    if (desiredShape != _currentCursorShape)
    {
        _surface.output().setCursorShape(desiredShape);
        _currentCursorShape = desiredShape;
    }
}

Component* Screen::componentAt(int row, int col) const
{
    auto* const found = hitTest(row, col);
    return found == _root.get() ? nullptr : found;
}

Component* Screen::hitTest(int row, int col) const
{
    // Overlays are drawn after the tree, in the order they were first shown, so the last of them is on
    // top. The tooltip is not a target: it opens under the pointer, and hit-testing it would end the
    // hover over the component it describes.
    for (auto const& entry: _overlays | std::views::reverse)
    {
        auto* const overlay = entry.component;
        if (overlay == nullptr || overlay == &_tooltip || !overlay->visible()
            || !overlay->screenBounds().contains(col, row))
            continue;
        return componentAtRecursive(*overlay, row, col);
    }
    return componentAtRecursive(*_root, row, col);
}

Component* Screen::componentAtRecursive(Component& component, int row, int col) const
{
    // Check children top-most first: the reverse of the order they are painted in.
    for (Component* child: paintOrder(component) | std::views::reverse)
    {
        if (child->visible() && child->screenBounds().contains(col, row))
        {
            // Recursively check this child's children
            if (Component* found = componentAtRecursive(*child, row, col))
                return found;
            return child;
        }
    }

    // No child contains the point
    if (component.screenBounds().contains(col, row))
        return &component;

    return nullptr;
}

EventResult Screen::bubbleEvent(Component* target, InputEvent const& event)
{
    while (target)
    {
        EventResult const result = target->onEvent(event);
        if (result != EventResult::Ignored)
            return result;
        target = target->parent();
    }
    return EventResult::Ignored;
}

EventResult Screen::dispatchKeyEvent(InputEvent const& event)
{
    // Auto-hide tooltip on any key press
    if (_tooltipVisible)
    {
        hideTooltip();
        _hoverState.reset();
    }

    Component* focused = focusedComponent();
    if (!focused)
        return EventResult::Ignored;

    return bubbleEvent(focused, event);
}

EventResult Screen::dispatchMouseEvent(MouseEvent const& mouse)
{
    // Note: We intentionally ignore mouse.uiHandled to ensure hover detection
    // works even when the terminal (e.g., Contour with passive tracking) has
    // already processed the event for its own UI purposes.

    // A press starts a new gesture, so it ends whatever capture is left: a release the terminal never
    // delivered (the window lost focus mid-drag) holds the pointer only until the next press. It ends
    // the hover too, as a key press does: a tooltip must not open over the drag that follows, and
    // captured moves do not update the hover state, so a timer left running would confirm.
    if (mouse.type == MouseEvent::Type::Press)
    {
        _pointerCapture = nullptr;
        hideTooltip();
        _hoverState.reset();
    }

    // Moves and the release go to the capture target; presses and scroll events are hit-tested.
    auto const isCaptured =
        _pointerCapture != nullptr
        && (mouse.type == MouseEvent::Type::Move || mouse.type == MouseEvent::Type::Release);

    int mouseRow = mouse.y - 1; // Convert to 0-based
    int const mouseCol = mouse.x - 1;

    // Translate mouse coordinates for inline mode
    // Use _mainContentHeight (content before overlays) to avoid tooltip affecting coordinates
    int const contentHeight = (_mainContentHeight > 0) ? _mainContentHeight : _previousContentHeight;
    if (_config.viewport == Viewport::Inline && contentHeight > 0)
    {
        // _inlineContentStartRow is calculated in flushInline() based on terminal size and peak content
        // height
        if (_inlineContentStartRow < 0)
        {
            // Not yet rendered - skip mouse handling. A release cannot be placed, so it is not
            // delivered, but it still ends the gesture: kept, the capture would take the buttonless
            // moves that follow the next draw.
            if (mouse.type == MouseEvent::Type::Move)
                _hoverState.onMouseMove(mouseCol + 1, 0, nullptr);
            else if (mouse.type == MouseEvent::Type::Release)
                _pointerCapture = nullptr;
            return EventResult::Ignored;
        }

        mouseRow = mouse.y - 1 - _inlineContentStartRow;

        // Outside the inline content only a captured event is delivered: the drag it belongs to may
        // leave the content and come back.
        if (!isCaptured && (mouseRow < 0 || mouseRow >= contentHeight))
        {
            // Still update hover state (to trigger leave if needed)
            if (mouse.type == MouseEvent::Type::Move)
                _hoverState.onMouseMove(mouseCol + 1, mouseRow + 1, nullptr);
            return EventResult::Ignored;
        }
    }

    if (isCaptured)
    {
        // No hit test and no bubbling: the capture target sees its whole gesture, in its own
        // coordinates, wherever the pointer is. A release ends the capture before it is delivered, so
        // its handler may start the next gesture or destroy the component.
        auto* const captureTarget = _pointerCapture;
        if (mouse.type == MouseEvent::Type::Release)
            _pointerCapture = nullptr;
        auto relative = mouse;
        auto const captureBounds = captureTarget->screenBounds();
        relative.x = mouseCol - captureBounds.x + 1;
        relative.y = mouseRow - captureBounds.y + 1;
        return captureTarget->onEvent(relative);
    }

    // Hit test to find target component
    Component* target = hitTest(mouseRow, mouseCol);

    // Update hover state for mouse move events
    // Use viewport-relative 1-based coordinates for consistency with component bounds
    if (mouse.type == MouseEvent::Type::Move)
    {
        _hoverState.onMouseMove(mouseCol + 1, mouseRow + 1, target);
    }

    if (!target)
        return EventResult::Ignored;

    // Create adjusted event with component-relative coordinates
    MouseEvent adjusted = mouse;
    Rect const bounds = target->screenBounds();
    adjusted.x = mouseCol - bounds.x + 1; // Back to 1-based for component
    adjusted.y = mouseRow - bounds.y + 1;

    if (mouse.type == MouseEvent::Type::Press)
        return dispatchPress(target, adjusted);
    return bubbleEvent(target, adjusted);
}

EventResult Screen::dispatchPress(Component* target, InputEvent const& event)
{
    // Each component is the capture target while it decides, and a component's teardown clears the
    // capture (componentDetached()): one destroyed or detached while handling its own press is never
    // left holding the pointer, and one that handles the press after calling releasePointer() declines
    // it. Whether the component left this screen is tracked apart from the capture, because only that
    // stops the press from bubbling: a component that ignores the press passes it on, as bubbleEvent()
    // does, whether or not it released the pointer.
    while (target)
    {
        _pointerCapture = target;
        _pressTarget = target;
        EventResult const result = target->onEvent(event);
        auto const hasLeft = _pressTarget != target;
        _pressTarget = nullptr;
        if (result != EventResult::Ignored)
            return result;
        // It left this screen while handling the press, so it may be gone and its parent cannot be read.
        if (hasLeft)
            return EventResult::Ignored;
        _pointerCapture = nullptr;
        target = target->parent();
    }
    return EventResult::Ignored;
}

void Screen::componentDetached(Component& component)
{
    // A focused component hears onBlur() once the tree change it leaves in closes (endTreeChange()).
    // Queued first: push_back() may throw, and then nothing here has changed yet.
    if (component._focused && !component._blurQueuedOn)
    {
        _pendingBlurs.push_back(&component);
        component._blurQueuedOn = this;
    }
    forgetComponent(component);
}

void Screen::forgetComponent(Component& component) noexcept
{
    if (_pointerCapture == &component)
        _pointerCapture = nullptr;
    if (_pressTarget == &component)
        _pressTarget = nullptr;
    // A focused component taken out of the tree -- a Responsive swapping its layout destroys the old one
    // -- would otherwise be asked for its cursor shape by the same draw() and handed the next key.
    for (auto& entry: _focusedComponents)
        if (entry.second == &component)
            entry.second = nullptr;
    component._focused = false;
    // The hover it was under ends, and the tooltip shown for it with it.
    _hoverState.forget(component);
    _dirtyComponents.erase(&component);
}

void Screen::componentDestroyed(Component& component) noexcept
{
    // Its derived part is gone: it loses its focus with no blur queued, so nothing here allocates.
    forgetComponent(component);
    std::erase_if(_overlays,
                  [&component](OverlayEntry const& entry) { return entry.component == &component; });
}

void Screen::endTreeChange()
{
    if (--_treeChanges > 0)
        return;
    // Held open while the blurs run: what a handler changes queues behind them rather than recursing.
    ++_treeChanges;
    while (!_pendingBlurs.empty() || !_retired.empty())
    {
        // One at a time from the front: a handler may destroy a component still waiting, which takes it
        // out of the list (forgetQueuedBlur()).
        while (!_pendingBlurs.empty())
        {
            auto* const component = _pendingBlurs.front();
            _pendingBlurs.erase(_pendingBlurs.begin());
            component->_blurQueuedOn = nullptr;
            component->onBlur();
        }
        // Retired components have left every screen, so destroying them queues nothing here.
        std::exchange(_retired, {}).clear();
    }
    --_treeChanges;
}

void Screen::forgetQueuedBlur(Component& component) noexcept
{
    std::erase(_pendingBlurs, &component);
    component._blurQueuedOn = nullptr;
}

void Screen::retire(std::unique_ptr<Component> component)
{
    auto const change = TreeChange { this };
    _retired.push_back(std::move(component));
}

int Screen::pollTimeoutMs() const
{
    return _hoverState.timeoutMs();
}

void Screen::tickHover()
{
    _hoverState.tick(_hoverState.clock().now());
}

void Screen::showTooltip(std::string_view text, Point position, TooltipContentType contentType)
{
    _tooltip.setContent(text, contentType);

    // Calculate tooltip size and adjust position to fit on screen
    auto const tooltipSize = _tooltip.preferredSize();
    auto const screenArea = viewportArea();

    // Adjust X to keep tooltip on screen
    int x = position.x;
    if (x + tooltipSize.width > screenArea.width)
        x = std::max(0, screenArea.width - tooltipSize.width);

    // Prefer showing below, but show above if not enough room
    int y = position.y + 1; // Below cursor
    if (y + tooltipSize.height > screenArea.height)
    {
        // Try above
        y = position.y - tooltipSize.height;
        y = std::max(y, 0); // Clamp to top
    }

    showOverlay(_tooltip, Point { .x = x, .y = y });
    _tooltipVisible = true;
}

void Screen::hideTooltip()
{
    if (_tooltipVisible)
    {
        hideOverlay(_tooltip);
        _tooltipVisible = false;
    }
}

void Screen::updateFocus(Component* oldFocus, Component* newFocus)
{
    if (oldFocus)
        oldFocus->setFocused(false);
    if (newFocus)
        newFocus->setFocused(true);
}

std::vector<Component*> Screen::collectFocusableComponents(FocusGroupId const& group) const
{
    std::vector<Component*> result;
    collectFocusableRecursive(*_root, group, result);
    return result;
}

void Screen::collectFocusableRecursive(Component& component,
                                       FocusGroupId const& group,
                                       std::vector<Component*>& out) const
{
    if (component.focusable() && component.visible() && component.focusGroup() == group)
        out.push_back(&component);

    for (Component* child: component.children())
        collectFocusableRecursive(*child, group, out);
}

} // namespace core::tui
