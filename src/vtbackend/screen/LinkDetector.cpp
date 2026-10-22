// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/LinkDetector.hpp>

#include <vtbackend/grid/Grid.hpp>
#include <vtbackend/links/LinkMatcher.hpp>
#include <vtbackend/links/PathResolution.hpp>
#include <vtbackend/screen/PrintedLinks.hpp>

#include <core/Assert.hpp>
#include <core/async/DetachedTask.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/ResumeOn.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <limits>
#include <mutex>
#include <ranges>
#include <span>
#include <variant>

namespace vtbackend
{

namespace
{
    constexpr auto NothingPending = std::numeric_limits<int64_t>::min();

    [[nodiscard]] int64_t ticksOf(std::chrono::steady_clock::time_point time) noexcept
    {
        return time.time_since_epoch().count();
    }

    [[nodiscard]] int64_t ticksOf(std::chrono::milliseconds duration) noexcept
    {
        return std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration).count();
    }

    /// The rows @p candidate covers, as index spans, top first.
    /// @param text The scan text it was found in.
    /// @param candidate The match.
    /// @param firstLine The grid line of @p scanned's first row.
    /// @param scanned The logical line's rows, from @p firstLine on.
    [[nodiscard]] std::vector<LinkRowSpan> spansOf(ScanText const& text,
                                                   LinkCandidate const& candidate,
                                                   LineOffset firstLine,
                                                   std::span<ScannedRow const> scanned)
    {
        auto spans = std::vector<LinkRowSpan> {};
        spans.reserve(candidate.last.row - candidate.first.row + 1);
        for (auto const row: std::views::iota(candidate.first.row, candidate.last.row + 1))
        {
            auto const index = unbox<std::size_t>(text.lineOf(row) - firstLine);
            spans.push_back(LinkRowSpan {
                .row = scanned[index].id,
                .first = row == candidate.first.row ? candidate.first.column : ColumnOffset(0),
                .last = row == candidate.last.row ? candidate.last.column
                                                  : ColumnOffset::cast_from(text.columnsOf(row) - 1),
                .link = {},
            });
        }
        return spans;
    }

    /// @p link, pointing at what @p answer found.
    /// @param directories The base each candidate was joined to (PathCandidates::directories), one per
    ///                    candidate of the query @p answer answers.
    [[nodiscard]] DetectedLink placed(DetectedLink link,
                                      ResolvedPath const& answer,
                                      std::span<std::string const> directories)
    {
        link.target = resolvedPathTarget(std::move(link.target), answer, directories);
        return link;
    }

    /// Whether every one of @p rows is still in @p grid, unchanged since it was read: no change awaiting its
    /// stamp, and the revision it was read at.
    [[nodiscard]] bool isCurrent(Grid const& grid, std::span<ScannedRow const> rows)
    {
        return std::ranges::all_of(rows, [&](ScannedRow const& row) {
            auto const offset = grid.lineOffsetOf(row.id);
            return offset && isUnchangedSinceScan(grid.lineAt(*offset), row.revision);
        });
    }

    /// Whether @p index still holds every one of @p rows at the revision it was read at, as LinkIndex::add()
    /// demands.
    [[nodiscard]] bool isIndexedAsScanned(LinkIndex const& index, std::span<ScannedRow const> rows)
    {
        return std::ranges::all_of(
            rows, [&](ScannedRow const& row) { return index.scannedRevision(row.id) == row.revision; });
    }

    /// Whether @p index holds a link on any of @p rows.
    [[nodiscard]] bool hasLinksOn(LinkIndex const& index, std::span<ScannedRow const> rows)
    {
        return std::ranges::any_of(rows, [&](ScannedRow const& row) { return !index.onRow(row.id).empty(); });
    }
} // namespace

bool isUnchangedSinceScan(Line const& line, uint64_t scannedRevision) noexcept
{
    return !line.isDirty() && line.revision() == scannedRevision;
}

std::span<LinkRowSpan const> currentSpansOn(LinkIndex const& index,
                                            Grid const& grid,
                                            LineOffset line) noexcept
{
    auto const spans = index.onRow(grid.stableLineIdOf(line));
    // Most rows hold no link: those cost the one lookup, and their line is not read.
    if (spans.empty() || !isScannedAsItIs(index, grid, line))
        return {};
    return spans;
}

