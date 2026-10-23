// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/links/LinkDetectionPolicy.hpp>
#include <vtbackend/links/LinkIndex.hpp>
#include <vtbackend/links/LinkPatterns.hpp>
#include <vtbackend/links/PathProbe.hpp>
#include <vtbackend/links/ScanText.hpp>
#include <vtbackend/screen/PrintedLinks.hpp>
#include <vtbackend/shell/CommandBlockPaths.hpp>

#include <core/async/DetachedTask.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace core::async
{
class IExecutor;
}
namespace core::platform
{
class FileSystem;
class IClock;
} // namespace core::platform

namespace vtbackend
{

class Grid;
class Line;

/// Whether @p line still holds what a scan read from it: no change awaits its stamp, and it carries the
/// revision the scan read it at. A row written since its scan holds other text at the columns its links were
/// found at, so what the scan found there no longer describes it.
/// @param line The row as its grid holds it now.
/// @param scannedRevision Line::revision() when the scan read the row.
/// @return Whether the scan's findings still describe @p line.
[[nodiscard]] bool isUnchangedSinceScan(Line const& line, uint64_t scannedRevision) noexcept;

/// The links @p index holds on row @p line of @p grid, while the row still holds what their scan read
/// (isUnchangedSinceScan()). What the renderer decorates and what the pointer hovers, so that the two agree.
/// @param index The links detected on @p grid.
/// @param grid The grid whose rows @p index names.
/// @param line A row of @p grid.
/// @return The row's spans, left to right; none when it has none, or was written since its scan.
[[nodiscard]] std::span<LinkRowSpan const> currentSpansOn(LinkIndex const& index,
                                                          Grid const& grid,
                                                          LineOffset line) noexcept;

/// Whether @p index holds row @p line of @p grid as the row is now: scanned, links or none, at its current
/// revision (isUnchangedSinceScan()). A row never scanned, or written since its scan, may hold links the
/// index does not know.
/// @param index The links detected on @p grid.
/// @param grid The grid whose rows @p index names.
/// @param line A row of @p grid.
/// @return Whether what the index says of the row is current.
[[nodiscard]] bool isScannedAsItIs(LinkIndex const& index, Grid const& grid, LineOffset line) noexcept;

/// Detection's timing and bounds (§5.2). Constructor-injected so that tests can shrink them; not user
/// configuration.
struct LinkDetectorTuning
{
    std::chrono::milliseconds debounce { 100 }; ///< Quiet time after output before a scan.
    std::chrono::milliseconds maxWait { 1000 }; ///< Longest a change waits under continuous output.
    int scanMarginPages = 1;                    ///< Scanned beyond the viewport, each way.
    int retentionMarginPages = 2;               ///< Kept indexed beyond the scan window, each way.
    PathProbeLimits probeLimits {};             ///< The probe cache.
    int motionSearchPages = 10;                 ///< How far `]x` / `[x` scan beyond the index per keypress.
    std::size_t maxScanCells = MaxScanCells;    ///< One regex input at most.
    /// How many paths may wait for the probe thread, those handed to the job in flight included. Beyond it
    /// the oldest not yet handed to a job are dropped, so a hung mount, which stops path decorations, does
    /// not also grow the queue for as long as output arrives; what the job in flight looks at is never
    /// dropped. Unset, it is the cell count of the rows the last collect scanned: a row prints at most a path
    /// in every two cells (a character and a separator), so a healthy probe loses nothing even with two
    /// scans' worth waiting.
    std::optional<std::size_t> maxAwaitingProbe {};
};

/// What passive link detection runs on, and how. Supplied by the application; absent means no passive
/// detection.
///
/// Every reference member must outlive the jobs in flight, not only the detector: ~LinkDetector does not wait
/// for them.
struct LinkDetectionServices
{
    core::async::IExecutor& matchExecutor;        ///< The app pool, shared with status-line commands.
    core::async::IExecutor& probeExecutor;        ///< One dedicated thread: a hung mount blocks only it.
    core::platform::FileSystem const& fileSystem; ///< Every existence check goes through it.
    /// Ages probe answers. The application's, not a terminal's: a probe job may outlive its terminal.
    core::platform::IClock const& clock;
    LinkDetectorTuning tuning {}; ///< Detection's timing and bounds; tests shrink them.
};

/// Which rows the stable ids of an index name: one page's, in one id generation.
///
/// Not the screen type: VT420 pages 2-15 and the alternate screen are all ScreenType::Alternate, and every
/// page's grid numbers its rows from the same ids, so only the page tells two of them apart.
struct LinkIndexKey
{
    PageIndex page {};       ///< The page the rows are on; the alternate screen is one too.
    uint64_t generation = 0; ///< Grid::stableIdGeneration() of that page's grid.

