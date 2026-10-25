// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/CommandBlockId.hpp>
#include <vtbackend/core/CommandBlockOutcome.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/SteadyClockRef.hpp>
#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/shell/CommandBlockAddress.hpp>
#include <vtbackend/shell/CommandLineText.hpp>

#include <core/platform/Clock.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

namespace vtbackend
{

class Grid;

/// Where a command block is in its cycle.
enum class CommandBlockState : uint8_t
{
    Prompting = 0, ///< The prompt is up (OSC 133;A); nothing has run yet.
    Running,       ///< The command started (OSC 133;C) and has not reported its end.
    Finished,      ///< The command ended: reported (;D) or implied (a newer prompt superseded it).
};

/// Where a record's command line came from.
enum class CommandLineSource : uint8_t
{
    None = 0,  ///< Nobody said: no cmdline_url, and no ;B to read the typed text from.
    Reported,  ///< The shell sent it (OSC 133;C cmdline_url= or cmdline=, or an OSC 3008 cmdline=).
    Recovered, ///< Read off the grid, between the ;B column and the output start.
};

/// How a Finished block ended.
enum class CommandBlockEnd : uint8_t
{
    Reported = 0, ///< OSC 133;D, or an OSC 3008 end= while OSC 3008 owns the marks.
    Implicit,     ///< A newer prompt superseded it: a nested shell's prompt, or a missing ;D.
};

/// How a record replicated from a daemon host enters the store.
/// @see CommandBlockStore::adopt
enum class AdoptMode : uint8_t
{
    Snapshot = 0, ///< An attach snapshot: history, so no finish is announced.
    Live,         ///< A live delta: a first transition to a reported Finished is announced.
};

/// What CommandBlockStore::placeHead() did.
enum class HeadPlacement : uint8_t
{
    Unchanged = 0, ///< The id is unknown, or its head is already placed in that generation.
    Placed,        ///< The block's head was re-recorded.
};

/// Longest command line a record keeps; longer ones are truncated at a UTF-8 boundary and end in "…".
constexpr inline size_t MaxRecordedCommandLineBytes = 4096;

/// Longest working directory path a record keeps: Linux's PATH_MAX. A longer one is dropped, never
/// truncated -- a cut path names a different directory.
constexpr inline size_t MaxRecordedWorkingDirectoryBytes = 4096;

/// The earliest instant a terminal takes from elsewhere -- a session epoch it adopts, a record's start:
/// the Unix epoch. @see LatestHeldInstant
constexpr inline auto EarliestHeldInstant = std::chrono::system_clock::time_point {};

/// The latest instant a terminal takes from elsewhere: 2^62 ns after the Unix epoch, early in the year
/// 2116, as this platform's system_clock names it.
///
/// For the arithmetic, not the calendar. A terminal adds up to Line::MaxBornAt seconds to its session
/// epoch for a row's birth time and subtracts the epoch from its own clock to stamp a row, and a record's
/// duration added to its start is when it ended. Near the limits of an int64_t count of nanoseconds, which
/// is what libstdc++'s system_clock is, each overflows: undefined behaviour. Between EarliestHeldInstant
/// and this, a span up to LongestHeldSpan can be added to the instant, and the instant subtracted from any
/// clock reading since 1970, without leaving int64_t nanoseconds on any platform. So the seams that take
/// an instant from a daemon, a tmux server or a restore hold it here, whoever produced it.
constexpr inline auto LatestHeldInstant = std::chrono::system_clock::time_point {
    std::chrono::floor<std::chrono::system_clock::duration>(std::chrono::nanoseconds { int64_t { 1 } << 62 })
};

/// The longest span a terminal takes from elsewhere -- a record's duration: one short of 2^62 ns, some 146
/// years, the longest whose sum with LatestHeldInstant is still an int64_t count of nanoseconds.
constexpr inline auto LongestHeldSpan = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
    std::chrono::nanoseconds { (int64_t { 1 } << 62) - 1 });

static_assert(std::chrono::nanoseconds { LatestHeldInstant.time_since_epoch() }.count()
                  <= std::numeric_limits<int64_t>::max()
                         - std::chrono::nanoseconds { LongestHeldSpan }.count(),
              "The latest held instant plus the longest held span must stay inside int64_t nanoseconds.");

/// @param instant An instant taken from elsewhere.
/// @return @p instant held to [EarliestHeldInstant, LatestHeldInstant]: the nearer edge where it overshoots.
[[nodiscard]] constexpr std::chrono::system_clock::time_point heldInstant(
    std::chrono::system_clock::time_point instant) noexcept
{
    return std::clamp(instant, EarliestHeldInstant, LatestHeldInstant);
}

