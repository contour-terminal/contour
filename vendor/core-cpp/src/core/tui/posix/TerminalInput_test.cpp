// SPDX-License-Identifier: Apache-2.0
//
// The bytes the POSIX TerminalInput writes when it enables its protocols, when the mouse tracking
// mode changes while they are enabled, and when it disables them, read back from a pipe that stands
// in for standard output. TerminalInput writes to STDOUT_FILENO and reads STDIN_FILENO and has no
// other seam for either, so each case swaps the two descriptors for the length of the run, as
// TerminalHangup_test.cpp does for standard input. Standard input is /dev/null: raw mode is then
// refused by the kernel, which TerminalInput ignores, and nothing is read.

#include <core/tui/MouseTracking.hpp>
#include <core/tui/TerminalInput.hpp>
#include <core/tui/TerminalProtocols.hpp>
#include <core/tui/posix/StandardStreamRedirect.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

using core::tui::MouseTracking;
using core::tui::TerminalInput;
using core::tui::test::StandardStreamRedirect;
namespace protocols = core::tui::protocols;

namespace
{

/// A non-blocking pipe, closed on destruction.
class ReadablePipe
{
  public:
    ReadablePipe() noexcept
    {
        if (::pipe(_ends.data()) != 0)
            return;
        if (auto const flags = ::fcntl(_ends[0], F_GETFL, 0); flags != -1)
            ::fcntl(_ends[0], F_SETFL, flags | O_NONBLOCK);
    }

    ~ReadablePipe()
    {
        for (auto const end: _ends)
            if (end >= 0)
                ::close(end);
    }

    ReadablePipe(ReadablePipe const&) = delete;
    ReadablePipe& operator=(ReadablePipe const&) = delete;
    ReadablePipe(ReadablePipe&&) = delete;
    ReadablePipe& operator=(ReadablePipe&&) = delete;

    [[nodiscard]] int writeEnd() const noexcept { return _ends[1]; }

    /// Everything written to the pipe since the last call.
    [[nodiscard]] std::string take() const
    {
        auto bytes = std::string {};
        auto chunk = std::array<char, 512> {};
        auto count = ::read(_ends[0], chunk.data(), chunk.size());
        while (count > 0)
        {
            bytes.append(chunk.data(), static_cast<std::size_t>(count));
            count = ::read(_ends[0], chunk.data(), chunk.size());
        }
        return bytes;
    }

  private:
    std::array<int, 2> _ends { -1, -1 };
};

/// What one TerminalInput wrote: when it enabled its protocols, during a change made while they were
/// enabled, and when it disabled them.
struct ProtocolRun
{
    bool isRedirected = false;
    bool isInitialized = false;
    std::string enabled;
    std::string changed;
    std::string disabled;
};

/// Runs a TerminalInput that asks for @p requested from initialize() to shutdown(), calling @p change
/// in between, with standard output in a pipe and standard input from /dev/null. The checks happen
/// after this returns, once standard output is the test runner's again.
template <typename Change>
[[nodiscard]] ProtocolRun runProtocols(MouseTracking requested, Change change)
{
    auto run = ProtocolRun {};
    auto const out = ReadablePipe {};
    auto const devNull = ::open("/dev/null", O_RDONLY);
    if (devNull < 0)
        return run;

    // Anything the test runner buffered goes to the real output first, not into the pipe.
    std::cout.flush();
    std::fflush(stdout);
    {
        auto const stdoutRedirect = StandardStreamRedirect { STDOUT_FILENO, out.writeEnd() };
        auto const stdinRedirect = StandardStreamRedirect { STDIN_FILENO, devNull };
        run.isRedirected = stdoutRedirect.isRedirected() && stdinRedirect.isRedirected();
        if (run.isRedirected)
        {
            auto input = TerminalInput {};
            input.setMouseTracking(requested);
            run.isInitialized = input.initialize().has_value();
            run.enabled = out.take();
            change(input);
            run.changed = out.take();
            input.shutdown();
            run.disabled = out.take();
        }
    }
    ::close(devNull);
    return run;
}

/// One mode and the set and reset it is written with.
struct ModeBytes
{
    MouseTracking mode;
    std::string_view set;
    std::string_view reset;
};

} // namespace