bool isScannedAsItIs(LinkIndex const& index, Grid const& grid, LineOffset line) noexcept
{
    auto const scanned = index.scannedRevision(grid.stableLineIdOf(line));
    return scanned && isUnchangedSinceScan(grid.lineAt(line), *scanned);
}

struct LinkDetector::State
{
    /// Where the probe job is. Only submitProbes() leaves Idle and only adoptProbes() leaves Done, so answers
    /// are taken before another job may leave its own.
    enum class ProbeStage : uint8_t
    {
        Idle = 0, ///< No job.
        InFlight, ///< A job is looking.
        Done,     ///< Its answers wait in the mailbox.
    };

    State(LinkDetectionServices services,
          std::shared_ptr<LinkPatterns const> patterns,
          LinkDetectionPolicy policy,
          std::shared_ptr<PathProbe> probe,
          std::size_t maxScanCells,
          std::function<void()> requestRedraw):
        services { services },
        patterns { std::move(patterns) },
        policy { policy },
        probe { std::move(probe) },
        maxScanCells { maxScanCells },
        requestRedraw { std::move(requestRedraw) }
    {
    }

    // Fixed at construction; read by every job.
    LinkDetectionServices services;
    std::shared_ptr<LinkPatterns const> patterns;
    LinkDetectionPolicy policy;
    std::shared_ptr<PathProbe> probe;
    std::size_t maxScanCells;

    std::mutex mutex;                      ///< Guards everything below that is not atomic.
    std::function<void()> requestRedraw;   ///< Cleared by ~LinkDetector.
    std::optional<ScanResult> scanResult;  ///< The match job's mailbox.
    std::vector<ProbeAnswer> probeAnswers; ///< The probe job's mailbox.
    std::atomic<bool> isScanInFlight = false;
    std::atomic<ProbeStage> probeStage = ProbeStage::Idle;

    /// Asks for a frame. Called with @ref mutex held, on purpose: ~LinkDetector clears the callback under it,
    /// so it waits for a delivery in flight, and no delivery reaches a destroyed terminal.
    void deliver() const
    {
        if (requestRedraw)
            requestRedraw();
    }
};

LinkDetector::LineResult LinkDetector::matchLine(State const& state,
                                                 LineJob const& line,
                                                 LinkMachine const& machine,
                                                 ScreenLinkDetection screen)
{
    auto result = LineResult { .scanned = line.scanned, .links = {}, .paths = {} };
    auto const firstLine = line.rows.front().line;
    for (auto const& text: joinLogicalLines(line.rows, line.continuesBelow, state.maxScanCells))
        for (auto& candidate:
             findLinks(text, *state.patterns, PatternSelection::ClickableOnly, LinkOverlap::Merge))
        {
            // Dropped before it is classified: a path this screen does not detect is never probed.
            if (!screen.detects(linkCategoryOf(candidate.kind, candidate.target)))
                continue;
            auto spans = spansOf(text, candidate, firstLine, line.scanned);
            auto link = DetectedLink {
                .id = {},
                .target = printedLinkTarget(candidate),
                .text = std::move(candidate.text),
                .spans = std::move(spans),
            };
            // A path the probe's cache knows is decorated with the scan rather than a probe round-trip later,
            // looked up here, off the terminal lock; a refused or a missing one is no link.
            auto classified =
                classifyPrintedLink(std::move(link.target), line.bases, machine, state.probe.get());
            if (auto* const target = std::get_if<LinkTarget>(&classified))
            {
                link.target = std::move(*target);
                result.links.push_back(std::move(link));
            }
            else if (auto* const toProbe = std::get_if<PathToProbe>(&classified))
            {
                link.target = std::move(toProbe->printed);
                result.paths.push_back(
                    PendingPath { .link = std::move(link),
                                  .rows = line.scanned,
                                  .query = std::make_shared<PathQuery const>(std::move(toProbe->query)),
                                  .directories = std::move(toProbe->directories) });
            }
        }
    return result;
}