/// @param span A span taken from elsewhere.
/// @return @p span held to [0, LongestHeldSpan]: a negative length is not a length.
[[nodiscard]] constexpr std::chrono::steady_clock::duration heldSpan(
    std::chrono::steady_clock::duration span) noexcept
{
    return std::clamp(span, std::chrono::steady_clock::duration::zero(), LongestHeldSpan);
}

/// A working directory as a record keeps it: resolved at one moment of a command's life -- its OSC 133;C, or
/// the next prompt's ;A.
struct WorkingDirectorySnapshot
{
    std::string path;                                    ///< Empty when nothing reported one.
    ContextLocality locality = ContextLocality::Unknown; ///< Whether @ref path names this machine.

    bool operator==(WorkingDirectorySnapshot const&) const = default;
};

/// Whether a working directory on another machine was reported while a block was open: a remote shell that
/// marks no commands of its own -- `ssh host` -- prints into the local block it started in.
enum class ForeignDirectory : uint8_t
{
    Unreported = 0, ///< None was, as far as the terminal knows.
    Reported,       ///< One was: rows of the block may have been printed on that machine.
};

/// Everything the terminal knows about one block that the grid cannot hold.
struct CommandBlockRecord
{
    CommandBlockId id {};                                   ///< Never zero for a stored record.
    CommandBlockState state = CommandBlockState::Prompting; ///< Where the block is in its cycle.
    CommandBlockEnd end = CommandBlockEnd::Reported;        ///< Meaningful once Finished.
    CommandLineText commandLine; ///< As reported or recovered; read through its purpose.
    CommandLineSource commandLineSource = CommandLineSource::None; ///< Where @ref commandLine came from.
    std::optional<int> exitCode; ///< From ;D, or ContextOutcome::asShellExitCode().
    ContextOutcome outcome {};   ///< Signal / crash / interrupt, when OSC 3008 said.
    /// Wall clock at ;A; none for a block a ;C opened with no ;A before it, which never had a prompt row.
    std::optional<std::chrono::system_clock::time_point> promptStartedAt;
    std::optional<std::chrono::system_clock::time_point> commandStartedAt; ///< Wall clock at ;C.
    std::optional<std::chrono::steady_clock::duration> duration;           ///< ;C to a reported end.
    WorkingDirectorySnapshot workingDirectory;                             ///< Snapshot at ;C.
    /// Snapshot at the next prompt's ;A: the directory the command left the shell in, another than
    /// @ref workingDirectory after a `cd` in its own command line. Empty until that prompt.
    WorkingDirectorySnapshot endWorkingDirectory;
    /// Whether a directory on another machine was reported while the block was open. Once reported, its
    /// rows name files there for good: the local prompt that closes the block does not bring them back.
    ForeignDirectory foreignDirectory = ForeignDirectory::Unreported;
    /// The command's own OSC 3008 context, zero until known: the command context active at its ;C, or one
    /// that started directly beneath the context active there while the command ran. What an OSC 3008 end=
    /// is matched by. @see CommandBlockStore::enrichOutcome
    ContextId commandContext {};
    int64_t headStableId {};      ///< The head row's stable id, valid in @ref headIdGeneration.
    uint64_t headIdGeneration {}; ///< The Grid::stableIdGeneration() of @ref headStableId.

    bool operator==(CommandBlockRecord const&) const = default;
};

/// The head generation of a record whose head row is not known: one no grid reaches, so whoever checks a
/// head against Grid::stableIdGeneration() -- headOffsetOf() -- finds none and looks again, rather than
/// trusting a row id from another grid. What a daemon mirror adopts a host's record with, its head naming
/// a row in the host's grid.
constexpr inline uint64_t UnknownHeadGeneration = std::numeric_limits<uint64_t>::max();

/// Where @p record's head row is in @p grid.
///
/// Declared beside the record although it reads a grid, which the store deliberately never does: the
/// record says which row heads it, and every reader asks this one question of it.
/// @param grid The grid the block's rows are stamped on: the primary screen's.
/// @param record The block.
/// @return The head row's line offset; nullopt when the head was recorded in another stable-id generation
///         -- UnknownHeadGeneration included -- or its row has left the grid.
[[nodiscard]] std::optional<LineOffset> headOffsetOf(Grid const& grid,
                                                     CommandBlockRecord const& record) noexcept;

