// SPDX-License-Identifier: Apache-2.0
//
// Whether a chord macOS maps to a text writing-direction command still reaches the terminal as a key press.
// StandardKeyBinding.dict binds Control+Command+Down/Left/Right (with or without Option) to such commands,
// and Qt's QNSView hands a key press to the input context whenever the focus item has the input method
// enabled: the context consumes these six chords and Qt delivers no QEvent::KeyPress at all (QTBUG-106516).
// The terminal must therefore answer Qt::ImEnabled false for them, which the real TerminalDisplay does from
// its session's keyboard platform. This tests that workaround for a Qt bug, tracked in
// contour-terminal/contour#2356; remove it with the workaround. The events go through
// -[NSApplication sendEvent:], the path a real key press takes. Needs the cocoa platform plugin and an
// unlocked screen, so every case runs in a display run (CONTOUR_TEST_DISPLAY=1) alone.

#include <contour/display/TerminalDisplay.hpp>
#include <contour/test/DisplayHarness.hpp>
#include <contour/test/TestEnvironment.hpp>

#include <core/Environment.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QObject>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

#import <AppKit/AppKit.h>
#include <QtTest/QTest>

namespace
{

#define REQUIRE_COCOA_OR_SKIP()                                                 \
    do                                                                          \
    {                                                                           \
        if (QGuiApplication::platformName() != QStringLiteral("cocoa")          \
            || !contour::test::displayTestsEnabled(core::LiveEnvironment {}))   \
            SKIP("Needs CONTOUR_TEST_DISPLAY=1 and the cocoa platform plugin"); \
    } while (false)

/// One QEvent::KeyPress as the display was delivered it.
struct DeliveredPress
{
    int key = 0;
    Qt::KeyboardModifiers modifiers;
};

/// Records every key press delivered to the object it is installed on, ahead of that object's own handling.
class PressRecorder final: public QObject
{
  public:
    std::vector<DeliveredPress> presses;

    bool eventFilter(QObject* /*watched*/, QEvent* event) override
    {
        if (event->type() == QEvent::KeyPress)
        {
            auto const* const key = static_cast<QKeyEvent const*>(event);
            presses.push_back({ .key = key->key(), .modifiers = key->modifiers() });
        }
        return false;
    }
};

/// Sends one key down and up for @p keyCode to @p window through the application, as the window server does.
void sendKey(NSWindow* window, unsigned short keyCode, unichar character, NSEventModifierFlags flags)
{
    auto* const text = [NSString stringWithCharacters:&character length:1];
    for (auto const type: { NSEventTypeKeyDown, NSEventTypeKeyUp })
    {
        auto* const event = [NSEvent keyEventWithType:type
                                             location:NSZeroPoint
                                        modifierFlags:flags
                                            timestamp:[NSProcessInfo processInfo].systemUptime
                                         windowNumber:window.windowNumber
                                              context:nil
                                           characters:text
                          charactersIgnoringModifiers:text
                                            isARepeat:NO
                                              keyCode:keyCode];
        if (event != nil)
            [NSApp sendEvent:event];
    }
    QCoreApplication::processEvents();
}

struct Chord
{
    char const* name = "";
    unsigned short keyCode = 0;
    unichar character = 0;
    NSEventModifierFlags flags = 0;
    Qt::Key qtKey = Qt::Key_unknown;
    Qt::KeyboardModifiers qtModifiers;
};

} // namespace

TEST_CASE("CocoaKeyDelivery.writingDirectionChordsReachTheTerminal", "[display][keyboard][cocoa]")
{
    REQUIRE_COCOA_OR_SKIP();

    // The real TerminalDisplay, bound to a real session and its keyboard platform, in a window.
    auto const h = contour::test::DisplayHarness { contour::test::HarnessWindow::Unrendered };
    auto recorder = PressRecorder {};
    h.display->installEventFilter(&recorder);
    h.window->requestActivate();
    [NSApp activateIgnoringOtherApps:YES];
    REQUIRE(QTest::qWaitForWindowActive(h.window.get()));
    h.display->forceActiveFocus();
    REQUIRE(h.display->hasActiveFocus());

    // winId() is the QNSView's address.
    auto* const rawView = reinterpret_cast<void*>(h.window->winId());
    auto* const view = (__bridge NSView*)rawView;
    auto* const nsWindow = [view window];
    REQUIRE(nsWindow != nil);
    [nsWindow makeKeyAndOrderFront:nil];

    auto const arrows = NSEventModifierFlagFunction | NSEventModifierFlagNumericPad;
    auto const ctrlCmd = NSEventModifierFlagControl | NSEventModifierFlagCommand;
    auto const ctrlOptCmd = ctrlCmd | NSEventModifierFlagOption;
    auto const qtCtrlCmd = Qt::ControlModifier | Qt::MetaModifier;
    auto const qtCtrlOptCmd = qtCtrlCmd | Qt::AltModifier;
    auto const chords = std::array {
        Chord { "Ctrl+Cmd+Down", 0x7d, NSDownArrowFunctionKey, ctrlCmd | arrows, Qt::Key_Down, qtCtrlCmd },
        Chord { "Ctrl+Cmd+Left", 0x7b, NSLeftArrowFunctionKey, ctrlCmd | arrows, Qt::Key_Left, qtCtrlCmd },
        Chord { "Ctrl+Cmd+Right", 0x7c, NSRightArrowFunctionKey, ctrlCmd | arrows, Qt::Key_Right, qtCtrlCmd },
        Chord {
            "Ctrl+Opt+Cmd+Down", 0x7d, NSDownArrowFunctionKey, ctrlOptCmd | arrows, Qt::Key_Down, qtCtrlOptCmd },
        Chord {
            "Ctrl+Opt+Cmd+Left", 0x7b, NSLeftArrowFunctionKey, ctrlOptCmd | arrows, Qt::Key_Left, qtCtrlOptCmd },
        Chord { "Ctrl+Opt+Cmd+Right",
                0x7c,
                NSRightArrowFunctionKey,
                ctrlOptCmd | arrows,
                Qt::Key_Right,
                qtCtrlOptCmd },
    };

    for (auto const& chord: chords)
    {
        INFO(chord.name);
        recorder.presses.clear();
        sendKey(nsWindow, chord.keyCode, chord.character, chord.flags);
        REQUIRE(recorder.presses.size() == 1);
        CHECK(recorder.presses.front().key == chord.qtKey);
        // The arrow keys carry the keypad modifier, which no chord names.
        CHECK((recorder.presses.front().modifiers & ~Qt::KeypadModifier) == chord.qtModifiers);
    }
}