core::async::DetachedTask LinkDetector::runMatch(std::shared_ptr<State> state, ScanBatch batch)
{
    co_await core::async::ResumeOn { state->services.matchExecutor };
    auto result = ScanResult { .key = batch.key, .lines = {} };
    result.lines.reserve(batch.lines.size());
    for (auto const& line: batch.lines)
        result.lines.push_back(matchLine(*state, line, batch.machine, batch.screen));
    auto const lock = std::scoped_lock { state->mutex };
    state->scanResult = std::move(result);
    state->isScanInFlight = false;
    state->deliver();
}

core::async::DetachedTask LinkDetector::runProbes(std::shared_ptr<State> state,
                                                  std::vector<std::shared_ptr<PathQuery const>> queries)
{
    co_await core::async::ResumeOn { state->services.probeExecutor };
    auto answers = std::vector<ProbeAnswer> {};
    answers.reserve(queries.size());
    for (auto& query: queries)
    {
        auto answer = state->probe->probe(*query);
        answers.push_back(ProbeAnswer { .query = std::move(query), .answer = std::move(answer) });
    }
    auto const lock = std::scoped_lock { state->mutex };
    state->probeAnswers = std::move(answers);
    state->probeStage = State::ProbeStage::Done;
    state->deliver();
}

LinkDetector::LinkDetector(LinkDetectionServices services,
                           std::shared_ptr<LinkPatterns const> patterns,
                           LinkDetectionPolicy policy,
                           std::function<void()> requestRedraw):
    _state { std::make_shared<State>(
        services,
        std::move(patterns),
        policy,
        std::make_shared<PathProbe>(services.fileSystem, services.clock, services.tuning.probeLimits),
        services.tuning.maxScanCells,
        std::move(requestRedraw)) },
    _tuning { services.tuning },
    _firstPending { NothingPending },
    _lastChange { NothingPending }
{
}

LinkDetector::~LinkDetector()
{
    // Taken out under the mutex, so a delivery in flight finishes first and none starts after; destroyed
    // after the unlock, so that a capture whose destructor takes a lock does not take it under the mailbox
    // mutex.
    auto requestRedraw = std::function<void()> {};
    {
        auto const lock = std::scoped_lock { _state->mutex };
        requestRedraw.swap(_state->requestRedraw);
    }
}

void LinkDetector::notePending(std::chrono::steady_clock::time_point now) noexcept
{
    auto expected = NothingPending;
    _firstPending.compare_exchange_strong(expected, ticksOf(now));
    _lastChange = ticksOf(now);
}

bool LinkDetector::isDue(std::chrono::steady_clock::time_point now) const noexcept
{
    auto const first = _firstPending.load();
    if (first == NothingPending)
        return false;
    auto const due =
        std::min(_lastChange.load() + ticksOf(_tuning.debounce), first + ticksOf(_tuning.maxWait));
    return ticksOf(now) >= due;
}

std::optional<std::chrono::milliseconds> LinkDetector::nextDue(
    std::chrono::steady_clock::time_point now) const
{
    // Two loads, without the lock: collect() may reset both in between, and either reads as nothing pending.
    auto const first = _firstPending.load();
    auto const last = _lastChange.load();
    if (first == NothingPending || last == NothingPending || _state->isScanInFlight)
        return std::nullopt;
    auto const due = std::min(last + ticksOf(_tuning.debounce), first + ticksOf(_tuning.maxWait));
    auto const wait = std::chrono::steady_clock::duration(std::max<int64_t>(0, due - ticksOf(now)));
    return std::chrono::ceil<std::chrono::milliseconds>(wait);
}

LinkIndexChange LinkDetector::onFrame(LinkSource& source,
                                      std::chrono::steady_clock::time_point now,
                                      FrameChange change)
{
    // Read before adopting: a match finishing after this leaves its result for the next frame, which its
    // redraw asks for, rather than racing a collect of this one.
    auto const scanWasInFlight = _state->isScanInFlight.load();

    auto const indexChange = adoptAll(source, now);

    auto const top = source.viewportTop();
    if (change == FrameChange::ScreenChanged || top != _lastViewportTop)
        notePending(now);
    _lastViewportTop = top;

    if (!scanWasInFlight && isDue(now))
        collect(source);
    return indexChange;
}