/// Whether @p record's head row has left @p grid: recorded in the grid's current stable-id generation, and
/// no longer among its rows -- scrolled out of the history, or recorded as evicted by a re-sync.
///
/// A head recorded in another generation -- UnknownHeadGeneration included -- is not known to be gone.
/// @param grid The grid the block's rows are stamped on: the primary screen's.
/// @param record The block.
/// @return True exactly when the head is known to be evicted.
[[nodiscard]] bool isHeadEvicted(Grid const& grid, CommandBlockRecord const& record) noexcept;

/// What a frontend needs about a block that just finished (value copy; safe across threads).
struct CommandBlockSummary
{
    CommandBlockId id {};        ///< The block that finished.
    CommandLineText commandLine; ///< As reported or recovered.
    std::optional<int> exitCode; ///< As reported by ;D, when reported.
    ContextOutcome outcome {};   ///< Signal / crash / interrupt, when OSC 3008 said.
    /// ;C to the reported end; nullopt when unknown -- a record from another process may carry none.
    std::optional<std::chrono::steady_clock::duration> duration;
    WorkingDirectorySnapshot workingDirectory; ///< Snapshot at ;C.

    bool operator==(CommandBlockSummary const&) const = default;
};

/// A record, and when what it replicates last changed.
/// @see CommandBlockStore::stampedRecords
struct StampedCommandBlock
{
    CommandBlockRecord const& record; ///< The record.
    uint64_t changeStamp = 0;         ///< The CommandBlockStore::contentRevision() it last changed at.
};

/// How much a CommandBlockStore keeps.
struct CommandBlockStoreLimits
{
    size_t maxRecords = 1000; ///< The oldest records are dropped first beyond this.

    bool operator==(CommandBlockStoreLimits const&) const = default;
};

/// Where a command started, as the store needs it at OSC 133;C.
///
/// Every member has a default initializer, so a designated initializer may name only what is known.
struct CommandStart
{
    std::optional<std::string> commandLine {}; ///< Taken only when @ref source is not None.
    /// Who supplied @ref commandLine; None means nobody did.
    CommandLineSource source = CommandLineSource::None;
    WorkingDirectorySnapshot workingDirectory {}; ///< The working directory the command starts in.
    /// The OSC 3008 context active at the ;C; zero when none was. A command context is the command's own;
    /// any other is the one the command's own context, should one start while it runs, starts beneath.
    ContextId context {};
    ContextType contextType = ContextType::None; ///< @ref context's type.
    int64_t headStableId {};                     ///< Used only when ;C mints a record (no preceding ;A).
    uint64_t headIdGeneration {};                ///< The Grid::stableIdGeneration() of @ref headStableId.
};

/// @p text cut to at most @p maxBytes bytes at a UTF-8 code point boundary, ending in "…" when anything
/// was cut. The one UTF-8 truncation helper of the feature: the record's command line, the finish
/// notification's command name, any other bounded display of untrusted text.
///
/// The ellipsis (three bytes) counts against @p maxBytes, so the result never exceeds it. Text that is
/// not UTF-8 at the cut is cut there anyway -- the bytes are kept raw, and sanitizeCommandLine() owns
/// making them presentable.
/// @param text The text as received.
/// @param maxBytes The most bytes the result may have, ellipsis included.
/// @return @p text itself when it fits; else its longest whole-code-point prefix plus "…"; an empty string
///         when @p text does not fit and @p maxBytes cannot even hold the ellipsis.
[[nodiscard]] std::string truncateUtf8(std::string_view text, size_t maxBytes);

/// @p text cut to MaxRecordedCommandLineBytes, at a UTF-8 code point boundary, ending in "…".
///
/// A command line arrives from whatever wrote to the pty, so its length is the writer's choice; this is
/// what bounds what a record costs.
/// @param text The command line as received.
/// @return truncateUtf8(@p text, MaxRecordedCommandLineBytes).
[[nodiscard]] std::string truncatedCommandLine(std::string_view text);

/// How a finished command ended, as far as its record can tell -- the one classification the gutter's
/// colour and the finish notification both read.
enum class CommandEnding : uint8_t
{
    Unknown = 0, ///< Neither an exit code nor an OSC 3008 verdict was reported.
    Succeeded,   ///< Exit code 0, or OSC 3008 `exit=success`, and nothing that says otherwise.
    Failed,      ///< A non-zero exit code, or OSC 3008 `exit=failure` (whatever its `status=`).
    Killed,      ///< OSC 3008 named the signal it died of, or a shell's 128 + n exit code names one.
    Crashed,     ///< OSC 3008 `exit=crash` without a signal name.
    Interrupted, ///< OSC 3008 `exit=interrupt` without a signal name.
};

