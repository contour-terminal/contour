// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/screen/Screen.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/vt/Functions.hpp>

#include <libunicode/convert.h>
#include <libunicode/width.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>

using namespace vtbackend;
using namespace std::string_view_literals;

namespace
{

/// Wraps @p body in `OSC 3008 ; ... ST`, exactly as an application would emit it.
std::string osc3008(std::string_view body)
{
    return "\033]3008;" + std::string { body } + "\033\\";
}

/// A MockTerm with a page big enough for the traffic these cases push through it.
MockTerm<> makeTerm()
{
    return MockTerm { PageSize { LineCount(6), ColumnCount(40) } };
}

} // namespace

// {{{ registration

TEST_CASE("Functions.OSC3008", "[context]")
{
    auto const availableSequences = SupportedSequences {};
    auto const* const function = selectOSCommand(3008, availableSequences.activeSequences());
    REQUIRE(function != nullptr);
    CHECK(*function == HIERCONTEXT);
    CHECK(function->documentation.mnemonic == "HIERCONTEXT");
}

// }}}
// {{{ dispatch

TEST_CASE("Screen.osc3008 start establishes an active context", "[context]")
{
    auto mock = makeTerm();
    mock.writeToScreen(osc3008("start=shell-a;type=shell;cwd=/home/user"));

    auto const& contexts = mock.terminal.contexts();
    REQUIRE(contexts.active() != nullptr);
    CHECK(contexts.active()->identifier == "shell-a");
    CHECK(contexts.active()->type == ContextType::Shell);
    CHECK(contexts.active()->workingDirectory == "/home/user");
    CHECK(contexts.depth() == 1);
}

TEST_CASE("Screen.osc3008 end pops the context", "[context]")
{
    auto mock = makeTerm();
    mock.writeToScreen(osc3008("start=shell-a;type=shell"));
    mock.writeToScreen(osc3008("start=cmd-1;type=command"));
    REQUIRE(mock.terminal.contexts().depth() == 2);

    mock.writeToScreen(osc3008("end=cmd-1;exit=failure;status=139;signal=SIGSEGV"));

    auto const& contexts = mock.terminal.contexts();
    CHECK(contexts.depth() == 1);
    CHECK(contexts.active()->identifier == "shell-a");
}

TEST_CASE("Screen.osc3008 a malformed payload leaves the stack untouched", "[context]")
{
    auto mock = makeTerm();
    mock.writeToScreen(osc3008("start=shell-a;type=shell"));
    auto const before = mock.terminal.contexts().revision();

    mock.writeToScreen(osc3008("garbage"));
    mock.writeToScreen(osc3008("start="));
    mock.writeToScreen(osc3008("start=" + std::string(65, 'x')));

    CHECK(mock.terminal.contexts().revision() == before);
    CHECK(mock.terminal.contexts().depth() == 1);
}

TEST_CASE("Screen.osc3008 replies nothing", "[context]")
{
    // The protocol has no query form. A stray reply would be written to the shell's stdin as if the
    // user had typed it.
    auto mock = makeTerm();
    mock.writeToScreen(osc3008("start=shell-a;type=shell;cwd=/tmp"));
    mock.writeToScreen(osc3008("end=shell-a;exit=success"));
    mock.writeToScreen(osc3008("nonsense"));

    CHECK(mock.terminal.peekInput().empty());
}

TEST_CASE("Screen.osc3008 an empty payload is rejected without disturbing the stack", "[context]")
{
    auto mock = makeTerm();
    mock.writeToScreen(osc3008("start=shell-a;type=shell"));

    mock.writeToScreen("\033]3008\033\\");

    CHECK(mock.terminal.contexts().depth() == 1);
}

// }}}
// {{{ reset immunity -- the property most likely to break silently

TEST_CASE("Screen.osc3008 survives a hard reset", "[context]")
{
    // UAPI.15 makes this a SAFETY property, not a convenience: "the usual terminal reset sequences
    // should not affect the stack of contexts ... a program down the stack should not be able to
    // affect the stack further up, possibly hiding relevant information".
    auto mock = makeTerm();
    mock.writeToScreen(osc3008("start=box;type=container;container=foobar"));
    mock.writeToScreen(osc3008("start=shell-a;type=shell"));
    REQUIRE(mock.terminal.contexts().depth() == 2);

    mock.writeToScreen("\033c"); // RIS

    auto const& contexts = mock.terminal.contexts();
    CHECK(contexts.depth() == 2);
    CHECK(contexts.chain().front().record->type == ContextType::Container);
    CHECK(contexts.active()->identifier == "shell-a");
}

TEST_CASE("Screen.osc3008 survives a soft reset", "[context]")
{
    auto mock = makeTerm();
    mock.writeToScreen(osc3008("start=box;type=container"));
    REQUIRE(mock.terminal.contexts().depth() == 1);

    mock.writeToScreen("\033[!p"); // DECSTR

    CHECK(mock.terminal.contexts().depth() == 1);
    CHECK(mock.terminal.contexts().active()->type == ContextType::Container);
}