TEST_CASE("TerminalInput.posix.mouse_tracking_off_writes_no_mouse_mode", "[tui]")
{
    auto const run = runProtocols(MouseTracking::Off, [](TerminalInput&) {});
    REQUIRE(run.isRedirected);
    REQUIRE(run.isInitialized);

    // The run saw enableProtocols(): Contour's passive mode is enabled as it always was.
    CHECK(run.enabled.contains(protocols::EnablePassiveMouseTracking));
    for (auto const* const mode: { "1000", "1002", "1003", "1006" })
    {
        CAPTURE(mode);
        CHECK_FALSE(run.enabled.contains(std::string("\033[?") + mode));
        CHECK_FALSE(run.disabled.contains(std::string("\033[?") + mode));
    }
}

TEST_CASE("TerminalInput.posix.a_mouse_tracking_mode_is_set_with_sgr_and_reset_in_reverse", "[tui]")
{
    auto const expected = GENERATE(values<ModeBytes>({
        { MouseTracking::Buttons, "\033[?1000h", "\033[?1000l" },
        { MouseTracking::Drag, "\033[?1002h", "\033[?1002l" },
        { MouseTracking::AnyMotion, "\033[?1003h", "\033[?1003l" },
    }));
    CAPTURE(expected.mode);
    auto const run = runProtocols(expected.mode, [](TerminalInput&) {});
    REQUIRE(run.isRedirected);
    REQUIRE(run.isInitialized);

    CHECK(run.enabled.contains(std::string(expected.set) + std::string(protocols::EnableSGRMouse)));
    CHECK(run.disabled.contains(std::string(protocols::DisableSGRMouse) + std::string(expected.reset)));
    for (auto const other: { protocols::EnableButtonTracking,
                             protocols::EnableDragTracking,
                             protocols::EnableAnyMotionTracking })
        if (other != expected.set)
            CHECK_FALSE(run.enabled.contains(other));
    CHECK(run.changed.empty());
}

TEST_CASE("TerminalInput.posix.a_mode_change_while_enabled_writes_only_the_transition", "[tui]")
{
    auto const run = runProtocols(MouseTracking::Buttons,
                                  [](TerminalInput& input) { input.setMouseTracking(MouseTracking::Drag); });
    REQUIRE(run.isRedirected);
    REQUIRE(run.isInitialized);
    CHECK(run.changed == "\033[?1000l\033[?1002h");
    CHECK(run.disabled.contains("\033[?1006l\033[?1002l"));
}

TEST_CASE("TerminalInput.posix.hover_raises_the_requested_mode_to_any_motion", "[tui]")
{
    auto raised = MouseTracking::Off;
    auto const run = runProtocols(MouseTracking::Drag, [&raised](TerminalInput& input) {
        input.setAnyMotionTracking(true);
        raised = input.mouseTracking();
    });
    REQUIRE(run.isRedirected);
    REQUIRE(run.isInitialized);
    CHECK(raised == MouseTracking::AnyMotion);
    CHECK(run.changed == "\033[?1002l\033[?1003h");
    CHECK(run.disabled.contains("\033[?1006l\033[?1003l"));
}

TEST_CASE("TerminalInput.posix.hover_from_off_sets_any_motion_with_sgr_and_lowering_resets_both", "[tui]")
{
    auto const run = runProtocols(MouseTracking::Off, [](TerminalInput& input) {
        input.setAnyMotionTracking(true);
        input.setAnyMotionTracking(false);
    });
    REQUIRE(run.isRedirected);
    REQUIRE(run.isInitialized);

    // Raising writes the set and then SGR encoding; lowering writes the reset of both in reverse.
    CHECK(run.changed == "\033[?1003h\033[?1006h\033[?1006l\033[?1003l");
    // Back at Off, so shutdown writes no mouse mode of its own.
    CHECK_FALSE(run.disabled.contains("\033[?1003"));
    CHECK_FALSE(run.disabled.contains("\033[?1006"));
}