/// Number of @c CommandEnding enumerators, derived from the last one; tables indexed by ending pin to it.
inline constexpr size_t CommandEndingCount = static_cast<size_t>(CommandEnding::Interrupted) + 1;

/// Classifies how a command ended. A command failed if either source says so: an explicit OSC 3008
/// `exit=success` does not outweigh a non-zero exit code.
///
/// A signal OSC 3008 named wins; then a crash, an interrupt or a failure it reported (which only it can
/// tell apart from an ordinary non-zero exit); then a shell's 128 + n code naming a signal, while OSC 3008
/// gave no verdict; then the exit code; then OSC 3008's success.
/// @see signalThatEnded
/// @param exitCode The reported exit code, if any (from OSC 133;D, or ContextOutcome::asShellExitCode()).
/// @param outcome  What OSC 3008 said about the end, if anything.
/// @return How the command ended.
[[nodiscard]] constexpr CommandEnding commandEndingOf(std::optional<int> exitCode,
                                                      ContextOutcome const& outcome) noexcept
{
    if (outcome.signal != ContextSignal::None)
        return CommandEnding::Killed;
    switch (outcome.exit)
    {
        case ContextExit::Crash: return CommandEnding::Crashed;
        case ContextExit::Interrupt: return CommandEnding::Interrupted;
        case ContextExit::Failure: return CommandEnding::Failed;
        case ContextExit::Success:
        case ContextExit::Unknown: break;
    }
    if (signalThatEnded(exitCode, outcome) != ContextSignal::None)
        return CommandEnding::Killed;
    if (exitCode.has_value())
        return *exitCode == 0 ? CommandEnding::Succeeded : CommandEnding::Failed;
    if (outcome.exit == ContextExit::Success)
        return CommandEnding::Succeeded;
    return CommandEnding::Unknown;
}

/// Whether @p ending is a failure: what the gutter colours red and `outcome: failure` announces.
/// @param ending How a command ended.
/// @return True for Failed, Killed, Crashed and Interrupted.
[[nodiscard]] constexpr bool isFailure(CommandEnding ending) noexcept
{
    switch (ending)
    {
        case CommandEnding::Unknown:
        case CommandEnding::Succeeded: return false;
        case CommandEnding::Failed:
        case CommandEnding::Killed:
        case CommandEnding::Crashed:
        case CommandEnding::Interrupted: return true;
    }
    return false;
}

/// What a description of how a command ended adds to its verdict: the finish notification's title in
/// parentheses, the sticky header's chip after its `✗`.
enum class FinishDetail : uint8_t
{
    None = 0, ///< Nothing.
    ExitCode, ///< The exit code, when it is not 0: `2`.
    Signal,   ///< The signal that ended the command: `SIGSEGV`.
};

/// The text of @p detail for a command that ended so: its exit code, or the name of its signal. Each
/// surface words it its own way -- the notification `exit 2`, the chip `✗ 2` -- around the same text.
/// @param detail   Which detail the surface asks for, for this ending.
/// @param exitCode The reported exit code, if any.
/// @param outcome  What OSC 3008 said about the end, if anything.
/// @return `2`, `SIGKILL`; empty for None, and for an exit code of 0 -- OSC 3008 alone reports no code,
///         and the one ContextOutcome::asShellExitCode() spells for a failure without a status says
///         nothing worth printing.
[[nodiscard]] std::string finishDetailText(FinishDetail detail,
                                           std::optional<int> exitCode,
                                           ContextOutcome const& outcome);

/// How @p record reads at a glance. A block with no exit code known reads as a success.
/// @see commandEndingOf
/// @param record The block.
/// @return Running while running; otherwise Failure exactly when its ending isFailure(), else Success.
[[nodiscard]] CommandBlockOutcome outcomeOf(CommandBlockRecord const& record) noexcept;

/// The value a frontend is handed when @p record finishes.
/// @param record The block.
/// @return Its summary; a block with no duration reports none.
[[nodiscard]] CommandBlockSummary summaryOf(CommandBlockRecord const& record);