    [[nodiscard]] bool operator==(LinkIndexKey const&) const noexcept = default;
};

/// Whether the screen changed since the previous frame (Terminal's _screenDirty).
enum class FrameChange : uint8_t
{
    Unchanged = 0,
    ScreenChanged,
};

/// Whether a frame changed the index, so that the hovered link needs looking up again.
enum class LinkIndexChange : uint8_t
{
    Unchanged = 0,
    Changed,
};

/// What one LinkDetector::scanNow() did.
struct ImmediateLinkScan
{
    LinkIndexChange index = LinkIndexChange::Unchanged; ///< Whether the index changed.
    /// Logical lines read and matched; none where every row was scanned at its current revision already.
    std::size_t linesRead = 0;
};

/// The terminal as the detector reads it. Every call happens under the terminal lock: in the render fill,
/// or in a vi link motion (LinkDetector::scanNow()).
class LinkSource
{
  public:
    virtual ~LinkSource() = default;

    /// The grid of the page the user sees.
    [[nodiscard]] virtual Grid& linkGrid() = 0;
    /// Which page that is: the primary screen is page 0, the alternate screen AlternateScreenPageIndex. A
    /// switch renames every row, as every page's grid numbers its rows from the same ids.
    [[nodiscard]] virtual PageIndex linkPage() const = 0;
    /// The grid line at the top of the viewport.
    [[nodiscard]] virtual LineOffset viewportTop() const = 0;
    /// Where relative paths printed on @p row resolve (Terminal::localPathBaseAt).
    [[nodiscard]] virtual std::expected<PathBases, PathRefusal> pathBasesAt(LineOffset row) const = 0;
    /// This machine: the home directory, for `~/`, and the host name a `file://` URL of it gives.
    [[nodiscard]] virtual LinkMachine localMachine() const = 0;
};

/// Finds links in output after it has gone quiet, off the parser's path and off the terminal lock (§5).
///
/// **Threads and locks.** onFrame() runs in the render fill, under the terminal lock the fill already holds:
/// detection adds no acquisition of the TERMINAL lock anywhere. Matching runs on the match executor and path
/// checks on the probe executor; neither job takes the terminal lock. They read only what onFrame()
/// collected, and hand results back through a mailbox under the detector's own mutex. The render fill takes
/// that one leaf mutex, for O(1) a frame; PathProbe's it leaves to the jobs, which consult the cache. The
/// lock order is terminal lock, then mailbox mutex, and nothing holding the mailbox mutex takes the terminal
/// lock.
///
/// The one exception to matching on the pool is scanNow(): a vi link motion that reaches past the index
/// matches on the calling thread, under the terminal lock the key press holds, at most
/// LinkDetectorTuning::motionSearchPages pages of rows a key press, each logical line within
/// LinkDetectorTuning::maxScanCells. Its paths still go to the probe thread, and it raises the cap on paths
/// waiting for it to what it read.
///
/// **Stack.** The match job runs on the application's pool, whose threads get the platform's default stack:
/// 512 KiB at the least (macOS; glibc typically gives 8 MiB, Windows 1 MiB). No engine the patterns run on
/// lets its stack grow with the line:
///  - libstdc++'s default executor recursed once per matched character until libstdc++ 16 (150-350 bytes
///    each: a long run overflowed even 8 MiB), so LinkPatterns compiles every pattern for its breadth-first
///    executor, on every libstdc++;
///  - libc++ matches iteratively and gives up with `error_complexity`; MSVC gives up with `error_stack`.
///    findLinks() catches both, per pattern.
///
/// The test `LinkDetector.recursionHeavyUserPatternsFitTheSmallestThreadStack` runs the built-ins and
/// recursion-heavy user patterns over budget-long lines on a 512 KiB thread. What stays platform-dependent is
/// what a long line yields, not whether it is survived: libc++ and MSVC may give up on a pattern where
/// libstdc++ finds its links, and libstdc++'s breadth-first executor takes time quadratic in the line for a
/// pattern whose attempts each run to the end of the line before they fail.
///
/// **Shutdown.** ~LinkDetector does not wait for jobs in flight: they keep the shared state alive, so the
/// executors, the FileSystem and the IClock of LinkDetectionServices must outlive them. A
/// core::async::ThreadPoolExecutor that was stopped resumes submitted work inline on the caller, so a match
/// or a probe submitted during shutdown may run on the render thread, under the terminal lock.
class LinkDetector
{
  public:
    /// @param services Where matching and probing run, the filesystem probes look at, and the timing and
    ///        bounds.
    /// @param patterns The profile's table.
    /// @param policy Which links are detected on which screen; fixed for the detector's lifetime. A category
    ///        a screen does not detect is dropped as it is matched, before it is classified or probed.
    /// @param requestRedraw Asks the display for a frame. Called on job threads with the mailbox mutex
    ///        held, so it follows Terminal::Events::renderBufferUpdated()'s contract: it takes no terminal
    ///        lock, reads no terminal state, and calls neither nextRender() nor refreshRenderBuffer().
    ///        Terminal::breakLoopAndRefreshRenderBuffer() qualifies.
    LinkDetector(LinkDetectionServices services,
                 std::shared_ptr<LinkPatterns const> patterns,
                 LinkDetectionPolicy policy,
                 std::function<void()> requestRedraw);
    ~LinkDetector();
    LinkDetector(LinkDetector const&) = delete;
    LinkDetector& operator=(LinkDetector const&) = delete;
    LinkDetector(LinkDetector&&) = delete;
    LinkDetector& operator=(LinkDetector&&) = delete;

