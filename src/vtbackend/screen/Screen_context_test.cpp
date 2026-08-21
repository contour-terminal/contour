// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/screen/Screen.hpp>
#include <vtbackend/screen/StatusLineBuilder.hpp>
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

// {{{ line association

TEST_CASE("Screen.osc3008 lines carry the context that wrote them", "[context]")
{
    auto mock = makeTerm();
    mock.writeToScreen(osc3008("start=one;type=command"));
    auto const first = mock.terminal.contexts().activeId();
    mock.writeToScreen("first\r\n");

    mock.writeToScreen(osc3008("end=one;exit=success"));
    mock.writeToScreen(osc3008("start=two;type=command"));
    auto const second = mock.terminal.contexts().activeId();
    mock.writeToScreen("second\r\n");

    auto const& screen = mock.terminal.primaryScreen();
    CHECK(screen.contextIdAt(LineOffset(0)) == first);
    CHECK(screen.contextIdAt(LineOffset(1)) == second);
    CHECK(first != second);
}

TEST_CASE("Screen.osc3008 a line written before any context carries none", "[context]")
{
    auto mock = makeTerm();
    mock.writeToScreen("before\r\n");
    mock.writeToScreen(osc3008("start=one;type=command"));
    mock.writeToScreen("after\r\n");

    auto const& screen = mock.terminal.primaryScreen();
    CHECK(!screen.contextIdAt(LineOffset(0)));
    CHECK(!!screen.contextIdAt(LineOffset(1)));
}

TEST_CASE("Screen.osc3008 a blank line inherits from the output above it", "[context]")
{
    // A run of blank lines below some output reads to a user as belonging to that output, and a line
    // the cursor never moved onto carries no stamp of its own.
    auto mock = makeTerm();
    mock.writeToScreen(osc3008("start=one;type=command"));
    auto const id = mock.terminal.contexts().activeId();
    mock.writeToScreen("output\r\n");

    auto const& screen = mock.terminal.primaryScreen();
    CHECK(screen.contextIdAt(LineOffset(0)) == id);
    CHECK(screen.contextIdAt(LineOffset(2)) == id); // never written to
}

TEST_CASE("Screen.osc3008 the stamp survives a scroll into history", "[context]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) }, LineCount(10) };
    mock.writeToScreen(osc3008("start=one;type=command"));
    auto const id = mock.terminal.contexts().activeId();
    mock.writeToScreen("scrolled\r\n");
    mock.writeToScreen("a\r\nb\r\nc\r\nd\r\n");

    REQUIRE(mock.terminal.primaryScreen().historyLineCount() > LineCount(0));
    CHECK(mock.terminal.primaryScreen().grid().lineAt(LineOffset(-3)).contextId() == id);
}

TEST_CASE("Screen.osc3008 a retired context still resolves for the lines it wrote", "[context]")
{
    // The record has to outlive end=, or the tint would vanish retroactively the moment a command
    // exits.
    auto mock = makeTerm();
    mock.writeToScreen(osc3008("start=one;type=command;cwd=/tmp"));
    auto const id = mock.terminal.contexts().activeId();
    mock.writeToScreen("output\r\n");
    mock.writeToScreen(osc3008("end=one;exit=success"));

    REQUIRE(mock.terminal.contexts().depth() == 0);
    auto const* const record = mock.terminal.contexts().find(id);
    REQUIRE(record != nullptr);
    CHECK(record->workingDirectory == "/tmp");
    CHECK(record->outcome.exit == ContextExit::Success);
}

