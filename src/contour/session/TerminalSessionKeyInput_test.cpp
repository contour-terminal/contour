// SPDX-License-Identifier: Apache-2.0
//
// The session's half of the key pipeline: a normalized KeyEvent, routed and executed -- the bindings, the
// repeat filter and an ended pane's footer, ahead of anything reaching the PTY.

#include <contour/config/KeyboardPlatformKind.hpp>
#include <contour/config/OptionAsAlt.hpp>
#include <contour/input/KeyboardLayout.hpp>
#include <contour/input/KeyboardPlatform.hpp>
#include <contour/session/EndReason.hpp>
#include <contour/session/KeyCharacterizationRows.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/GuiTestFixtures.hpp>

#include <vtbackend/screen/Terminal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <tuple>

namespace
{
using contour::input::NativeKeyEvent;
using contour::session::KeyDelivery;
using namespace contour::test::keybits;

/// Delivers @p event to @p session the way the display does: through the session's platform, then routed.
KeyDelivery deliver(contour::session::TerminalSession& session, NativeKeyEvent const& event)
{
    return session.sendKeyboardEvent(session.keyboardPlatform().normalize(event),
                                     std::chrono::steady_clock::now());
}
} // namespace

TEST_CASE("a repeat of a bound non-repeatable chord sends nothing", "[session][input]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    contour::test::TestApp app { std::move(factoryOwned) };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };
    auto* session = manager.createSession(window.id);
    REQUIRE(session != nullptr);
    auto& pty = contour::test::mockPtyOf(*session);

    auto const newTab = NativeKeyEvent { .qtKey = 'T', .text = U"\x14", .qtModifiers = Ctrl | Shift };
    auto repeat = newTab;
    repeat.type = vtbackend::KeyboardEventType::Repeat;

    // The press is the positive control: the chord is bound, and opens a tab.
    auto const ptysBefore = factory->createdPtys.size();
    pty.stdinBuffer().clear();
    CHECK(deliver(*session, newTab) == KeyDelivery::Handled);
    REQUIRE(factory->createdPtys.size() == ptysBefore + 1);
    CHECK(pty.stdinBuffer().empty());

    // Its repeat runs nothing and reaches nothing: the binding owns the chord, so ^T must not leak out.
    CHECK(deliver(*session, repeat) == KeyDelivery::Handled);
    CHECK(factory->createdPtys.size() == ptysBefore + 1);
    CHECK(pty.stdinBuffer().empty());

    contour::test::closeAllTabs(*window);
}

TEST_CASE("an ended pane runs bindings and restarts on bare Return only", "[session][input]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    contour::test::TestApp app { std::move(factoryOwned) };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };
    auto* ended = manager.createSession(window.id);
    REQUIRE(ended != nullptr);
    auto const endedId = ended->modelSessionId();
    auto& pty = contour::test::mockPtyOf(*ended);

    ended->terminal().device().close();
    ended->onClosed();
    REQUIRE(ended->getSessionEnd() == static_cast<int>(contour::session::EndReason::Exited));
    auto const ptysBefore = factory->createdPtys.size();
    pty.stdinBuffer().clear();

    // The bindings still run: Ctrl+Shift+T opens a tab -- a new session, while the ended one stays put.
    // Shift+Return is a chord the user may bind, not a request to restart.
    CHECK(deliver(*ended, { .qtKey = 'T', .text = U"\x14", .qtModifiers = Ctrl | Shift })
          == KeyDelivery::Handled);
    CHECK(factory->createdPtys.size() == ptysBefore + 1);
    CHECK(deliver(*ended, { .qtKey = KeyReturn, .text = U"\r", .qtModifiers = Shift })
          == KeyDelivery::Handled);
    CHECK(factory->createdPtys.size() == ptysBefore + 1);
    CHECK(manager.sessionForId(endedId) == ended);
    CHECK(ended->getSessionEnd() == static_cast<int>(contour::session::EndReason::Exited));
    CHECK(pty.stdinBuffer().empty());

    // A bare Return is.
    CHECK(deliver(*ended, { .qtKey = KeyReturn, .text = U"\r" }) == KeyDelivery::Handled);
    CHECK(factory->createdPtys.size() == ptysBefore + 2);
    CHECK(manager.sessionForId(endedId) == nullptr);

    contour::test::closeAllTabs(*window);
}

TEST_CASE("an event the platform cannot translate is left for Qt", "[session][input]")
{
    contour::test::TestApp app;
    auto session = contour::test::makeTestSession(
        &app.app().sessionsManager(),
        std::make_unique<vtpty::MockPty>(vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) }),
        app.app());
    auto& pty = contour::test::mockPtyOf(*session);
    pty.stdinBuffer().clear();
    CHECK(deliver(*session, { .qtKey = 0x01ffffff }) == KeyDelivery::Unhandled);
    CHECK(pty.stdinBuffer().empty());
}

TEST_CASE("reloading option_as_alt replaces the session's keyboard platform", "[session][input][mac]")
{
    contour::test::TestApp app;
    app.app().config().profile().optionAsAlt.value() = contour::config::OptionAsAlt::Off;
    auto const layout = contour::input::makeMacUsAnsiKeyboardLayout();
    auto session = contour::test::makeTestSession(
        &app.app().sessionsManager(),
        std::make_unique<vtpty::MockPty>(vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) }),
        app.app(),
        contour::input::makeKeyboardPlatformFactory(contour::config::KeyboardPlatformKind::Mac, *layout));
    auto& pty = contour::test::mockPtyOf(*session);
    auto const optionE = NativeKeyEvent { .qtKey = 'E',
                                          .text = U"\u00e9",
                                          .qtModifiers = Alt,
                                          .nativeModifiers = MacLeftOption,
                                          .nativeVirtualKey = 0x0e };

    pty.stdinBuffer().clear();
    std::ignore = deliver(*session, optionE);
    CHECK(pty.stdinBuffer() == "\xc3\xa9");

    auto reloaded = app.app().config();
    reloaded.profile(session->profileName())->optionAsAlt.value() = contour::config::OptionAsAlt::Both;
    session->applyReloadedConfig(reloaded, session->profileName());

    pty.stdinBuffer().clear();
    std::ignore = deliver(*session, optionE);
    CHECK(pty.stdinBuffer()
          == "\x1b"
             "e");
}
