// SPDX-License-Identifier: Apache-2.0
#include <contour/display/TerminalAccessible.hpp>
#include <contour/display/TerminalDisplay.hpp>
#include <contour/display/ViewportTextIndex.hpp>
#include <contour/geometry/CellRectangle.hpp>
#include <contour/session/TerminalSession.hpp>

#include <vtbackend/Terminal.hpp>

#include <QtQuick/QQuickWindow>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>

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

/// Emits an @p Event about @p iface, naming the subject the way Qt's own layout requires.
///
/// QAccessibleEvent stores `m_child` and `m_uniqueId` in a UNION, and EVERY event class's
/// QAccessibleInterface overload writes the unique id into it *and* sets `m_object = iface->object()`.
/// So for an interface that HAS a QObject, Qt takes its QObject branch and reads that id back as a
/// child index; ids are allocated from 0x80000000 up, so the lookup gets a large negative number,
/// fails, and Qt DISCARDS the event -- leaving nothing but "Invalid child in QAccessibleEvent" on
/// stderr to say the report never reached the OS.
///
/// Resolving the subject HERE is what makes that unreachable. Every event class carries both
/// overloads over the same union, so a construction written out at the call site is correct only by
/// the author's discipline, and the failure is silent when it is not.
///
/// @tparam Event The QAccessibleEvent subclass to construct.
/// @param iface  The interface the event is about. Must not be null.
/// @param args   The event's remaining constructor arguments.
template <typename Event, typename... Args>
void notifyAbout(QAccessibleInterface* iface, Args&&... args)
{
    if (auto* object = iface->object())
        notifyAccessibility<Event>(object, std::forward<Args>(args)...);
    else
        notifyAccessibility<Event>(iface, std::forward<Args>(args)...);
}