TEST_CASE("Screen.osc3008 line association survives a reflow", "[context]")
{
    // Unlike the two column offsets, the context is NOT head-only: it names who WROTE the line, and
    // re-chopping a logical line into different physical pieces does not change the author of any
    // piece. So every physical row of a wrapped logical line must still resolve after a resize.
    auto mock = MockTerm { PageSize { LineCount(6), ColumnCount(10) }, LineCount(20) };
    mock.terminal.setMode(DECMode::TextReflow, true);

    mock.writeToScreen(osc3008("start=one;type=command"));
    auto const id = mock.terminal.contexts().activeId();
    mock.writeToScreen("aaaaaaaaaabbbbbbbbbbcccccccccc"); // three physical rows at 10 columns

    // Only rows holding part of the logical line: the row the cursor sits on is stamped too, and the
    // property under test is that no PIECE of the wrapped line loses its author.
    auto physicalRowsCarrying = [&](ContextId wanted) {
        auto count = 0;
        auto const& grid = mock.terminal.primaryScreen().grid();
        auto const top = -unbox<int>(grid.historyLineCount());
        for (auto i = top; i < unbox<int>(mock.terminal.pageSize().lines); ++i)
        {
            if (grid.lineText(LineOffset(i)).find_first_of("abc") == std::string::npos)
                continue;
            CHECK(grid.lineAt(LineOffset(i)).contextId() == wanted);
            ++count;
        }
        return count;
    };
    REQUIRE(physicalRowsCarrying(id) == 3);

    // Narrower: the logical line is re-split into more physical rows, all still that context's.
    mock.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(5) });
    CHECK(physicalRowsCarrying(id) == 6);

    // Wider again: rejoined and re-split, and still that context's.
    mock.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(15) });
    CHECK(physicalRowsCarrying(id) == 2);
}

TEST_CASE("Screen.osc3008 a reflow keeps two contexts apart", "[context]")
{
    auto mock = MockTerm { PageSize { LineCount(8), ColumnCount(10) }, LineCount(20) };
    mock.terminal.setMode(DECMode::TextReflow, true);

    mock.writeToScreen(osc3008("start=one;type=command"));
    auto const first = mock.terminal.contexts().activeId();
    mock.writeToScreen("aaaaaaaaaaaaaaa\r\n");
    mock.writeToScreen(osc3008("end=one;exit=success"));
    mock.writeToScreen(osc3008("start=two;type=command"));
    auto const second = mock.terminal.contexts().activeId();
    mock.writeToScreen("bbbbbbbbbbbbbbb\r\n");

    mock.terminal.resizeScreen(PageSize { LineCount(8), ColumnCount(5) });

    // Counting only rows that hold content: the cursor's landing row is stamped too -- correctly, since
    // it is where the next output goes -- and that is not what this case is about.
    auto const& grid = mock.terminal.primaryScreen().grid();
    auto seenFirst = 0;
    auto seenSecond = 0;
    auto const top = -unbox<int>(grid.historyLineCount());
    for (auto i = top; i < unbox<int>(mock.terminal.pageSize().lines); ++i)
    {
        auto const text = grid.lineText(LineOffset(i));
        if (text.find_first_of("ab") == std::string::npos)
            continue;
        auto const id = grid.lineAt(LineOffset(i)).contextId();
        CHECK(id == (text.front() == 'a' ? first : second));
        if (id == first)
            ++seenFirst;
        else if (id == second)
            ++seenSecond;
    }
    CHECK(seenFirst == 3);
    CHECK(seenSecond == 3);
}

// }}}

// {{{ synthesised semantic marks

namespace
{

/// The systemd prompt cycle for one command, as PROMPT_COMMAND and PS0 actually emit it.
void systemdCycle(MockTerm<>& mock, std::string_view commandId, std::string_view previousCommandId = {})
{
    // The real order, which is what makes the synthesised marks land where OSC 133's would:
    //   PROMPT_COMMAND closes the previous command, then re-announces the shell context (before the
    //   prompt is painted), the prompt is painted, the user's command line and its Enter are echoed,
    //   and only THEN does PS0 fire -- with the cursor already at the start of the output area.
    if (!previousCommandId.empty())
        mock.writeToScreen(osc3008("end=" + std::string { previousCommandId } + ";exit=success"));
    mock.writeToScreen(osc3008("start=shell-uuid;type=shell;cwd=/home/user"));
    mock.writeToScreen("$ ls\r\n");
    mock.writeToScreen(osc3008("start=" + std::string { commandId } + ";type=command;cwd=/home/user"));
    mock.writeToScreen("output\r\n");
}

/// Whether the logical line at @p line carries @p flag.
bool lineHas(MockTerm<>& mock, int line, LineFlag flag)
{
    return mock.terminal.primaryScreen().isLogicalLineFlagEnabled(LineOffset(line), flag);
}

} // namespace