TEST_CASE("Screen.osc3008 a hard reset clears the grid but not the ancestry", "[context]")
{
    // Pinning both halves together, because the bug this guards against is a future "tidy" of
    // hardReset() that adds the stack to the list of things it clears.
    auto mock = makeTerm();
    mock.writeToScreen(osc3008("start=box;type=container"));
    mock.writeToScreen("some output");
    REQUIRE(mock.terminal.primaryScreen().grid().lineText(LineOffset(0)).starts_with("some output"));

    mock.writeToScreen("\033c");

    CHECK(!mock.terminal.primaryScreen().grid().lineText(LineOffset(0)).starts_with("some output"));
    CHECK(mock.terminal.contexts().depth() == 1);
}

// }}}
// {{{ screen and page independence

TEST_CASE("Screen.osc3008 survives a screen switch", "[context]")
{
    // The ancestry describes the APPLICATION's process tree: vim running in a container is still in
    // that container while the alternate screen is up.
    auto mock = makeTerm();
    mock.writeToScreen(osc3008("start=box;type=container"));

    mock.writeToScreen("\033[?1049h"); // to the alternate screen
    CHECK(mock.terminal.contexts().depth() == 1);

    mock.writeToScreen("\033[?1049l"); // and back
    CHECK(mock.terminal.contexts().depth() == 1);
    CHECK(mock.terminal.contexts().active()->type == ContextType::Container);
}

TEST_CASE("Screen.osc3008 a context started on the alternate screen is still there afterwards", "[context]")
{
    auto mock = makeTerm();
    mock.writeToScreen("\033[?1049h");
    mock.writeToScreen(osc3008("start=app;type=app"));
    mock.writeToScreen("\033[?1049l");

    CHECK(mock.terminal.contexts().depth() == 1);
    CHECK(mock.terminal.contexts().active()->type == ContextType::App);
}

// }}}
// {{{ notification gating

TEST_CASE("Screen.osc3008 a context change notifies once", "[context]")
{
    auto mock = makeTerm();
    auto const announcement = "start=shell-a;type=shell;cwd=/home/user;pid=1234"sv;

    mock.writeToScreen(osc3008(announcement));
    auto const afterFirst = mock.terminal.contexts().revision();

    // systemd re-announces the shell context on EVERY prompt with byte-identical metadata. Reporting a
    // change for that would redraw the frontend once per prompt for nothing.
    for (auto i = 0; i < 10; ++i)
        mock.writeToScreen(osc3008(announcement));

    CHECK(mock.terminal.contexts().revision() == afterFirst);
    CHECK(mock.terminal.contexts().depth() == 1);
    CHECK(mock.terminal.contexts().retainedCount() == 1);
}

// }}}
// {{{ the real traffic

TEST_CASE("Screen.osc3008 the systemd prompt cycle replays", "[context]")
{
    // Three commands through the cycle /etc/profile.d/80-systemd-osc-context.sh actually emits, with
    // real output in between, driven through the real parser.
    auto mock = makeTerm();
    auto const common = ";machineid=3deb5353d3ba43d08201c136a47ead7b;user=lennart;hostname=zeta"
                        ";bootid=d4a3d0fdf2e24fdea6d971ce73f4fbf2;pid=1062862;cwd=/home/lennart"sv;

    for (auto const& [index, command]:
         std::vector<std::pair<int, std::string_view>> { { 0, "cmd-1" }, { 1, "cmd-2" }, { 2, "cmd-3" } })
    {
        (void) index;
        // PROMPT_COMMAND: announce (or re-announce) the shell context.
        mock.writeToScreen(osc3008("start=shell-uuid;type=shell" + std::string { common }));
        mock.writeToScreen("$ ");

        // PS0: push the command context.
        mock.writeToScreen(
            osc3008("start=" + std::string { command } + ";type=command" + std::string { common }));
        CHECK(mock.terminal.contexts().depth() == 2);
        mock.writeToScreen("output\r\n");

        // The next PROMPT_COMMAND closes it.
        mock.writeToScreen(osc3008("end=" + std::string { command } + ";exit=success"));
        CHECK(mock.terminal.contexts().depth() == 1);
    }

    auto const& contexts = mock.terminal.contexts();
    CHECK(contexts.active()->identifier == "shell-uuid");
    CHECK(contexts.retainedCount() == 4); // one shell + three commands
    CHECK(mock.terminal.peekInput().empty());
}

TEST_CASE("Screen.osc3008 the effective working directory walks up the ancestry", "[context]")
{
    auto mock = makeTerm();
    mock.writeToScreen(osc3008("start=shell-a;type=shell;cwd=/home/user"
                               ";machineid=3deb5353d3ba43d08201c136a47ead7b"));
    // A command context with no cwd= of its own, which is what systemd sends when nothing changed.
    mock.writeToScreen(osc3008("start=cmd-1;type=command"));

    auto const self = LocalIdentity { .machineId = "3deb5353d3ba43d08201c136a47ead7b", .hostname = "zeta" };
    auto const cwd = mock.terminal.contexts().effectiveWorkingDirectory(self);

    REQUIRE(cwd.has_value());
    CHECK(cwd->path == "/home/user");
    CHECK(cwd->locality == ContextLocality::Local);
}

// }}}