/// How long a command ran, as every surface prints it -- the gutter tooltip, the finish notification, the
/// sticky header's chip: `850ms`, `12s`, `3m 05s`, `1h 02m`.
///
/// Whole units below the coarsest one shown are dropped, not rounded, and hours are never folded into
/// days (a build that ran 25 hours reads `25h 00m`). A negative duration -- a wall clock stepped
/// backwards under a running command -- reads as `0ms`.
/// @param duration How long the command ran.
/// @return The text: below 1 s `{n}ms`; below 1 min `{n}s`; below 1 h `{m}m {ss}s`; else `{h}h {mm}m`.
[[nodiscard]] std::string formatCommandDuration(std::chrono::steady_clock::duration duration);

/// A change to what a record the store holds says, as the store's change listener is told of it.
struct CommandBlockChange
{
    CommandBlockId id {}; ///< The block whose record changed.
    /// The newest block whose command started, once the change is made -- @ref id itself when its command
    /// just started or ended --; zero when no record ran a command. Carried with the change, so a listener
    /// that needs it -- a finish notice the next command withdraws -- reads nothing of the store.
    CommandBlockId lastCommand {};

    bool operator==(CommandBlockChange const&) const = default;
};

/// Told of every change to what a record the store holds says.
/// @see CommandBlockStore::CommandBlockStore
using CommandBlockChangeListener = std::function<void(CommandBlockChange const&)>;

/// Every command block this terminal knows of, oldest first, bounded by CommandBlockStoreLimits.
///
/// Fed by OSC 133 -- and, while OSC 3008 owns the marks, by its command contexts -- through Terminal,
/// and read by the gutter, the scrollbar marks, the sticky header and finish notifications.
/// Deliberately free of Screen, Terminal and the grid: given an event it produces the next state, so every
/// transition is exercised with manual clocks and nothing behind it.
/// ContextStack is the model.
///
/// Invariants: ids strictly ascend from oldest to newest; the newest record is never evicted; of the records
/// the store mints, only the NEWEST can be unfinished (the next prompt finishes or discards it). Adopted
/// records carry the host's state, which out-of-order deltas may briefly leave unfinished behind a newer one.
///
/// Every transition is TOTAL: the protocol is lenient, so a malformed order (a ;D with nothing running, a
/// ;C with no ;A) is an outcome, never an error.
class CommandBlockStore
{
  public:
    /// @param limits How many records to keep. Fixed for the store's life; a maxRecords of zero is raised
    ///        to one, because the block rows are being stamped with must always resolve.
    /// @param steadyClock Measures how long commands run. Borrowed: it must outlive the store, and a
    ///        temporary is refused.
    /// @param wallClock Says when prompts and commands started. Borrowed likewise.
    /// @param onRecordChanged Called with the change whenever what a record the store already holds says
    ///        changes, for any cause: its command started or finished (reported, or superseded by a newer
    ///        prompt), an OSC 3008 outcome or a command line arrived for it, or a newer copy of it was
    ///        adopted -- every move of its change stamp but its birth at a prompt and its head leaving the
    ///        grid (@see headsLeft). So only from a transition, never from a read. Never for a head that
    ///        moved, nor for a record forgotten. Called mid-transition, so it must neither read nor change
    ///        the store: what it needs travels in the change. It must be callable.
    CommandBlockStore(CommandBlockStoreLimits limits,
                      SteadyClockRef steadyClock,
                      core::platform::WallClockRef wallClock,
                      CommandBlockChangeListener onRecordChanged);

    /// OSC 133;A: a new prompt starts.
    ///
    /// A Prompting record that never reached ;C is DISCARDED (an empty Enter, Ctrl-C at the prompt); a
    /// Running one is closed as Finished with CommandBlockEnd::Implicit, no exit code and no duration --
    /// a nested shell's prompt (ssh, a subshell) or a missing ;D. Nothing is announced for it: the `ssh`
    /// the user is still inside has not finished. Then a new Prompting record is minted.
    ///
    /// The block whose command ran last -- closed so, or by its ;D since the previous prompt -- records
    /// @p directory as the one its command left the shell in (CommandBlockRecord::endWorkingDirectory),
    /// none when the path exceeds MaxRecordedWorkingDirectoryBytes.
    /// @param headStableId The stable id of the prompt's head row.
    /// @param headIdGeneration The Grid::stableIdGeneration() that id is valid in.
    /// @param directory The working directory in effect as the prompt starts; empty when none was reported.
    /// @return The new record's id.
    CommandBlockId promptStarted(int64_t headStableId,
                                 uint64_t headIdGeneration,
                                 WorkingDirectorySnapshot directory = {});