TEST_CASE("Screen.osc3008 synthesises the OSC 133 marks when 133 is silent", "[context]")
{
    auto mock = makeTerm();

    // The FIRST cycle pushes the shell context, and a push marks nothing: systemd opens it when
    // profile.d is sourced, which can be arbitrarily far above the prompt. So the prompt mark appears
    // from the second cycle on, where the shell context is UPDATED.
    systemdCycle(mock, "cmd-1");
    // Line 0 is the prompt+command line; line 1 is where output begins.
    CHECK(lineHas(mock, 1, LineFlag::OutputStart)); // type=command start stands in for 133;C
    CHECK(!lineHas(mock, 0, LineFlag::Marked));     // the shell context was PUSHED, not updated

    mock.writeToScreen(osc3008("end=cmd-1;exit=success"));
    CHECK(lineHas(mock, 2, LineFlag::CommandEnd)); // where the output left the cursor -> 133;D

    // Second cycle: the shell context is re-announced, which IS the prompt boundary -> 133;A.
    systemdCycle(mock, "cmd-2");
    CHECK(lineHas(mock, 2, LineFlag::Marked));
    CHECK(lineHas(mock, 3, LineFlag::OutputStart));
}

TEST_CASE("Screen.osc3008 never synthesises PromptEnd", "[context]")
{
    // OSC 3008 has no event at the prompt/input border: its type=command context is announced from
    // PS0, after the user pressed Enter, by which time the cursor has left the prompt line. Guessing
    // would put the mark on the wrong line AND the wrong column.
    auto mock = makeTerm();
    systemdCycle(mock, "cmd-1");
    mock.writeToScreen(osc3008("end=cmd-1;exit=success"));
    systemdCycle(mock, "cmd-2");

    for (auto line = 0; line < 5; ++line)
        CHECK(!lineHas(mock, line, LineFlag::PromptEnd));
}

TEST_CASE("Screen.osc3008 stamps nothing once OSC 133 has spoken", "[context]")
{
    auto mock = makeTerm();
    mock.writeToScreen("\033]133;A\033\\"); // a shell integration is installed
    mock.writeToScreen("$ ");
    mock.writeToScreen("\033]133;B\033\\");
    mock.writeToScreen("\r\n");

    // Now the systemd stream arrives too, on a fresh line that 133 has not marked.
    mock.writeToScreen(osc3008("start=shell-uuid;type=shell"));
    mock.writeToScreen(osc3008("start=cmd-1;type=command"));

    CHECK(!lineHas(mock, 1, LineFlag::Marked));
    CHECK(!lineHas(mock, 1, LineFlag::OutputStart));
    CHECK(mock.terminal.markArbiter().owner() == MarkOwner::ShellIntegration);
}

TEST_CASE("Screen.osc3008 the initial shell context push marks nothing", "[context]")
{
    // systemd opens the shell context when profile.d is sourced, which can be arbitrarily far above
    // the first prompt -- above the motd. A mark there would land on it.
    auto mock = makeTerm();
    mock.writeToScreen(osc3008("start=shell-uuid;type=shell"));

    CHECK(!lineHas(mock, 0, LineFlag::Marked));
}

