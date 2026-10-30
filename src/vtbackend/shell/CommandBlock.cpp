// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/CommandBlock.hpp>

#include <vtbackend/core/Utf8Prefix.hpp>
#include <vtbackend/grid/Grid.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <ranges>
#include <utility>

namespace vtbackend
{

namespace
{
    /// U+2026 HORIZONTAL ELLIPSIS, which a truncated command line ends in.
    constexpr auto Ellipsis = std::string_view { "\xE2\x80\xA6" };

    /// @p limits with maxRecords raised to at least one: the block rows are being stamped with must resolve.
    [[nodiscard]] CommandBlockStoreLimits clamped(CommandBlockStoreLimits limits) noexcept
    {
        limits.maxRecords = std::max(limits.maxRecords, size_t { 1 });
        return limits;
    }

    /// @p snapshot while its path fits MaxRecordedWorkingDirectoryBytes, else no working directory at all.
    /// Never a cut path: that would name a different directory.
    [[nodiscard]] WorkingDirectorySnapshot boundedWorkingDirectory(WorkingDirectorySnapshot snapshot) noexcept
    {
        if (snapshot.path.size() > MaxRecordedWorkingDirectoryBytes)
            return {};
        return snapshot;
    }

    /// @p text held to MaxRecordedCommandLineBytes: passed through untouched -- moved, never copied -- when
    /// it fits, which is nearly always; cut by truncatedCommandLine() otherwise.
    [[nodiscard]] std::string boundedCommandLine(std::string text)
    {
        if (text.size() <= MaxRecordedCommandLineBytes)
            return text;
        return truncatedCommandLine(text);
    }

    /// Where @p id sits in @p records (sorted by id), or end(). A template so a const and a mutable
    /// lookup share the one search.
    template <typename Records>
    [[nodiscard]] auto positionOf(Records& records, CommandBlockId id) noexcept
    {
        auto const it = std::ranges::lower_bound(records, id, std::less {}, &CommandBlockRecord::id);
        return (it != records.end() && it->id == id) ? it : records.end();
    }

    /// The newest record of @p records while it is unfinished, else nullptr.
    template <typename Records>
    [[nodiscard]] auto newestUnfinishedIn(Records& records) noexcept -> decltype(&records.back())
    {
        if (records.empty() || records.back().state == CommandBlockState::Finished)
            return nullptr;
        return &records.back();
    }

    /// The newest Finished record of @p records, else nullptr.
    template <typename Records>
    [[nodiscard]] auto newestFinishedIn(Records& records) noexcept -> decltype(&records.back())
    {
        auto const reversed = records | std::views::reverse;
        auto const it = std::ranges::find(reversed, CommandBlockState::Finished, &CommandBlockRecord::state);
        if (it == reversed.end())
            return nullptr;
        return &*it;
    }