    /// OSC 133;C: the current block's command starts.
    ///
    /// A Prompting record becomes Running, with its start time and working directory -- none when the path
    /// exceeds MaxRecordedWorkingDirectoryBytes. With no unfinished record at all (a shell integration
    /// sourced mid-session, or a prompt theme that drops the ;A) one is minted at @p start's head. A command
    /// line is recorded once, truncated to MaxRecordedCommandLineBytes: a repeated ;C may fill a missing
    /// one, never replace one.
    /// @param start What the terminal knew at the ;C.
    void commandStarted(CommandStart start);

    /// OSC 133;D, or an OSC 3008 `end=` while OSC 3008 owns the marks: the running command finished.
    ///
    /// Ignored when nothing is Running -- tcsh's unconditional ;D, a nested shell's exit.
    /// @param exitCode The exit code the shell reported.
    /// @param outcome How the command ended, when OSC 3008 said; a default outcome keeps one an earlier
    ///        enrichOutcome() recorded.
    /// @return What a frontend needs to announce the finish, or nullopt when nothing finished.
    [[nodiscard]] std::optional<CommandBlockSummary> commandFinished(int exitCode,
                                                                     ContextOutcome outcome = {});

    /// An OSC 3008 command context started while OSC 133 owns the marks: when a command is running whose ;C
    /// saw no command context of its own, one starting directly beneath the context its ;C did see is that
    /// command's -- the shell ran its OSC 133 hook before its OSC 3008 one.
    /// @param started The command context that started.
    /// @param parent The context it started beneath; zero for none.
    void commandContextStarted(ContextId started, ContextId parent);

    /// An OSC 3008 command context's `end=` arrived while OSC 133 owns the marks: records HOW the command
    /// ended (a signal, a crash), which ;D's bare number cannot say. Never closes, re-closes or announces a
    /// block.
    ///
    /// Only for the most recent command, and only when @p ended is that command's own context
    /// (CommandBlockRecord::commandContext): its end= may follow its ;D, and even the next prompt's ;A, but
    /// not the next command's ;C. A block that already carries an outcome keeps it.
    /// @param ended The command context that ended.
    /// @param outcome The ENDFIELDs OSC 3008 reported; a default outcome changes nothing.
    void enrichOutcome(ContextId ended, ContextOutcome outcome);

    /// A working directory on another machine was reported (OSC 7, OSC 3008): what a shell there prints
    /// lands in the newest block while it is open, which records so for good
    /// (CommandBlockRecord::foreignDirectory). Nothing changes when no block is open or it says so already.
    void foreignDirectoryReported();

    /// Re-records where a block's head row is, after a reflow destroyed stable row identity.
    ///
    /// Unknown ids are ignored. Safe to call from inside forEachRecord(): it never adds or removes a
    /// record.
    /// @param id The block.
    /// @param headStableId The head row's stable id, or one below the grid's floor when it was evicted.
    /// @param headIdGeneration The Grid::stableIdGeneration() that id is valid in.
    void updateHeadPosition(CommandBlockId id, int64_t headStableId, uint64_t headIdGeneration);

    /// Re-records where a block's head row is, unless it already has one in @p headIdGeneration -- so of the
    /// rows a re-sync offers for one block, the first is the one it keeps.
    ///
    /// One search, where find() followed by updateHeadPosition() would make two.
    /// @param id The block.
    /// @param headStableId The head row's stable id.
    /// @param headIdGeneration The Grid::stableIdGeneration() that id is valid in.
    /// @return Placed when the head moved, else Unchanged.
    HeadPlacement placeHead(CommandBlockId id, int64_t headStableId, uint64_t headIdGeneration);

    /// Counts the records whose heads just left the grid as changed: each one whose head, in
    /// @p headIdGeneration, lies between the oldest row the grid held before and the oldest it holds now --
    /// the history let go of it -- and each one @p erased names, whose block an erase left no row of.
    ///
    /// A replica is told whether a record's head row is gone (isHeadEvicted), so the head going is a change
    /// of the record: it moves the content revision and is stamped, once where both say so, and a sender
    /// walking the stamps sends the record again. The change listener is not told: what the record says is
    /// unchanged, and this runs inside whichever read of the store found the head gone, on that reader's
    /// thread. One pass over the records.
    /// @param headIdGeneration The generation the heads left in.
    /// @param formerFloor The oldest stable id the grid held before (Grid::stableRangeFloor()).
    /// @param floor The oldest it holds now; the history let go of no head unless it is above @p formerFloor.
    /// @param erased The blocks a re-sync found no row of and recorded as evicted, ascending.
    void headsLeft(uint64_t headIdGeneration,
                   int64_t formerFloor,
                   int64_t floor,
                   std::span<CommandBlockId const> erased);

