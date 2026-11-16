// SPDX-License-Identifier: Apache-2.0
#include <core/Environment.hpp>
#include <core/tui/SgrBuilder.hpp>
#include <core/tui/TerminalOutput.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace
{

/// Which kind of stream the test's output pretends to write to.
enum class Destination : std::uint8_t
{
    File,    ///< Not a terminal: no colours, no progress, no synchronised output.
    Terminal ///< A terminal.
};

/// A @c TerminalOutput whose destination is a string this test can read back.
///
/// It overrides only what says where the bytes go, which is the whole point of the two
/// seams under test here: everything else -- the escape-sequence composition, the
/// synchronised-output guard -- must reach this capture unchanged.
class CapturingOutput: public core::tui::TerminalOutput
{
  public:
    explicit CapturingOutput(Destination destination) noexcept: _destination(destination) {}

    [[nodiscard]] bool isTerminal() const noexcept override { return _destination == Destination::Terminal; }

    /// @return Every byte written to the destination so far, in order.
    [[nodiscard]] std::string const& captured() const noexcept { return _captured; }

  protected:
    void writeToDestination(std::string_view bytes) override { _captured.append(bytes); }

  private:
    std::string _captured;
    Destination _destination;
};

} // namespace

TEST_CASE("tui.TerminalOutput: SyncGuard writes through writeToDestination")
{
    auto output = CapturingOutput { Destination::Terminal };
    output.writeRaw("before");

    {
        auto const guard = output.syncGuard();
        // The guard flushes what was buffered before it opens the mode, so the begin sequence
        // brackets everything written from here on and nothing that came before it.
        CHECK(output.captured() == "before\033[?2026h");

        output.writeRaw("inside");
        output.flush();
    }

    // The end sequence goes to the same destination, after the guarded bytes. A guard that wrote
    // to the process's standard output instead would leave the capture at "before" plus "inside"
    // and put both sequences on someone else's screen.
    CHECK(output.captured() == "before\033[?2026hinside\033[?2026l");
}

TEST_CASE("tui.TerminalOutput: a directly constructed SyncGuard flushes what came before it")
{
    auto output = CapturingOutput { Destination::Terminal };
    output.writeRaw("before"); // Composed, not yet flushed.

    {
        // The natural RAII spelling, and the one that used to get this wrong: while the flush on
        // the way in belonged to syncGuard() rather than to the constructor, "before" was still in
        // the buffer here and reached the terminal INSIDE the synchronized region -- a frame
        // carrying bytes composed before it, which is the tearing the class exists to prevent.
        auto const guard = core::tui::SyncGuard { output };
        CHECK(output.captured() == "before\033[?2026h");

        output.writeRaw("inside");
    }

    CHECK(output.captured() == "before\033[?2026hinside\033[?2026l");
}

TEST_CASE("tui.TerminalOutput: ~SyncGuard flushes before it ends the synchronized region")
{
    auto output = CapturingOutput { Destination::Terminal };

    {
        auto const guard = output.syncGuard();
        output.writeRaw("inside"); // Composed, not yet flushed.
    }

    // The point of the region is that everything composed inside it is applied at once. A
    // destructor that wrote the end sequence without flushing first left "inside" in the buffer,
    // so it reached the terminal after `CSI ?2026l` -- outside the very region it was composed in.
    // `Screen::flush()`'s applyCursorShape() is the live case: it composes after its last flush.
    CHECK(output.captured() == "\033[?2026hinside\033[?2026l");
}

TEST_CASE("tui.TerminalOutput: move-assignment flushes the region it ends")
{
    auto first = CapturingOutput { Destination::Terminal };
    auto second = CapturingOutput { Destination::Terminal };

    auto guard = first.syncGuard();
    first.writeRaw("inside");

    // Two different outputs, so that the right-hand side's own flush cannot be what empties
    // `first`: ending `first`'s region is the assignment's job, and so is flushing it.
    guard = second.syncGuard();
    CHECK(first.captured() == "\033[?2026hinside\033[?2026l");
}

TEST_CASE("tui.TerminalOutput: a moved-from SyncGuard leaves the mode to its successor")
{
    auto output = CapturingOutput { Destination::Terminal };

    {
        auto first = output.syncGuard();
        auto const second = std::move(first);
        CHECK(output.captured() == "\033[?2026h");
    }

    // Exactly one end sequence: the moved-from guard must not write one of its own. This is also
    // the only seam a guard holding no output has -- a default-constructed one has nowhere to
    // write at all, so no case can distinguish it from any other implementation of `~SyncGuard()`;
    // what the null check is actually for is the moved-from state exercised here.
    CHECK(output.captured() == "\033[?2026h\033[?2026l");
}

