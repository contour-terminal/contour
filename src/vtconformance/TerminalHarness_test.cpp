// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <string>
#include <string_view>

#include <vtconformance/TerminalHarness.h>

using namespace std::chrono_literals;
using namespace std::string_literals;
using namespace std::string_view_literals;
using namespace vtconformance;

/// Skips a case that needs a POSIX shell driven over a pseudo-terminal.
///
/// Every case in this file spawns `/bin/sh` and talks to it through a real PTY, which Windows has
/// neither of. They are skipped rather than compiled out: a test that does not exist reads exactly
/// like a test that passed, and this file is where the harness's process half is measured.
#if defined(_WIN32)
    #define SKIP_WITHOUT_POSIX_SHELL() SKIP("needs a POSIX shell over a PTY, which Windows has not")
#else
    #define SKIP_WITHOUT_POSIX_SHELL() ((void) 0)
#endif

namespace
{
/// `Process` prepends argv[0] itself, so `arguments` carries only what follows the program name.
[[nodiscard]] vtpty::Process::ExecInfo shell(std::string script)
{
    return vtpty::Process::ExecInfo {
        .program = "/bin/sh",
        .arguments = { "-c", std::move(script) },
        .workingDirectory = ".",
        .env = { { "TERM", "xterm-256color" }, { "LC_ALL", "C" } },
    };
}

/// Generous, because it is never *waited out* -- every barrier below is the child's own doing (it
/// exits, or it announces itself), so this only bounds a genuine wedge.
constexpr auto Patience = 30s;
} // namespace

// The barrier in every test here is causal: the child exits, or it says something. None waits out a
// fixed silence -- a test that fails when the code under it gets slower was measuring the wrong thing.
//
// It does NOT fix these four under `clang-coverage`, and the roadmap used to claim it would. They still
// die there, with SIGILL, and the cause is the fork rather than the clock: every test in this module
// that spawns no child passes under instrumentation (TerminalEngine's 8, and the 23 pure ones), and
// every one that forks dies. The fork path carries the parent's gcov state through login(), chdir(),
// closeAllFileDescriptorsAbove() and execvp.

TEST_CASE("TerminalHarness.drives a child and renders its output", "[vtconformance]")
{
    SKIP_WITHOUT_POSIX_SHELL();
    auto harness = TerminalHarness { shell(R"(printf 'hello harness')"s), {} };
    harness.start();

    REQUIRE(harness.waitForExit(Patience));
    CHECK(harness.screenText().find("hello harness") != std::string::npos);

    harness.stop();
}

TEST_CASE("TerminalHarness.answers a device-attributes query", "[vtconformance]")
{
    SKIP_WITHOUT_POSIX_SHELL();
    // The load-bearing contract of the whole harness. `Terminal::reply()` only QUEUES a reply into
    // the input generator; a frontend has to push it onto the wire. If the harness forgets to, every
    // query in every suite comes back unanswered and the conformance report blames the engine for a
    // defect in its own driver.
    //
    // This pins the reply's CONTENT: the bytes reach the slave, whose line discipline echoes them
    // back onto the screen. That the child can also *consume* them is pinned separately, below,
    // because a canonical-mode read cannot.
    //
    // Which is also why the barrier here cannot be the child's exit: a DA1 reply carries no newline,
    // so this child's canonical `read` never returns and it never exits, by design. The echo arriving
    // on the wire is the event worth waiting for, so wait for exactly that.
    //
    // Contour identifies as a VT525, so DA1 must lead with `?65`.
    constexpr auto Markers = std::array { "[?65;"sv };

    auto harness =
        TerminalHarness { shell(R"(printf '\033[c'; read -r reply; printf 'REPLY=%s' "$reply")"s), {} };
    harness.start(TerminalHarness::Pumping::ByTheCaller);
    harness.startScanning(Markers);

    auto const deadline = std::chrono::steady_clock::now() + Patience;
    auto echoed = false;
    while (!echoed && std::chrono::steady_clock::now() < deadline)
    {
        auto const batch = harness.readBatch();
        REQUIRE(!batch.closed);
        harness.feed(batch.bytes);
        echoed = !batch.matches.empty();
    }

    INFO(harness.screenText());
    CHECK(echoed);

    harness.stop();
}

TEST_CASE("TerminalHarness.writeInput reaches the child's stdin", "[vtconformance]")
{
    SKIP_WITHOUT_POSIX_SHELL();
    // The child announces readiness before reading. Writing to the master before the child has
    // called exec and configured its tty risks the pending input being flushed away by tcsetattr --
    // is exactly why the real driver only ever types in response to a prompt.
    //
    // So the announcement IS the barrier, and this is the driver's own mechanism in miniature: scan
    // the byte stream for the marker, and answer the moment it arrives.
    constexpr auto Markers = std::array { "READY"sv };

    auto harness = TerminalHarness { shell(R"(printf 'READY'; read -r line; printf 'GOT:%s' "$line")"s), {} };
    harness.start(TerminalHarness::Pumping::ByTheCaller);
    harness.startScanning(Markers);

    auto announced = false;
    while (!announced)
    {
        auto const batch = harness.readBatch();
        REQUIRE(!batch.closed);
        harness.feed(batch.bytes);
        announced = !batch.matches.empty();
    }

    harness.writeInput("ping\n");

    // Drain what the child says next; it exits once it has echoed our line back.
    while (true)
    {
        auto const batch = harness.readBatch();
        harness.feed(batch.bytes);
        if (batch.closed)
            break;
    }

    CHECK(harness.screenText().find("GOT:ping") != std::string::npos);

    harness.stop();
}

TEST_CASE("TerminalHarness.answers a query that follows a charset designation", "[vtconformance]")
{
    SKIP_WITHOUT_POSIX_SHELL();
    // This is vttest's literal startup: it selects the default character set and then immediately
    // asks for primary device attributes, blocking until the answer arrives. If `ESC % @` were to
    // derail the parser, the DA1 behind it would be swallowed and vttest would hang forever with a
    // blank screen — which is precisely the failure this harness was built to catch.
    //
    // A DA1 reply carries no newline, so a canonical-mode read would never return it. Real VT
    // applications — vttest included — therefore put the tty into raw mode before querying, and the
    // probe has to do the same or it would be testing the shell's line discipline, not the terminal.
    auto harness = TerminalHarness { shell(R"(stty -icanon -echo min 1 time 0
                 printf '\033%%@\033[0c'
                 dd bs=1 count=1 >/dev/null 2>&1
                 stty icanon echo
                 printf 'CONSUMED')"s),
                                     {} };
    harness.start();

    REQUIRE(harness.waitForExit(Patience));

    // The child's read() actually returned, so the reply reached its stdin — not merely the tty's
    // echo. This is the full round trip: child -> pty -> engine -> reply -> pty -> child.
    auto const text = harness.screenText();
    INFO(text);
    CHECK(text.find("CONSUMED") != std::string::npos);

    harness.stop();
}