    /// The newest record of @p records whose command started, running or finished, else nullptr.
    template <typename Records>
    [[nodiscard]] auto newestCommandIn(Records& records) noexcept -> decltype(&records.back())
    {
        auto const reversed = records | std::views::reverse;
        auto const it = std::ranges::find_if(
            reversed, [](CommandBlockRecord const& record) { return record.commandStartedAt.has_value(); });
        if (it == reversed.end())
            return nullptr;
        return &*it;
    }
} // namespace

std::string truncateUtf8(std::string_view text, size_t maxBytes)
{
    if (text.size() <= maxBytes)
        return std::string { text };
    if (maxBytes < Ellipsis.size())
        return {};

    // Cut at the start of a code point so the ellipsis never splits one.
    auto result = std::string { text.substr(0, utf8PrefixLength(text, maxBytes - Ellipsis.size())) };
    result += Ellipsis;
    return result;
}

std::string truncatedCommandLine(std::string_view text)
{
    return truncateUtf8(text, MaxRecordedCommandLineBytes);
}

bool isTruncatedCommandLine(std::string_view text) noexcept
{
    // A cut keeps a whole-code-point prefix of at most Max - |…| bytes (a code point is up to 4 bytes, so at
    // least Max - |…| - 3 of them), then the ellipsis.
    constexpr auto LongestCodePointTail = size_t { 3 };
    constexpr auto ShortestCut = MaxRecordedCommandLineBytes - LongestCodePointTail;
    return text.size() >= ShortestCut && text.size() <= MaxRecordedCommandLineBytes
           && text.ends_with(Ellipsis);
}

std::optional<LineOffset> headOffsetOf(Grid const& grid, CommandBlockRecord const& record) noexcept
{
    if (record.headIdGeneration != grid.stableIdGeneration())
        return std::nullopt;
    return grid.lineOffsetOf(record.headStableId);
}

bool isHeadEvicted(Grid const& grid, CommandBlockRecord const& record) noexcept
{
    return record.headIdGeneration == grid.stableIdGeneration() && !grid.lineOffsetOf(record.headStableId);
}

CommandBlockOutcome outcomeOf(CommandBlockRecord const& record) noexcept
{
    if (record.state == CommandBlockState::Running)
        return CommandBlockOutcome::Running;

    return isFailure(commandEndingOf(record.exitCode, record.outcome)) ? CommandBlockOutcome::Failure
                                                                       : CommandBlockOutcome::Success;
}

CommandBlockSummary summaryOf(CommandBlockRecord const& record)
{
    return CommandBlockSummary {
        .id = record.id,
        .commandLine = record.commandLine,
        .exitCode = record.exitCode,
        .outcome = record.outcome,
        .duration = record.duration,
        .workingDirectory = record.workingDirectory,
    };
}

std::string finishDetailText(FinishDetail detail, std::optional<int> exitCode, ContextOutcome const& outcome)
{
    switch (detail)
    {
        case FinishDetail::None: break;
        case FinishDetail::ExitCode:
            if (auto const code = exitCode.value_or(outcome.asShellExitCode()); code != 0)
                return std::to_string(code);
            break;
        case FinishDetail::Signal:
            // The same signal the gutter's tooltip names for this record.
            return std::string { contextSignalName(signalThatEnded(exitCode, outcome)) };
    }
    return {};
}

std::string formatCommandDuration(std::chrono::steady_clock::duration duration)
{
    namespace chrono = std::chrono;
    using Duration = chrono::steady_clock::duration;

    auto const clamped = duration < Duration::zero() ? Duration::zero() : duration;
    if (clamped < chrono::seconds(1))
        return std::format("{}ms", chrono::duration_cast<chrono::milliseconds>(clamped).count());

    auto const parts = chrono::hh_mm_ss { chrono::duration_cast<chrono::seconds>(clamped) };
    if (clamped < chrono::minutes(1))
        return std::format("{}s", parts.seconds().count());
    if (clamped < chrono::hours(1))
        return std::format("{}m {:02}s", parts.minutes().count(), parts.seconds().count());
    return std::format("{}h {:02}m", parts.hours().count(), parts.minutes().count());
}

CommandBlockStore::CommandBlockStore(CommandBlockStoreLimits limits,
                                     SteadyClockRef steadyClock,
                                     core::platform::WallClockRef wallClock,
                                     CommandBlockChangeListener onRecordChanged):
    _limits { clamped(limits) },
    _steadyClock { steadyClock },
    _wallClock { wallClock },
    _onRecordChanged { std::move(onRecordChanged) }
{
}

CommandBlockId CommandBlockStore::promptStarted(int64_t headStableId,
                                                uint64_t headIdGeneration,
                                                WorkingDirectorySnapshot directory)
{
    if (auto* const open = unfinished())
    {
        if (open->state == CommandBlockState::Prompting)
        {
            _records.pop_back(); // unfinished() is always the newest record
            _changeStamps.pop_back();
        }
        else
        {
            open->state = CommandBlockState::Finished;
            open->end = CommandBlockEnd::Implicit;
            open->endWorkingDirectory = boundedWorkingDirectory(std::move(directory));
            endRun();
            stamp(open->id, StampNotice::Told);
        }
    }
    else if (!_records.empty())
    {
        // Finished by its ;D since the previous prompt, which would otherwise be the newest record.
        auto& last = _records.back();
        if (auto bounded = boundedWorkingDirectory(std::move(directory)); bounded != last.endWorkingDirectory)
        {
            last.endWorkingDirectory = std::move(bounded);
            stamp(last.id, StampNotice::Told);
        }
    }

    auto& record = mint(headStableId, headIdGeneration);
    record.promptStartedAt = _wallClock.now();
    ++_revision;
    return record.id;
}

void CommandBlockStore::commandStarted(CommandStart start)
{
    auto* record = unfinished();
    if (!record)
        record = &mint(start.headStableId, start.headIdGeneration);

    // A minted record is Prompting, so a ;C that mints one always starts it.
    auto const startsCommand = record->state == CommandBlockState::Prompting;
    auto const fillsCommandLine = record->commandLineSource == CommandLineSource::None
                                  && start.source != CommandLineSource::None && start.commandLine.has_value();
    if (!startsCommand && !fillsCommandLine)
        return; // a repeated ;C that brings nothing new

    if (startsCommand)
    {
        record->state = CommandBlockState::Running;
        record->commandStartedAt = _wallClock.now();
        record->workingDirectory = boundedWorkingDirectory(std::move(start.workingDirectory));
        _runningSince = _steadyClock.now();
        // A command context at the ;C is the command's own; beneath any other, its own may yet start.
        if (start.contextType == ContextType::Command)
            record->commandContext = start.context;
        else
            _awaitedCommandContextParent = start.context;
    }

    if (fillsCommandLine)
    {
        record->commandLine = CommandLineText { boundedCommandLine(std::move(*start.commandLine)) };
        record->commandLineSource = start.source;
    }

    stamp(record->id, StampNotice::Told);
    ++_revision;
}

std::optional<CommandBlockSummary> CommandBlockStore::commandFinished(int exitCode, ContextOutcome outcome)
{
    auto* const record = running();
    if (!record)
        return std::nullopt;

    record->state = CommandBlockState::Finished;
    record->end = CommandBlockEnd::Reported;
    record->exitCode = exitCode;
    if (outcome != ContextOutcome {})
        record->outcome = outcome;
    if (_runningSince)
        record->duration = _steadyClock.now() - *_runningSince;
    endRun();
    stamp(record->id, StampNotice::Told);
    ++_revision;
    return summaryOf(*record);
}

void CommandBlockStore::abandonRunning()
{
    auto* const record = running();
    if (!record)
        return;

    record->state = CommandBlockState::Finished;
    record->end = CommandBlockEnd::Abandoned;
    endRun();
    stamp(record->id, StampNotice::Told);
    ++_revision;
}

void CommandBlockStore::commandContextStarted(ContextId started, ContextId parent)
{
    auto* const record = running();
    if (!record || _awaitedCommandContextParent != parent)
        return;

    // The command's own context stays behind on a replica, so nothing it holds changed: no stamp.
    record->commandContext = started;
    _awaitedCommandContextParent.reset();
    ++_revision;
}

void CommandBlockStore::enrichOutcome(ContextId ended, ContextOutcome outcome)
{
    if (!ended || outcome == ContextOutcome {})
        return;

    // The most recent command, whether it still runs or has finished since -- a newer prompt may even have
    // opened -- matched by the context it ran in, never by where it stands.
    auto* const command = newestCommandIn(_records);
    if (command == nullptr || command->commandContext != ended || command->outcome != ContextOutcome {})
        return;

    command->outcome = outcome;
    stamp(command->id, StampNotice::Told);
    ++_revision;
}

void CommandBlockStore::foreignDirectoryReported()
{
    auto* const record = unfinished();
    if (!record || record->foreignDirectory == ForeignDirectory::Reported)
        return;

    record->foreignDirectory = ForeignDirectory::Reported;
    stamp(record->id, StampNotice::Told);
    ++_revision;
}

void CommandBlockStore::updateHeadPosition(CommandBlockId id, int64_t headStableId, uint64_t headIdGeneration)
{
    auto const it = positionOf(_records, id);
    if (it == _records.end()
        || (it->headStableId == headStableId && it->headIdGeneration == headIdGeneration))
        return;

    it->headStableId = headStableId;
    it->headIdGeneration = headIdGeneration;
    ++_revision;
}

HeadPlacement CommandBlockStore::placeHead(CommandBlockId id, int64_t headStableId, uint64_t headIdGeneration)
{
    auto const it = positionOf(_records, id);
    if (it == _records.end() || it->headIdGeneration == headIdGeneration)
        return HeadPlacement::Unchanged;

    it->headStableId = headStableId;
    it->headIdGeneration = headIdGeneration;
    ++_revision;
    return HeadPlacement::Placed;
}

void CommandBlockStore::headsLeft(uint64_t headIdGeneration,
                                  int64_t formerFloor,
                                  int64_t floor,
                                  std::span<CommandBlockId const> erased)
{
    auto left = false;
    for (auto const& record: _records)
    {
        if (record.headIdGeneration != headIdGeneration)
            continue;
        auto const scrolledOut = formerFloor <= record.headStableId && record.headStableId < floor;
        if (!scrolledOut && !std::ranges::binary_search(erased, record.id))
            continue;
        stamp(record.id, StampNotice::Silent);
        left = true;
    }
    if (left)
        ++_revision;
}

void CommandBlockStore::retireUnplacedHeads(uint64_t headIdGeneration, int64_t evictedStableId)
{
    auto retired = false;
    for (auto& record: _records)
    {
        if (record.headIdGeneration == headIdGeneration)
            continue;
        record.headStableId = evictedStableId;
        record.headIdGeneration = headIdGeneration;
        retired = true;
    }
    if (retired)
        ++_revision;
}

std::optional<CommandBlockSummary> CommandBlockStore::adopt(CommandBlockRecord record, AdoptMode mode)
{
    if (!record.id)
        return std::nullopt;

    // Held to the bounds a minted record is, and before the comparison below: resending an over-long
    // record is still a resend.
    auto const id = record.id;
    if (record.commandLine.raw().size() > MaxRecordedCommandLineBytes)
        record.commandLine = CommandLineText { truncatedCommandLine(record.commandLine.raw()) };
    record.workingDirectory = boundedWorkingDirectory(std::move(record.workingDirectory));
    record.endWorkingDirectory = boundedWorkingDirectory(std::move(record.endWorkingDirectory));
    record.promptStartedAt = record.promptStartedAt.transform(heldInstant);
    record.commandStartedAt = record.commandStartedAt.transform(heldInstant);
    record.duration = record.duration.transform(heldSpan);

    // Ids ascend, so one search finds both the record this one replaces and, failing that, where it goes --
    // usually the back.
    auto const at = std::ranges::lower_bound(_records, id, std::less {}, &CommandBlockRecord::id);
    auto const replaces = at != _records.end() && at->id == id;
    if (replaces && *at == record)
        return std::nullopt; // a resend, or a reattach's snapshot of what is already here
    if (!replaces && at == _records.begin() && _records.size() >= _limits.maxRecords)
        return std::nullopt; // older than all a full store keeps: the cap would evict it on arrival

    // A live delta announces a block's FIRST transition to a reported Finished. Decided, and the summary
    // taken, before the record moves in -- where it stays: the cap only ever evicts from the front, and a
    // record that would land there in a full store was refused above.
    auto const wasFinished = replaces && at->state == CommandBlockState::Finished;
    auto const announces = mode == AdoptMode::Live && record.state == CommandBlockState::Finished
                           && record.end == CommandBlockEnd::Reported && !wasFinished;
    auto summary = announces ? std::optional { summaryOf(record) } : std::nullopt;

    if (replaces)
        *at = std::move(record);
    else
    {
        _changeStamps.insert(std::next(_changeStamps.begin(), std::distance(_records.begin(), at)), 0);
        _records.insert(at, std::move(record));
        evictOverflow();
    }
    stamp(id, StampNotice::Told);

    // A mirror mints nothing itself; were it ever to, it must not reuse an id the host handed it. The
    // largest id is never minted -- mint() starts over there -- so adopting it leaves the counter at the
    // wrap, not past it.
    if (auto const value = unbox(id); value >= _nextId)
        _nextId = value == std::numeric_limits<IdValue>::max() ? value : value + 1;
    ++_revision;
    return summary;
}

void CommandBlockStore::retire(std::span<CommandBlockId const> ids) noexcept
{
    auto retired = false;
    for (auto const id: ids)
    {
        auto const it = positionOf(_records, id);
        if (it == _records.end())
            continue;
        if (std::next(it) == _records.end())
            endRun(); // what is kept about a running command belongs to the newest record alone
        auto const position = std::distance(_records.begin(), it);
        _records.erase(it);
        _changeStamps.erase(std::next(_changeStamps.begin(), position));
        retired = true;
    }
    if (!retired)
        return;
    ++_contentRevision; // nothing to stamp, and a replica still has records to drop
    ++_revision;
}

void CommandBlockStore::clear() noexcept
{
    if (_records.empty())
        return; // nothing to forget, so nothing changed
    _records.clear();
    _changeStamps.clear();
    endRun();
    ++_contentRevision; // nothing to stamp, and a replica still has records to drop
    ++_revision;
}

CommandBlockRecord const* CommandBlockStore::find(CommandBlockId id) const noexcept
{
    if (!id)
        return nullptr;
    auto const it = positionOf(_records, id);
    return it == _records.end() ? nullptr : &*it;
}

CommandBlockRecord const* CommandBlockStore::current() const noexcept
{
    return newestUnfinishedIn(_records);
}

CommandBlockId CommandBlockStore::currentId() const noexcept
{
    return _records.empty() ? CommandBlockId {} : _records.back().id;
}

CommandBlockRecord const* CommandBlockStore::lastFinished() const noexcept
{
    return newestFinishedIn(_records);
}

CommandBlockRecord const* CommandBlockStore::lastCommand() const noexcept
{
    return newestCommandIn(_records);
}

CommandBlockRecord* CommandBlockStore::unfinished() noexcept
{
    return newestUnfinishedIn(_records);
}

CommandBlockRecord* CommandBlockStore::running() noexcept
{
    auto* const record = unfinished();
    return (record && record->state == CommandBlockState::Running) ? record : nullptr;
}

CommandBlockRecord& CommandBlockStore::mint(int64_t headStableId, uint64_t headIdGeneration)
{
    // 2^32 prompts is out of reach of any session a person sits at, but not of a hostile stream replaying
    // OSC 133;A as fast as the pty delivers it. Starting over keeps the ids ascending, which find() relies
    // on. A new epoch tells whoever holds an id from before -- a stamped row -- that it names nothing now.
    if (_nextId == std::numeric_limits<IdValue>::max())
    {
        _records.clear();
        _changeStamps.clear();
        endRun();
        _nextId = 1;
        ++_idEpoch;
    }

    auto& record = _records.emplace_back();
    record.id = CommandBlockId { _nextId };
    ++_nextId;
    record.headStableId = headStableId;
    record.headIdGeneration = headIdGeneration;
    // Stamped as stamp() would, but a birth is not a change of a record anyone could have read before, so
    // the listener is not told.
    ++_contentRevision;
    _changeStamps.push_back(_contentRevision);
    evictOverflow();
    return _records.back();
}

void CommandBlockStore::endRun() noexcept
{
    _runningSince.reset();
    _awaitedCommandContextParent.reset();
}

void CommandBlockStore::evictOverflow() noexcept
{
    if (_records.size() <= _limits.maxRecords)
        return;
    auto const excess = static_cast<std::ptrdiff_t>(_records.size() - _limits.maxRecords);
    _records.erase(_records.begin(), _records.begin() + excess);
    _changeStamps.erase(_changeStamps.begin(), _changeStamps.begin() + excess);
}

void CommandBlockStore::stamp(CommandBlockId id, StampNotice notice)
{
    auto const it = positionOf(_records, id);
    ++_contentRevision;
    _changeStamps[static_cast<size_t>(std::distance(_records.begin(), it))] = _contentRevision;
    if (notice == StampNotice::Silent)
        return;

    // Here, where every change to what a record says is recorded, so no transition -- today's or a later
    // one -- can change a record without its readers hearing of it. The newest command is searched for from
    // the newest record back, and of the records the store mints only the newest can still be at its prompt,
    // so no flood of transitions makes the search longer than two.
    auto const* const command = newestCommandIn(_records);
    _onRecordChanged(
        CommandBlockChange { .id = id, .lastCommand = command != nullptr ? command->id : CommandBlockId {} });
}

} // namespace vtbackend