    /// One render fill, with the terminal lock held: adopts finished work, notes a change, collects due rows.
    /// @param source The terminal.
    /// @param now The terminal's current time.
    /// @param change Whether the screen changed since the previous frame.
    /// @return Whether the index changed.
    [[nodiscard]] LinkIndexChange onFrame(LinkSource& source,
                                          std::chrono::steady_clock::time_point now,
                                          FrameChange change);

    /// How long until a collect is due; nullopt when nothing is pending or a match is in flight (whose
    /// completion asks for a frame itself). Safe without the terminal lock.
    ///
    /// The display must ask after every fill. A match that completes during a fill asks for its frame then,
    /// which a display that clears its "needs a frame" flag at the end of the fill coalesces away; that fill
    /// adopts the result, and the collect it may leave due is known only from here.
    /// @param now The terminal's current time.
    /// @return The wait, zero when a collect is due already.
    [[nodiscard]] std::optional<std::chrono::milliseconds> nextDue(
        std::chrono::steady_clock::time_point now) const;

    /// Adopts what finished off the lock since the last frame, as a frame does: the match job's result and
    /// the probe's answers. vi's link motions call it before they look, so that a path confirmed since the
    /// last frame counts at once. The caller holds the terminal lock.
    /// @param source The terminal.
    /// @return Whether the index changed.
    [[nodiscard]] LinkIndexChange adoptFinished(LinkSource& source);

    /// Scans rows [@p first, @p last] now, on the calling thread, and publishes what it finds: vi's link
    /// motions reaching past the index (§7.5). Only logical lines with a row not scanned at its current
    /// revision are read; their paths are queued for the probe as the background scan queues them, and what
    /// finished off the lock is adopted first (adoptFinished()). The caller holds the terminal lock.
    /// @param source The terminal.
    /// @param first The topmost row to scan, a row of the page on display.
    /// @param last The bottommost row to scan.
    /// @return Whether the index changed, and how many logical lines were read.
    [[nodiscard]] ImmediateLinkScan scanNow(LinkSource& source, LineOffset first, LineOffset last);