    /// Records every head not placed in @p headIdGeneration as evicted: what a re-sync concludes of a block
    /// none of whose rows it found. One pass over the records.
    /// @param headIdGeneration The generation the re-sync placed heads in.
    /// @param evictedStableId A stable id below the grid's floor, which is what "evicted" means everywhere.
    void retireUnplacedHeads(uint64_t headIdGeneration, int64_t evictedStableId);

    /// Adopts a record replicated from a daemon host, keeping the host's id.
    ///
    /// Snapshot: an attach brings history in and announces nothing. Live: a delta, which announces a
    /// block's FIRST transition to a reported Finished -- so a reattach never replays finishes that
    /// happened while detached, and a repeated delta never announces twice.
    ///
    /// The record is held to the bounds a minted one is: its command line truncated to
    /// MaxRecordedCommandLineBytes, either working directory beyond MaxRecordedWorkingDirectoryBytes dropped,
    /// its instants and duration held to the range a terminal computes with (heldInstant(), heldSpan()).
    /// One older than every record a full store keeps is not taken at all: the cap would evict it on
    /// arrival.
    /// @param record The host's record; a zero id is refused.
    /// @param mode How it arrived.
    /// @return The finish to announce, or nullopt.
    [[nodiscard]] std::optional<CommandBlockSummary> adopt(CommandBlockRecord record, AdoptMode mode);

    /// Forgets the records @p ids name, in place: what a daemon mirror does with the records its host
    /// retired. Every other record keeps its fields, its head and its change stamp, and ids are not reused.
    ///
    /// One search per id, and an erase that costs nothing at either end of the store, where the host's
    /// retirements land: its cap evicts the oldest, and a prompt that ran nothing discards the newest.
    /// Like clear(), it moves both revisions when it forgets anything and neither when it does not; a
    /// running command whose record it forgets is no longer timed.
    /// @param ids The records to forget, in any order; an id the store does not hold is passed over.
    void retire(std::span<CommandBlockId const> ids) noexcept;

    /// Forgets every record. Ids are NOT reused afterwards. Used by RIS / hard reset.
    void clear() noexcept;

    /// @return The record @p id names, or nullptr when it was never minted nor adopted, was discarded or
    ///         evicted, or was refused on arrival.
    [[nodiscard]] CommandBlockRecord const* find(CommandBlockId id) const noexcept;

    /// @return The newest record while it is unfinished (Prompting or Running), else nullptr.
    [[nodiscard]] CommandBlockRecord const* current() const noexcept;

    /// @return The id freshly reached rows are stamped with: the newest record's, or zero when empty.
    [[nodiscard]] CommandBlockId currentId() const noexcept;

    /// @return The newest Finished record, however it ended, or nullptr.
    [[nodiscard]] CommandBlockRecord const* lastFinished() const noexcept;

    /// @return The newest record whose command started -- still running, or finished since, a newer prompt
    ///         perhaps open below it -- or nullptr when no record ran a command.
    [[nodiscard]] CommandBlockRecord const* lastCommand() const noexcept;

    /// Visits every record, oldest first.
    ///
    /// The visitor may call updateHeadPosition(), which re-heads a record in place, but no other mutator.
    /// @param fn Called as fn(CommandBlockRecord const&).
    template <typename F>
    void forEachRecord(F const& fn) const
    {
        for (auto const& record: _records)
            fn(record);
    }

    /// Every record, oldest first, as a read-only view. Reversible: a reader after the newest few walks it
    /// newest first and stops as soon as it has them.
    /// @return A view over the records, valid until the store next changes.
    [[nodiscard]] auto records() const noexcept { return std::views::all(_records); }

    /// Every record paired with its change stamp, oldest first: what a sender keeping a replica walks for
    /// the records stamped after the content revision it last caught the replica up to. Random access, so
    /// a walk cut short resumes with a lower_bound on the id.
    /// @return A view of StampedCommandBlock, valid until the store next changes.
    [[nodiscard]] auto stampedRecords() const noexcept
    {
        return std::views::iota(size_t { 0 }, _records.size())
               | std::views::transform([this](size_t position) {
                     return StampedCommandBlock { .record = _records[position],
                                                  .changeStamp = _changeStamps[position] };
                 });
    }

    /// @return How many records are kept.
    [[nodiscard]] size_t size() const noexcept { return _records.size(); }