LinkIndexChange LinkDetector::adoptScan(Grid const& grid, std::chrono::steady_clock::time_point now)
{
    auto result = std::optional<ScanResult> {};
    {
        auto const lock = std::scoped_lock { _state->mutex };
        result.swap(_state->scanResult);
    }
    if (!result || !indexes(result->key))
        return LinkIndexChange::Unchanged; // a key change already started over

    auto changed = LinkIndexChange::Unchanged;
    for (auto& line: result->lines)
        if (adoptLine(std::move(line), grid, now) == LinkIndexChange::Changed)
            changed = LinkIndexChange::Changed;
    submitProbes();
    return changed;
}

LinkIndexChange LinkDetector::syncKey(LinkSource& source, std::chrono::steady_clock::time_point now)
{
    auto const key =
        LinkIndexKey { .page = source.linkPage(), .generation = source.linkGrid().stableIdGeneration() };
    if (indexes(key))
        return LinkIndexChange::Unchanged;
    auto const changed = _index.empty() ? LinkIndexChange::Unchanged : LinkIndexChange::Changed;
    _index.clear();
    _probing.clear();
    _awaitingProbe.clear();
    _dropped.clear();
    _indexKey = key;
    _indexKeyState = IndexKeyState::Set;
    notePending(now);
    return changed;
}

LinkIndexChange LinkDetector::adoptLine(LineResult line,
                                        Grid const& grid,
                                        std::chrono::steady_clock::time_point now)
{
    // Stale: a row changed after it was collected. Rescanned next cycle (§5.1); the links it had stay until
    // then.
    if (!isCurrent(grid, line.scanned))
    {
        notePending(now);
        return LinkIndexChange::Unchanged;
    }
    // Read as it is by scanNow() while this match ran: indexed already, its paths with the probe already.
    if (isIndexedAsScanned(_index, line.scanned))
        return LinkIndexChange::Unchanged;
    auto const hadLinks = hasLinksOn(_index, line.scanned);
    _index.publish(line.scanned, std::move(line.links));
    std::ranges::move(line.paths, std::back_inserter(_awaitingProbe));
    return hadLinks || hasLinksOn(_index, line.scanned) ? LinkIndexChange::Changed
                                                        : LinkIndexChange::Unchanged;
}

LinkIndexChange LinkDetector::adoptAll(LinkSource& source, std::chrono::steady_clock::time_point now)
{
    auto changed = syncKey(source, now);
    if (adoptScan(source.linkGrid(), now) == LinkIndexChange::Changed)
        changed = LinkIndexChange::Changed;
    if (adoptProbes() == LinkIndexChange::Changed)
        changed = LinkIndexChange::Changed;
    return changed;
}

LinkIndexChange LinkDetector::adoptFinished(LinkSource& source)
{
    // No time of its own: a key change, or a match found stale, notes a rescan at the epoch, which the next
    // frame finds due at once, as a reset wants.
    return adoptAll(source, std::chrono::steady_clock::time_point {});
}

ImmediateLinkScan LinkDetector::scanNow(LinkSource& source, LineOffset first, LineOffset last)
{
    auto result = ImmediateLinkScan { .index = adoptFinished(source), .linesRead = 0 };
    auto const screen = _state->policy.forPage(source.linkPage());
    if (!screen.detectsAny())
        return result; // nothing to find on this page: no row is read
    auto& grid = source.linkGrid();
    grid.finalizeRevisions();
    auto const extent = scanExtentOf(grid, first, last, _tuning.maxScanCells);
    // Raised, never lowered: a frame's collect may have allowed more for its window.
    _awaitingProbeLimit = std::max(_awaitingProbeLimit, awaitingProbeLimitFor(grid, extent));
    auto const jobs = collectLines(source, extent);
    result.linesRead = jobs.size();
    auto const machine = source.localMachine();
    for (auto const& job: jobs)
        if (adoptLine(matchLine(*_state, job, machine, screen), grid, {}) == LinkIndexChange::Changed)
            result.index = LinkIndexChange::Changed;
    submitProbes();
    return result;
}