TEST_CASE("Screen.osc3008 a boundary context never synthesises a mark", "[context]")
{
    // run0's elevate and systemd-nspawn's container contexts are announced wherever the cursor happens
    // to be, so they carry no positional guarantee at all.
    auto mock = makeTerm();
    mock.writeToScreen(osc3008("start=box;type=container"));
    mock.writeToScreen(osc3008("start=root;type=elevate"));
    mock.writeToScreen(osc3008("start=ssh;type=remote"));

    CHECK(!lineHas(mock, 0, LineFlag::Marked));
    CHECK(!lineHas(mock, 0, LineFlag::OutputStart));
    CHECK(!lineHas(mock, 0, LineFlag::CommandEnd));
}

TEST_CASE("Screen.osc3008 the arbiter survives RIS and DECSTR", "[context]")
{
    // Like the ancestry, it describes the shells attached to the pty rather than the screen: a program
    // emitting RIS must not be able to flip which protocol owns the user's markers.
    auto mock = makeTerm();
    mock.writeToScreen("\033]133;A\033\\");
    REQUIRE(mock.terminal.markArbiter().owner() == MarkOwner::ShellIntegration);

    mock.writeToScreen("\033c");
    CHECK(mock.terminal.markArbiter().owner() == MarkOwner::ShellIntegration);

    mock.writeToScreen("\033[!p");
    CHECK(mock.terminal.markArbiter().owner() == MarkOwner::ShellIntegration);
}

TEST_CASE("Screen.osc3008 the first prompt race resolves to shell integration either way", "[context]")
{
    SECTION("133 first, then 3008 -- the shipped Fedora ordering")
    {
        auto mock = makeTerm();
        mock.writeToScreen("\033]133;A\033\\");
        mock.writeToScreen(osc3008("start=shell-uuid;type=shell"));
        mock.writeToScreen(osc3008("start=cmd-1;type=command"));
        mock.writeToScreen(osc3008("end=cmd-1;exit=success"));

        CHECK(mock.terminal.markArbiter().owner() == MarkOwner::ShellIntegration);
    }

    SECTION("3008 first, then 133 -- a user integration appended after systemd's")
    {
        auto mock = makeTerm();
        mock.writeToScreen(osc3008("start=shell-uuid;type=shell"));
        mock.writeToScreen(osc3008("start=cmd-1;type=command"));
        mock.writeToScreen("\033]133;A\033\\");
        mock.writeToScreen(osc3008("end=cmd-1;exit=success"));

        // 3008 stamped a flag or two before 133 spoke, which is harmless -- both land on the same
        // logical line head, and a flag is an idempotent bit. What matters is that 133 owns it now.
        CHECK(mock.terminal.markArbiter().owner() == MarkOwner::ShellIntegration);
    }
}

TEST_CASE("Screen.osc3008 synthesised marks drive folding", "[context]")
{
    // The payoff: a Fedora user with no shell integration installed gets fold ranges, mark navigation
    // and "copy last command output" for free. computeFoldRanges reads exactly the three flags OSC
    // 3008 can supply and never touches PromptEnd, so folding is fully correct on synthesised marks.
    auto mock = MockTerm { PageSize { LineCount(12), ColumnCount(40) } };
    systemdCycle(mock, "cmd-1");
    mock.writeToScreen(osc3008("end=cmd-1;exit=success"));
    systemdCycle(mock, "cmd-2");
    mock.writeToScreen(osc3008("end=cmd-2;exit=success"));
    systemdCycle(mock, "cmd-3");

    CHECK(!mock.terminal.foldRanges().empty());
}

TEST_CASE("Screen.osc3008 drives the semantic block tracker once it owns the session", "[context]")
{
    auto mock = makeTerm();
    mock.terminal.semanticBlockTracker().setEnabled(true);

    systemdCycle(mock, "cmd-1");
    mock.writeToScreen(osc3008("end=cmd-1;exit=success"));
    // The first cycle only DECIDES; the callbacks start with the second.
    systemdCycle(mock, "cmd-2");
    mock.writeToScreen(osc3008("end=cmd-2;exit=failure;status=139;signal=SIGSEGV"));

    // commandFinished() marks the CURRENT block; it moves to completedBlocks on the next prompt.
    auto const& current = mock.terminal.semanticBlockTracker().currentBlock();
    REQUIRE(current.has_value());
    CHECK(current->finished);
    // 128+11, as every POSIX shell spells a SIGSEGV death -- which OSC 133;D, carrying only a numeric
    // exit code, cannot distinguish from a command that merely returned 139.
    CHECK(current->exitCode == 139);
}

