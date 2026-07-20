// SPDX-License-Identifier: Apache-2.0
#include <contour/TerminalSession.h>
#include <contour/display/CaretGeometry.h>
#include <contour/display/TerminalAccessible.h>
#include <contour/display/TerminalDisplay.h>
#include <contour/display/ViewportTextIndex.h>

#include <vtbackend/Terminal.h>

#include <QtQuick/QQuickWindow>

#include <atomic>
#include <mutex>

namespace
{

/// Emits an accessibility event to the platform's assistive technology.
///
/// QAccessible::updateAccessibility() does not take ownership of the event it is handed, so the
/// event must outlive the call but not the scope -- a stack object, never a `new` expression.
///
/// @tparam Event The QAccessibleEvent subclass to construct.
/// @param args   Constructor arguments for that event.
template <typename Event, typename... Args>
void notifyAccessibility(Args&&... args)
{
    auto event = Event(std::forward<Args>(args)...);
    QAccessible::updateAccessibility(&event);
}

} // namespace

namespace contour::display
{

namespace
{
    /// Whether an assistive client is attached. See TerminalAccessible::isActive() for why this is a flag
    /// of our own rather than a call to QAccessible::isActive().
    std::atomic<bool> accessibilityActive { false };

    /// Keeps @ref accessibilityActive current. Qt calls this on the GUI thread when a client attaches or
    /// detaches, which is what makes the flag safe to read from the terminal thread.
    class ActivationObserver final: public QAccessible::ActivationObserver
    {
      public:
        void accessibilityActiveChanged(bool active) override
        {
            accessibilityActive.store(active, std::memory_order_release);
        }
    };