LinkIndexChange LinkDetector::adoptProbes()
{
    auto done = State::ProbeStage::Done;
    if (!_state->probeStage.compare_exchange_strong(done, State::ProbeStage::Idle))
        return LinkIndexChange::Unchanged;
    auto answers = std::vector<ProbeAnswer> {};
    {
        auto const lock = std::scoped_lock { _state->mutex };
        answers.swap(_state->probeAnswers);
    }

    // An answer names its paths by the query object they share with it: no comparison of paths and no
    // PathProbe lock under the terminal lock.
    auto const queryOf = [](ProbeAnswer const& answer) {
        return answer.query.get();
    };
    std::ranges::sort(answers, std::less {}, queryOf);
    auto changed = LinkIndexChange::Unchanged;
    for (auto& path: _probing)
    {
        // Every path handed to the job was answered: the job answers every query it is given.
        auto const found = std::ranges::lower_bound(answers, path.query.get(), std::less {}, queryOf);
        Require(found != answers.end() && found->query == path.query);
        if (found->answer.kind == PathKind::Missing)
            continue;
        // Added with the revisions it was scanned at: a row changed since is refused by LinkIndex::add().
        _index.add(placed(std::move(path.link), found->answer, path.directories), path.rows);
        changed = LinkIndexChange::Changed;
    }
    _probing.clear();
    submitProbes();
    return changed;
}

void LinkDetector::prunePendingPaths()
{
    // What LinkIndex::add() would refuse: a row changed, or left the index, since the path was found. Left
    // in, a row retained out and scanned again would get its paths twice. A pruned path the job in flight
    // looks at still gets its answer, which then names no path.
    auto const stale = [this](PendingPath const& path) {
        return !isIndexedAsScanned(_index, path.rows);
    };
    std::erase_if(_probing, stale);
    std::erase_if(_awaitingProbe, stale);
    std::erase_if(_dropped, stale);
}

void LinkDetector::submitProbes()
{
    prunePendingPaths();
    // The cap drops only what no job holds yet, oldest first: what the job in flight looks at stays. What it
    // drops is kept, as many at most, for a click to look for.
    auto const room = _awaitingProbeLimit - std::min(_awaitingProbeLimit, _probing.size());
    if (_awaitingProbe.size() > room)
    {
        auto const end = _awaitingProbe.begin() + static_cast<std::ptrdiff_t>(_awaitingProbe.size() - room);
        std::move(_awaitingProbe.begin(), end, std::back_inserter(_dropped));
        _awaitingProbe.erase(_awaitingProbe.begin(), end);
        if (_dropped.size() > _awaitingProbeLimit)
            _dropped.erase(_dropped.begin(),
                           _dropped.begin()
                               + static_cast<std::ptrdiff_t>(_dropped.size() - _awaitingProbeLimit));
    }
    if (_awaitingProbe.empty())
        return;
    auto idle = State::ProbeStage::Idle;
    if (!_state->probeStage.compare_exchange_strong(idle, State::ProbeStage::InFlight))
        return; // in flight; or its answers wait for adoptProbes(), which submits again after taking them
    // Idle means adoptProbes() took the last job's answers and emptied _probing.
    Require(_probing.empty());
    auto queries = std::vector<std::shared_ptr<PathQuery const>> {};
    queries.reserve(_awaitingProbe.size());
    for (auto const& path: _awaitingProbe)
        queries.push_back(path.query);
    _probing.swap(_awaitingProbe);
    (void) runProbes(_state, std::move(queries));
}

std::size_t LinkDetector::awaitingProbeLimitFor(Grid const& grid, ScanExtent const& extent) const noexcept
{
    // At most a path in every two cells of what was read: twice that may wait.
    return _tuning.maxAwaitingProbe.value_or(unbox<std::size_t>(extent.last - extent.first + LineOffset(1))
                                             * unbox<std::size_t>(grid.pageSize().columns));
}