TEST_CASE("Screen.osc3008 fires no callbacks during its first, undecided cycle", "[context]")
{
    // The cost of deferring, stated plainly: with no OSC 133 anywhere, the session's FIRST command
    // block gets line flags but no tracker entry. One block, once, in a protocol that is off by
    // default -- cheaper than letting a cmdline-less 3008 command clobber a good cmdline_url.
    auto mock = makeTerm();
    mock.terminal.semanticBlockTracker().setEnabled(true);

    systemdCycle(mock, "cmd-1");
    mock.writeToScreen(osc3008("end=cmd-1;exit=success"));

    CHECK(mock.terminal.semanticBlockTracker().completedBlocks().empty());
    CHECK(mock.terminal.markArbiter().owner() == MarkOwner::ContextSignalling);
}

// }}}

// {{{ the enabled gate

TEST_CASE("Screen.osc3008 is not read at all when signalling is disabled", "[context]")
{
    // The single gate, and what it is worth: it is checked where the sequence ENTERS, so the ancestry
    // stays empty -- and with it the breadcrumb, the page tint, the derived marks and what a daemon
    // replicates, none of which needs a flag of its own. The configuration's `enabled: false`
    // promises exactly this ("nothing is tracked, nothing is derived and nothing is tinted").
    auto mock = makeTerm();
    mock.terminal.settings().contextSignalling = ContextSignalling::Disabled;

    mock.writeToScreen("\033]3008;start=abc;type=container;container=foobar\033\\");

    CHECK(mock.terminal.contexts().depth() == 0);
    CHECK(!mock.terminal.contexts().activeId());
    CHECK(mock.terminal.contexts().active() == nullptr);

    // And with nothing on the ancestry the breadcrumb collapses, rather than being separately silenced.
    CHECK(mock.terminal.contexts().chain().empty());
}

// }}}

// {{{ background tinting

namespace
{

/// Gives @p mock a scheme that tints containers and commands, with the scope the configuration would
/// supply. Not a factory, because MockTerm holds a Terminal and is therefore not movable.
void tintContainers(MockTerm<>& mock, ContextTintScope scope = ContextTintScope::Boundaries)
{
    mock.terminal.settings().contextTintScope = scope;
    auto& palette = mock.terminal.colorPalette();
    palette.contextTints[static_cast<size_t>(ContextType::Container)] = RGBColor { 0x16, 0x1c, 0x26 };
    palette.contextTints[static_cast<size_t>(ContextType::Command)] = RGBColor { 0x14, 0x14, 0x14 };
}

/// The background the renderer actually paints at @p line, column 0.
RGBColor backgroundAt(MockTerm<>& mock, int line)
{
    mock.terminal.refreshRenderBuffer(true);
    auto const buffer = mock.terminal.renderBuffer();
    for (auto const& cell: buffer.get().cells)
        if (cell.position.line == LineOffset(line) && cell.position.column == ColumnOffset(0))
            return cell.attributes.backgroundColor;
    for (auto const& row: buffer.get().lines)
        if (row.lineOffset == LineOffset(line))
            return row.fillAttributes.backgroundColor;
    return {};
}

} // namespace

TEST_CASE("Screen.osc3008 a line under a tinted context takes the tint", "[context]")
{
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(10) } };
    tintContainers(mock);
    auto const plain = mock.terminal.colorPalette().defaultBackground;

    mock.writeToScreen(osc3008("start=box;type=container"));
    mock.writeToScreen("inside\r\n");

    CHECK(backgroundAt(mock, 0) == RGBColor { 0x16, 0x1c, 0x26 });
    CHECK(backgroundAt(mock, 0) != plain);
}

