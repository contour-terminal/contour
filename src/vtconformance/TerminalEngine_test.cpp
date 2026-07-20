// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <utility>

#include <vtconformance/TerminalEngine.h>

using namespace std::string_literals;
using namespace std::string_view_literals;

namespace
{

/// An engine driven by a `MockPty`, with the mock kept reachable so a test can see both sides.
///
/// The whole point of injecting the device: a real engine, a real parser, a real input generator —
/// and no child process, no PTY, no thread, and no clock. Every test below is a pure function of the
/// bytes it feeds in.
struct MockedEngine
{
    vtpty::MockPty* pty {};
    std::unique_ptr<vtconformance::TerminalEngine> engine;

    explicit MockedEngine(vtconformance::TerminalEngine::Options options = {})
    {
        auto mock = std::make_unique<vtpty::MockPty>(options.pageSize);
        pty = mock.get();
        engine = std::make_unique<vtconformance::TerminalEngine>(std::move(mock), options);
    }

    /// @return What the terminal has written back to the application so far.
    [[nodiscard]] std::string const& wire() const noexcept { return pty->stdinBuffer(); }
};

} // namespace

TEST_CASE("TerminalEngine.renders bytes written to the screen", "[vtconformance]")
{
    auto mocked = MockedEngine {};
    mocked.engine->writeToScreen("hello engine"sv);

    CHECK(mocked.engine->screenText().contains("hello engine"));
}

TEST_CASE("TerminalEngine.a query reply reaches the wire", "[vtconformance]")
{
    // The load-bearing contract. `Terminal::reply()` only QUEUES a reply into the input generator; a
    // frontend has to push it onto the wire. If the engine forgets to, every query in every suite comes
    // back unanswered and the conformance report blames the engine for a defect in its own driver.
    auto mocked = MockedEngine {};
    mocked.engine->writeToScreen("\033[c"sv); // DA1

    // Contour identifies as a VT525, so DA1 must lead with `?65`.
    INFO(mocked.wire());
    CHECK(mocked.wire().contains("[?65;"));
}

TEST_CASE("TerminalEngine.Return honours New Line mode", "[vtconformance]")
{
    // This is what vttest's chapter 6 actually asks, and why the driver must press a KEY rather than
    // type bytes: `tst_NLM` (reports.c:585-627) sets LNM, asks for RETURN and demands CR LF, then
    // resets LNM and demands CR alone. The answer is a property of the terminal's key encoding
    // (InputGenerator.cpp:241) -- a driver that wrote a literal would only be testing its own literal.

    SECTION("set: LNM makes Return send CR LF")
    {
        auto mocked = MockedEngine {};
        mocked.engine->writeToScreen("\033[20h"sv); // SM 20 -- LNM on
        mocked.engine->pressKey(vtbackend::Key::Enter);

        CHECK(mocked.wire() == "\r\n"s);
    }

    SECTION("reset: without LNM, Return sends CR alone")
    {
        auto mocked = MockedEngine {};
        mocked.engine->writeToScreen("\033[20l"sv); // RM 20 -- LNM off
        mocked.engine->pressKey(vtbackend::Key::Enter);

        CHECK(mocked.wire() == "\r"s);
    }
}

TEST_CASE("TerminalEngine.Backspace honours Backarrow Key Mode", "[vtconformance]")
{
    // The same shape as LNM, and vttest's chapter 11.3.4 asks it the same way ("Press the backspace
    // key", vt420.c): DECBKM decides what the key SENDS, so only a key press can answer it.
    // InputGenerator.cpp:83-90 -- reset (the default) sends DEL, set sends BS.

    SECTION("reset: Backspace sends DEL")
    {
        auto mocked = MockedEngine {};
        mocked.engine->writeToScreen("\033[?67l"sv); // DECRST 67 -- DECBKM off
        mocked.engine->pressKey(vtbackend::Key::Backspace);

        CHECK(mocked.wire() == "\x7f"s);
    }

    SECTION("set: Backspace sends BS")
    {
        auto mocked = MockedEngine {};
        mocked.engine->writeToScreen("\033[?67h"sv); // DECSET 67 -- DECBKM on
        mocked.engine->pressKey(vtbackend::Key::Backspace);

        CHECK(mocked.wire() == "\x08"s);
    }
}

TEST_CASE("TerminalEngine.pressing a key does not consult a clock", "[vtconformance]")
{
    // The engine must be a pure function of its input, so its timestamps are synthetic. Pressing the
    // same key twice from the same state must therefore produce the same bytes, both times.
    auto mocked = MockedEngine {};
    mocked.engine->pressKey(vtbackend::Key::Enter);
    mocked.engine->pressKey(vtbackend::Key::Enter);

    CHECK(mocked.wire() == "\r\r"s);
}

TEST_CASE("TerminalEngine.writeInput reaches the wire verbatim", "[vtconformance]")
{
    auto mocked = MockedEngine {};
    mocked.engine->writeInput("3\n"sv);

    CHECK(mocked.wire() == "3\n"s);
}

TEST_CASE("TerminalEngine.a resize request reaches the device", "[vtconformance]")
{
    // DECCOLM asks for 132 columns; the grid must follow, and so must the device -- a child that asks
    // its terminal how wide it is has to be told the truth. This is the path that used to need a real
    // child to exercise at all.
    auto mocked = MockedEngine {};
    REQUIRE(mocked.pty->pageSize().columns == vtpty::ColumnCount(80));

    // DECCOLM only obeys once 80/132 switching is allowed, as in xterm -- and vttest knows it, sending
    // `SM ?40` at startup with the comment "Enable 80/132 switch (xterm)" (main.c:1486). Without this
    // line the terminal is right to stay at 80.
    mocked.engine->writeToScreen("\033[?40h"sv); // DECSET 40 -- allow 80/132
    mocked.engine->writeToScreen("\033[?3h"sv);  // DECCOLM -- 132 columns

    CHECK(mocked.pty->pageSize().columns == vtpty::ColumnCount(132));
}

TEST_CASE("TerminalEngine.OSC 52 round-trips through the clipboard", "[vtconformance]")
{
    // The suites write the clipboard and read it straight back, so a headless engine has to model one.
    auto mocked = MockedEngine {};
    mocked.engine->writeToScreen("\033]52;c;aGVsbG8=\033\\"sv); // set clipboard to "hello"
    mocked.engine->writeToScreen("\033]52;c;?\033\\"sv);        // read it back

    INFO(mocked.wire());
    CHECK(mocked.wire().contains("aGVsbG8="));
}

TEST_CASE("TerminalEngine.collects a diagnostic for an unknown sequence", "[vtconformance]")
{
    // Oracle A: whatever the parser could not make sense of. ECMA-48 leaves 0x5B..0x5F unassigned as
    // CSI final bytes, so this one can never become implemented and quietly invalidate the test.
    auto mocked = MockedEngine {};
    mocked.engine->writeToScreen("\033[_"sv);

    CHECK(!mocked.engine->diagnostics().empty());
}