    /// The links found so far, keyed by stable row id.
    /// @return The index, valid while the terminal lock is held.
    [[nodiscard]] LinkIndex const& index() const noexcept { return _index; }
    /// The screen the index describes: the type of its page.
    /// @return The screen, valid while the terminal lock is held; nullopt before the first frame.
    [[nodiscard]] std::optional<ScreenType> indexedScreen() const noexcept;
    /// Whether the index names the rows of @p key: those of its page, in its id generation.
    /// @param key A page and its grid's id generation.
    /// @return Whether it is @p key's index, valid while the terminal lock is held; false before the first
    ///         frame.
    [[nodiscard]] bool indexes(LinkIndexKey key) const noexcept
    {
        return _indexKeyState == IndexKeyState::Set && _indexKey == key;
    }
    /// How many path candidates wait for the probe thread, those it is looking at included.
    /// @return The count, valid while the terminal lock is held. After any frame it is at most the cap
    ///         (LinkDetectorTuning::maxAwaitingProbe), or what the job in flight holds if that is more.
    [[nodiscard]] std::size_t awaitingProbeCount() const noexcept
    {
        return _probing.size() + _awaitingProbe.size();
    }
    /// A path candidate waiting for its probe.
    struct PendingPath
    {
        DetectedLink link;            ///< Its target is the path as printed until the probe answers.
        std::vector<ScannedRow> rows; ///< The logical line's rows as scanned.
        std::shared_ptr<PathQuery const> query; ///< Where to look; shared with the probe job, not copied.
        std::vector<std::string> directories;   ///< Parallel to query->candidates (PathCandidates).
    };

    /// The path found at (@p row, @p column) that is not confirmed yet: handed to the probe, waiting for it,
    /// or dropped by the cap on what may wait (LinkDetectorTuning::maxAwaitingProbe). Its row is indexed as
    /// it is, so the index answers nothing there: a click is the user asking, and looks for it (§7.2).
    /// @param row The row's stable id.
    /// @param column A column of the row.
    /// @return The path; nullptr where none is. Valid while the terminal lock is held.
    [[nodiscard]] PendingPath const* unconfirmedPathAt(int64_t row, ColumnOffset column) const noexcept;

    /// The existence checks and their cache, shared with the jobs.
    /// @return The probe.
    [[nodiscard]] std::shared_ptr<PathProbe> const& pathProbe() const noexcept;
    /// The pattern table detection matches with.
    /// @return The table.
    [[nodiscard]] LinkPatterns const& patterns() const noexcept;
    /// Which links detection finds on which screen.
    /// @return The policy given at construction.
    [[nodiscard]] LinkDetectionPolicy const& policy() const noexcept;
    /// What detection runs on.
    /// @return The services given at construction.
    [[nodiscard]] LinkDetectionServices const& services() const noexcept;

  private:
    struct State; ///< The mailbox the jobs share; defined in the .cpp.

    /// Whether _indexKey names the index's rows yet.
    enum class IndexKeyState : uint8_t
    {
        None = 0, ///< Before the first frame: no key, and the index is empty.
        Set,      ///< _indexKey names the index's rows.
    };

    /// One logical line collected under the lock, matched on the pool.
    struct LineJob
    {
        std::vector<ScanRow> rows;                              ///< Top first, consecutive.
        std::vector<ScannedRow> scanned;                        ///< Parallel to rows.
        LineContinuation continuesBelow = LineContinuation::No; ///< Whether the row below the last goes on.
        std::expected<PathBases, PathRefusal> bases;            ///< Where its relative paths resolve.
    };

    /// What one collect hands the match job.
    struct ScanBatch
    {
        LinkIndexKey key;           ///< The page collected from, and its id generation then.
        LinkMachine machine;        ///< This machine, as its paths and `file://` URLs are named.
        ScreenLinkDetection screen; ///< What the policy detects on the page collected from.
        std::vector<LineJob> lines;
    };

    /// What matching one logical line found.
    struct LineResult
    {
        std::vector<ScannedRow> scanned; ///< The rows, as collected.
        std::vector<DetectedLink> links; ///< To publish: URLs, custom links, paths the cache already knew.
        std::vector<PendingPath> paths;  ///< Paths the probe thread must look at first.
    };

    /// What the match job leaves in the mailbox.
    struct ScanResult
    {
        LinkIndexKey key; ///< As collected.
        std::vector<LineResult> lines;
    };

    /// One answer of the probe job.
    struct ProbeAnswer
    {
        std::shared_ptr<PathQuery const> query; ///< The query as submitted; its address names it.
        ResolvedPath answer;
    };