TEST_CASE("Screen.osc3008 a cell the application coloured itself is left alone", "[context]")
{
    // Otherwise `ls --color` inside a container comes out repainted, and a hint about who is running
    // costs the user the output they ran it for.
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(10) } };
    tintContainers(mock);
    mock.writeToScreen(osc3008("start=box;type=container"));
    mock.writeToScreen("\033[41mred\033[m\r\n");

    // The scheme's own red, untouched: the tint stands in for the PAGE background and nothing else.
    CHECK(backgroundAt(mock, 0) == mock.terminal.colorPalette().normalColor(1));
}

TEST_CASE("Screen.osc3008 tinting off silences a scheme that sets every slot", "[context]")
{
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(10) } };
    tintContainers(mock, ContextTintScope::Off);
    auto const plain = mock.terminal.colorPalette().defaultBackground;

    mock.writeToScreen(osc3008("start=box;type=container"));
    mock.writeToScreen("inside\r\n");

    CHECK(backgroundAt(mock, 0) == plain);
}

TEST_CASE("Screen.osc3008 the boundaries scope ignores a command tint", "[context]")
{
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(10) } };
    tintContainers(mock, ContextTintScope::Boundaries);
    auto const plain = mock.terminal.colorPalette().defaultBackground;

    mock.writeToScreen(osc3008("start=cmd;type=command"));
    mock.writeToScreen("ordinary\r\n");
    CHECK(backgroundAt(mock, 0) == plain);

    // The same scheme, the same slot, but a scope that admits it.
    auto all = MockTerm { PageSize { LineCount(4), ColumnCount(10) } };
    tintContainers(all, ContextTintScope::All);
    all.writeToScreen(osc3008("start=cmd;type=command"));
    all.writeToScreen("ordinary\r\n");
    CHECK(backgroundAt(all, 0) == RGBColor { 0x14, 0x14, 0x14 });
}

TEST_CASE("Screen.osc3008 the alternate screen is never tinted", "[context]")
{
    // The equality guard would make it PATCHY there rather than uniform: a full-screen application
    // paints most of its canvas with its own background, so only the cells it left at default would
    // tint, and a striped vim reads as a rendering fault.
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(10) } };
    tintContainers(mock);
    mock.writeToScreen(osc3008("start=box;type=container"));
    mock.writeToScreen("\033[?1049h");
    mock.writeToScreen("full screen app\r\n");

    CHECK(backgroundAt(mock, 0) != RGBColor { 0x16, 0x1c, 0x26 });
}

TEST_CASE("Screen.osc3008 a tinted line stays on the batched render path", "[context]")
{
    // The performance guard. A tint is uniform over a line, so it rides RenderLine::fillAttributes;
    // demoting a trivial line to per-cell rendering because of one would be a serious regression.
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(10) } };
    tintContainers(mock);
    mock.writeToScreen(osc3008("start=box;type=container"));
    mock.writeToScreen("plain text\r\n");
    mock.terminal.refreshRenderBuffer(true);

    auto const buffer = mock.terminal.renderBuffer();
    CHECK(!buffer.get().lines.empty());
    auto const tinted = std::ranges::any_of(buffer.get().lines, [](auto const& row) {
        return row.fillAttributes.backgroundColor == RGBColor { 0x16, 0x1c, 0x26 };
    });
    CHECK(tinted);
}

TEST_CASE("Screen.osc3008 a scheme with no tints costs the render path nothing", "[context]")
{
    // hasContextTints() is false, so the per-line lookup is skipped for the overwhelmingly common case
    // of a scheme that never opted in.
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(10) } };
    CHECK(!mock.terminal.colorPalette().hasContextTints());

    mock.writeToScreen(osc3008("start=box;type=container"));
    mock.writeToScreen("inside\r\n");
    mock.terminal.refreshRenderBuffer(true);
    CHECK(!mock.terminal.renderBuffer().get().lines.empty());
}

// }}}

// {{{ the breadcrumb