TEST_CASE("tui.TerminalOutput: syncGuard() on a destination that is not a terminal writes no markers")
{
    // Synchronized output is a property of where the bytes go, like colour: a pipe or a file gets
    // the frame and not `CSI ? 2026 h` / `l` around it. Every caller used to have to ask
    // isTerminal() first and choose between a guard and none; the guard asks instead.
    auto output = CapturingOutput { Destination::File };
    output.writeRaw("before");

    {
        auto const guard = output.syncGuard();
        // It still flushes on the way in, so what was composed before the region is written
        // before it, exactly as on a terminal -- only the begin sequence is missing.
        CHECK(output.captured() == "before");
        output.writeRaw("inside");
    }

    // And on the way out: a caller that composed inside the region and relied on the guard to
    // flush it gets the bytes on a pipe too.
    CHECK(output.captured() == "beforeinside");
}

TEST_CASE("tui.TerminalOutput: a directly constructed SyncGuard asks the destination too")
{
    // The same answer by either spelling, so no caller can get the markers onto a pipe by
    // constructing the guard itself.
    auto output = CapturingOutput { Destination::File };

    {
        auto const guard = core::tui::SyncGuard { output };
        output.writeRaw("inside");
    }

    CHECK(output.captured() == "inside");
}

TEST_CASE("tui.TerminalOutput: move-assigning a non-terminal guard ends nothing it did not begin")
{
    auto file = CapturingOutput { Destination::File };
    auto terminal = CapturingOutput { Destination::Terminal };

    auto guard = file.syncGuard();
    file.writeRaw("inside");
    guard = terminal.syncGuard();

    // The file's region is flushed as it is ended, and gets no end sequence it never had a begin
    // for; the terminal's region has begun.
    CHECK(file.captured() == "inside");
    CHECK(terminal.captured() == "\033[?2026h");
}

TEST_CASE("tui.TerminalOutput: isTerminal() reflects the destination")
{
    auto file = CapturingOutput { Destination::File };
    auto terminal = CapturingOutput { Destination::Terminal };

    // Through the base, which is how a renderer holds it: the answer is the destination's, not
    // the process's standard output.
    core::tui::TerminalOutput const& fileRef = file;
    core::tui::TerminalOutput const& terminalRef = terminal;
    CHECK_FALSE(fileRef.isTerminal());
    CHECK(terminalRef.isTerminal());
}

TEST_CASE("tui.TerminalOutput: the default destination's isTerminal() asks the operating system")
{
    // ctest runs a test binary with its output on a pipe, and sets this in the test's environment.
    // Without it the binary was started some other way and standard output may well be a terminal,
    // which is exactly what this case cannot then distinguish from a constant `true`.
    if (!core::defaultEnvironment().get("CTEST_INTERACTIVE_DEBUG_MODE"))
        SKIP("not run by ctest, so standard output is not known to be a pipe");

    auto const output = core::tui::TerminalOutput {};
    CHECK_FALSE(output.isTerminal());
}

TEST_CASE("tui.TerminalOutput: copyToClipboard emits OSC 52 with the base64 of the text")
{
    auto output = CapturingOutput { Destination::File };

    // "man" is three bytes, so it encodes without padding; "ma" and "m" take one and two '='.
    output.copyToClipboard("man");
    output.copyToClipboard("ma");
    output.copyToClipboard("m");
    output.flush();

    CHECK(output.captured() == "\033]52;c;bWFu\033\\\033]52;c;bWE=\033\\\033]52;c;bQ==\033\\");
}

TEST_CASE("tui.TerminalOutput: a bare SGR reset is public, and is the one writeText ends a style with")
{
    // dbtool had no way to ask for it and hardcoded "\033[0m". buildSgrReset() is the string and
    // resetStyle() the verb, and both must be what writeText itself writes after styled text --
    // a second spelling of the same reset is how the two drift apart.
    auto styled = CapturingOutput { Destination::Terminal };
    auto style = core::tui::Style {};
    style.bold = true;
    styled.writeText("x", style);
    styled.flush();

    CHECK(core::tui::buildSgrReset() == "\033[m");
    CHECK(styled.captured().ends_with(core::tui::buildSgrReset()));

    auto reset = CapturingOutput { Destination::Terminal };
    reset.resetStyle();
    reset.flush();
    CHECK(reset.captured() == core::tui::buildSgrReset());
}

TEST_CASE("tui.TerminalOutput: deleteLines writes DL with its count")
{
    auto output = CapturingOutput { Destination::Terminal };
    output.deleteLines(3);
    output.deleteLines(0); // nothing to delete, nothing written
    output.flush();
    CHECK(output.captured() == "\033[3M");
}
