// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/links/LinkTypes.hpp>
#include <vtbackend/links/PathResolution.hpp>
#include <vtbackend/render/RenderBuffer.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <core/Escape.hpp>
#include <core/platform/FileSystem.hpp>
#include <core/platform/FileUri.hpp>
#include <core/platform/PathUtils.hpp>

#include <libunicode/convert.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <latch>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace vtbackend::test
{

constexpr auto CapsLockOnly = LockKeys { LockKey::CapsLock };
constexpr auto NumLockOnly = LockKeys { LockKey::NumLock };
constexpr auto BothLocks = LockKeys { LockKey::CapsLock } | LockKey::NumLock;

/// Every combination of latched lock keys, including none at all.
///
/// Keyboard input must encode identically across all of these, except in the three places that
/// report the lock state on purpose: the Kitty keyboard protocol, Win32 input mode, and the numpad
/// under application-keypad mode.
constexpr auto LockCombinations = std::array { LockKeys {}, CapsLockOnly, NumLockOnly, BothLocks };

constexpr LineOffset operator""_lineOffset(unsigned long long value) noexcept
{
    return LineOffset::cast_from(value);
}

constexpr ColumnOffset operator""_columnOffset(unsigned long long value) noexcept
{
    return ColumnOffset::cast_from(value);
}

constexpr CellLocation operator+(LineOffset line, ColumnOffset column) noexcept
{
    return { .line = line, .column = column };
}

template <typename S>
[[nodiscard]] inline decltype(auto) e(S const& s)
{
    return core::escape(s);
}

/// Takes a textual screenshot using the terminals render buffer.
[[nodiscard]] inline std::vector<std::string> textScreenshot(vtbackend::Terminal const& terminal)
{
    vtbackend::RenderBufferRef const renderBuffer = terminal.renderBuffer();

    std::vector<std::string> lines;
    lines.resize(terminal.pageSize().lines.as<size_t>());

    vtbackend::CellLocation lastPos = {};
    size_t lastCount = 0;
    for (vtbackend::RenderCell const& cell: renderBuffer.get().cells)
    {
        auto const gap = (cell.position.column + static_cast<int>(lastCount) - 1) - lastPos.column;
        auto& currentLine = lines.at(unbox<size_t>(cell.position.line));
        if (*gap > 0) // Did we jump?
            currentLine.insert(currentLine.end(), unbox<size_t>(gap) - 1, ' ');

        currentLine += unicode::convert_to<char>(std::u32string_view(cell.codepoints));
        lastPos = cell.position;
        lastCount = 1;
    }
    for (vtbackend::RenderLine const& line: renderBuffer.get().lines)
    {
        auto& currentLine = lines.at(unbox<size_t>(line.lineOffset));
        currentLine = unicode::convert_to<char>(std::u32string_view(line.text));
    }

    return lines;
}

[[nodiscard]] inline std::string trimRight(std::string text)
{
    constexpr auto Whitespaces = std::string_view("\x20\t\r\n");
    while (!text.empty() && Whitespaces.contains(text.back()))
        text.resize(text.size() - 1);
    return text;
}

[[nodiscard]] inline std::string join(std::vector<std::string> const& lines)
{
    std::string output;
    for (std::string const& line: lines)
    {
        output += trimRight(line);
        output += '\n';
    }
    return output;
}

template <typename T = vtpty::MockPty>
[[nodiscard]] std::string trimmedTextScreenshot(MockTerm<T> const& mt)
{
    return trimRight(join(textScreenshot(mt.terminal)));
}

/// A fresh prompt as a shell with OSC 133 integration draws one: the prompt start and a "$ " prompt.
constexpr auto PromptStart = std::string_view { "\033]133;A\033\\$ " };

/// What a shell with OSC 133 integration writes as the user types at a fresh prompt: PromptStart, the
/// prompt end, what the user typed, and the Enter that runs it -- everything up to, not including, the ;C.
/// @param typed What the user typed at the prompt.
/// @return The bytes.
[[nodiscard]] inline std::string typedAtPrompt(std::string_view typed)
{
    return std::format("{}\033]133;B\033\\{}\r\n", PromptStart, typed);
}

/// The command end a precmd hook emits at the top of the NEXT prompt.
/// @param exitCode The exit code it reports.
/// @return The OSC 133;D.
[[nodiscard]] inline std::string commandEnd(int exitCode)
{
    return std::format("\033]133;D;{}\033\\", exitCode);
}

/// What a shell with OSC 133 integration writes as it starts one command: typedAtPrompt(), then the output
/// start. The command runs until a commandEnd() follows.
/// @param command The command line typed at the prompt.
/// @return The bytes.
[[nodiscard]] inline std::string commandStarted(std::string_view command)
{
    return typedAtPrompt(command) + "\033]133;C\033\\";
}

/// What a shell with OSC 133 integration writes as it runs one command: commandStarted(), the output
/// itself, and commandEnd().
/// @param command The command line typed at the prompt.
/// @param output The lines the command prints, each one ended by a newline.
/// @param exitCode The exit code the command end reports.
/// @return The bytes.
[[nodiscard]] inline std::string commandRun(std::string_view command,
                                            std::vector<std::string> const& output,
                                            int exitCode = 0)
{
    auto bytes = commandStarted(command);
    for (auto const& line: output)
        bytes += line + "\r\n";
    return bytes + commandEnd(exitCode);
}

/// The output start as kitty's shell integration writes it, naming the command line.
/// @param commandLine The command line as `cmdline_url` carries it, percent-encoded.
/// @return The OSC 133;C.
[[nodiscard]] inline std::string reportedOutputStart(std::string_view commandLine)
{
    return std::format("\033]133;C;cmdline_url={}\033\\", commandLine);
}

/// A command started as kitty's shell integration reports one: PromptStart, then the output start naming
/// the command line -- no prompt end, so none is recovered from the screen -- and the Enter.
/// @param commandLine The command line as `cmdline_url` carries it, percent-encoded.
/// @return The bytes.
[[nodiscard]] inline std::string reportedCommandStart(std::string_view commandLine)
{
    return std::format("{}{}\r\n", PromptStart, reportedOutputStart(commandLine));
}

/// Types at a fresh prompt: writes typedAtPrompt() to @p mt.
/// @param mt The terminal the shell writes to.
/// @param typed What the user typed at the prompt.
template <typename T = vtpty::MockPty>
void typeAtPrompt(MockTerm<T>& mt, std::string_view typed)
{
    mt.writeToScreen(typedAtPrompt(typed));
}

/// Starts one command and leaves it running: writes commandStarted() to @p mt.
/// @param mt The terminal the shell writes to.
/// @param command The command line typed at the prompt.
template <typename T = vtpty::MockPty>
void startCommand(MockTerm<T>& mt, std::string_view command)
{
    mt.writeToScreen(commandStarted(command));
}

/// Runs one command: writes commandRun() to @p mt.
/// @param mt The terminal the shell writes to.
/// @param command The command line typed at the prompt.
/// @param output The lines the command prints, each one ended by a newline.
/// @param exitCode The exit code the command end reports.
template <typename T = vtpty::MockPty>
void runCommand(MockTerm<T>& mt,
                std::string_view command,
                std::vector<std::string> const& output,
                int exitCode = 0)
{
    mt.writeToScreen(commandRun(command, output, exitCode));
}

/// Prints @p count output lines `<prefix> 0`, `<prefix> 1`, ... to @p mt in a single write, as a command
/// printing a long log does.
/// @param mt The terminal the command writes to.
/// @param prefix What each line starts with.
/// @param count How many lines to print.
template <typename T = vtpty::MockPty>
void printLines(MockTerm<T>& mt, std::string_view prefix, int count)
{
    auto text = std::string {};
    for (auto const i: std::views::iota(0, count))
        text += std::format("{} {}\r\n", prefix, i);
    mt.writeToScreen(text);
}

/// Moves the pointer over @p column of row @p line, as hovering would.
/// @param mt     The terminal.
/// @param line   The screen row.
/// @param column The column.
/// @param pixels Where that is in device pixels, from the grid's top left.
template <typename T = vtpty::MockPty>
void pointAt(MockTerm<T>& mt, int line, int column, PixelCoordinate pixels = {})
{
    mt.terminal.sendMouseMoveEvent(Modifier::None,
                                   CellLocation { .line = LineOffset(line), .column = ColumnOffset(column) },
                                   pixels,
                                   /*uiHandledHint=*/false);
}

/// Presses @p button over @p column of row @p line.
/// @param mt     The terminal.
/// @param button The button.
/// @param line   The screen row.
/// @param column The column.
/// @param pixels Where that is in device pixels, from the grid's top left.
template <typename T = vtpty::MockPty>
void pressAt(MockTerm<T>& mt, MouseButton button, int line, int column, PixelCoordinate pixels = {})
{
    (void) mt.terminal.sendMousePressEvent(
        Modifier::None,
        button,
        CellLocation { .line = LineOffset(line), .column = ColumnOffset(column) },
        pixels,
        /*uiHandledHint=*/false);
}

/// Releases @p button wherever the pointer is.
/// @param mt     The terminal.
/// @param button The button.
template <typename T = vtpty::MockPty>
void release(MockTerm<T>& mt, MouseButton button)
{
    (void) mt.terminal.sendMouseReleaseEvent(
        Modifier::None, button, PixelCoordinate {}, /*uiHandledHint=*/false);
}

/// Moves the cursor to page 3 with DECPCCM reset, so page 0, the primary screen, stays on display: the page
/// on display and the cursor's part ways. The viewport, the window onto the page on display, still scrolls.
/// @param mt The terminal, its cursor on page 0.
template <typename T = vtpty::MockPty>
void moveCursorOffTheDisplayedPage(MockTerm<T>& mt)
{
    mt.writeToScreen("\033[?64l");
    mt.terminal.setPage(PageIndex(3), PageCursorPlacement::Home);
    REQUIRE(mt.terminal.isAlternateScreen());
    REQUIRE(mt.terminal.foldingAppliesToDisplayedPage());
}

/// Puts page 3 on display, then brings the cursor back to page 0 with DECPCCM reset: the cursor writes to the
/// primary screen while a page with no history is on display, so the viewport, the window onto it, may not
/// scroll.
/// @param mt The terminal, its cursor on page 0.
template <typename T = vtpty::MockPty>
void moveDisplayOffTheCursorsPage(MockTerm<T>& mt)
{
    mt.writeToScreen("\033[?64h");
    mt.terminal.setPage(PageIndex(3), PageCursorPlacement::Home);
    mt.writeToScreen("\033[?64l");
    mt.terminal.setPage(PageIndex(0), PageCursorPlacement::PagesOwn);
    REQUIRE(mt.terminal.isPrimaryScreen());
    REQUIRE_FALSE(mt.terminal.foldingAppliesToDisplayedPage());
}

/// Collapses the one fold @p mt has, through the fold state as a click would leave it.
/// @param mt The terminal; exactly one fold must exist.
template <typename T = vtpty::MockPty>
void collapseOnlyFold(MockTerm<T>& mt)
{
    auto const ranges = mt.terminal.foldRanges();
    REQUIRE(ranges.size() == 1);
    mt.terminal.foldState().collapse(ranges[0].headStableId);
}

/// A freshly filled render buffer, selection included.
/// @param terminal The terminal to render.
/// @return What a frame of it holds.
[[nodiscard]] inline RenderBuffer renderOf(Terminal& terminal)
{
    auto buffer = RenderBuffer {};
    terminal.fillRenderBuffer(buffer, /*includeSelection*/ true);
    return buffer;
}

/// The background @p buffer paints at @p location: its cell's, or the fill of the row it lies on.
/// @param buffer A rendered frame.
/// @param location The screen row and column, as RenderCell::position counts them.
/// @return The background; default-constructed when the frame has neither a cell nor a row there.
[[nodiscard]] inline RGBColor backgroundAt(RenderBuffer const& buffer, CellLocation location)
{
    for (auto const& cell: buffer.cells)
        if (cell.position == location)
            return cell.attributes.backgroundColor;
    for (auto const& row: buffer.lines)
        if (row.lineOffset == location.line)
            return row.fillAttributes.backgroundColor;
    return {};
}

/// Draws a frame the way the render thread gets one -- filled into the back buffer, then swapped to the
/// front -- so Terminal::stickyHeaderBlock() answers for it, as it does for a frame on screen. The terminal's
/// lock is taken here, so a terminal another thread writes to is drawn safely too.
/// @param terminal The terminal to draw.
/// @return A copy of the frame now in front.
[[nodiscard]] inline RenderBuffer drawFrame(Terminal& terminal)
{
    REQUIRE(terminal.refreshRenderBuffer(/*locked=*/false));
    auto const front = terminal.renderBuffer();
    return front.get();
}

/// The gutter as text, one string per screen row that has any cell: character c of a row is the cell at
/// column c - totalColumns, and a space where there is none.
/// @param buffer A rendered frame.
/// @param totalColumns The gutter's width.
/// @return The rows that have a gutter cell, by screen row.
[[nodiscard]] inline std::map<int, std::u32string> gutterRows(RenderBuffer const& buffer, int totalColumns)
{
    auto rows = std::map<int, std::u32string> {};
    for (auto const& cell: buffer.gutter)
    {
        auto& row = rows[unbox<int>(cell.lineOffset)];
        row.resize(static_cast<size_t>(totalColumns), U' ');
        auto const index = unbox<int>(cell.column) + totalColumns;
        row.at(static_cast<size_t>(index)) = cell.codepoint;
    }
    return rows;
}

/// A command-block record as a mirror adopts it from its host: host id @p id in @p state, running `make`
/// as the application reported it, its prompt 1'700'000'000 s after the Unix epoch and its command --
/// unless it never left the prompt -- 2 s later.
///
/// Built by assignment: CommandBlockRecord has members without default member initializers, so a
/// designated initializer naming a subset would warn.
/// @param id The host's block id.
/// @param state The block's state.
/// @return The record.
[[nodiscard]] inline CommandBlockRecord mirroredRecord(uint32_t id, CommandBlockState state)
{
    auto record = CommandBlockRecord {};
    record.id = CommandBlockId { id };
    record.state = state;
    record.commandLine = CommandLineText { "make" };
    record.commandLineSource = CommandLineSource::Reported;
    record.promptStartedAt = std::chrono::system_clock::time_point { std::chrono::seconds { 1'700'000'000 } };
    if (state != CommandBlockState::Prompting)
        record.commandStartedAt = *record.promptStartedAt + std::chrono::seconds { 2 };
    return record;
}

/// mirroredRecord() for block @p id, running in @p directory as the shell reported it, judged @p locality.
/// @param id The host's block id.
/// @param directory The directory the command ran in; empty when none was reported.
/// @param locality Whether that directory is on this machine.
/// @return The record.
[[nodiscard]] inline CommandBlockRecord ranIn(uint32_t id,
                                              std::string_view directory,
                                              ContextLocality locality)
{
    auto record = mirroredRecord(id, CommandBlockState::Running);
    record.workingDirectory =
        WorkingDirectorySnapshot { .path = std::string { directory }, .locality = locality };
    return record;
}

/// mirroredRecord() for block @p id once its command finished, 3 s after it started.
/// @param id The host's block id.
/// @param exitCode The exit code the host recorded, if any.
/// @param end What closed it: OSC 133;D (Reported) or a superseding prompt (Implicit).
/// @return The record.
[[nodiscard]] inline CommandBlockRecord mirroredFinish(uint32_t id,
                                                       std::optional<int> exitCode,
                                                       CommandBlockEnd end = CommandBlockEnd::Reported)
{
    auto record = mirroredRecord(id, CommandBlockState::Finished);
    record.end = end;
    record.exitCode = exitCode;
    record.duration = std::chrono::seconds { 3 };
    return record;
}

/// @param store A command-block store.
/// @return The ids of its records, oldest first.
[[nodiscard]] inline std::vector<uint32_t> recordIds(CommandBlockStore const& store)
{
    auto ids = std::vector<uint32_t> {};
    store.forEachRecord([&](CommandBlockRecord const& record) { ids.push_back(record.id.value); });
    return ids;
}

/// The DECDMAC that defines macro @p id as @p body, hex-encoded (Pen=1): only that encoding carries an ESC,
/// which in a plain body would end the control string instead.
/// @param id The macro's id.
/// @param body What the macro is to send, control functions included.
/// @return The DCS that defines it.
[[nodiscard]] inline std::string macroDefinition(int id, std::string_view body)
{
    auto hex = std::string {};
    for (auto const ch: body)
        hex += std::format("{:02X}", static_cast<unsigned char>(ch));
    return std::format("\033P{};0;1!z{}\033\\", id, hex);
}

[[nodiscard]] inline std::string mainPageText(Screen const& screen)
{
    return screen.renderMainPageText();
}

/// @return Every line of @p screen, scrollback first and then the page, one text line each.
[[nodiscard]] inline std::string screenText(Screen const& screen)
{
    auto text = std::string {};
    auto const top = -unbox<int>(screen.historyLineCount());
    auto const bottom = unbox<int>(screen.pageSize().lines);
    for (auto const line: std::views::iota(top, bottom))
        text += screen.grid().lineText(LineOffset(line)) + '\n';
    return text;
}

inline void logScreenTextAlways(Screen const& screen, std::string const& headline = "")
{
    std::cout << std::format("{}: ZI={} cursor={} HM={}..{}\n",
                             headline.empty() ? "screen dump" : headline,
                             screen.grid().zeroIndex(),
                             screen.realCursorPosition(),
                             screen.margin().horizontal.from,
                             screen.margin().horizontal.to);
    std::cout << std::format("{}\n", dumpGrid(screen.grid()));
}

template <typename T = vtpty::MockPty>
void logScreenTextAlways(MockTerm<T> const& mock, std::string const& headline = "")
{
    logScreenTextAlways(mock.terminal.primaryScreen(), headline);
}

inline void logScreenText(Screen const& screen, std::string const& headline = "")
{
    if (headline.empty())
        UNSCOPED_INFO("dump:");
    else
        UNSCOPED_INFO(headline + ":");

    for (auto const line: std::views::iota(0, *screen.pageSize().lines))
        UNSCOPED_INFO(std::format("[{}] \"{}\"", line, screen.grid().lineText(LineOffset::cast_from(line))));
}

/// The text of @p part of the block @p target names, as the block actions copy it.
/// @param mt The terminal.
/// @param target Which block.
/// @param part Which part of it.
/// @param pinned When non-zero, this block regardless of @p target.
/// @return The plain text, or "<error>" when there is none.
template <typename T = vtpty::MockPty>
[[nodiscard]] std::string textOf(MockTerm<T>& mt,
                                 CommandBlockTarget target,
                                 CommandBlockPart part,
                                 CommandBlockId pinned = {})
{
    return mt.terminal.commandBlockText(target, part, CaptureRendition::PlainText, pinned)
        .value_or("<error>");
}

/// The cells the terminal renders on screen row @p row, by column.
/// @param mock The terminal; its render buffer is refreshed first.
/// @param row The screen row.
/// @return The row's render cells, keyed by column.
template <typename T = vtpty::MockPty>
[[nodiscard]] std::map<int, RenderCell> renderedRow(MockTerm<T>& mock, LineOffset row)
{
    mock.terminal.refreshRenderBuffer();
    auto const buffer = mock.terminal.renderBuffer();
    auto cells = std::map<int, RenderCell> {};
    for (auto const& cell: buffer.get().cells)
        if (cell.position.line == row)
            cells.emplace(unbox<int>(cell.position.column), cell);
    return cells;
}

/// @return @p directory as OSC 7 reports it.
[[nodiscard]] inline std::string fileUrlOf(std::filesystem::path const& directory)
{
    return core::platform::fileUri(core::platform::normalizePath(directory), "");
}

/// What the follow gesture finds under the pointer (Terminal::lookUpLinkAt()). Takes the terminal's lock.
/// @param terminal The terminal; the pointer is on its grid.
/// @return The lookup; an empty one when the pointer is on no cell.
[[nodiscard]] inline LinkLookup lookUpLinkUnderPointer(Terminal& terminal)
{
    auto const _ = std::scoped_lock { terminal };
    auto const position = terminal.currentMouseGridPosition();
    return position ? terminal.lookUpLinkAt(*position) : LinkLookup {};
}

/// The path the follow gesture finds under the pointer, as printed, whether or not it names a file anywhere.
/// @param terminal The terminal; the pointer is on its grid.
/// @return The path; nullopt where no path is under the pointer.
[[nodiscard]] inline std::optional<std::string> printedPathUnderPointer(Terminal& terminal)
{
    return lookUpLinkUnderPointer(terminal).printedPath.transform(
        [](LinkTarget const& printed) { return printed.target; });
}

/// The local path the follow gesture opens under the pointer: what it knows already, or what @p fileSystem
/// answers for the path it hands the probe, as the probe thread would look it up.
/// @param terminal The terminal; the pointer is on its grid.
/// @param fileSystem Where the probe looks.
/// @return The path; nullopt where no path is under the pointer or it names no file; the refusal where it
///         names a file on another machine.
[[nodiscard]] inline std::expected<std::optional<std::string>, PathRefusal> followedPathUnderPointer(
    Terminal& terminal, core::platform::FileSystem const& fileSystem)
{
    auto const lookup = lookUpLinkUnderPointer(terminal);
    if (lookup.refusal)
        return std::unexpected(*lookup.refusal);
    if (lookup.target)
        return lookup.target->kind == LinkKind::Path ? std::optional { lookup.target->target } : std::nullopt;
    if (!lookup.pathToProbe || !lookup.printedPath)
        return std::nullopt;
    auto const found = resolveLocalPath(fileSystem, lookup.pathToProbe->candidates);
    if (!found)
        return std::nullopt;
    return resolvedPathTarget(*lookup.printedPath, *found, lookup.pathDirectories).target;
}

inline void logScreenText(vtbackend::Terminal const& terminal, std::string const& headline = "")
{
    logScreenText(terminal.primaryScreen(), headline);
}

/// Pauses a thread that takes the terminal's lock round after round while another thread waits for it, as a
/// parser thread waiting on the PTY pauses, or a render thread waiting for the next frame: it sleeps after
/// every eighth round and yields after the others. Neither Windows' lock nor valgrind, which runs one thread
/// at a time, is fair: a thread that took the lock straight back kept the other waiting through thousands of
/// rounds, and under valgrind for good.
/// @param rounds How many rounds the thread has run.
inline void pauseAfter(size_t rounds)
{
    auto constexpr RoundsPerPause = 8U;
    if (rounds % RoundsPerPause == 0)
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    else
        std::this_thread::yield();
}

/// Runs @p round over and over on a thread of its own until that thread is stopped, as a parser thread writes
/// output, and pauses after each one as pauseAfter() does.
/// @param round One round of writing.
/// @return The writing thread. Destroying it stops and joins it.
[[nodiscard]] inline std::jthread writeOnOwnThread(std::function<void()> round)
{
    return std::jthread { [round = std::move(round)](std::stop_token const& stop) {
        for (auto const rounds: std::views::iota(size_t { 1 }))
        {
            if (stop.stop_requested())
                break;
            round();
            pauseAfter(rounds);
        }
    } };
}

/// Runs @p step over and over while writeOnOwnThread() runs @p write, as a key or a frame comes while the
/// parser thread parses: from the end of the writer's first round until it has written 64 rounds. A count of
/// steps would not do: the writer pauses between rounds, on Windows for a whole timer tick, and two hundred
/// keys took less than one. Each step pauses after it as each round does, so that neither thread starves the
/// other of the lock.
/// @param write One round of writing.
/// @param step One step on this thread.
/// @return How many steps ran.
[[nodiscard]] inline size_t runWhileWriting(std::function<void()> write, std::function<void()> const& step)
{
    auto constexpr Rounds = 64;
    auto written = std::atomic<int> { 0 };
    auto firstWritten = std::latch { 1 };
    auto const writer = writeOnOwnThread([&] {
        write();
        if (written.fetch_add(1) == 0)
            firstWritten.count_down();
    });
    firstWritten.wait();
    auto steps = size_t { 0 };
    do
    {
        step();
        pauseAfter(++steps);
    } while (written.load() < Rounds);
    return steps;
}

// Whether this translation unit is built with ThreadSanitizer: GCC says so with __SANITIZE_THREAD__,
// clang with __has_feature(thread_sanitizer). Local to this header; undefined right below.
#ifdef __SANITIZE_THREAD__
    #define VTBACKEND_TEST_HELPERS_TSAN
#endif
#ifdef __has_feature
    #if __has_feature(thread_sanitizer)
        #define VTBACKEND_TEST_HELPERS_TSAN
    #endif
#endif

/// How many times longer a budget for work that must finish is in this build than in an unsanitized one.
/// ThreadSanitizer runs code 5 to 15 times slower (its documentation), so a budget sized for an unsanitized
/// run can run out while the work is merely slow: the upper end of that range. AddressSanitizer's slowdown,
/// about 2x, fits the budgets as they are. A real deadlock still fails, only later.
#ifdef VTBACKEND_TEST_HELPERS_TSAN
inline constexpr auto SanitizerSlowdown = 15;
#else
inline constexpr auto SanitizerSlowdown = 1;
#endif
#undef VTBACKEND_TEST_HELPERS_TSAN

/// Runs @p body, and ends the test run with a message should it not return within @p budget: a thread that
/// deadlocked on the terminal lock holds it for good, so the case could neither fail nor unwind.
/// @param budget How long @p body may take in an unsanitized build; a sanitized one stretches it by
///               SanitizerSlowdown.
/// @param body What might deadlock.
inline void failOnDeadlock(std::chrono::milliseconds budget, std::function<void()> const& body)
{
    auto const allowed = budget * SanitizerSlowdown;
    auto const watchdog = std::jthread { [allowed](std::stop_token const& returned) {
        auto mutex = std::mutex {};
        auto lock = std::unique_lock { mutex };
        auto wake = std::condition_variable_any {};
        // Ask the token too: libstdc++'s wait_for answers false, as if timed out, when the stop lands
        // between its first look at the predicate and its wait — that is, whenever @p body returns quickly.
        if (wake.wait_for(lock, returned, allowed, [&returned] { return returned.stop_requested(); })
            || returned.stop_requested())
            return;
        std::fputs("Deadlocked: the thread holding the terminal lock took it again.\n", stderr);
        std::_Exit(EXIT_FAILURE);
    } };
    body();
}

/// Takes a terminal's lock on another thread, as the parser thread would, to check that it is free where a
/// frontend is asked for what may block on another process: opening a document, or reading the clipboard.
class LockProbe
{
  public:
    /// Takes @p terminal's lock on another thread, and waits up to five seconds for it. That thread is kept
    /// beyond the call, so that a lock still held here makes lockWasFree() fail instead of the test hang.
    /// @param terminal Whose lock to take.
    void probe(Terminal& terminal)
    {
        _locker =
            std::async(std::launch::async, [&terminal] { auto const _ = std::scoped_lock { terminal }; });
        _waited = _locker.wait_for(std::chrono::seconds(5));
    }

    /// Joins the thread the last probe() started.
    /// @return Whether it took the lock in time.
    [[nodiscard]] bool lockWasFree()
    {
        REQUIRE(_locker.valid());
        _locker.get();
        return _waited == std::future_status::ready;
    }

  private:
    std::future<void> _locker;
    std::future_status _waited = std::future_status::timeout;
};

} // namespace vtbackend::test