std::vector<LinkDetector::LineJob> LinkDetector::collectLines(LinkSource& source,
                                                              ScanExtent const& extent) const
{
    auto& grid = source.linkGrid();
    auto jobs = std::vector<LineJob> {};
    auto head = extent.first;
    while (head <= extent.last)
    {
        // One logical line: its head and the rows the grid wrapped onto it, the rows ScanRow::continuation
        // marks. Split here, before reading, so that only a line with an unscanned row is read at all.
        auto end = head;
        while (end < extent.last && grid.isLineWrapped(end + LineOffset(1)))
            ++end;
        auto const rows = std::views::iota(unbox(head), unbox(end) + 1)
                          | std::views::transform([](int row) { return LineOffset(row); });
        auto const unscanned = std::ranges::any_of(rows, [&](LineOffset row) {
            return _index.scannedRevision(grid.stableLineIdOf(row)) != grid.lineAt(row).revision();
        });
        if (unscanned)
        {
            auto job = LineJob {
                .rows = scanRowsOf(grid, head, end),
                .scanned = {},
                .continuesBelow = end == extent.last ? extent.continuesBelow : LineContinuation::No,
                .bases = source.pathBasesAt(head),
            };
            job.scanned.reserve(job.rows.size());
            for (auto const& row: job.rows)
                job.scanned.push_back(ScannedRow { .id = grid.stableLineIdOf(row.line),
                                                   .revision = grid.lineAt(row.line).revision() });
            jobs.push_back(std::move(job));
        }
        head = end + LineOffset(1);
    }
    return jobs;
}

void LinkDetector::collect(LinkSource& source)
{
    auto& grid = source.linkGrid();
    grid.finalizeRevisions();

    auto const pageLines = unbox<int>(grid.pageSize().lines);
    auto const historyTop = -boxed_cast<LineOffset>(grid.historyLineCount());
    auto const bottom = LineOffset(pageLines - 1);
    auto const top = source.viewportTop();
    auto const margin = LineOffset(pageLines * _tuning.scanMarginPages);
    auto const first = std::max(historyTop, top - margin);
    auto const last = std::min(bottom, top + LineOffset(pageLines - 1) + margin);
    // Whole logical lines, so a link wrapped across the window's edge is matched in full: no further than one
    // scan budget either way.
    auto const extent = scanExtentOf(grid, first, last, _tuning.maxScanCells);

    // Kept: the window, a margin either way, and at least every row this collect may read, so that a logical
    // line reaching beyond the margin is not forgotten and read again by every collect.
    auto const keep = LineOffset(pageLines * _tuning.retentionMarginPages);
    _index.retain(grid.stableLineIdOf(std::max(historyTop, std::min(extent.first, first - keep))),
                  grid.stableLineIdOf(std::min(bottom, std::max(extent.last, last + keep))));
    prunePendingPaths();
    _awaitingProbeLimit = awaitingProbeLimitFor(grid, extent);

    _firstPending = NothingPending;
    _lastChange = NothingPending;
    // A page that detects nothing is not read, and no job is started for it; nothing stays pending either,
    // so no frame is asked for on its account.
    auto const screen = _state->policy.forPage(source.linkPage());
    if (!screen.detectsAny())
        return;

    auto batch = ScanBatch {
        .key = LinkIndexKey { .page = source.linkPage(), .generation = grid.stableIdGeneration() },
        .machine = source.localMachine(),
        .screen = screen,
        .lines = collectLines(source, extent),
    };
    if (batch.lines.empty())
        return;
    _state->isScanInFlight = true;
    (void) runMatch(_state, std::move(batch));
}

std::optional<ScreenType> LinkDetector::indexedScreen() const noexcept
{
    return _indexKeyState == IndexKeyState::Set ? std::optional { screenTypeFromPage(_indexKey.page) }
                                                : std::nullopt;
}

LinkDetector::PendingPath const* LinkDetector::unconfirmedPathAt(int64_t row,
                                                                 ColumnOffset column) const noexcept
{
    auto const covers = [row, column](PendingPath const& path) {
        return std::ranges::any_of(path.link.spans, [row, column](LinkRowSpan const& span) {
            return span.row == row && span.first <= column && column <= span.last;
        });
    };
    for (auto const* const queue: { &_probing, &_awaitingProbe, &_dropped })
        if (auto const found = std::ranges::find_if(*queue, covers); found != queue->end())
            return &*found;
    return nullptr;
}

std::shared_ptr<PathProbe> const& LinkDetector::pathProbe() const noexcept
{
    return _state->probe;
}

LinkPatterns const& LinkDetector::patterns() const noexcept
{
    return *_state->patterns;
}

LinkDetectionPolicy const& LinkDetector::policy() const noexcept
{
    return _state->policy;
}

LinkDetectionServices const& LinkDetector::services() const noexcept
{
    return _state->services;
}

} // namespace vtbackend