namespace
{

/// The `{Context}` segment as it would be written into the indicator status line.
std::string breadcrumb(
    MockTerm<>& mock,
    StatusLineDefinitions::ContextVerbosity verbosity = StatusLineDefinitions::ContextVerbosity::Boundaries,
    ColumnCount maxWidth = ColumnCount(32))
{
    auto item = StatusLineDefinitions::Context {};
    item.verbosity = verbosity;
    item.maxWidth = maxWidth;
    auto const segment = StatusLineSegment { item };
    return serializeToVT(mock.terminal, segment, StatusLineStyling::Disabled);
}

} // namespace

TEST_CASE("Screen.osc3008 an empty ancestry renders no breadcrumb", "[context]")
{
    auto mock = makeTerm();
    CHECK(breadcrumb(mock).empty());
}

TEST_CASE("Screen.osc3008 a shell-and-command-only session renders no breadcrumb", "[context]")
{
    // The property that lets this ship in the DEFAULT status line: on a stock systemd install the
    // shim emits only session/shell/command, so an ordinary session pays nothing for the segment.
    auto mock = makeTerm();
    mock.writeToScreen(osc3008("start=session-1;type=session"));
    mock.writeToScreen(osc3008("start=shell-1;type=shell;comm=bash"));
    mock.writeToScreen(osc3008("start=cmd-1;type=command;comm=ls"));

    CHECK(breadcrumb(mock).empty());
}

TEST_CASE("Screen.osc3008 a container renders with its type prefix", "[context]")
{
    // A bare name reads like a hostname, so the type goes in front -- while a hostname and a user are
    // self-describing in position and take none.
    auto mock = makeTerm();
    mock.writeToScreen(osc3008("start=box;type=container;container=foobar"));

    CHECK(breadcrumb(mock) == "container:foobar");
}

TEST_CASE("Screen.osc3008 an elevate renders its target user without a prefix", "[context]")
{
    auto mock = makeTerm();
    mock.writeToScreen(osc3008("start=root;type=elevate;targetuser=root"));

    CHECK(breadcrumb(mock) == "root");
}

TEST_CASE("Screen.osc3008 an elevate to the session's own user renders nothing", "[context]")
{
    // run0 while already root is not news -- and suppressing it removes the commonest honest false
    // positive, so a SPOOFED one has to actually differ before it appears at all.
    auto mock = makeTerm();
    mock.writeToScreen(osc3008("start=outer;type=session;user=root"));
    mock.writeToScreen(osc3008("start=elev;type=elevate;targetuser=root"));

    CHECK(breadcrumb(mock).empty());
}

TEST_CASE("Screen.osc3008 a full ancestry reads root-first", "[context]")
{
    auto mock = makeTerm();
    mock.writeToScreen(osc3008("start=s;type=session;user=lennart;hostname=zeta"));
    mock.writeToScreen(osc3008("start=box;type=container;container=foobar"));
    mock.writeToScreen(osc3008("start=root;type=elevate;targetuser=root"));
    mock.writeToScreen(osc3008("start=app;type=app;comm=vim"));

    CHECK(breadcrumb(mock, StatusLineDefinitions::ContextVerbosity::Full, ColumnCount(80))
          == "zeta › container:foobar › root › vim");
}

TEST_CASE("Screen.osc3008 the Active verbosity shows the innermost context alone", "[context]")
{
    auto mock = makeTerm();
    mock.writeToScreen(osc3008("start=box;type=container;container=foobar"));
    mock.writeToScreen(osc3008("start=app;type=app;comm=vim"));

    CHECK(breadcrumb(mock, StatusLineDefinitions::ContextVerbosity::Active, ColumnCount(80)) == "vim");
}