    /// @return A counter that moves with every change and never without one; caches (gutter, scrollbar
    ///         marks, sticky header) key on it.
    [[nodiscard]] uint64_t revision() const noexcept { return _revision; }

    /// A counter that moves exactly when a replica of the store would see a change: a record added or
    /// dropped, a field of one that travels -- every field but the head position and the command's own
    /// OSC 3008 context, which a replica keeps for itself -- or its head leaving the grid, which a replica
    /// is told of (@see headsLeft). Head placement, which moves revision(), leaves it alone, so a sender
    /// gated on it does not walk the store for a reflow or an erase that leaves every block a row.
    /// @return The current content revision. Every change stamp is a value it took; zero is none of them.
    [[nodiscard]] uint64_t contentRevision() const noexcept { return _contentRevision; }

    /// How many times the ids have started over at 1: once every 2^32 - 1 mints, which only a stream
    /// replaying OSC 133 as fast as the pty delivers it ever reaches.
    ///
    /// An id names a block only within its epoch. Whoever holds ids across mints -- the rows stamped with
    /// them, the mode 2034 reader's visibility floor -- compares this with the epoch it took them in, and
    /// forgets them when it moved: they now name nothing, or a block minted after. clear() does not move
    /// it, because the ids are not reused after a clear.
    /// @return The current epoch; zero until the first wrap.
    [[nodiscard]] uint64_t idEpoch() const noexcept { return _idEpoch; }

    /// @return The limits in force (maxRecords already raised to at least one).
    [[nodiscard]] CommandBlockStoreLimits const& limits() const noexcept { return _limits; }

  private:
    /// The integer a CommandBlockId boxes, so the id counter and its wrap limit follow the id's type.
    using IdValue = CommandBlockId::inner_type;

    /// The newest record while unfinished, for mutation.
    /// @see current
    [[nodiscard]] CommandBlockRecord* unfinished() noexcept;

    /// The newest record while its command runs, for mutation; nullptr at a prompt or with none unfinished.
    [[nodiscard]] CommandBlockRecord* running() noexcept;

    /// Appends a Prompting record headed at the given row, evicting the oldest beyond the cap. Its prompt
    /// start is the caller's to set: a ;C that mints one marked no prompt.
    CommandBlockRecord& mint(int64_t headStableId, uint64_t headIdGeneration);

    /// Forgets what is kept about the running command alone, for it runs no longer.
    void endRun() noexcept;

    /// Drops the oldest records until the cap holds.
    void evictOverflow() noexcept;

    /// Whether stamp() tells the change listener.
    enum class StampNotice : uint8_t
    {
        Silent = 0, ///< Only a replica sees the change: the record's head left the grid.
        Told,       ///< What the record says changed.
    };

    /// Records that what @p id's record replicates just changed: moves the content revision, and stamps the
    /// record with it.
    /// @param id A record the store holds, saying what it now says.
    /// @param notice Whether the change listener is told, and with it the newest record that ran a command.
    void stamp(CommandBlockId id, StampNotice notice);

    CommandBlockStoreLimits _limits;             ///< Fixed at construction; maxRecords is at least one.
    SteadyClockRef _steadyClock;                 ///< Borrowed; times how long commands run.
    core::platform::WallClockRef _wallClock;     ///< Borrowed; stamps when prompts and commands start.
    CommandBlockChangeListener _onRecordChanged; ///< Told by stamp() of what a record held says changing.
    std::deque<CommandBlockRecord> _records;     ///< Oldest first, so ids ascend front to back.
    /// Each record's change stamp, at the record's position in _records: the content revision at which
    /// what it replicates last changed. A side table rather than a record field, being the store's
    /// bookkeeping and no fact about the block.
    std::deque<uint64_t> _changeStamps;

    /// When the running command started, by the steady clock. One field suffices: the one Running record
    /// is always the newest.
    std::optional<core::platform::SteadyTimePoint> _runningSince;

    /// While the running command's own OSC 3008 context is unknown: the context it would start directly
    /// beneath. One field suffices for the reason _runningSince gives. @see commandContextStarted
    std::optional<ContextId> _awaitedCommandContextParent;

    IdValue _nextId = 1;           ///< Never zero; zero means "no block".
    uint64_t _idEpoch = 0;         ///< Moves each time mint() starts the ids over. @see idEpoch
    uint64_t _revision = 0;        ///< Moves with every change, never without one.
    uint64_t _contentRevision = 0; ///< @see contentRevision
};

} // namespace vtbackend