/// @ref notifyAbout for a plain QAccessibleEvent, whose only other argument is the event type.
void notifySubject(QAccessibleInterface* iface, QAccessible::Event type)
{
    notifyAbout<QAccessibleEvent>(iface, type);
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

TerminalAccessible::TerminalAccessible(TerminalDisplay* display):
    QAccessibleObject { display }, _prompt { new PromptAccessible(this) }
{
    // Registered up front rather than on first use, so the two views of the prompt's ownership can
    // never disagree: an interface reaches the cache the instant it becomes the subject of a
    // QAccessibleEvent anyway, because that constructor calls QAccessible::uniqueId() on it.
    // NB: PromptAccessible only stores the pointer, so passing a half-constructed `this` is safe.
    QAccessible::registerAccessibleInterface(_prompt);
}

TerminalAccessible::~TerminalAccessible()
{
    // Hand the prompt back to Qt's accessibility cache, which is what deletes it -- ~QAccessibleInterface
    // is protected precisely to say that the cache owns every interface it has handed an id to.
    //
    // It learns that one has gone away through QObject::destroyed alone, a hook QAccessibleCache::insert
    // installs only for interfaces that HAVE an object. The prompt has none, so nothing would ever drop
    // its entry: deleting it here instead left the cache holding a dangling pointer, which it then
    // dereferenced and freed a second time from ~QAccessibleCache at application exit -- issue #2015.
    //
    // The id is looked up rather than remembered because the cache RECYCLES ids: a stored one could
    // name someone else's interface by the time it is used.
    QAccessible::deleteAccessibleInterface(QAccessible::uniqueId(_prompt));
}

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

    return geometry::toGlobalRect(QRectF { 0.0, 0.0, item->width(), item->height() },
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

PromptAccessible* TerminalAccessible::promptInterface() const noexcept
{
    return _prompt;
}

int TerminalAccessible::childCount() const
{
    return _shownPrompt.has_value() ? 1 : 0;
}

QAccessibleInterface* TerminalAccessible::child(int index) const
{
    if (index != 0 || !_shownPrompt.has_value())
        return nullptr;
    return promptInterface();
}

int TerminalAccessible::indexOfChild(QAccessibleInterface const* child) const
{
    if (_shownPrompt.has_value() && child == _prompt)
        return 0;
    return -1;
}

QAccessibleInterface* TerminalAccessible::childAt(int x, int y) const
{
    if (!_shownPrompt.has_value())
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
    auto const local =
        geometry::cellRectangle(metrics.pageMargin, metrics.cellSize, cell, 1, item->devicePixelRatio());

    return geometry::toGlobalRect(local, item->mapToGlobal(QPointF { 0.0, 0.0 }));
}

QString TerminalAccessible::text(int startOffset, int endOffset) const
{
    auto* item = display();
    if (item == nullptr || !item->hasSession()) // terminal() asserts a session; check before touching it
        return {};

    auto const lock = std::lock_guard { item->terminal() };
    auto const& terminal = item->terminal();
    auto const pageSize = terminal.pageSize();
    auto const columns = pageSize.columns;

    // Subsumes startOffset >= endOffset, since begin >= startOffset and end <= endOffset.
    auto const begin = std::max(0, startOffset);
    auto const end = std::min(endOffset, flatTextLength(pageSize));
    if (begin >= end)
        return {};

    // Rebuilt from the grid rather than cached: an assistive client asks for a range at a time, and a
    // cache would have to be invalidated on every screen update.
    //
    // Read one LINE at a time, by COLUMN RANGE, into one buffer. Each of those matters on a path a
    // platform bridge walks after every caret move -- macOS turns AXValue into text(0, characterCount())
    // -- on the GUI thread, holding this lock. Asking the grid once per character instead made a
    // full-viewport read quadratic in the line length, because every call rebuilt the whole line.
    auto utf8 = std::string {};
    utf8.reserve(static_cast<size_t>(end - begin));

    auto const& screen = terminal.currentScreen();
    auto const lastColumn = boxed_cast<vtbackend::ColumnOffset>(columns);
    auto const stride = unbox<int>(columns) + 1;
    auto const lastLine = cellAtFlatOffset(end - 1, columns).line;

    for (auto line = cellAtFlatOffset(begin, columns).line; line <= lastLine; ++line)
    {
        auto const lineBegin = flatOffsetOf({ .line = line, .column = vtbackend::ColumnOffset(0) }, columns);
        auto const from = vtbackend::ColumnOffset(std::max(begin, lineBegin) - lineBegin);
        auto const to = vtbackend::ColumnOffset(std::min(end, lineBegin + stride) - lineBegin);

        if (from < lastColumn)
            utf8 += screen.lineTextColumnAlignedAt(line, from, std::min(to, lastColumn));

        if (to > lastColumn) // the newline that ends this row falls inside the range
            utf8 += '\n';
    }
    return QString::fromStdString(utf8);
}

int TerminalAccessible::offsetAtPoint(QPoint const& point) const
{
    auto* item = display();
    if (item == nullptr || !item->hasSession() || item->window() == nullptr)
        return -1;

    auto const local = item->mapFromGlobal(QPointF { point });
    auto const metrics = item->gridMetrics();
    auto const dpr = item->devicePixelRatio();

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

    // ONE pass under the lock, yielding the state the gate judges and the caret's flat offset. Asking
    // cursorPosition() for the latter afterwards would take the lock a second time for a value in hand.
    auto const [current, caretOffset] = [&] {
        auto const lock = std::lock_guard { item->terminal() };
        auto const& terminal = item->terminal();

        // BLINK-FREE deliberately: cursorCurrentlyVisible() folds in the blink phase, and following that
        // would announce a stationary caret twice a second.
        auto const visible =
            terminal.isModeEnabled(vtbackend::DECMode::VisibleCursor) && terminal.isCursorInViewport();

        auto state = CaretState { .visible = visible,
                                  .position = terminal.currentScreen().cursor().position,
                                  .prompt = std::nullopt };

        // The prompt scan is the EXPENSIVE half: findLivePromptRegion climbs up to MaxPromptScanLines
        // logical lines, and without shell integration it cannot conclude "no prompt" until it has
        // spent that whole budget. This runs at frame rate on the GUI thread holding the terminal lock,
        // so skip it whenever it cannot change the verdict -- which the gate can answer without it.
        //
        // A prompt appearing while the caret stands perfectly still is the one case this defers, and it
        // does not occur in practice: the shell writes the prompt, which moves the cursor.
        if (visible && !_gate.promptScanRedundant(visible, state.position))
            if (auto const span = terminal.livePromptSpan(); span.has_value())
                state.prompt = *span;

        return std::pair { state, flatOffsetOf(state.position, terminal.pageSize().columns) };
    }();

    if (!_gate.shouldReport(current))
        return;

    // ONE history: what was last ANNOUNCED. The gate's own memory records even when it declines, so it
    // answers a different question and the two must not be mixed. This one also backs childCount()/
    // child()/indexOfChild(), which must agree with the ObjectShow/ObjectHide pair.
    auto const previous = std::exchange(_shownPrompt, current.prompt);

    if (current.prompt.has_value() && !previous.has_value())
    {
        auto* prompt = promptInterface();
        notifySubject(prompt, QAccessible::ObjectShow);
        notifySubject(prompt, QAccessible::Focus);
    }
    else if (!current.prompt.has_value() && previous.has_value())
    {
        notifySubject(promptInterface(), QAccessible::ObjectHide);
        notifySubject(this, QAccessible::Focus);
    }
    else if (current.prompt.has_value() && current.prompt != previous)
    {
        // Same prompt region, MOVED: the viewport scrolled or the shell repainted it. Only when it
        // actually moved -- the caret walking along an unchanged prompt does not relocate the region.
        notifySubject(promptInterface(), QAccessible::LocationChanged);
    }

    notifyAbout<QAccessibleTextCursorEvent>(this, caretOffset);
}

void TerminalAccessible::reportLocation()
{
    notifySubject(this, QAccessible::LocationChanged);
    if (_shownPrompt.has_value())
        notifySubject(promptInterface(), QAccessible::LocationChanged);
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

    // One lock for both reads: taking it twice gave the grid a window to be resized between the span
    // and the width it is measured against.
    auto const [span, columns] = [&] {
        auto const lock = std::lock_guard { item->terminal() };
        return std::pair { item->terminal().livePromptSpan(), item->terminal().pageSize().columns };
    }();
    if (!span.has_value())
        return {};

    auto const local = geometry::rowBandRectangle(metrics.pageMargin,
                                                  metrics.cellSize,
                                                  span->firstLine,
                                                  span->lastLine,
                                                  columns,
                                                  item->devicePixelRatio());

    return geometry::toGlobalRect(local, item->mapToGlobal(QPointF { 0.0, 0.0 }));
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