    /// Matches one logical line, dropping every candidate of a category @p screen does not detect before it
    /// is classified, so that no path of a screen without path detection reaches the probe.
    [[nodiscard]] static LineResult matchLine(State const& state,
                                              LineJob const& line,
                                              LinkMachine const& machine,
                                              ScreenLinkDetection screen);
    static core::async::DetachedTask runMatch(std::shared_ptr<State> state, ScanBatch batch);
    static core::async::DetachedTask runProbes(std::shared_ptr<State> state,
                                               std::vector<std::shared_ptr<PathQuery const>> queries);

    /// What the policy detects on the page @p source shows.
    [[nodiscard]] ScreenLinkDetection screenOf(LinkSource const& source) const;
    /// Whether the policy detects anything on the page @p source shows. Where it does not, nothing is noted
    /// pending, collected or read.
    [[nodiscard]] bool detectsOnPageShown(LinkSource const& source) const;
    void notePending(std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] bool isDue(std::chrono::steady_clock::time_point now) const noexcept;
    /// Starts over when the page on display or its id generation is not what the index names: a reflow, a
    /// history-limit change, a reset or a switch of page or screen renames rows (§5.1).
    /// @param now When the rescan this asks for was noted.
    /// @return Whether that dropped links.
    [[nodiscard]] LinkIndexChange syncKey(LinkSource& source, std::chrono::steady_clock::time_point now);
    /// syncKey(), then what finished off the lock: the match job's result and the probe's answers.
    /// @param now When a rescan this asks for is noted.
    /// @return Whether the index changed.
    [[nodiscard]] LinkIndexChange adoptAll(LinkSource& source, std::chrono::steady_clock::time_point now);
    [[nodiscard]] LinkIndexChange adoptScan(Grid const& grid, std::chrono::steady_clock::time_point now);
    /// Publishes what matching one logical line found and queues its paths for the probe, unless a row of it
    /// changed since it was read; then a rescan is noted, and the links it had stay until then. A line the
    /// index already holds at the revisions it was read at (scanNow() read it while this match ran) is left
    /// as it is: published twice, its paths would be probed and added twice.
    /// @return Whether the index changed.
    [[nodiscard]] LinkIndexChange adoptLine(LineResult line,
                                            Grid const& grid,
                                            std::chrono::steady_clock::time_point now);
    [[nodiscard]] LinkIndexChange adoptProbes();
    void prunePendingPaths();
    void submitProbes();
    /// How many paths may wait for the probe after reading @p extent (LinkDetectorTuning::maxAwaitingProbe).
    [[nodiscard]] std::size_t awaitingProbeLimitFor(Grid const& grid,
                                                    ScanExtent const& extent) const noexcept;
    /// One job per logical line of @p extent that holds a row not scanned at its current revision; only
    /// those are read.
    [[nodiscard]] std::vector<LineJob> collectLines(LinkSource& source, ScanExtent const& extent) const;
    void collect(LinkSource& source);

    std::shared_ptr<State> _state;
    LinkDetectorTuning _tuning;
    LinkIndex _index;
    /// Whose rows the index names, once _indexKeyState says so. Not an optional: always initialised, so that
    /// no comparison reads a disengaged payload.
    LinkIndexKey _indexKey {};
    IndexKeyState _indexKeyState = IndexKeyState::None; ///< Whether _indexKey is set.
    std::optional<LineOffset> _lastViewportTop;
    std::vector<PendingPath> _probing;       ///< Handed to the probe job in flight.
    std::vector<PendingPath> _awaitingProbe; ///< Not handed to a job yet, oldest first.
    /// Dropped by the cap, oldest first, and as many at most: a click on one looks for it
    /// (unconfirmedPathAt()).
    std::vector<PendingPath> _dropped;
    /// LinkDetectorTuning::maxAwaitingProbe, as of the last collect, or raised since by scanNow().
    std::size_t _awaitingProbeLimit = 0;
    /// steady_clock ticks; NothingPending when there is no change to scan. Atomic: nextDue() runs without the
    /// lock.
    std::atomic<int64_t> _firstPending;
    std::atomic<int64_t> _lastChange;
};

} // namespace vtbackend