TEST_CASE("Screen.osc3008 an over-long breadcrumb elides its middle and keeps both ends", "[context]")
{
    // The first segment answers *where* and the last answers *who*; the ones between are the least
    // costly to lose.
    auto mock = makeTerm();
    mock.writeToScreen(osc3008("start=s;type=session;hostname=zeta"));
    mock.writeToScreen(osc3008("start=box;type=container;container=a-rather-long-container-name"));
    mock.writeToScreen(osc3008("start=root;type=elevate;targetuser=root"));

    auto const text = breadcrumb(mock, StatusLineDefinitions::ContextVerbosity::Full, ColumnCount(20));
    CHECK(text.starts_with("zeta"));
    CHECK(text.ends_with("root"));
    CHECK(text.contains("…"));
}

TEST_CASE("Screen.osc3008 a single over-long segment is cut on a codepoint boundary", "[context]")
{
    // The last resort of the elision: the innermost segment alone, cut at its own START. Cutting one
    // BYTE at a time left the result beginning on a UTF-8 continuation byte -- mojibake on screen, and
    // an invalid sequence handed to writeToScreenInternal(), which parses it.
    auto mock = makeTerm();
    // Eight double-width CJK codepoints: 24 bytes, 16 columns, and no byte boundary that is also a
    // codepoint boundary except every third one.
    mock.writeToScreen(osc3008("start=box;type=container;container=容器容器容器容器"));

    auto const text = breadcrumb(mock, StatusLineDefinitions::ContextVerbosity::Full, ColumnCount(8));

    // Every byte of the result decodes: a lone continuation byte would round-trip to U+FFFD.
    auto const decoded = unicode::convert_to<char32_t>(std::string_view { text });
    CHECK(std::ranges::none_of(decoded, [](char32_t cp) { return cp == U'�'; }));
    CHECK(text.starts_with("…"));
    CHECK(text.ends_with("器")); // the TAIL survives: it is the part that identifies the context

    // And the ellipsis is BUDGETED for rather than added on top, so the whole thing fits.
    auto columns = 0;
    for (auto const codepoint: decoded)
        columns += static_cast<int>(unicode::width(codepoint));
    CHECK(columns <= 8);
}

TEST_CASE("Screen.osc3008 control bytes in a context field never reach the status screen", "[context]")
{
    // Defence in depth: this string is handed to writeToScreenInternal(), which PARSES it. The
    // decoder already rejects control bytes, so one arriving here would mean a parser bug -- and
    // without the strip, that bug would be an injection able to clear the status screen from a field
    // any program on the tty can write.
    auto mock = makeTerm();
    mock.writeToScreen(osc3008("start=box;type=container;container=safe"));

    // Reach past the sequence decoder, as a parser bug would.
    auto record = *mock.terminal.contexts().active();
    record.container = "ev\033[2Jil";
    mock.terminal.adoptContext(record);

    // The ESC is what makes the rest a SEQUENCE; strip it and "[2J" is inert text on the status line.
    auto const text = breadcrumb(mock);
    CHECK(!text.contains('\033'));
    CHECK(text == "container:ev[2Jil");
}

TEST_CASE("Screen.osc3008 the breadcrumb round-trips through the status-line serializer", "[context]")
{
    auto item = StatusLineDefinitions::Context {};
    item.verbosity = StatusLineDefinitions::ContextVerbosity::Full;
    item.separator = " | ";
    item.maxWidth = ColumnCount(40);

    auto const written = serializeStatusLineSegment(StatusLineSegment { item });
    auto const parsed = parseStatusLineSegment(written);

    REQUIRE(parsed.size() == 1);
    auto const* const back = std::get_if<StatusLineDefinitions::Context>(&parsed.front());
    REQUIRE(back != nullptr);
    CHECK(back->verbosity == StatusLineDefinitions::ContextVerbosity::Full);
    CHECK(back->separator == " | ");
    CHECK(back->maxWidth == ColumnCount(40));
}

TEST_CASE("Screen.osc3008 a default breadcrumb writes no redundant attributes", "[context]")
{
    // A config Contour wrote must not gain noise when it writes it again.
    auto const written = serializeStatusLineSegment(StatusLineSegment { StatusLineDefinitions::Context {} });
    CHECK(written == "{Context}");
}

// }}}
