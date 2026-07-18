// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/display/CaretReportGate.h>

#include <QtGui/QAccessible>
#include <QtGui/QAccessibleInterface>
#include <QtGui/QAccessibleObject>

#include <memory>

namespace contour::display
{

class TerminalDisplay;
class TerminalAccessible;

/// The shell's live prompt, exposed to assistive technology as a focusable text region.
///
/// Both major trackers follow FOCUS before they follow the caret — AT-SPI's
/// `object:state-changed:focused` is what KWin/Plasma's zoom consumes, and Qt's UIA bridge turns
/// QAccessible::Focus into UIA_AutomationFocusChangedEventId for Windows Magnifier. So "expose the prompt
/// area" only means something to the OS if the prompt is an object that can hold focus; a larger
/// rectangle on the terminal itself would be read by nothing.
///
/// Has no QObject behind it: it is a region of the terminal, not a widget. Qt 6 allows a QObject-less
/// interface to be the subject of a QAccessibleEvent, which is all this needs. Owned by its parent
/// @ref TerminalAccessible and destroyed with it.
class PromptAccessible final: public QAccessibleInterface
{
  public:
    /// @param parent The terminal interface this prompt belongs to. Must outlive this object.
    explicit PromptAccessible(TerminalAccessible* parent): _parent { parent } {}

    [[nodiscard]] bool isValid() const override;
    [[nodiscard]] QObject* object() const override { return nullptr; }
    [[nodiscard]] QAccessible::Role role() const override { return QAccessible::EditableText; }
    [[nodiscard]] QAccessible::State state() const override;
    [[nodiscard]] QRect rect() const override;
    [[nodiscard]] QString text(QAccessible::Text which) const override;
    void setText(QAccessible::Text which, QString const& text) override;

    [[nodiscard]] QAccessibleInterface* parent() const override;
    [[nodiscard]] QAccessibleInterface* child(int index) const override;
    [[nodiscard]] int childCount() const override { return 0; }
    [[nodiscard]] int indexOfChild(QAccessibleInterface const* child) const override;
    [[nodiscard]] QAccessibleInterface* childAt(int x, int y) const override;

  private:
    TerminalAccessible* _parent;
};

/// The terminal grid, exposed to assistive technology.
///
/// Exists so that OS magnifiers can follow the terminal caret, which they do by reading a text
/// interface's character extents and listening for caret-moved events. Contour had no accessibility
/// plumbing at all before this, so a magnifier simply could not track where the user was typing.
///
/// Registered through QAccessible::installFactory rather than QML's Accessible attached properties: those
/// can set a name and a role but cannot supply a QAccessibleTextInterface, and the text interface is
/// precisely what caret tracking needs.
///
/// Every state read takes the terminal lock and copies ONE GridMetrics snapshot, the same tear-free
/// idiom TerminalDisplay::inputMethodQuery() uses.
class TerminalAccessible final: public QAccessibleObject, public QAccessibleTextInterface
{
  public:
    /// @param display The display item this interface adapts.
    explicit TerminalAccessible(TerminalDisplay* display);
    ~TerminalAccessible() override;

    /// Installs the factory that hands out these interfaces. Idempotent; call once during app startup.
    ///
    /// Not a static initializer on purpose: the ordering against QGuiApplication matters, and a test
    /// needs to be able to trigger it deliberately.
    static void installFactory();

    /// Whether an assistive client is attached.
    ///
    /// Maintained by a QAccessible activation observer on the GUI thread, so it can be read from the
    /// terminal thread — QAccessible::isActive() reads a Qt-internal static with no memory ordering, and
    /// the whole point of this flag is to be read on the hot path from another thread.
    [[nodiscard]] static bool isActive() noexcept;

    // {{{ QAccessibleInterface
    [[nodiscard]] bool isValid() const override;
    [[nodiscard]] QAccessible::Role role() const override { return QAccessible::Terminal; }
    [[nodiscard]] QAccessible::State state() const override;
    [[nodiscard]] QRect rect() const override;
    [[nodiscard]] QString text(QAccessible::Text which) const override;
    void setText(QAccessible::Text which, QString const& text) override;

    [[nodiscard]] QAccessibleInterface* parent() const override;
    [[nodiscard]] QAccessibleInterface* child(int index) const override;
    [[nodiscard]] int childCount() const override;
    [[nodiscard]] int indexOfChild(QAccessibleInterface const* child) const override;
    [[nodiscard]] QAccessibleInterface* childAt(int x, int y) const override;
    [[nodiscard]] void* interface_cast(QAccessible::InterfaceType type) override;
    // }}}

    // {{{ QAccessibleTextInterface
    /// The screen rectangle of the cell at @p offset. THE method a magnifier reads to place its viewport.
    [[nodiscard]] QRect characterRect(int offset) const override;
    [[nodiscard]] int cursorPosition() const override;
    [[nodiscard]] int characterCount() const override;
    [[nodiscard]] QString text(int startOffset, int endOffset) const override;
    [[nodiscard]] int offsetAtPoint(QPoint const& point) const override;
    [[nodiscard]] QString attributes(int offset, int* startOffset, int* endOffset) const override;

    void selection(int selectionIndex, int* startOffset, int* endOffset) const override;
    [[nodiscard]] int selectionCount() const override;
    void addSelection(int startOffset, int endOffset) override;
    void removeSelection(int selectionIndex) override;
    void setSelection(int selectionIndex, int startOffset, int endOffset) override;

    /// Declined: the caret belongs to the shell, not to us. Moving it would mean synthesizing input.
    void setCursorPosition(int position) override;

    void scrollToSubstring(int startIndex, int endIndex) override;
    // }}}

    /// The display this interface adapts, or nullptr once it has been destroyed.
    [[nodiscard]] TerminalDisplay* display() const;

    /// Recomputes the caret state and, if it changed in a way worth announcing, tells the OS.
    ///
    /// Runs on the GUI THREAD only. Reads the BLINK-FREE visibility predicate deliberately: the render
    /// buffer's cursor is simply absent while the cursor is blinked off, so following it would announce a
    /// stationary caret twice a second. @see CaretReportGate.
    void reportCaret();

    /// Forgets what was last announced, so the next state is announced afresh. @see CaretReportGate::reset.
    void resetCaretGate();

  private:
    /// The prompt child, created on demand while a live prompt exists.
    [[nodiscard]] PromptAccessible* promptInterface() const;

    CaretReportGate _gate;
    /// Mirrors whether a prompt child currently exists, so show/hide are announced once each.
    bool _promptShown = false;
    /// Created on first use, including from const query methods; owned here and destroyed with this.
    mutable std::unique_ptr<PromptAccessible> _prompt;
};

} // namespace contour::display