    QAccessibleInterface* accessibleFactory(QString const& className, QObject* object)
    {
        if (className != QLatin1String("contour::display::TerminalDisplay"))
            return nullptr;

        if (auto* display = qobject_cast<TerminalDisplay*>(object))
            return new TerminalAccessible(display);

        return nullptr;
    }
} // namespace

// {{{ TerminalAccessible

TerminalAccessible::TerminalAccessible(TerminalDisplay* display): QAccessibleObject { display }
{
}

TerminalAccessible::~TerminalAccessible() = default;

void TerminalAccessible::installFactory()
{
    static auto installed = false;
    if (installed)
        return;
    installed = true;

    QAccessible::installFactory(&accessibleFactory);

    // The observer is how the terminal thread may cheaply ask whether anyone is listening. Ownership
    // stays here (Qt does not take it), so a function-local static outlives every use of the flag.
    static auto observer = ActivationObserver {};
    QAccessible::installActivationObserver(&observer);

    // Seed it: a client may already be attached by the time the app gets here.
    accessibilityActive.store(QAccessible::isActive(), std::memory_order_release);
}

bool TerminalAccessible::isActive() noexcept
{
    return accessibilityActive.load(std::memory_order_acquire);
}

TerminalDisplay* TerminalAccessible::display() const
{
    return qobject_cast<TerminalDisplay*>(object());
}

bool TerminalAccessible::isValid() const
{
    auto const* item = display();
    return item != nullptr && item->hasSession();
}

QAccessible::State TerminalAccessible::state() const
{
    auto state = QAccessible::State {};
    state.focusable = true;
    state.multiLine = true;
    state.selectableText = true;

    if (auto const* item = display())
        state.focused = item->hasActiveFocus();

    return state;
}

QRect TerminalAccessible::rect() const
{
    auto* item = display();
    if (item == nullptr || item->window() == nullptr)
        return {};

    return toGlobalRect(QRectF { 0.0, 0.0, item->width(), item->height() },
                        item->mapToGlobal(QPointF { 0.0, 0.0 }));
}

QString TerminalAccessible::text(QAccessible::Text which) const
{
    auto* item = display();
    if (item == nullptr || !item->hasSession())
        return {};

    switch (which)
    {
        case QAccessible::Name: return item->session().title();
        case QAccessible::Value: {
            auto const lock = std::lock_guard { item->terminal() };
            auto const& screen = item->terminal().currentScreen();
            return QString::fromStdString(screen.lineTextAt(screen.cursor().position.line));
        }
        default: return {};
    }
}

void TerminalAccessible::setText(QAccessible::Text /*which*/, QString const& /*text*/)
{
    // The terminal's contents belong to the application running in it.
}

QAccessibleInterface* TerminalAccessible::parent() const
{
    auto* item = display();
    if (item == nullptr || item->window() == nullptr)
        return nullptr;
    return QAccessible::queryAccessibleInterface(item->window());
}

PromptAccessible* TerminalAccessible::promptInterface() const
{
    if (!_prompt)
        _prompt = std::make_unique<PromptAccessible>(const_cast<TerminalAccessible*>(this));
    return _prompt.get();
}

int TerminalAccessible::childCount() const
{
    return _promptShown ? 1 : 0;
}

QAccessibleInterface* TerminalAccessible::child(int index) const
{
    if (index != 0 || !_promptShown)
        return nullptr;
    return promptInterface();
}

int TerminalAccessible::indexOfChild(QAccessibleInterface const* child) const
{
    if (_promptShown && child == _prompt.get())
        return 0;
    return -1;
}

QAccessibleInterface* TerminalAccessible::childAt(int x, int y) const
{
    if (!_promptShown)
        return nullptr;

    auto* prompt = promptInterface();
    return prompt->rect().contains(x, y) ? prompt : nullptr;
}

void* TerminalAccessible::interface_cast(QAccessible::InterfaceType type)
{
    if (type == QAccessible::TextInterface)
        return static_cast<QAccessibleTextInterface*>(this);
    return nullptr;
}

// }}}
// {{{ TerminalAccessible: QAccessibleTextInterface

int TerminalAccessible::characterCount() const
{
    auto* item = display();
    if (item == nullptr || !item->hasSession())
        return 0;

    auto const lock = std::lock_guard { item->terminal() };
    return flatTextLength(item->terminal().pageSize());
}

int TerminalAccessible::cursorPosition() const
{
    auto* item = display();
    if (item == nullptr || !item->hasSession())
        return 0;

    auto const lock = std::lock_guard { item->terminal() };
    auto const& terminal = item->terminal();
    if (!terminal.isCursorInViewport())
        return 0;

    return flatOffsetOf(terminal.currentScreen().cursor().position, terminal.pageSize().columns);
}

void TerminalAccessible::setCursorPosition(int /*position*/)
{
    // Declined. The caret is the shell's; placing it would mean synthesizing keystrokes and guessing at
    // the application's idea of where it is.
}

QRect TerminalAccessible::characterRect(int offset) const
{
    auto* item = display();
    if (item == nullptr || !item->hasSession() || item->window() == nullptr)
        return {};

    auto const metrics = item->gridMetrics();
    auto const columns = [&] {
        auto const lock = std::lock_guard { item->terminal() };
        return item->terminal().pageSize().columns;
    }();

    auto const cell = cellAtFlatOffset(offset, columns);
    auto const local = cellRectangle(metrics.pageMargin, metrics.cellSize, cell, 1, item->contentScale());

    return toGlobalRect(local, item->mapToGlobal(QPointF { 0.0, 0.0 }));
}

QString TerminalAccessible::text(int startOffset, int endOffset) const
{
    auto* item = display();
    if (item == nullptr || !item->hasSession() || startOffset >= endOffset)
        return {};

    auto const lock = std::lock_guard { item->terminal() };
    auto const& terminal = item->terminal();
    auto const columns = terminal.pageSize().columns;
    auto const clampedEnd = std::min(endOffset, flatTextLength(terminal.pageSize()));

    // Rebuilt from the grid rather than cached: an assistive client asks for a range at a time, and a
    // cache would have to be invalidated on every screen update.
    auto result = QString {};
    auto const& screen = terminal.currentScreen();
    for (auto offset = std::max(0, startOffset); offset < clampedEnd; ++offset)
    {
        auto const cell = cellAtFlatOffset(offset, columns);
        if (cell.column == boxed_cast<vtbackend::ColumnOffset>(columns))
        {
            result += QLatin1Char('\n');
            continue;
        }
        if (cell.line >= boxed_cast<vtbackend::LineOffset>(terminal.pageSize().lines))
            break;
        result += QString::fromStdString(screen.lineTextAt(cell.line)).mid(unbox<int>(cell.column), 1);
    }
    return result;
}

int TerminalAccessible::offsetAtPoint(QPoint const& point) const
{
    auto* item = display();
    if (item == nullptr || !item->hasSession() || item->window() == nullptr)
        return -1;

    auto const local = item->mapFromGlobal(QPointF { point });
    auto const metrics = item->gridMetrics();
    auto const dpr = item->contentScale();

    auto const cellWidth = unbox<double>(metrics.cellSize.width) / dpr;
    auto const cellHeight = unbox<double>(metrics.cellSize.height) / dpr;
    if (cellWidth <= 0.0 || cellHeight <= 0.0)
        return -1;

    auto const column = static_cast<int>((local.x() - (metrics.pageMargin.left / dpr)) / cellWidth);
    auto const line = static_cast<int>((local.y() - (metrics.pageMargin.top / dpr)) / cellHeight);
    if (column < 0 || line < 0)
        return -1;

    auto const lock = std::lock_guard { item->terminal() };
    auto const pageSize = item->terminal().pageSize();
    if (column >= unbox<int>(pageSize.columns) || line >= unbox<int>(pageSize.lines))
        return -1;

    return flatOffsetOf({ .line = vtbackend::LineOffset(line), .column = vtbackend::ColumnOffset(column) },
                        pageSize.columns);
}

QString TerminalAccessible::attributes(int offset, int* startOffset, int* endOffset) const
{
    // No per-character attributes are reported; the whole grid reads as one run.
    if (startOffset != nullptr)
        *startOffset = offset;
    if (endOffset != nullptr)
        *endOffset = offset + 1;
    return {};
}

void TerminalAccessible::selection(int /*selectionIndex*/, int* startOffset, int* endOffset) const
{
    if (startOffset != nullptr)
        *startOffset = 0;
    if (endOffset != nullptr)
        *endOffset = 0;
}

int TerminalAccessible::selectionCount() const
{
    return 0;
}

void TerminalAccessible::addSelection(int /*startOffset*/, int /*endOffset*/)
{
}

void TerminalAccessible::removeSelection(int /*selectionIndex*/)
{
}

void TerminalAccessible::setSelection(int /*selectionIndex*/, int /*startOffset*/, int /*endOffset*/)
{
}

void TerminalAccessible::scrollToSubstring(int /*startIndex*/, int /*endIndex*/)
{
}

// }}}
// {{{ TerminalAccessible: reporting

void TerminalAccessible::resetCaretGate()
{
    _gate.reset();
}

void TerminalAccessible::reportCaret()
{
    auto* item = display();
    if (item == nullptr || !item->hasSession())
        return;

    // Only the FOCUSED pane speaks. Every pane of a split runs its own terminal thread and fires its own
    // cursor notifications; without this they would fight over the client's caret and a magnifier would
    // ping-pong between them.
    if (!item->hasActiveFocus())
        return;

    auto const current = [&] {
        auto const lock = std::lock_guard { item->terminal() };
        auto const& terminal = item->terminal();

        // BLINK-FREE deliberately: cursorCurrentlyVisible() folds in the blink phase, and following that
        // would announce a stationary caret twice a second.
        auto const visible =
            terminal.isModeEnabled(vtbackend::DECMode::VisibleCursor) && terminal.isCursorInViewport();

        auto state = CaretState { .visible = visible,
                                  .position = terminal.currentScreen().cursor().position,
                                  .prompt = std::nullopt };

        if (visible)
            if (auto const span = terminal.livePromptSpan(); span.has_value())
                state.prompt = *span;

        return state;
    }();

    if (!_gate.shouldReport(current))
        return;

    auto const hadPrompt = std::exchange(_promptShown, current.prompt.has_value());

    if (current.prompt.has_value() && !hadPrompt)
    {
        auto* prompt = promptInterface();
        notifyAccessibility<QAccessibleEvent>(prompt, QAccessible::ObjectShow);
        notifyAccessibility<QAccessibleEvent>(prompt, QAccessible::Focus);
    }
    else if (!current.prompt.has_value() && hadPrompt)
    {
        notifyAccessibility<QAccessibleEvent>(promptInterface(), QAccessible::ObjectHide);
        notifyAccessibility<QAccessibleEvent>(this, QAccessible::Focus);
    }
    else if (current.prompt.has_value())
    {
        // Same prompt region, moved: the viewport scrolled or the shell repainted it.
        notifyAccessibility<QAccessibleEvent>(promptInterface(), QAccessible::LocationChanged);
    }

    // Emitted alongside, never instead of: TextCaretMoved says the OFFSET changed, which does not tell a
    // magnifier that the same offset now sits at different pixels after a scroll.
    notifyAccessibility<QAccessibleTextCursorEvent>(object(), cursorPosition());
    notifyAccessibility<QAccessibleEvent>(this, QAccessible::LocationChanged);
}

// }}}
// {{{ PromptAccessible

bool PromptAccessible::isValid() const
{
    return _parent != nullptr && _parent->isValid();
}

QAccessible::State PromptAccessible::state() const
{
    auto state = QAccessible::State {};
    state.focusable = true;
    state.focused = true;
    state.editable = true;
    state.multiLine = true;
    return state;
}

QRect PromptAccessible::rect() const
{
    auto* item = _parent != nullptr ? _parent->display() : nullptr;
    if (item == nullptr || !item->hasSession() || item->window() == nullptr)
        return {};

    auto const metrics = item->gridMetrics();

    auto const span = [&] {
        auto const lock = std::lock_guard { item->terminal() };
        return item->terminal().livePromptSpan();
    }();
    if (!span.has_value())
        return {};

    auto const columns = [&] {
        auto const lock = std::lock_guard { item->terminal() };
        return item->terminal().pageSize().columns;
    }();

    auto const local = rowBandRectangle(
        metrics.pageMargin, metrics.cellSize, span->firstLine, span->lastLine, columns, item->contentScale());

    return toGlobalRect(local, item->mapToGlobal(QPointF { 0.0, 0.0 }));
}

QString PromptAccessible::text(QAccessible::Text which) const
{
    if (which == QAccessible::Name)
        return QObject::tr("shell prompt");
    return {};
}

void PromptAccessible::setText(QAccessible::Text /*which*/, QString const& /*text*/)
{
}

QAccessibleInterface* PromptAccessible::parent() const
{
    return _parent;
}

QAccessibleInterface* PromptAccessible::child(int /*index*/) const
{
    return nullptr;
}

int PromptAccessible::indexOfChild(QAccessibleInterface const* /*child*/) const
{
    return -1;
}

QAccessibleInterface* PromptAccessible::childAt(int /*x*/, int /*y*/) const
{
    return nullptr;
}

// }}}

} // namespace contour::display
