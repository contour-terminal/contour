// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Logging.hpp>
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/core/FileUrl.hpp>
#include <vtbackend/core/Hyperlink.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/RowMove.hpp>
#include <vtbackend/core/Search.hpp>
#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/core/WorkingDirectory.hpp>
#include <vtbackend/grid/Grid.hpp>
#include <vtbackend/input/ClickCounter.hpp>
#include <vtbackend/input/InputGenerator.hpp>
#include <vtbackend/input/InputHandler.hpp>
#include <vtbackend/input/vi/HintModeHandler.hpp>
#include <vtbackend/input/vi/Selector.hpp>
#include <vtbackend/input/vi/ViCommands.hpp>
#include <vtbackend/input/vi/ViInputHandler.hpp>
#include <vtbackend/links/ScanText.hpp>
#include <vtbackend/render/RenderBuffer.hpp>
#include <vtbackend/screen/Cursor.hpp>
#include <vtbackend/screen/GitHead.hpp>
#include <vtbackend/screen/Gutter.hpp>
#include <vtbackend/screen/LinkDetector.hpp>
#include <vtbackend/screen/LinkPresentation.hpp>
#include <vtbackend/screen/ProgramStatusSet.hpp>
#include <vtbackend/screen/SearchQuery.hpp>
#include <vtbackend/screen/SearchTallyWalk.hpp>
#include <vtbackend/screen/Settings.hpp>
#include <vtbackend/screen/StatusLineBuilder.hpp>
#include <vtbackend/screen/StatusLineCommandCache.hpp>
#include <vtbackend/screen/StatusLineExtensions.hpp>
#include <vtbackend/screen/TerminalClocks.hpp>
#include <vtbackend/screen/Viewport.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/shell/CommandBlockPaths.hpp>
#include <vtbackend/shell/CommandBlockRows.hpp>
#include <vtbackend/shell/Folding.hpp>
#include <vtbackend/shell/MarkArbiter.hpp>
#include <vtbackend/shell/ScrollbarMarks.hpp>
#include <vtbackend/shell/SemanticBlockTracker.hpp>
#include <vtbackend/shell/StickyHeader.hpp>
#include <vtbackend/shell/UserMarkIndex.hpp>
#include <vtbackend/vt/DesktopNotification.hpp>
#include <vtbackend/vt/MacroDefinition.hpp>
#include <vtbackend/vt/PointerShape.hpp>
#include <vtbackend/vt/ProgramStatus.hpp>
#include <vtbackend/vt/ProgressState.hpp>
#include <vtbackend/vt/Sequence.hpp>
#include <vtbackend/vt/SequenceBuilder.hpp>

#include <vtparser/Parser.hpp>

#include <vtpty/Pty.hpp>

#include <crispy/Animation.hpp>
#include <crispy/BufferObject.hpp>
#include <crispy/LifetimeAnnotatedMutex.hpp>

#include <core/Assert.hpp>
#include <core/Defines.hpp>
#include <core/Environment.hpp>
#include <core/platform/FileSystem.hpp>

#include <gsl/pointers>

#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <concepts>
#include <condition_variable>
#include <deque>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <stack>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <tracy/Tracy.hpp>

namespace vtbackend
{

class Screen;

/// Result of applying a smooth-scroll pixel delta.
enum class SmoothScrollResult : uint8_t
{
    Applied,         ///< Pixel delta was applied to the viewport.
    Disabled,        ///< Smooth scrolling is disabled, or the page on display has no history to scroll.
    InvalidCellSize, ///< Cell pixel size is zero or negative; cannot compute scroll.
};

/// Why Terminal::scrollToCommandBlockHead() could not bring a block's head to the top.
enum class CommandBlockJumpError : uint8_t
{
    UnknownBlock = 0, ///< No record carries the id: it never existed, or the store's bound dropped it.
    HeadEvicted,      ///< The record outlives its head row, which the scrollback no longer holds.
    /// The record's head was placed in another row generation than this grid's -- a daemon mirror's record
    /// before the rows that head it arrive -- so where it is is not known yet, which is not the same as gone.
    HeadNotPlaced,
    /// The viewport cannot bring the primary page's history into view: another page is on display, or the
    /// cursor is on one while the primary page is shown (DECPCCM reset).
    NotScrollable,
};

/// Why Terminal::revealStableLineAtTop() could not bring a line to the top.
enum class RevealError : uint8_t
{
    Evicted = 0, ///< The id names no row any more: the scrollback dropped it.
    /// The id was read in another stable-id generation than the grid's: after a reflow or a reset it names
    /// some other row, and scrolling there would be worse than doing nothing.
    OtherGeneration,
    /// The viewport cannot bring the primary page's history into view: another page is on display, or the
    /// cursor is on one while the primary page is shown (DECPCCM reset).
    NotScrollable,
};

/// What new output does to a view scrolled back into the history. @see Terminal::Events::outputFollowing
enum class OutputFollowing : uint8_t
{
    Stay = 0, ///< The view stays where it was put: output scrolling the page keeps it on the lines it shows.
    Follow,   ///< The view comes down to the output, where Settings::autoScrollOnUpdate asks for it.
};

/// Platform-independent scroll gesture phase, mapped from Qt::ScrollPhase.
enum class ScrollPhase : uint8_t
{
    NoPhase,  ///< No phase information (e.g. mouse wheel events on X11).
    Begin,    ///< Touchpad gesture started (finger touched).
    Update,   ///< Touchpad gesture is ongoing (finger moving).
    End,      ///< Touchpad gesture ended (finger lifted).
    Momentum, ///< OS-generated momentum phase (we implement our own).
};

/// Whether a terminal replicates one a daemon hosts, and if so where the daemon runs: what every locality
/// the terminal judges is relative to.
enum class ReplicaRole : uint8_t
{
    None = 0,         ///< No replica: its own application writes to it, and localities are judged here.
    SameMachine = 1,  ///< A daemon's terminal replicated on the daemon's machine: judged as its own.
    OtherMachine = 2, ///< A daemon's terminal replicated on another machine: no directory its application
                      ///< reports is one here, so every locality is ContextLocality::Foreign.
};

/// The answer to an application's request for the host-writable status line (DECSSDT 2).
/// @see Terminal::answerHostWritableStatusLineRequest
enum class HostWritableStatusLineAnswer : uint8_t
{
    Denied = 0, ///< The line stays hidden; what the application writes there may go to the window title.
    Shown,      ///< The line is put up.
};

/// Helping information to visualize IME text that has not been committed yet.
struct InputMethodData
{
    // If this string is non-empty, the IME is active and the given data
    // shall be displayed at the cursor's location.
    std::string preeditString;
};

// {{{ Modes
/// API for setting/querying terminal modes.
///
/// This abstracts away the actual implementation for more intuitive use and easier future adaptability.
class Modes
{
  public:
    void set(AnsiMode mode, bool enabled)
    {
        if (enabled)
            _ansi.fetch_or(bitOf(mode), std::memory_order_relaxed);
        else
            _ansi.fetch_and(~bitOf(mode), std::memory_order_relaxed);
    }

    bool set(DECMode mode, bool enabled)
    {
        if (_decFrozen[static_cast<size_t>(mode)])
        {
            errorLog()("Attempt to change frozen DEC mode {}. Ignoring.", mode);
            return false;
        }
        auto& word = _dec[wordOf(mode)];
        if (enabled)
            word.fetch_or(bitOf(mode), std::memory_order_relaxed);
        else
            word.fetch_and(~bitOf(mode), std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] bool enabled(AnsiMode mode) const noexcept
    {
        return (_ansi.load(std::memory_order_relaxed) & bitOf(mode)) != 0;
    }
    [[nodiscard]] bool enabled(DECMode mode) const noexcept
    {
        return (_dec[wordOf(mode)].load(std::memory_order_relaxed) & bitOf(mode)) != 0;
    }

    [[nodiscard]] bool frozen(DECMode mode) const noexcept
    {
        return _decFrozen.test(static_cast<size_t>(mode));
    }

    void freeze(DECMode mode)
    {
        if (mode == DECMode::BatchedRendering)
        {
            errorLog()("Attempt to freeze batched rendering. Ignoring.");
            return;
        }

        _decFrozen.set(static_cast<size_t>(mode), true);
        terminalLog()("Freezing {} DEC mode to permanently-{}.", mode, enabled(mode) ? "set" : "reset");
    }

    void unfreeze(DECMode mode)
    {
        _decFrozen.set(static_cast<size_t>(mode), false);
        terminalLog()("Unfreezing permanently-{} DEC mode {}.", mode, enabled(mode));
    }

    void save(std::vector<DECMode> const& modes)
    {
        for (DECMode const mode: modes)
            _savedModes[mode].push_back(enabled(mode));
    }

    void restore(std::vector<DECMode> const& modes)
    {
        for (DECMode const mode: modes)
        {
            if (auto i = _savedModes.find(mode); i != _savedModes.end() && !i->second.empty())
            {
                auto& saved = i->second;
                set(mode, saved.back());
                saved.pop_back();
            }
        }
    }

    /// Resets every mode, and forgets which were frozen and which saved: what a hard reset leaves.
    void reset()
    {
        _ansi.store(0, std::memory_order_relaxed);
        for (auto& word: _dec)
            word.store(0, std::memory_order_relaxed);
        _decFrozen.reset();
        _savedModes.clear();
    }

  private:
    [[nodiscard]] static constexpr uint32_t bitOf(AnsiMode mode) noexcept
    {
        return uint32_t { 1 } << static_cast<unsigned>(mode);
    }

    /// The DEC modes are kept 64 to a word.
    static constexpr auto DecModesPerWord = size_t { 64 };

    [[nodiscard]] static constexpr size_t wordOf(DECMode mode) noexcept
    {
        return static_cast<size_t>(mode) / DecModesPerWord;
    }

    [[nodiscard]] static constexpr uint64_t bitOf(DECMode mode) noexcept
    {
        return uint64_t { 1 } << (static_cast<size_t>(mode) % DecModesPerWord);
    }

    /// The ANSI modes, a bit each. Atomic: the GUI thread reads KAM and SRM without the terminal lock as a
    /// key is typed (Terminal::allowInput(), Terminal::flushInput()), so that typing waits for no parse,
    /// while the parser thread sets them under that lock. Relaxed, as no other state is published through
    /// them.
    std::atomic<uint32_t> _ansi = 0;
    /// The DEC modes, a bit each, atomic for the reason the ANSI modes are: a key repeat reads DECARM.
    std::array<std::atomic<uint64_t>,
               (static_cast<size_t>(DECMode::DECModeCount) + DecModesPerWord - 1) / DecModesPerWord>
        _dec {};
    std::bitset<static_cast<size_t>(DECMode::DECModeCount)> _decFrozen; // DECMode
    std::map<DECMode, std::vector<bool>> _savedModes;                   // saved DEC modes
};
// }}}

/// The terminal's one active search: what is being looked for, and how.
///
/// There is exactly one per terminal, and it outlives whatever surface put it there -- closing the
/// find bar leaves the pattern (and therefore the highlights) standing, which is what lets F3 keep
/// stepping afterwards. Terminal::clearSearch() is what ends it.
struct Search
{
    std::u32string pattern;                    ///< As the user gave it; kept when it does not compile.
    SearchOptions options {};                  ///< The find bar's toggles, as the user set them.
    SearchOrigin origin = SearchOrigin::Typed; ///< Where the pattern came from.

    /// The pattern compiled under the options its origin reads it with; null for an empty pattern, one that
    /// does not compile, or one that proved too complex to match.
    ///
    /// Derived, and maintained by Terminal's setters -- never assigned from anywhere else. Shared, so that a
    /// reader working through the grid in steps keeps the query it began with.
    std::shared_ptr<SearchQuery const> query {};

    /// Why the pattern is not searched for, while it is not; nothing otherwise.
    std::optional<SearchPatternError> problem {};
};

/// What the per-keystroke probe learnt. @see Terminal::probeSearchReverse
enum class SearchProbeOutcome : uint8_t
{
    Undecided = 0, //!< Nothing within the budget, and grid beyond it: not known until a walk reads it.
    Found,         //!< A match; SearchProbe::match names it.
    Absent,        //!< The probe read the whole grid: there is no match anywhere.
};

/// The per-keystroke probe's answer.
struct SearchProbe
{
    SearchProbeOutcome outcome = SearchProbeOutcome::Undecided; //!< What it learnt.
    std::optional<CellLocation> match {};                       //!< The match, under Found.
};

/// A search tally walked in steps over one page, and the page it walks: a page switch restarts it.
struct TerminalSearchTally
{
    SearchTallyWalk walk; ///< The walk.
    PageIndex page;       ///< The page on display when the walk began.
};

/// Folds the 7-bit C1 control introducers in a terminal reply to their single-byte 8-bit forms, as
/// required when S8C1T (8-bit C1 transmission) is in effect. Every `ESC X` with X in 0x40..0x5F becomes
/// the single byte X + 0x40 -- e.g. `ESC [` -> CSI (0x9B), `ESC P` -> DCS (0x90), `ESC \` -> ST (0x9C).
/// A lone trailing ESC, or an ESC followed by a byte outside 0x40..0x5F, is passed through unchanged.
/// @param sevenBit A reply string that uses 7-bit (ESC-introduced) C1 controls.
/// @return The reply with its C1 introducers folded to single 8-bit bytes.
[[nodiscard]] std::string foldC1ControlsToEightBit(std::string_view sevenBit);

// Mandates what execution mode the terminal will take to process VT sequences.
//
enum class ExecutionMode : uint8_t
{
    // Normal execution mode, with no tracing enabled.
    Normal,

    // Trace mode is enabled and waiting for command to continue execution.
    Waiting,

    // Tracing mode is enabled and execution is stopped after each VT sequence.
    SingleStep,

    // Tracing mode is enabled, execution is stopped after queue of pending VT sequences is empty.
    BreakAtEmptyQueue,

    // Trace mode is enabled and execution is stopped at frame marker.
    // TODO: BreakAtFrame,
};

/// Whether a mouse binding took a press before the terminal's own multi-click selection saw it.
enum class MousePressClaim : uint8_t
{
    Unclaimed = 0, ///< The terminal handles the press as it always has.
    Claimed,       ///< A binding acts on it: the terminal neither selects nor starts a drag.
};

/// What the left mouse button's press does while the button is held.
enum class LeftButtonPress : uint8_t
{
    Released = 0, ///< The button is up.
    Selecting,    ///< The press went to the terminal's own selection, which a drag extends.
    Claimed,      ///< A binding claimed the press: no drag, but the action it runs may select.
};

/// Where the pointer stands, for what it hovers.
enum class PointerPlacement : uint8_t
{
    /// On no cell: beside the grid, over something drawn on top of it, outside the terminal, or nowhere yet,
    /// before the first pointer event.
    OffGrid = 0,
    OnGrid, ///< On the grid cell the last move or press named.
};

/// Where the cursor stands on the page a change of page takes it to.
enum class PageCursorPlacement : uint8_t
{
    /// The page's own cursor, where it was left: a cursor restore (DECRC, leaving the alternate screen on
    /// 1049) brings it back.
    PagesOwn = 0,
    /// The cursor moves on, with its rendition and character sets, to the same line and column (PPA, PPR,
    /// PPB).
    SameLineAndColumn,
    /// The cursor moves on, with its rendition and character sets, to the home position obeying origin mode
    /// (NP, PP).
    Home,
};

/// Which way a vi link motion goes.
enum class LinkDirection : uint8_t
{
    Forward = 0, ///< `]x`
    Backward,    ///< `[x`
};

/// What one key press of a vi link motion may still scan on the spot, whatever its count: pages that held a
/// row not scanned yet. Pages scanned already cost nothing, so that each press gets further.
struct LinkScanBudget
{
    int pages = 0; ///< Pages left; LinkDetectorTuning::motionSearchPages at the start of a press.
};

/// Asked, with a press's click count (1..ClickCounter::MaxClickCount), whether a binding takes the press.
/// @see Terminal::sendMousePressEvent
using MousePressClaimant = std::function<MousePressClaim(uint8_t clickCount)>;

// Implements Trace mode handling for the given controls.
//
// It either directly forwards the sequences to the actually current main display,
// or puts them into a pending queue if execution is currently suspend.
//
// In case of single-step, only one sequence will be handled and the execution mode put to suspend mode,
// and in case of break-at-frame, the execution will conditionally break iff the currently
// pending VT sequence indicates a frame start.
class TraceHandler: public SequenceHandler
{
  public:
    explicit TraceHandler(Terminal& terminal);

    void executeControlCode(char controlCode) override;
    void processSequence(Sequence const& sequence) override;
    void processAPC(std::string_view body) override;
    void writeText(char32_t codepoint) override;
    void writeText(std::string_view codepoints, size_t cellCount) override;
    void writeTextEnd() override;

    struct CodepointSequence
    {
        std::string_view text;
        size_t cellCount;
    };

    /// One queued `APC` body, waiting its turn behind the sequences that preceded it.
    ///
    /// Owns its bytes rather than viewing them: everything else in this queue is drained within the
    /// parse that produced it, but an APC body is held until the user steps the trace forward, long
    /// after the parser buffer it arrived in has been reused.
    struct ApplicationProgramCommand
    {
        std::string body;
    };

    using PendingSequence = std::variant<char32_t, CodepointSequence, Sequence, ApplicationProgramCommand>;
    using PendingSequenceQueue = std::deque<PendingSequence>;

    [[nodiscard]] PendingSequenceQueue const& pendingSequences() const noexcept { return _pendingSequences; }

    void flushAllPending();
    void flushOne();

  private:
    void flushOne(PendingSequence const& pendingSequence);

    /// Where a macro's body goes while one runs: straight to the display. The body is part of the one DECINVM
    /// being stepped through, and runs with it; queued instead, it would land in the very queue being
    /// flushed.
    ///
    /// Asked here, by the handler the trace modes route through, rather than by Terminal::sequenceHandler(),
    /// which the parser asks for every event it raises in every mode.
    /// @return The display while a macro runs, otherwise null: the event is queued.
    [[nodiscard]] SequenceHandler* macroBodyDisplay() const noexcept;

    gsl::not_null<Terminal*> _terminal;
    PendingSequenceQueue _pendingSequences = {};
};

struct TabsInfo
{
    struct Tab
    {
        std::optional<std::string> name;
        Color color;

        bool operator==(Tab const&) const = default;
    };

    std::vector<Tab> tabs;
    size_t activeTabPosition = 1;

    bool operator==(TabsInfo const&) const = default;
};

/// What lies at a cell for the follow gesture, found without touching the filesystem
/// (Terminal::lookUpLinkAt()).
struct LinkLookup
{
    /// An OSC 8 link: the application's own, followed as the application wrote it, a `file://` one to a
    /// program run as such. Never a link found in printed text, which is not trusted so (§7.6).
    std::shared_ptr<HyperlinkInfo const> hyperlink;
    /// Known now: an indexed link, a URL found on demand, or a path the probe cache knows.
    std::optional<LinkTarget> target;
    /// A path the probe has not answered yet: the caller has it probed, off the terminal lock.
    std::optional<PathQuery> pathToProbe;
    /// The path found at the cell, its target as printed (kind Path), whether or not it is opened: what an
    /// answer to @ref pathToProbe completes (resolvedPathTarget()), and what a refusal names. None where the
    /// cell holds no path, or a link answered from the index.
    std::optional<LinkTarget> printedPath;
    /// PathCandidates::directories of @ref pathToProbe, one per candidate: the base each joined, empty for an
    /// absolute or `~/` path. An answer's candidateIndex names its entry.
    std::vector<std::string> pathDirectories {};
    /// Why the path at the cell is not opened: it names a file on another machine.
    std::optional<PathRefusal> refusal;
};

/// Terminal API to manage input and output devices of a pseudo terminal, such as keyboard, mouse, and screen.
///
/// With a terminal being attached to a Process, the terminal's screen
/// gets updated according to the process' outputted text,
/// whereas input to the process can be send high-level via the various
/// send(...) member functions.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding) // TODO
/// The terminal is the IStatusLinePublishTarget its extensions' StatusLinePublishSink delivers to.
class Terminal: public IStatusLinePublishTarget
{
  public:
    class Events
    {
      public:
        virtual ~Events() = default;

        virtual void requestCaptureBuffer(LineCount /*lines*/, bool /*logical*/) {}
        virtual void bell() {}
        /// Another page came on display -- not the cursor's page, which with DECPCCM reset may be another.
        /// @param type The screen type of the page now on display.
        virtual void bufferChanged(ScreenType /*type*/) {}
        /// The render buffer is stale, or the next wakeup (@see Terminal::nextRender) changed.
        /// May be called from ANY thread, including a core::async executor thread with the
        /// status-line command cache's mutex held. Implementations must not take the terminal's state
        /// mutex, must not read terminal state, and must not call nextRender() or refreshRenderBuffer()
        /// synchronously; hand the work to the thread that owns it instead.
        virtual void renderBufferUpdated() {}
        virtual void screenUpdated() {}
        /// As screenUpdated(), for a frame that repainted what the screen showed already rather than
        /// output: a mirror's full replay (vthost::client::ScreenMirror::fullReplay) -- the attach, a resync.
        /// A listener that does not tell the two apart need not override it.
        virtual void screenReplayed() { screenUpdated(); }

        /// A status-line extension published a frame. Called on the extension's worker, holding no lock,
        /// under renderBufferUpdated()'s rules. For a host that renders nothing itself (the daemon), which
        /// repaints the indicator status line on its own thread then (@see
        /// Terminal::refreshIndicatorStatusLine).
        virtual void extensionFramePublished() {}

        /// What an indicator status-line item shows changed off the parser: a `{Git}` lookup found another
        /// branch. Called on the lookup's executor, holding no terminal lock, under renderBufferUpdated()'s
        /// rules, after the terminal asked for a frame. For a host that renders nothing itself (the daemon),
        /// which repaints the indicator status line on its own thread then, as for extensionFramePublished().
        virtual void indicatorStatusLineItemChanged() {}

        /// The page was resized, so any row may have moved: a change of width reflowed every row under
        /// new stable ids, and a change of height moved rows between the page and the history. What is
        /// drawn from rows' places -- the scrollbar's travel and its ticks -- is stale, even when no output
        /// follows.
        ///
        /// Raised by resizeScreen() when the page's size changed, not for a call that only brings new pixel
        /// sizes. With Terminal::_stateMutex held by its caller -- the GUI thread for a window, font or
        /// status-line change, the parser thread for DECCOLM --, and with the consequence progressChanged()
        /// documents: do not read terminal state here; defer to the GUI thread.
        virtual void pageResized() {}

        virtual FontDef getFontDef() { return {}; }
        virtual void setFontDef(FontDef const& /*fontSpec*/) {}
        virtual void copyToClipboard(std::string_view /*data*/) {}

        /// The application asked for a different mouse pointer shape, by CSS name (`OSC 22`).
        /// @see vtbackend::pointer_shape::SupportedNames.
        virtual void setPointerShape(std::string_view /*cssName*/) {}
        /// Returns the current clipboard contents, for an OSC 52 read (`OSC 52 ; Pc ; ? ST`). The base
        /// implementation returns nothing; only a frontend that permits clipboard reading answers. Reads
        /// are additionally gated by Settings::allowClipboardRead. @see Terminal::requestClipboardRead.
        virtual std::string getClipboard() { return {}; }
        virtual void openDocument(std::string_view /*fileOrUrl*/) = 0;
        /// Opens a resolved link: a path the filesystem confirmed, at its location, or a URL. The default
        /// hands its target to openDocument(), dropping the location.
        /// @param link The link; a path only once confirmed.
        virtual void openLink(LinkTarget const& link) { openDocument(link.target); }
        /// Opens what lies at a cell, as the follow gesture would there (vi's `gx`).
        /// @param gridPosition A cell of the page on display, in grid coordinates.
        virtual void followLinkAt(CellLocation /*gridPosition*/) {}
        virtual void inspect() {}
        virtual void notify(std::string_view /*title*/, std::string_view /*body*/) {}
        /// An application raised an OSC 99 notification, whatever its occasion (`o=`): whether to show it
        /// is the listener's decision, for only the frontend knows where the pane is. It is listed as live
        /// already (`p=alive`), so a listener that declines it removes it again through
        /// DesktopNotificationManager::removeActiveNotification().
        ///
        /// Raised on the parser thread with Terminal::_stateMutex ALREADY HELD.
        /// @param notification The notification as the application assembled it.
        virtual void showDesktopNotification(DesktopNotification const& /*notification*/) {}
        virtual void discardDesktopNotification(std::string_view /*identifier*/) {}

        /// The application changed its progress indicator (`OSC 9 ; 4`), or a reset withdrew it.
        ///
        /// Raised on the parser thread with Terminal::_stateMutex ALREADY HELD, and that mutex is a
        /// plain non-recursive std::mutex — so an implementation must not read terminal state here,
        /// nor synchronously re-enter code that does. Defer to the GUI thread instead.
        /// @param progress The state now in effect; ProgressState::Inactive means "show nothing".
        virtual void progressChanged(Progress /*progress*/) {}

        /// The application's hierarchical context ancestry changed (`OSC 3008`).
        ///
        /// Raised on the parser thread with Terminal::_stateMutex ALREADY HELD, with the same
        /// consequence progressChanged() above documents: do not read terminal state here, and do not
        /// synchronously re-enter code that does.
        ///
        /// Raised only when something a frontend can SEE moved. systemd re-announces the shell context
        /// on every prompt with byte-identical metadata, and notifying for that would be a redraw per
        /// prompt for no change.
        /// @param activeContext The context now in effect; a zero id means the ancestry is empty.
        virtual void contextChanged(ContextId /*activeContext*/) {}

        /// The application reported a working directory (`OSC 7`) that differs from the one before.
        ///
        /// Raised on the parser thread with Terminal::_stateMutex ALREADY HELD, with the consequence
        /// progressChanged() documents: do not read terminal state here; defer instead. A shell that
        /// re-reports the same URL at every prompt raises nothing.
        virtual void workingDirectoryChanged() {}

        /// The terminal's program-status records (OSC 7501) changed: a report, a new prompt, the process's
        /// exit, RIS, or a daemon mirror's replacement. Never raised for a change that did nothing.
        ///
        /// Raised with Terminal::_stateMutex ALREADY HELD -- on the parser thread, or on the thread that
        /// called Terminal::markProcessExited() or Terminal::replaceProgramStatus() -- with the consequence
        /// progressChanged() documents: do not read terminal state here; defer instead. @p change is a value
        /// a listener may copy across threads; the records themselves are read back under the lock through
        /// Terminal::programStatus().
        /// @param change The ids set and removed, and why.
        virtual void programStatusChanged(ProgramStatusChange const& /*change*/) {}

        /// A shell command block finished with a REPORTED end (OSC 133;D, or an OSC 3008 `end=` while OSC
        /// 3008 owns the marks) -- never for a block a newer prompt closed implicitly, such as the `ssh`
        /// the user is still inside.
        ///
        /// Raised on the parser thread with Terminal::_stateMutex ALREADY HELD, with the consequence
        /// progressChanged() documents: do not read terminal state here; defer to the GUI thread. The
        /// summary is a value copy and safe to carry across threads.
        /// @param summary What a frontend needs to announce the finish.
        virtual void commandBlockFinished(CommandBlockSummary const& /*summary*/) {}

        /// A command block's record changed what it says, for any cause: its command started, or
        /// finished -- with a reported end, or implicitly when a newer prompt superseded it --, an OSC 3008
        /// `end=` told how it ended after the fact, a repeated OSC 133;C filled in its command line, or a
        /// daemon's newer copy of it was adopted. What a view showing a record -- the gutter's block tooltip
        /// -- re-reads it on, so that none of those leaves it showing what the record used to say.
        ///
        /// Not raised for a record's birth at a new prompt, for one the store forgets (evicted, retired, or
        /// cleared by RIS), nor when only its head row moved or left the grid: only a change of what it says,
        /// so only from what changes the store, never from a read of it.
        ///
        /// commandBlockFinished() is the announcement, raised for a reported end alone; this is the change. A
        /// reported end raises this first and that second.
        ///
        /// Raised on the parser thread with Terminal::_stateMutex ALREADY HELD, with the consequence
        /// progressChanged() documents: do not read terminal state here -- the store included: what a
        /// frontend needs of it travels in @p change --; defer to the GUI thread.
        /// @param change The block whose record changed, and the newest block that ran a command.
        virtual void commandBlockChanged(CommandBlockChange const& /*change*/) {}

        /// The user placed or took off a user mark (vi `mm`) on a replica, whose marks are its host's
        /// (Terminal::toggleUserMark): the terminal leaves its line alone, and the listener asks the host,
        /// whose row brings the mark back.
        ///
        /// Raised with Terminal::_stateMutex ALREADY HELD, on the thread handling the key, with the
        /// consequence progressChanged() documents: do not read terminal state here; defer instead.
        /// @param request The row, by this terminal's own stable id, and what to do to its line.
        virtual void userMarkRequested(UserMarkRequest const& /*request*/) {}

        /// This machine's identity, for deciding whether the directory a block ran in is local.
        ///
        /// Read on the parser thread with Terminal::_stateMutex held, at every OSC 133;C: an
        /// implementation answers from data fixed at construction. The default knows nothing, which makes
        /// only an OSC 7 URL with no host provably local -- the conservative answer.
        /// @return Views that stay valid for the listener's lifetime.
        [[nodiscard]] virtual LocalIdentity localIdentity() const noexcept { return {}; }

        /// What new output does to a view scrolled back into the history. The default leaves the view where
        /// it is, as a headless terminal wants it; a frontend brings it down to the output, except where the
        /// user parked it on something they went looking for.
        ///
        /// Asked under Terminal::_stateMutex, on the thread applying the output, and only while the view is
        /// scrolled back in Insert mode with Settings::autoScrollOnUpdate set: answer from what is lock-free.
        /// @see Terminal::followOutput
        [[nodiscard]] virtual OutputFollowing outputFollowing() const noexcept
        {
            return OutputFollowing::Stay;
        }

        virtual void focusTerminalWindow() {}
        virtual void onClosed() {}
        virtual void pasteFromClipboard(unsigned /*count*/, bool /*strip*/) {}
        virtual void onSelectionCompleted() {}
        virtual void requestWindowResize(LineCount, ColumnCount) {}
        virtual void requestWindowResize(Width, Height) {}

        /// The application asked the window manager to iconify (minimize) the window, or to restore it.
        /// @see XTWINOPS `CSI 2 t` and `CSI 1 t`.
        virtual void requestWindowIconify(bool /*iconify*/) {}

        /// The application asked to move the window's top-left corner. @see XTWINOPS `CSI 3 ; x ; y t`.
        ///
        /// A window manager may refuse, or place the window elsewhere; whatever it does, the frontend
        /// reports the outcome back through Terminal::setWindowState() rather than the terminal assuming
        /// the move took.
        virtual void requestWindowMove(WindowPosition /*position*/) {}

        /// The application asked to maximize the window, or to restore it. @see XTWINOPS `CSI 9 ; Ps t`.
        ///
        /// Which size that lands on -- and which size to come back to -- is the frontend's to decide;
        /// @see WindowSizeStack, which every frontend shares rather than deciding it anew.
        virtual void requestWindowMaximize(WindowMaximize /*how*/) {}

        /// The application asked for full screen, or out of it. @see XTWINOPS `CSI 10 ; Ps t`.
        virtual void requestWindowFullScreen(WindowFullScreen /*how*/) {}

        virtual void requestShowHostWritableStatusLine() {}
        virtual void setWindowTitle(std::string_view /*title*/) {}

        /// The application set the icon (or tab) title, via `OSC 0` or `OSC 1`.
        virtual void setIconTitle(std::string_view /*title*/) {}

        virtual void setTabName(std::string_view /*title*/) {}
        /// The application assigned a window-frame color (DECAC item 2), which the GUI maps to the
        /// tab background color. A color the user picked themselves outranks it and stays visible.
        /// @param color The color to paint the frame/tab with.
        virtual void setWindowFrameColor(RGBColor /*color*/) {}
        /// The application withdrew its window-frame color (DECAC item 2 with no color parameters, or a
        /// hard reset). The frontend falls back to a user-chosen color, if any, else its own default.
        /// It never clears a color the user picked.
        virtual void resetWindowFrameColor() {}
        /// The palette's default background changed: by OSC 11 or 111, DECAC item 1, a reset (DECSTR, RIS),
        /// XTPOPCOLORS, or the frontend re-seeding the palette. Reported only on an actual change.
        ///
        /// Invoked by whichever thread holds the terminal lock -- the parser thread, or the GUI thread for a
        /// re-seed -- so an implementation must not touch GUI state here: defer to the GUI thread instead.
        /// @param color The new default background.
        virtual void defaultBackgroundColorChanged(RGBColor /*color*/) {}
        virtual void setTerminalProfile(std::string const& /*configProfileName*/) {}
        virtual void discardImage(Image const&) {}
        virtual void inputModeChanged(ViMode /*mode*/) {}

        /// The active search was cleared, by whatever route -- the NoSearchHighlight action, entering
        /// Insert mode, or the find bar's term being emptied.
        ///
        /// A frontend showing search state has to hear about it: the matches it had lit are gone, and
        /// anything displaying the pattern is now displaying one the terminal no longer has. A hook of
        /// its own rather than screenUpdated(), which reports that what is on screen changed.
        virtual void searchCleared() {}

        /// The active search's query was ended because matching it proved too complex (R-f): Search::problem
        /// says so, and nothing highlights or steps any more. Raised wherever matching met it -- a step, a
        /// count, or a frame being drawn, which may be on another thread and with the state mutex held -- so
        /// a frontend showing the search hears about it on every path, not only on the ones it drives itself.
        virtual void searchQueryEnded() {}

        /// The user asked for the search prompt -- by the SearchReverse action, or by `/` in vi mode.
        ///
        /// The terminal states the request and never learns what draws it. A frontend opens its find
        /// bar here; a headless build inherits the empty default, which is correct, since there is no
        /// interactive search without something to type into.
        virtual void searchPromptRequested() {}

        virtual void updateHighlights() {}
        virtual void playSound(Sequence::Parameters const&) {}
        /// The render buffer's cursor moved (or appeared/disappeared, including the blink phase).
        ///
        /// May be invoked on the terminal or render thread, with the terminal's state mutex ALREADY
        /// HELD on one of ensureFreshRenderBuffer()'s two paths — and that mutex is a plain
        /// non-recursive std::mutex. Implementations must not read terminal state here and must not
        /// synchronously re-enter code that does (e.g. QInputMethod::update(), whose Wayland backend
        /// re-queries the grid on the calling thread): defer to the GUI thread instead.
        virtual void cursorPositionChanged() {}
        virtual void onScrollOffsetChanged(ScrollOffset) {}
    };

    class NullEvents: public Events
    {
      public:
        void requestCaptureBuffer(LineCount /*lines*/, bool /*logical*/) override {}
        void bell() override {}
        void bufferChanged(ScreenType) override {}
        void renderBufferUpdated() override {}
        void screenUpdated() override {}
        void pageResized() override {}
        FontDef getFontDef() override { return {}; }
        void setFontDef(FontDef const& /*fontSpec*/) override {}
        void copyToClipboard(std::string_view /*data*/) override {}
        void setPointerShape(std::string_view /*cssName*/) override {}
        void openDocument(std::string_view /*fileOrUrl*/) override {}
        void inspect() override {}
        void notify(std::string_view /*title*/, std::string_view /*body*/) override {}
        void showDesktopNotification(DesktopNotification const& /*notification*/) override {}
        void discardDesktopNotification(std::string_view /*identifier*/) override {}
        void progressChanged(Progress /*progress*/) override {}
        void contextChanged(ContextId /*activeContext*/) override {}
        void workingDirectoryChanged() override {}
        void programStatusChanged(ProgramStatusChange const& /*change*/) override {}
        void commandBlockFinished(CommandBlockSummary const& /*summary*/) override {}
        void commandBlockChanged(CommandBlockChange const& /*change*/) override {}
        [[nodiscard]] LocalIdentity localIdentity() const noexcept override { return {}; }
        [[nodiscard]] OutputFollowing outputFollowing() const noexcept override
        {
            return OutputFollowing::Stay;
        }
        void focusTerminalWindow() override {}
        void onClosed() override {}
        void pasteFromClipboard(unsigned /*count*/, bool /*strip*/) override {}
        void onSelectionCompleted() override {}
        void requestWindowResize(LineCount, ColumnCount) override {}
        void requestWindowResize(Width, Height) override {}
        void requestShowHostWritableStatusLine() override {}
        void setWindowTitle(std::string_view /*title*/) override {}
        void setTabName(std::string_view /*title*/) override {}
        void setWindowFrameColor(RGBColor /*color*/) override {}
        void resetWindowFrameColor() override {}
        void defaultBackgroundColorChanged(RGBColor /*color*/) override {}
        void setTerminalProfile(std::string const& /*configProfileName*/) override {}
        void discardImage(Image const&) override {}
        void inputModeChanged(ViMode /*mode*/) override {}
        void updateHighlights() override {}
        void playSound(Sequence::Parameters const&) override {}
        void cursorPositionChanged() override {}
        void onScrollOffsetChanged(ScrollOffset) override {}
    };

    /// @param eventListener   Receives everything the terminal wants its host to do.
    /// @param env             The process environment. Read here and only here: what this terminal
    ///                        takes from it (`$HOME`, `$CONTOUR_SYNC_PTY_OUTPUT`) describes how the
    ///                        session was launched, so it is resolved once, at construction.
    /// @param fileSystem      Where a path printed on the screen is looked up: the one under the pointer,
    ///                        a hint's target. Borrowed: it must outlive the terminal. Production passes
    ///                        the native one.
    /// @param pty             The pseudo-terminal this drives.
    /// @param factorySettings The settings a hard reset (RIS) restores.
    /// @param commandServices Who runs `{Command}` status-line items, and where; nullopt for a host
    ///                        that runs none (such items then render empty). The lifetime rules are
    ///                        those of StatusLineCommandServices: the capture must outlive in-flight
    ///                        runs, so the executor is joined before the capture is destroyed.
    /// @param clocks          Every clock the terminal reads: its own time starts at what the steady one
    ///                        says now, and command durations and row birth times are read from them.
    ///                        Borrowed: the caller keeps them alive for the terminal's whole life.
    ///                        Production passes TerminalClocks::system().
    /// @param sessionTokens   Where the token of each DEC mode 2034 reader session comes from.
    ///                        Production passes randomSemanticBlockTokens().
    /// @param linkServices    Passive link detection's executors, filesystem and tuning; absent, nothing is
    ///                        detected, as on the daemon side. Its filesystem must be @p fileSystem: a path
    ///                        detection decorates and the path under the pointer are looked up in one place.
    ///                        The lifetime rules are LinkDetectionServices'. A ThreadPoolExecutor that was
    ///                        stopped runs what is submitted to it inline, so a scan or a probe started at
    ///                        shutdown may run on the render thread, under the terminal lock.
    /// @param extensionServices The status-line extensions to show and to tell about state changes; absent,
    ///                        `{Extension}` items render empty and every event hook is one untaken branch.
    ///                        The lifetime rules are StatusLineExtensionServices'.
    /// @param gitServices     Where `{Git}` looks up the branch, off the state lock; absent, `{Git}` shows
    ///                        nothing and nothing is read. The lifetime rules are GitHeadServices'. A
    ///                        ThreadPoolExecutor that was stopped runs what is submitted to it inline, so a
    ///                        lookup asked for at shutdown may run on the render thread, under the terminal
    ///                        lock, as a link probe may.
    Terminal(Events& eventListener,
             core::Environment const& env,
             core::platform::FileSystem const& fileSystem,
             std::unique_ptr<vtpty::Pty> pty,
             Settings factorySettings,
             std::optional<StatusLineCommandServices> commandServices,
             TerminalClocks clocks,
             SemanticBlockTokenSource sessionTokens,
             std::optional<LinkDetectionServices> linkServices = std::nullopt,
             std::optional<StatusLineExtensionServices> extensionServices = std::nullopt,
             std::optional<GitHeadServices> gitServices = std::nullopt);

    /// Revokes the extensions' publish sink first, waiting for a delivery in flight, so that none reaches a
    /// terminal whose members are being destroyed.
    ~Terminal() override;

    Terminal(Terminal const&) = delete;
    Terminal(Terminal&&) = delete;
    Terminal& operator=(Terminal const&) = delete;
    Terminal& operator=(Terminal&&) = delete;

    void start();

    void setRefreshRate(RefreshRate refreshRate);
    void setLastMarkRangeOffset(LineOffset value) noexcept;

    void setHistoryLimits(HistoryLimits historyLimits);
    LineCount maxHistoryLineCount() const noexcept;

    void setTerminalId(VTType id) noexcept;
    VTType terminalId() const noexcept { return _terminalId; }

    /// Sets the current operating conformance level (used by DECSCL).
    /// Does not change the maximum terminal identity reported by DA1/DA2.
    void setOperatingLevel(VTType level) noexcept;
    VTType operatingLevel() const noexcept { return _operatingLevel; }

    /// Enables or disables a single xterm title-mode feature (XTSMTITLE / XTRMTITLE).
    /// @param feature The feature to change.
    /// @param enabled true to enable, false to disable.
    void setTitleModeFeature(TitleModeFeature feature, bool enabled) noexcept
    {
        _titleModes.set(static_cast<size_t>(feature), enabled);
    }
    /// @return Whether the given title-mode feature is currently enabled.
    [[nodiscard]] bool isTitleModeEnabled(TitleModeFeature feature) const noexcept
    {
        return _titleModes.test(static_cast<size_t>(feature));
    }
    /// Resets all title-mode features to their default (all disabled), as xterm's DEF_TITLE_MODES.
    void resetTitleModes() noexcept { _titleModes.reset(); }

    /// Decodes an OSC 0/1/2 title argument per the current title modes: when TitleModeFeature::SetHex is
    /// enabled the argument is a hex string decoded to raw bytes, otherwise it is used verbatim.
    /// @param raw The title argument as received.
    /// @return The decoded title to store.
    [[nodiscard]] std::string decodeTitle(std::string_view raw) const;

    /// Encodes a title for a query report (`CSI 20 t` / `CSI 21 t`) per the current title modes: when
    /// TitleModeFeature::QueryHex is enabled the title is hex-encoded (lowercase), otherwise verbatim.
    /// @param title The stored title.
    /// @return The title as it should appear in the report.
    [[nodiscard]] std::string encodeTitleForReport(std::string_view title) const;

    /// Enters or leaves VT52 compatibility mode by switching the parser to (or from) the VT52 escape
    /// grammar. Entered by resetting DECANM (`CSI ? 2 l`), left by `ESC <`. Leaving VT52 enters ANSI
    /// mode at the VT100 level (VT52 is level-less, so there is no prior level to restore).
    void setVT52Mode(bool enable) noexcept;
    [[nodiscard]] bool isVT52Mode() const noexcept { return _parser.isVT52Mode(); }

    void setC1TransmissionMode(ControlTransmissionMode mode) noexcept { _c1TransmissionMode = mode; }
    ControlTransmissionMode c1TransmissionMode() const noexcept { return _c1TransmissionMode; }

    // {{{ Text Macros (DECDMAC / DECINVM)
    static constexpr int MaxMacroCount = MacroCount;

    /// How deep macros may nest: DEC STD 070, "Macros can invoke other macros with up to 16 levels of
    /// nesting, but cannot invoke themselves recursively."
    static constexpr int MaxMacroNestingDepth = 16;

    /// How many bytes the bodies of all macros may take between them: DEC STD 070, "6k bytes of RAM is
    /// available for storage of macro definitions" (the VT510 manual's DECDMAC: "6 Kbytes of memory").
    static constexpr size_t MacroSpaceBytes = size_t { 6 } * 1024;

    /// How many bytes of macro bodies one invocation from the stream may run, its own body and every body it
    /// invokes, however deep: Windows Terminal's bound in MacroBuffer::InvokeMacro, whose MAX_SPACE this is.
    ///
    /// Nesting alone does not bound the work: a body may invoke the next macro many times over, and each of
    /// those the one after it. The count starts afresh with each invocation from the stream, which costs the
    /// host the bytes that ask for it -- and so does not depend on how the stream was split into reads.
    static constexpr size_t MaxMacroBytesPerInvocation = size_t { 256 } * 1024;

    /// DECDMAC: defines macro @p id as @p body, after deleting what @p deletion says. A body that does not
    /// fit the room then left in MacroSpaceBytes is ignored whole, as DEC STD 070 has it; an empty one
    /// defines nothing.
    /// @param id The macro's id; one outside [0, MaxMacroCount) defines and deletes nothing.
    /// @param deletion Which definitions go first (Pdt).
    /// @param body The decoded body.
    void defineMacro(int id, MacroDeletion deletion, std::string body);

    /// The room left for macro bodies, which DECMSR reports.
    /// @return MacroSpaceBytes less the bytes the defined bodies take.
    [[nodiscard]] size_t macroSpaceAvailable() const noexcept;

    /// The room a DECDMAC defining macro @p id has for its body, once @p deletion has deleted what it does.
    /// @param id The macro being defined.
    /// @param deletion Which definitions go first (Pdt).
    /// @return macroSpaceAvailable() and what the deletion frees.
    [[nodiscard]] size_t macroSpaceFor(int id, MacroDeletion deletion) const noexcept;

    /// The checksum of the macro definitions, which DECCKSR reports.
    /// @return Every byte of every definition, negated and summed in 16 bits.
    [[nodiscard]] uint16_t macroChecksum() const noexcept;

    /// DECINVM: runs the macro @p id where the invocation stands, as though its body had been received in
    /// its place (DEC STD 070): during a parse, once the main parser has finished the DECINVM. Ignored for a
    /// macro not defined, one already running (recursion), past MaxMacroNestingDepth, or whose body would
    /// take the invocation from the stream it runs under past MaxMacroBytesPerInvocation.
    /// @param id The macro to run.
    void invokeMacro(int id);

    /// Whether a macro body is being run right now.
    [[nodiscard]] bool isMacroRunning() const noexcept { return !_runningMacros.empty(); }

    /// Clears all defined macros. The macro bodies running meanwhile end there, every one of them: Windows
    /// Terminal's ClearMacrosIfInUse voids them along with the definitions.
    void clearMacros() noexcept;

    /// Returns the macro body for the given ID, or nullopt if not defined.
    [[nodiscard]] std::optional<std::string_view> macroBody(int id) const noexcept
    {
        if (auto const it = _macros.find(id); it != _macros.end())
            return it->second;
        return std::nullopt;
    }
    // }}}

    // {{{ User-Defined Keys (DECUDK)
    /// Programs user-defined keys from DECUDK DCS payload.
    /// @param clearAll if true, all existing UDKs are cleared before loading.
    /// @param locked if true, keys cannot be reprogrammed after this call.
    /// @param data the raw payload of `Ky1/St1;Ky2/St2;...` pairs.
    void programUDK(bool clearAll, bool locked, std::string_view data);

    /// Returns the programmed string for a UDK key ID, or nullopt if not programmed.
    ///
    /// Read under the input lock, which the parser thread programs the keys under: a function key looks its
    /// string up without the state lock, so as not to wait for a parse.
    [[nodiscard]] std::optional<std::string> udkString(int keyId) const
    {
        auto const _ = std::scoped_lock { _inputMutex };
        if (auto const it = _userDefinedKeys.find(keyId); it != _userDefinedKeys.end())
            return it->second;
        return std::nullopt;
    }

    /// Checks if a UDK is defined for the given function key and returns its string.
    /// @param key the Key enum value (e.g., Key::F6)
    /// @return the programmed string or nullopt if no UDK is defined.
    [[nodiscard]] std::optional<std::string> udkStringForKey(Key key) const;

    /// Clears all user-defined keys.
    void clearUDKs()
    {
        auto const _ = std::scoped_lock { _inputMutex };
        _userDefinedKeys.clear();
        _udkLocked = false;
    }
    // }}}

    // {{{ DRCS (DECDLD — Dynamically Redefinable Character Sets)
    struct DRCSGlyph
    {
        int width = 0;               ///< Glyph width in pixels
        int height = 0;              ///< Glyph height in pixels
        std::vector<uint8_t> bitmap; ///< Row-major pixel data (1=set, 0=clear)
    };

    struct DRCSCharset
    {
        std::unordered_map<int, DRCSGlyph> glyphs; ///< Position (0x21-0x7E) → glyph data
    };

    /// Defines DRCS glyphs from DECDLD DCS payload.
    void defineDRCS(int fontNumber,
                    int startingCharacter,
                    int eraseControl,
                    int charMatrixWidth,
                    int fontWidth,
                    int textOrFullCell,
                    int charMatrixHeight,
                    int charsetSize,
                    std::string_view designator,
                    std::string_view data);

    /// Returns the DRCS charset for the given font number, or nullptr.
    [[nodiscard]] DRCSCharset const* drcsCharset(int fontNumber) const noexcept
    {
        if (auto const it = _drcsCharsets.find(fontNumber); it != _drcsCharsets.end())
            return &it->second;
        return nullptr;
    }

    /// Clears all DRCS charsets and designator mappings.
    void clearDRCS() noexcept
    {
        _drcsCharsets.clear();
        _drcsDesignatorMap.clear();
    }

    /// Maps a DRCS designator string (from DECDLD Dscs field) to a font number.
    [[nodiscard]] std::optional<int> drcsDesignatorToFont(std::string_view designator) const noexcept
    {
        auto const key = std::string(designator);
        if (auto const it = _drcsDesignatorMap.find(key); it != _drcsDesignatorMap.end())
            return it->second;
        return std::nullopt;
    }

    /// Creates an RGBA ImageFragment from a DRCS glyph bitmap using the given foreground color.
    [[nodiscard]] std::shared_ptr<RasterizedImage> createDRCSImage(DRCSGlyph const& glyph,
                                                                   RGBColor foregroundColor);
    // }}}

    // {{{ DEC Locator (DECELR / DECLRP / DECSLE / DECRQLP)
    enum class LocatorCoordUnit : uint8_t
    {
        CharacterCells, ///< Coordinates in character cell units (default)
        DevicePixels,   ///< Coordinates in device pixel units
    };

    /// Whether the locator reports, and for how long: DECELR's Ps.
    enum class LocatorReporting : uint8_t
    {
        Off = 0,    ///< No reports (Ps 0, and the default).
        Continuous, ///< Reports until DECELR turns them off (Ps 1).
        OneShot,    ///< One report, after which reporting is off (Ps 2).
    };

    struct LocatorState
    {
        LocatorReporting reporting = LocatorReporting::Off;
        LocatorCoordUnit coordUnit = LocatorCoordUnit::CharacterCells;
        bool reportButtonDown = true;
        bool reportButtonUp = true;
    };

    /// DECELR: enables or disables DEC Locator reporting, in the unit it selects. An undefined Ps or Pu
    /// ignores the whole sequence (DEC STD 070 ch. 13: "Undefined values for Ps or Pu causes the entire
    /// sequence to be ignored").
    /// @param ps 0 disables the locator, 1 enables it, 2 enables it for one report (one shot).
    /// @param pu 0 (or omitted) and 2 report in character cells, 1 in device pixels.
    void setLocatorMode(int ps, int pu) noexcept;

    /// Selects which locator events generate reports.
    void selectLocatorEvents(std::span<int const> params) noexcept;

    /// A button going down or up, as the DEC locator reports it.
    enum class LocatorEvent : uint8_t
    {
        ButtonDown = 0, ///< DECLRP's event 2, 4 or 6, by the button.
        ButtonUp,       ///< DECLRP's event 3, 5 or 7, by the button.
    };

    /// Sends a DEC locator report (DECLRP) of the pointer to the PTY: `CSI Pe ; Pb ; Pr ; Pc ; Pp & w`, Pb
    /// the buttons held down, Pr and Pc in the unit DECELR selected (cells or device pixels, from 1) and Pp
    /// the page on display. For Pe 0 it is `CSI 0 & w` alone (DEC STD 070 ch. 13: "If Pe is 0 or omitted,
    /// all other parameters shall not be sent"), and so it is while the pointer, or in device pixels its
    /// pixel, is off the grid, as xterm answers a position outside its window; DEC STD 070's DECRQLP would
    /// answer that with event 1 and the coordinates omitted. In one shot mode (DECELR Ps 2) the report, any
    /// of these, disables the locator.
    /// @param event Pe, the event code: 0 for an unavailable locator, 1 for a request, 2 to 7 for a button.
    void sendLocatorReport(int event);

    /// Requests the current locator position.
    void requestLocatorPosition();

    /// Returns the current locator state.
    [[nodiscard]] LocatorState const& locatorState() const noexcept { return _locatorState; }

    /// Resets locator state (called by soft reset).
    void resetLocator() noexcept { _locatorState = {}; }

    /// Reports a button going down or up to the host, as DECLRP, while the locator is on and DECSLE selects
    /// the event. Keeps the buttons held down, which every report names, also while the locator is off.
    /// @param event  Whether the button went down or up.
    /// @param button The button; one the locator has no code for is not reported.
    /// @return Whether the event was reported, and is therefore not the application's.
    [[nodiscard]] Handled handleLocatorMouseEvent(LocatorEvent event, MouseButton button);
    // }}}

    // {{{ Image canvas size
    //
    // Two layered authorities, so that "what the frontend allows" and "what the application asked
    // for" cannot be confused:
    //
    //   ceiling   -- the hard cap, derived from the monitor. Only the frontend sets it.
    //   effective -- what images are actually clamped to. An application may lower it via
    //                XTSMGRAPHICS, never raise it above the ceiling.
    //
    // These deliberately do not share a name: the previous setMaxImageSize() overload pair differed
    // only in arity, and the one-argument form silently set the effective size alone -- leaving the
    // ceiling stale and, on the config-reload path, the canvas at 0x0.

    /// Sets the hard cap on image size. This is the only entry point for the frontend.
    ///
    /// Deliberately does NOT touch what an application negotiated: this fires whenever the display
    /// changes -- a window dragged to another monitor, a session re-attached to a display on a tab
    /// switch or split rebuild -- and resetting the effective size here raised the canvas back to the
    /// full monitor behind an application's back, contradicting the reply it had already cached.
    /// @param ceiling the monitor-derived maximum.
    void setImageCanvasCeiling(ImageSize ceiling) noexcept { _settings.maxImageSize = ceiling; }

    /// @return the hard cap an application may not exceed.
    [[nodiscard]] ImageSize imageCanvasCeiling() const noexcept { return _settings.maxImageSize; }

    /// Clamps @p requested to the ceiling. The single clamp rule; component-wise by construction.
    /// @param requested the size an application asked for.
    /// @return the largest permitted size not exceeding @p requested.
    [[nodiscard]] ImageSize clampedImageCanvasSize(ImageSize requested) const noexcept
    {
        return vtpty::min(requested, _settings.maxImageSize);
    }

    /// Records the canvas size an application negotiated. For XTSMGRAPHICS only.
    ///
    /// The raw request is kept, not the clamped result: the request is the application's standing
    /// wish, and clamping is the ceiling's business at the moment the size is read. Keeping the
    /// clamped value instead would silently make a ceiling that happened to be small when the
    /// request arrived permanent.
    /// @param requested the size an application asked for.
    /// @return the size actually applied.
    ImageSize setEffectiveImageCanvasSize(ImageSize requested) noexcept
    {
        _negotiatedImageCanvasSize = requested;
        return maxImageSize();
    }

    /// Returns the canvas to following the ceiling, as XTSMGRAPHICS' reset-to-default asks.
    /// @return the size now applied.
    ImageSize resetEffectiveImageCanvasSize() noexcept
    {
        _negotiatedImageCanvasSize.reset();
        return maxImageSize();
    }

    /// @return the size images are currently clamped to.
    ///
    /// Derived rather than stored: the effective size is a function of the ceiling and of what the
    /// application asked for, and both change independently. Caching it meant every change to either
    /// had to remember to recompute it, and the frontend's did not.
    [[nodiscard]] ImageSize maxImageSize() const noexcept
    {
        return _negotiatedImageCanvasSize ? clampedImageCanvasSize(*_negotiatedImageCanvasSize)
                                          : _settings.maxImageSize;
    }
    // }}}

    // {{{ Modes handling
    bool isModeEnabled(AnsiMode m) const noexcept { return _modes.enabled(m); }
    bool isModeEnabled(DECMode m) const noexcept { return _modes.enabled(m); }

    /// Whether rendering is currently suppressed by synchronized output (DECSET 2026).
    ///
    /// Distinct from `isModeEnabled(DECMode::BatchedRendering)`, which reports the mode as the
    /// application set it and is what DECRQM must answer. This asks the narrower question the
    /// render paths actually care about, and answers false once the block has outlived
    /// `Settings::synchronizedOutputTimeout`, so a block left open cannot freeze the display.
    [[nodiscard]] bool isRenderingSuppressed() const noexcept;
    void setMode(AnsiMode mode, bool enable);
    void setMode(DECMode mode, bool enable);
    void saveModes(std::vector<DECMode> const& modes) { _modes.save(modes); }
    void restoreModes(std::vector<DECMode> const& modes) { _modes.restore(modes); }
    void freezeMode(DECMode mode, bool enable)
    {
        setMode(mode, enable);
        _modes.freeze(mode);
    }
    void unfreezeMode(DECMode mode) { _modes.unfreeze(mode); }
    // }}}

    void setTopBottomMargin(std::optional<LineOffset> top, std::optional<LineOffset> bottom);
    void setLeftRightMargin(std::optional<ColumnOffset> left, std::optional<ColumnOffset> right);

    void moveCursorTo(LineOffset line, ColumnOffset column);

    void setGraphicsRendition(GraphicsRendition rendition);
    void setForegroundColor(Color color);
    void setBackgroundColor(Color color);
    void setUnderlineColor(Color color);
    void setHighlightRange(HighlightRange range);

    // {{{ cursor
    /// Clamps given logical coordinates to margins as used in when DECOM (origin mode) is enabled.
    // [[nodiscard]] CellLocation clampToOrigin(CellLocation coord) const noexcept
    // {
    //     return { std::clamp(coord.line, LineOffset { 0 }, _margin.vertical.to),
    //              std::clamp(coord.column, ColumnOffset { 0 }, _margin.horizontal.to) };
    // }

    [[nodiscard]] LineOffset clampedLine(LineOffset line) const noexcept
    {
        return std::clamp(line, LineOffset(0), boxed_cast<LineOffset>(_settings.pageSize.lines) - 1);
    }

    [[nodiscard]] ColumnOffset clampedColumn(ColumnOffset column) const noexcept
    {
        return std::clamp(column, ColumnOffset(0), boxed_cast<ColumnOffset>(_settings.pageSize.columns) - 1);
    }

    [[nodiscard]] CellLocation clampToScreen(CellLocation coord) const noexcept
    {
        return { .line = clampedLine(coord.line), .column = clampedColumn(coord.column) };
    }

    // Tests if given coordinate is within the visible screen area.
    // Exclusive on both axes; see Screen::contains() for why the column bound is not `<=`.
    [[nodiscard]] constexpr bool contains(CellLocation coord) const noexcept
    {
        return LineOffset(0) <= coord.line && coord.line < boxed_cast<LineOffset>(_settings.pageSize.lines)
               && ColumnOffset(0) <= coord.column
               && coord.column < boxed_cast<ColumnOffset>(_settings.pageSize.columns);
    }

    /// Whether the caret -- the terminal cursor, the active position on the cursor's page -- is in view: its
    /// page is the page on display, the main display is selected (not the status line DECSASD selects), and
    /// its line is visible in the viewport. With DECPCCM reset, DEC STD 070 has "The cursor is not shown".
    ///
    /// Not noexcept: the fold projection this goes through is built lazily, and building it allocates.
    /// @return true while the caret is in view.
    [[nodiscard]] bool isCursorInViewport() const
    {
        return _cursorPage == _displayedPage && _activeStatusDisplay == ActiveStatusDisplay::Main
               && viewport().isLineVisible(currentScreen().cursor().position.line);
    }
    // }}}

    [[nodiscard]] constexpr ImageSize cellPixelSize() const noexcept { return _cellPixelSize; }
    constexpr void setCellPixelSize(ImageSize cellPixelSize) { _cellPixelSize = cellPixelSize; }

    /// @return Where and how the window sits on the screen, as the frontend last reported it.
    [[nodiscard]] constexpr WindowState const& windowState() const noexcept { return _windowState; }

    /// The frontend reports that the window moved, was iconified, or landed on a different screen.
    constexpr void setWindowState(WindowState state) noexcept { _windowState = state; }

    /// @return The size of the screen the window is on, in pixels.
    ///
    /// Falls back to the window's own size when the frontend has no screen to speak of: a headless
    /// terminal is exactly as large as the display it does not have, which is a more useful answer to
    /// `CSI 15 t` than zero, and keeps "maximize" (a resize to the screen's size) meaningful.
    [[nodiscard]] ImageSize screenPixelSize() const noexcept
    {
        auto const screen = _windowState.screenPixelSize;
        return (unbox(screen.width) != 0 && unbox(screen.height) != 0) ? screen : pixelSize();
    }

    /// @return The size of the screen the window is on, in character cells.
    [[nodiscard]] PageSize screenPageSize() const noexcept
    {
        auto const screen = screenPixelSize();
        auto const cell = cellPixelSize();
        if (unbox(cell.width) == 0 || unbox(cell.height) == 0)
            return totalPageSize();

        return PageSize { .lines = LineCount::cast_from(unbox(screen.height) / unbox(cell.height)),
                          .columns = ColumnCount::cast_from(unbox(screen.width) / unbox(cell.width)) };
    }

    /// The terminal's clock: the time the frame being built is built at. @see tick()
    [[nodiscard]] std::chrono::steady_clock::time_point currentTime() const noexcept
    {
        return _currentTime.load(std::memory_order_relaxed);
    }

    /// The last output of a `{Command}` status-line item; never blocks (the cache refreshes it off thread).
    /// @param item The status-line command item.
    /// @return Its last displayed text; empty before the first result, or without command services.
    [[nodiscard]] std::string statusLineCommandOutput(StatusLineDefinitions::Command const& item) const;

    /// The latest frame of an `{Extension}` status-line item, as text and SGR; never blocks. Within one
    /// repaint of the indicator status line every item naming the same extension reads the same frame.
    /// @param item The status-line extension item.
    /// @return Its frame's text; empty before the first frame, or without extension services.
    [[nodiscard]] std::string statusLineExtensionText(StatusLineDefinitions::Extension const& item) const;

    /// Tells the terminal that an extension published a frame, or changed its subscriptions: refreshes the
    /// cached subscriptions and asks for a repaint of the status line.
    ///
    /// Called on an extension worker, through the StatusLinePublishSink of the extension services. Takes no
    /// lock (the state lock may be held by the thread that is waiting for the worker), and touches only
    /// atomics and the cross-thread wake-up, as onStatusLineCommandCompleted() does.
    void statusLineExtensionsPublished() noexcept override;

    /// Tells the terminal that the IStatusLineExtensions it was constructed with now forwards to other
    /// extensions (a configuration reload replaced them): reads the take-over name anew, drops the take-over
    /// frame it cached, tells the new extensions the status line's size, and re-reads their subscriptions.
    /// Call with the state lock held, on the thread that replaced them; a no-op without extension services.
    void statusLineExtensionsReplaced();

    /// Does for the status-line extensions what a frame refresh does, for a host that renders nothing itself
    /// (the daemon): tells them the state they newly follow, and repaints the indicator status line while
    /// they draw it (@see indicatorStatusLineDrivenByExtensions) -- whether this terminal shows it or not, as
    /// its clients may -- so that its row can be read off indicatorStatusLineDisplay(). Call with the state
    /// lock held.
    void refreshIndicatorStatusLine();

    /// @return Whether the indicator status line shows what only this terminal's extensions draw: a take-over
    ///         extension, or an `{Extension}` item of its template. Call with the state lock held.
    [[nodiscard]] bool indicatorStatusLineDrivenByExtensions() const noexcept;

    /// Shows @p row as the indicator status line in place of its template and of any take-over, until told
    /// none: what a mirror shows of the row its host drew. Call with the state lock held.
    /// @param row The row to show, fitted to the status line; none for the terminal's own again.
    void setMirroredIndicatorStatusLine(std::optional<Line> row);

    /// Tells the status-line extensions the metrics of the font this terminal's cells are drawn in: on every
    /// change of font size and of pixel density, and once its host first knows them. vtbackend draws no
    /// font; its host does. Every extension is told, whether it follows the font or not, for its own
    /// surface takes its cell size from it; the terminal remembers them, to tell extensions loaded or
    /// subscribing later (@see statusLineExtensionsReplaced). Unchanged metrics are not told again. Call
    /// with the state lock held; without extension services, one untaken branch.
    /// @param metrics The font's size and the size of one cell in device pixels.
    void setStatusLineFontMetrics(StatusLineFontMetrics metrics);

    /// Tells the status-line extensions that the pane's foreground process changed. vtbackend does not
    /// watch the process; its host does, and tells each name once: the terminal remembers it, to tell
    /// extensions that follow it later (@see statusLineExtensionsReplaced). Call with the state lock held.
    /// @param name The process's name.
    void foregroundProcessChanged(std::string_view name);

    /// Tells the status-line extensions that a program set a user variable (OSC 1337 ; SetUserVar), and
    /// remembers its latest value to tell extensions that follow them later -- for the first
    /// StatusLineExtensionServices::maxUserVars names only. Call with the state lock held.
    /// @param name The variable's name.
    /// @param base64Value Its value, as the program sent it: decoded only if an extension subscribed.
    void userVarChanged(std::string_view name, std::string_view base64Value);

    /// @p when in local civil time, through the converter the terminal was constructed with.
    /// @see TerminalClocks::localTime
    [[nodiscard]] std::chrono::local_seconds localTimeOf(
        std::chrono::system_clock::time_point when) const noexcept
    {
        return _clocks.localTime(when);
    }

    /// The current time in local civil time, read from the wall clock and through the converter the
    /// terminal was constructed with.
    /// @return The wall clock's present instant, in local civil time to the second.
    /// @see TerminalClocks::wall, TerminalClocks::localTime
    [[nodiscard]] std::chrono::local_seconds localTimeNow() const noexcept
    {
        return _clocks.localTime(_clocks.wall.now());
    }

    /// @return The wall clock's present instant, from the clock the terminal was constructed with.
    /// @see TerminalClocks::wall
    [[nodiscard]] std::chrono::system_clock::time_point wallTimeNow() const noexcept
    {
        return _clocks.wall.now();
    }

    /// Retrieves reference to the underlying PTY device.
    [[nodiscard]] vtpty::Pty& device() noexcept { return *_pty; }
    [[nodiscard]] vtpty::Pty const& device() const noexcept { return *_pty; }

    [[nodiscard]] PageSize pageSize() const noexcept { return _pty->pageSize(); }

    /// Returns the total page size (main page + status line), read lock-free from the atomic mirror so the
    /// render thread observes a consistent (non-torn) value even while resizeScreen() updates it. Callers
    /// already holding _stateMutex observe the same value (it is written in lockstep with
    /// _settings.pageSize).
    [[nodiscard]] PageSize totalPageSize() const noexcept
    {
        return _atomicTotalPageSize.load(std::memory_order_acquire);
    }

    [[nodiscard]] ImageSize pixelSize() const noexcept { return cellPixelSize() * totalPageSize(); }

    /// How many lines a status line takes, the indicator and the host-writable one alike. The status screens
    /// are made this tall, and statusLineHeight() answers from it rather than from their grids.
    static constexpr auto StatusLineHeight = LineCount(1);

    /// The indicator status line's page: one row, as wide as the gutter and the page together, so its text
    /// spans the whole bar and its middle is centred on it (#2139). The host-writable status line is the
    /// application's, and keeps the page's width.
    /// @return Its page size.
    [[nodiscard]] PageSize indicatorStatusLinePageSize() const;

    /// How many lines the status line on display takes: none without one. From the status display type alone,
    /// which is race-free to read, as the renderer asks each frame without the state lock; the status
    /// screens' grids are rewritten by a resize.
    /// @return The status line's height.
    [[nodiscard]] LineCount statusLineHeight() const noexcept
    {
        return _statusDisplayType.get() == StatusDisplayType::None ? LineCount(0) : StatusLineHeight;
    }

    /// The main page's size, read race-free: the total page size without the status line, which is what
    /// the main page is laid out at. Unlike the PTY's page size (@see pageSize()), which another thread
    /// rewrites as it resizes, and which stops following the window once the PTY has closed.
    /// @return The main page's size.
    [[nodiscard]] PageSize mainPageSize() const noexcept { return totalPageSize() - statusLineHeight(); }

    /// The screen row the MAIN page starts on.
    ///
    /// Screen rows count from the top of everything drawn, so a status line positioned at the top
    /// occupies the first rows and the grid begins below them. Every coordinate that crosses the
    /// boundary between "a row on the display" and "a row of the grid" has to apply this shift --
    /// the render buffer when it places cells, and the GUI's hit-tests when they map a pixel back --
    /// which is why it is stated once here rather than re-derived at each of them.
    ///
    /// @return The first screen row of the main page; zero unless a status line sits above it.
    [[nodiscard]] LineOffset mainPageTopRow() const noexcept
    {
        return _settings.statusDisplayPosition == StatusDisplayPosition::Top
                   ? statusLineHeight().as<LineOffset>()
                   : LineOffset(0);
    }

    /// Clamps a requested total page size to the minimum the backend can accept for the currently
    /// visible status line, applying exactly the rule resizeScreen() enforces internally.
    ///
    /// The main page is derived as `total - statusLineHeight()`, so the total must leave room for at
    /// least one main-display line on top of the status line(s). Frontend callers use this to keep their
    /// own bookkeeping (resize early-outs, renderer geometry) in agreement with what resizeScreen() will
    /// actually apply, rather than re-deriving the `statusLineHeight() + 1` rule at each call site.
    /// @param requested The requested total page size (main page + status line).
    /// @return The requested size raised to at least `statusLineHeight() + 1` lines and 1 column.
    [[nodiscard]] PageSize clampedTotalPageSize(PageSize requested) const noexcept
    {
        requested.lines = std::max(requested.lines, statusLineHeight() + LineCount(1));
        requested.columns = std::max(requested.columns, ColumnCount(1));
        return requested;
    }

    /// Resizes the terminal screen to the given amount of grid cells with their pixel dimensions.
    /// Important! In case a status line is currently visible, the status line count is being
    /// accumulated into the screen size, too.
    void resizeScreen(PageSize totalPageSize, std::optional<ImageSize> pixels = std::nullopt);

    /// Resizes the terminal screen to @p totalPageSize without changing the cell size, and drops the
    /// selection.
    ///
    /// This is what a sequence that changes the page size on its own authority wants (DECCOLM, and RIS
    /// undoing it): the cells keep their size, the child is still told a pixel geometry, and the
    /// selection -- which is anchored to columns the new page may not have -- goes away.
    void resizeScreenKeepingCellSize(PageSize totalPageSize);

    void clearScreen();

    void setMouseProtocolBypassModifiers(Modifiers value) { _settings.mouseProtocolBypassModifiers = value; }
    void setMouseBlockSelectionModifiers(Modifiers value) { _settings.mouseBlockSelectionModifiers = value; }

    /// Applies a profile's link pattern table to a running terminal. The caller holds the terminal lock.
    ///
    /// A post-construction setter on purpose: live reconfiguration is the feature. A profile switch or a
    /// config reload re-applies the profile to live sessions, so the table cannot be fixed at construction.
    ///
    /// A new table replaces the link detector rather than changing it, and the links found so far go with it
    /// until the next quiet time. The table already in use changes nothing, which matters: a profile without
    /// patterns of its own applies the same built-in table on every reload and profile switch.
    /// @param patterns The compiled table; never null.
    void setLinkPatterns(std::shared_ptr<LinkPatterns const> patterns);

    /// Applies a profile's link detection policy to a running terminal. The caller holds the terminal lock.
    ///
    /// A post-construction setter on purpose, as setLinkPatterns() is: live reconfiguration is the feature.
    /// A new policy replaces the link detector, which fixes its policy for its lifetime; the links found so
    /// far go with it until the next quiet time, and the hovered link is forgotten. An equal policy changes
    /// nothing. A policy that detects nothing anywhere leaves no detector at all.
    /// @param policy Which links detection finds on which screen.
    void setLinkDetectionPolicy(LinkDetectionPolicy policy);

    /// The links detected on the page on display. Does NOT take the lock; callers hold it.
    /// @return The index; nullptr without detection, and while the index still describes another page, or
    ///         the rows of this one before a reflow or a reset renamed them: until the next frame after a
    ///         switch, a link there would open what is not on screen.
    [[nodiscard]] LinkIndex const* detectedLinks() const noexcept;

    /// The link detector, for the frontend's lookups. Does NOT take the lock; callers hold it.
    /// @return The detector; nullptr without detection services, with no clickable pattern, or with a policy
    ///         that detects nothing on any screen.
    [[nodiscard]] LinkDetector* linkDetector() noexcept { return _linkDetector.get(); }

    /// The detected link covering a cell of the page on display. The caller holds the lock.
    ///
    /// A row written since its scan holds other text at the columns its links were found at, so it answers
    /// none until it is scanned again.
    /// @param gridPosition A cell of the page on display, in grid coordinates.
    /// @return The link, valid until the next frame; nullptr where none is, or none is known yet.
    [[nodiscard]] DetectedLink const* detectedLinkAt(CellLocation gridPosition) const;

    /// What one key press of a vi link motion may scan on the spot.
    /// @return LinkDetectorTuning::motionSearchPages pages; none without detection.
    [[nodiscard]] LinkScanBudget linkScanBudget() const noexcept;

    /// The start of the next (or previous) detected link from @p from, in reading order (§7.5). The index
    /// answers for the rows it holds as they are now; a row between @p from and its answer that it does not
    /// (never scanned, or written since its scan) is scanned on the spot first, a page at a time from the
    /// first such row, publishing what it holds. A path counts once its probe has confirmed it. The caller
    /// holds the lock.
    /// @param from A cell of the page on display, in grid coordinates.
    /// @param direction Which way to look.
    /// @param budget What this key press may still scan; spent by the pages that held rows to read.
    /// @return The link's first cell; nullopt when there is none within reach, or no detection.
    [[nodiscard]] std::optional<CellLocation> linkStartFrom(CellLocation from,
                                                            LinkDirection direction,
                                                            LinkScanBudget& budget);

    /// Opens the link at @p gridPosition through the frontend (vi's `gx`), once the lock is released.
    /// @param gridPosition A cell of the page on display, in grid coordinates.
    void followLinkAt(CellLocation gridPosition);

    /// The detected link under the pointer as of the last pointer event, or the last frame that changed the
    /// index; none when there was none. Safe from any thread, without the lock. A scroll or new output moves
    /// the link under a still pointer without an event, so this can be stale, as hoveringHyperlinkId() can: a
    /// frame looks up the pointer's cell itself, and a follow gesture uses the live
    /// detectedLinkAt(currentMouseGridPosition()).
    /// @return The link's id.
    [[nodiscard]] LinkId hoveringLinkId() const noexcept { return _hoveringLinkId.load(); }

    /// Applies a profile's hyperlink_decoration.mode to a running terminal.
    ///
    /// A post-construction setter on purpose: live reconfiguration is the feature. A config reload or the
    /// settings page changes the mode of live sessions; the initial value arrives through Settings.
    /// @param mode When links are decorated.
    void setLinkDecorationMode(LinkDecorationMode mode);

    /// Applies the modifiers of the FollowHyperlink mouse binding to a running terminal.
    ///
    /// A post-construction setter on purpose: live reconfiguration is the feature. A config reload can rebind
    /// the gesture; the initial value arrives through Settings.
    /// @param modifiers The binding's modifiers; none when no binding follows links.
    void setLinkFollowModifiers(std::optional<Modifiers> modifiers);

    /// Records which chord modifiers are held. Called by the frontend for modifier-only key presses and
    /// releases, by mouse events, and with none on focus loss.
    /// @param held The chord now held.
    /// @return Whether that changes how links are presented; the frontend then re-decides the pointer shape.
    [[nodiscard]] LinkRevealChange updateHeldModifiers(Modifiers held);

    /// Whether the follow gesture's modifiers are held (Settings::linkFollowModifiers).
    [[nodiscard]] FollowModifiers followModifiers() const noexcept;

    /// Where the pointer counts as being when it is over a link: Elsewhere while an application takes clicks
    /// (mouse tracking on, the bypass modifier not held), so that the link stays at rest.
    [[nodiscard]] LinkPointer hoverPointer() const noexcept;

    /// Whether the pointer is over a link, an OSC 8 one or a detected one, as of the last pointer event
    /// (hoveringHyperlinkId(), hoveringLinkId()): what linkPointerShape() decides for. Stale as they are
    /// once output or a scroll moves the page under a still pointer, so a follow gesture asks
    /// lookUpLinkAt() instead.
    /// @return True when there is one.
    [[nodiscard]] bool isPointerOverLink() const noexcept
    {
        return isMouseHoveringHyperlink() || _hoveringLinkId.load() != LinkId {};
    }

    /// The pointer shape the link under the pointer asks for, an OSC 8 link or a detected one, as of the last
    /// pointer event (hoveringHyperlinkId(), hoveringLinkId()); Default when there is none.
    [[nodiscard]] LinkPointerShape linkPointerShape() const noexcept;

    // {{{ input proxy
    using Timestamp = std::chrono::steady_clock::time_point;
    Handled sendKeyEvent(Key key, KeyboardModifiers modifiers, KeyboardEventType eventType, Timestamp now);
    Handled sendCharEvent(char32_t ch,
                          KeyIdentity keyIdentity,
                          KeyboardModifiers modifiers,
                          KeyboardEventType eventType,
                          Timestamp now);
    /// Handles a mouse press, first offering it, with its click count, to @p claimant.
    ///
    /// The claimant is asked only when the press would reach the terminal's own selection -- no mouse
    /// protocol, or one the bypass modifier overrides -- so a binding never takes a click an application
    /// asked for. A claimed press starts no selection and no drag; everything else (hyperlink hover,
    /// DEC locator, passive tracking) proceeds as for any press.
    /// @param uiHandledHint A pre-existing bool; turning it into an enum means turning every caller at once.
    /// @param claimant Asked at most once, under the caller's lock; it must not take the lock itself. An
    ///                 empty one claims nothing.
    /// @return Whether the press was handled by the application.
    Handled sendMousePressEvent(Modifiers modifiers,
                                MouseButton button,
                                CellLocation newPosition,
                                PixelCoordinate pixelPosition,
                                bool uiHandledHint,
                                MousePressClaimant const& claimant = {});
    void sendMouseMoveEvent(Modifiers modifiers,
                            CellLocation newPosition,
                            PixelCoordinate pixelPosition,
                            bool uiHandledHint);

    /// Takes the pointer off the grid: it now stands on no cell -- over the gutter, over the sticky header
    /// drawn on top of row 0, or outside the terminal. Nothing is hovered there, so the hyperlink it was on
    /// stops being drawn as hovered, and nothing answers for the pointer's cell, until the next move or press
    /// lands on one -- the cell it left included. Nothing is reported to the application.
    void sendMouseLeaveEvent();

    Handled sendMouseReleaseEvent(Modifiers modifiers,
                                  MouseButton button,
                                  PixelCoordinate pixelPosition,
                                  bool uiHandledHint);

    /// Sets the cell that this terminal's top-left cell stands for in the grid the application sees.
    ///
    /// Zero for a terminal that runs its own application. A mirror of a daemon-hosted grid larger than
    /// this terminal shows a window into it (vthost::client::viewportOrigin), so a cell here is the cell
    /// @p origin further down and to the right there: every cell position reported to the application in
    /// a mouse press, move or release has @p origin added. Runtime state, set on every update the mirror
    /// applies -- the origin follows the cursor.
    /// @param origin The grid cell this terminal's top-left cell shows.
    void setReportedCellOrigin(CellLocation origin) noexcept { _reportedCellOrigin = origin; }

    /// @return The cell this terminal's top-left cell stands for in the application's grid.
    /// @see setReportedCellOrigin
    [[nodiscard]] CellLocation reportedCellOrigin() const noexcept { return _reportedCellOrigin; }
    bool sendFocusInEvent();
    bool sendFocusOutEvent();
    void sendPaste(std::string_view text); // Sends verbatim text in bracketed mode to application.

    /// Whether @p text would reach the application exactly as it is if pasted now (sendPaste()).
    ///
    /// Inside a bracketed paste, where the application asked for one, the paste drops ESC
    /// (InputGenerator::generatePaste); without one, a shell reads TAB and a line break as keys. A text that
    /// loses a character either way -- a hint's path, a command line run again -- names another file or runs
    /// another command, and is not to be pasted at all.
    /// @param text The text to paste.
    /// @return Whether sanitizing it for the way it would arrive leaves it unchanged.
    [[nodiscard]] bool pastesUnchanged(std::string_view text) const;

    /// Asks the frontend to paste its clipboard. @see Events::pasteFromClipboard.
    ///
    /// Asked from the key path, which holds the state lock, the request goes out once that lock is released:
    /// the frontend reads its clipboard, a round trip to whichever process owns it, and the parser thread
    /// must not wait for that.
    /// @param count How many times to paste it.
    /// @param strip Whether to strip the text: a pre-existing bool, as the frontend's hook has it.
    void sendPasteFromClipboard(unsigned count, bool strip);
    void sendRawInput(std::string_view text);

    void inputModeChanged(ViMode mode);

    /// Asks the frontend to open its search prompt. @see Events::searchPromptRequested.
    void requestSearchPrompt() { _eventListener.searchPromptRequested(); }
    void updateHighlights() { _eventListener.updateHighlights(); }
    void playSound(vtbackend::Sequence::Parameters const& params) { _eventListener.playSound(params); }

    /// Notifies the frontend that the application assigned a window-frame (tab) color (DECAC item 2).
    /// @param color The color to paint the frame/tab with.
    void setWindowFrameColor(RGBColor color) { _eventListener.setWindowFrameColor(color); }
    /// Notifies the frontend that the window-frame (tab) color was reset to the host default.
    void resetWindowFrameColor() { _eventListener.resetWindowFrameColor(); }
    /// Notifies the frontend of the palette's current default background.
    /// The caller holds the terminal lock and calls this only when the default background changed.
    void notifyDefaultBackgroundColorChanged()
    {
        _eventListener.defaultBackgroundColorChanged(_colorPalette.defaultBackground);
    }

    bool applicationCursorKeys() const noexcept { return _inputGenerator.applicationCursorKeys(); }
    bool applicationKeypad() const noexcept { return _inputGenerator.applicationKeypad(); }

    bool hasInput() const;

    /// Writes the input queued so far to the PTY, or, under InputWriter::ReaderThread on any thread but the
    /// PTY's reader, wakes the reader to write it. Under the state lock, sent once it is released.
    void flushInput();

    /// Names @p reader as the thread that reads the PTY, before it first reads: under
    /// InputWriter::ReaderThread the only one that writes to it from then on. Its first read names it as
    /// well; named as it starts, no input queued in between is written by the thread queuing it.
    /// @param reader The thread that reads the PTY.
    void setReaderThread(std::thread::id reader) noexcept
    {
        _readerThread.store(reader, std::memory_order_relaxed);
    }

    /// Returns a copy of the input queued but not yet flushed to the PTY.
    [[nodiscard]] std::string peekInput() const;
    // }}}

    /// Writes a given VT-sequence to screen.
    void writeToScreen(std::string_view vtStream);

    /// Echoes @p bytes onto our own screen, as SRM (reset) asks for.
    ///
    /// The bytes cannot always be parsed on the spot: a flush can run while a parse is on this thread's
    /// stack -- where the parse lets go of the state lock mid-batch, as a smooth-scrolling linefeed does --
    /// and the parser is not safe to re-enter. Those bytes are deferred to processPendingLocalEcho()
    /// instead, which runs once the parse has unwound.
    /// This is what xterm does with its deferred area. @see Terminal::flushInput().
    void echoLocally(std::string_view bytes);

    /// Parses the local echo that echoLocally() deferred while the parser was running.
    ///
    /// Requires _stateMutex to be held and the parser to not be running on this thread.
    void processPendingLocalEcho();

    /// Parses @p vtStream into the current screen, splitting it across PTY buffer objects as needed.
    ///
    /// Requires _stateMutex to be held. Marks the parser as running for as long as it is on the stack,
    /// so that a reply issued from a sequence handler defers its echo rather than re-entering here.
    void parseFragmentChunked(std::string_view vtStream);

    /// Writes a given VT-sequence to screen - but without acquiring the lock (must be already acquired).
    /// This version of the function is used to write to the status line and should not be used by the shell.
    void writeToScreenInternal(Screen& screen, std::string_view vtStream);

    // viewport management
    [[nodiscard]] Viewport& viewport() noexcept { return _viewport; }
    [[nodiscard]] Viewport const& viewport() const noexcept { return _viewport; }

    // {{{ Output folding (OSC 133 semantic blocks)
    //
    // Folding is a PROJECTION applied between the grid and the render pass, and nothing else: the grid,
    // the page size and every VT semantic are untouched, so a collapsed block changes what the user sees
    // and never what the shell is told. The two places that must agree about it are the row list handed
    // to Grid::render and Viewport's coordinate translation -- disagreement there misplaces a selection.

    /// Which folds the user has collapsed. Mutable, because collapsing one is a user action.
    [[nodiscard]] FoldState& foldState() noexcept { return _foldState; }
    [[nodiscard]] FoldState const& foldState() const noexcept { return _foldState; }

    /// The foldable regions currently within reach, most recent first.
    ///
    /// Re-derived from the marks rather than stored, and cached until the grid moves underneath it:
    /// a fold's EXISTENCE follows from the shell's marks, and only whether the user collapsed it is
    /// worth keeping (@see FoldState).
    [[nodiscard]] std::span<FoldRange const> foldRanges() const;

    /// The grid rows the viewport shows, top first -- or EMPTY when that is simply the contiguous page.
    ///
    /// Empty is the fast path and the common one: with nothing collapsed the linear walk is already
    /// right, so no projection is built and no ranges are scanned for. Every caller must treat empty as
    /// "unfolded" rather than as "no rows".
    ///
    /// @note The span is valid until the grid, the viewport or the fold state changes -- it views a
    ///       cache, and a change rebuilds it. Within one render frame none of the three moves, so a
    ///       caller may hold it across the coordinate translations that read it in turn.
    [[nodiscard]] std::span<LineOffset const> foldProjection() const;

    /// The screen row @ref foldProjection's FIRST row is drawn on.
    ///
    /// NEGATIVE while smooth scrolling supplies rows beyond the page: those belong above it, so the last
    /// row still lands on the bottom. Zero otherwise -- including when collapsed folds hide more rows
    /// than the history can backfill, where the walk ran out of grid at the top, there is nothing above
    /// what it found, and the page is short at the BOTTOM instead, exactly as a terminal that has not
    /// filled its page already looks.
    ///
    /// Grid::render places the row list by exactly this rule, and Viewport's coordinate translation
    /// reads it from here rather than assuming row 0 -- those two disagreeing is precisely what
    /// misplaces a selection.
    ///
    /// @return The screen row of the projection's first row; 0 when nothing is folded.
    [[nodiscard]] LineOffset foldProjectionTopRow() const;

    /// How many rows collapsed folds currently hide, over the whole addressable grid.
    ///
    /// What the viewport's scroll bounds are short by: scrolling counts VISIBLE rows, and a collapsed
    /// block removes its output from that count.
    [[nodiscard]] LineCount hiddenLineCount() const;

    /// Whether folding applies to the page currently on display.
    ///
    /// The DISPLAYED page, which is what fillRenderBufferInternal() keys on when it hands Grid::render
    /// a folded row list -- every other page, the alternate screen among them, is drawn contiguously.
    /// The projection, the gutter and the render pass all read this one predicate so they cannot
    /// disagree; a viewport translating coordinates through rows the render pass never drew would
    /// misplace the cursor and every selection on that page.
    ///
    /// Deliberately NOT isAlternateScreen(), which follows the CURSOR page: with DECPCCM reset the two
    /// diverge, and a terminal showing page 0 while the cursor sits on another would fold what it drew
    /// and then deny having done so.
    [[nodiscard]] bool foldingAppliesToDisplayedPage() const noexcept
    {
        return _displayedPage == PageIndex(0);
    }

    /// Whether a line of the primary screen's scrollback can be brought into view: that screen is the page
    /// on display. The viewport, the window onto the page on display, scrolls exactly while page 1 is.
    ///
    /// The one question the scrollbar's ticks (scrollbarMarkInputs()) and the reveal a click on one performs
    /// (revealStableLineAtTop()) both ask, so a tick is offered exactly where a click on it lands. With
    /// DECPCCM reset the cursor may write to another page meanwhile, which changes neither.
    [[nodiscard]] bool canRevealScrollback() const noexcept { return foldingAppliesToDisplayedPage(); }

    /// Drops fold state the grid can no longer account for: heads evicted from the scrollback, and
    /// everything at all when a reflow destroyed row identity wholesale.
    ///
    /// Reached from foldRanges() and ensureFoldProjection(), which between them front EVERY read of the
    /// fold state and every mutation of it -- rather than from each caller that might have a stale one.
    /// It was called from the render pass alone, which left a resize snapping the Vi cursor, and a
    /// viewport change between two frames, reading ids minted by the previous grid generation.
    ///
    /// const, and the state it reconciles is mutable, for the reason the projection beside it is: this
    /// is cache coherency, not a change of what the user asked to collapse. Self-guarding, because the
    /// per-cell coordinate translation reaches it -- the guard is what runs, not the work.
    void refreshFoldState() const;

    /// Discards the cached fold ranges, a shell mark (ShellMarkLineFlags) having been stamped or cleared.
    ///
    /// The grid's own identity cannot stand in for this: a mark arriving on a page that has not
    /// scrolled moves neither the generation nor the stable base, so a cache keyed on those alone would
    /// go on reporting the ranges from before the command that has just finished.
    /// The bump is the whole invalidation: the revision is a field of both cache keys, so neither can
    /// hit again until the caches are rebuilt against the new one.
    void invalidateFoldRanges() noexcept { ++_semanticMarkRevision; }

    /// A scroll on the primary screen is about to move or drop what rows of command blocks hold under their
    /// stable ids (@see RowMove): the heads among those ids are looked at again on the next read of
    /// commandBlocks(), as after an erase.
    /// @param move What the scroll does, in the ids as they stand before it.
    void commandBlockRowsMoved(RowMove const& move) noexcept;

    /// A scroll on the primary screen is about to move or drop rows that carry shell marks under their
    /// stable ids (@see RowMove): a collapsed fold follows its prompt line, and the cached ranges are
    /// dropped.
    /// @param move What the scroll does, in the ids as they stand before it.
    void foldRowsMoved(RowMove const& move);

    /// What the gutter shows on @p gridLine: which part of a fold's column, if any, that row carries.
    ///
    /// Every row a fold TOUCHES gets a marker, not only its head: the gutter draws a column spanning the
    /// whole block, which is what makes a fold visible as an extent rather than as a lone triangle, and
    /// what gives the mouse a target bigger than a single cell.
    ///
    /// @param gridLine A grid line (0 = page top, negative = into the scrollback).
    /// @return The marker, or FoldMarker::None when no fold reaches that line.
    [[nodiscard]] FoldMarker foldMarkerAt(LineOffset gridLine) const;

    /// The fold whose range covers @p gridLine -- its head row included.
    ///
    /// The lookup every mouse and menu affordance goes through, because a user points at the OUTPUT they
    /// want folded away, not at the prompt line it hangs off.
    ///
    /// @param gridLine A grid line (0 = page top, negative = into the scrollback).
    /// @return The covering fold, or nullopt when none reaches that line.
    [[nodiscard]] std::optional<FoldRange> foldContaining(LineOffset gridLine) const;

    /// Collapses or expands the fold covering @p gridLine, wherever in the block that line sits.
    ///
    /// Refused while folding is unavailable (Settings::foldingAvailability), as every verb here that can
    /// hide output is: the toggles and the collapses, autoCollapseOnNewPrompt() among them. Only the
    /// expands -- expandFoldContaining() and expandAllFolds() -- run regardless, so switching folding off
    /// never strands output behind it.
    /// @param gridLine A grid line (0 = page top, negative = into the scrollback).
    /// @return Whether a fold was found and toggled; false while folding is unavailable.
    bool toggleFoldContaining(LineOffset gridLine);

    /// What a left press on the gutter's block column beside @p gridLine does: toggle the fold that reaches
    /// it -- with folding available and its controls drawn (GutterSettings::foldMarkers) -- or open the menu
    /// of the block whose status mark it shows, or nothing.
    ///
    /// The one decision the hover that turns the pointer into a hand, the press and the block tooltip's hint
    /// all read, so the gutter never offers a click it then ignores, nor acts where it drew nothing to click.
    /// @param gridLine A grid line (0 = page top, negative = into the scrollback).
    /// @return What the press does; Swallow beside any page but the primary screen's.
    [[nodiscard]] BlockColumnPress blockColumnPressAt(LineOffset gridLine) const;

    /// The head of the collapsed fold whose `⋯ N lines` label is drawn over @p position: what a click on the
    /// label expands. The label is laid out where fillFoldLabels() draws it, by the same placement.
    /// @param position A cell of the main page, as the pointer names it (main-page rows).
    /// @return The fold head's grid line, or nullopt where no label is drawn.
    [[nodiscard]] std::optional<LineOffset> collapsedFoldLabelAt(CellLocation position) const;

    /// Whether the primary screen's shell shows a prompt that a command typed now would run at: the newest
    /// command block is at its prompt, and the primary screen is the one on display.
    /// @return Whether it does; false without shell integration, which reports no prompt.
    [[nodiscard]] bool shellIsPrompting() const noexcept;

    /// Expands the fold covering @p gridLine, if a collapsed one does.
    ///
    /// What FoldJumpBehavior::Expand reaches for: a jump that names a hidden target opens the block
    /// rather than refusing to go there.
    ///
    /// @param gridLine A grid line (0 = page top, negative = into the scrollback).
    /// @return Whether anything changed.
    bool expandFoldContaining(LineOffset gridLine);

    /// Collapses the fold covering @p gridLine, leaving an already-collapsed one alone -- vim's `zc`,
    /// which unlike a toggle never opens what it is pointed at. Refused while folding is unavailable.
    /// @param gridLine A grid line (0 = page top, negative = into the scrollback).
    void collapseFoldContaining(LineOffset gridLine);

    /// Collapses or expands the most recently FINISHED command's fold.
    ///
    /// This verb and the three below act only while the primary page is on display
    /// (foldingAppliesToDisplayedPage()), as foldContaining() does. It and the two collapses below are
    /// also refused while folding is unavailable.
    /// @return Whether there was one to toggle.
    bool toggleLastFold();

    /// Collapses the most recently FINISHED command's fold, leaving an already-collapsed one alone.
    /// @return Whether there was one to collapse.
    bool collapseLastFold();

    /// Collapses every foldable command within reach.
    /// @return Whether anything changed.
    bool collapseAllFolds();

    /// Expands everything that is collapsed.
    /// @return Whether anything changed.
    bool expandAllFolds();

    /// Collapses the command that just finished, when Settings::autoCollapseFoldOnNewCommand asks for it
    /// and folding is available. Called as a new prompt starts.
    void autoCollapseOnNewPrompt();

    /// The id runs collapsed folds currently hide, ascending and disjoint.
    ///
    /// Cached beside the projection rather than recomputed, because the fold-aware Vi motions below ask
    /// for it on every keystroke and it is a function of exactly the same inputs.
    [[nodiscard]] std::span<HiddenInterval const> hiddenIntervals() const;

    /// Whether a collapsed fold hides @p gridLine, so that the row is drawn nowhere.
    ///
    /// Deliberately NOT Viewport::isLineVisible(), which asks whether the row is on screen RIGHT NOW.
    /// A line merely scrolled out of the viewport needs no fold opened, and opening one for it would be
    /// a plain motion changing fold state behind the user's back.
    ///
    /// @param gridLine The grid line to test.
    /// @return true when a collapsed fold hides it.
    [[nodiscard]] bool isLineHiddenByFold(LineOffset gridLine) const;

    /// How many VISIBLE grid lines lie in the half-open range [@p from, @p to).
    ///
    /// The distance a scroll would have to travel to bring @p to where @p from is -- which is measured
    /// in rows the user can see, not in grid lines, and the two differ by whatever collapsed folds hide
    /// in between. Negative when @p to precedes @p from.
    ///
    /// @param from The first grid line of the range, counted.
    /// @param to The grid line one past its end, not counted.
    /// @return The number of visible lines between them, signed.
    [[nodiscard]] int visibleDistance(LineOffset from, LineOffset to) const;

    /// The grid line @p count VISIBLE lines from @p line in @p direction.
    ///
    /// What makes `3j` mean three lines the user can SEE. Clamped to the addressable grid, so a motion
    /// that runs off either end stops there -- on the last line before the edge that is DRAWN, since
    /// the oldest addressable line may itself sit inside a collapsed block.
    ///
    /// @param line The grid line to start from; snapped out of a collapsed fold first, if it is in one.
    /// @param count How many visible lines to travel; zero snaps without moving.
    /// @param direction Which way to travel.
    /// @return The grid line arrived at.
    [[nodiscard]] LineOffset advanceVisibleLines(LineOffset line,
                                                 int count,
                                                 VerticalDirection direction) const;

    /// The nearest grid line at or beyond @p line, in @p direction, that no collapsed fold hides.
    /// @param line The grid line to snap.
    /// @param direction Which way to leave a collapsed block.
    /// @return @p line itself when it is already visible, the nearest visible line otherwise.
    [[nodiscard]] LineOffset snapToVisibleLine(LineOffset line, VerticalDirection direction) const;

    /// Points the fold column's hover highlight at @p gridLine, or clears it with nullopt.
    ///
    /// The whole run of a fold lights up rather than one row, because the whole run is the click target
    /// -- the highlight has to say how far the thing you are about to click reaches.
    ///
    /// @param gridLine The grid line the pointer is over in the gutter, or nullopt when it left.
    void setGutterHoverLine(std::optional<LineOffset> gridLine);

    // }}}

    // {{{ Sticky command header (OSC 133 blocks)

    /// The block the sticky header covered row 0 with on the last frame, if it showed one -- what a press
    /// on row 0 lands on.
    ///
    /// Published with the frame, when it is swapped to the front, rather than decided again here: the user
    /// clicks what was DRAWN, and a fresh decision after the viewport moved under the pointer would answer
    /// for a frame nobody saw -- as would a fill that never reached the screen.
    /// Nullopt whenever the displayed page is not the primary one, so a press on a full-screen
    /// application's first row is never taken for one on a header the previous page showed.
    /// @return The block, or nullopt when the frame in front drew no header.
    [[nodiscard]] std::optional<CommandBlockId> stickyHeaderBlock() const noexcept
    {
        auto const block = _renderBuffer.frontStickyHeaderBlock.load();
        if (!foldingAppliesToDisplayedPage() || block == CommandBlockId {})
            return std::nullopt;
        return block;
    }

    /// Scrolls the viewport so block @p id's head is its top row, expanding the fold that hangs off it, and
    /// flashes the row its command was typed on (RenderBuffer::lineFlash), so the eye finds where the view
    /// moved to.
    ///
    /// What a press on the sticky header does. The head is located through the record's cached position,
    /// so the jump costs the same for a block a million rows up. It lands the way a click on a scrollbar
    /// tick does, through the one reveal path.
    /// @see revealStableLineAtTop()
    /// @param id The block to jump to.
    /// @return Nothing once the viewport moved (or already showed the head as high as it can); why not
    ///         otherwise, in which case nothing changed.
    [[nodiscard]] std::expected<void, CommandBlockJumpError> scrollToCommandBlockHead(CommandBlockId id);

    // }}}

    // {{{ Scrollbar marks

    /// The marks the scrollbar may draw as ticks: one per command block whose head is a row of the grid,
    /// then one per vi user mark.
    ///
    /// Positions are VISIBLE rows counted from the oldest history row -- the range the scrollbar itself
    /// spans (Viewport::scrollableLineCount() plus the page) -- so a collapsed fold counts as its one head
    /// row, and a user mark a fold hides stands on that head. O(runs + (records + marks) log runs), runs
    /// being the hidden runs of the collapsed folds: heads come from each record's cached headStableId and
    /// marks from userMarks(), never from a walk of the grid, and each is placed by binary searches into
    /// the runs.
    ///
    /// Left out: blocks still at their prompt (no command yet), heads evicted from the grid or recorded in
    /// another stable-id generation (a daemon mirror's record before its head row is placed), and
    /// everything while no click could reveal them (canRevealScrollback()): the alternate screen is up, or
    /// another page is on display.
    ///
    /// Caller holds the terminal lock.
    /// @return The marks, records oldest first and then user marks top-down; neither bucketed nor filtered
    ///         by the sources a frontend shows (@see scrollbarMarks()).
    [[nodiscard]] std::vector<ScrollbarMarkInput> scrollbarMarkInputs() const;

    /// A counter that moves whenever what scrollbarMarkInputs() returns may have: a block's record or head,
    /// a vi mark, a fold, rows evicted or renamed, or whether a tick can be revealed at all. A frontend keys
    /// the ticks it keeps on this, and on what it adds itself (the track, the kinds it shows, their colours,
    /// the rows the scrollbar spans), so a refresh that finds it unmoved copies nothing out.
    ///
    /// Reconciles each store with the grid first, as scrollbarMarkInputs() does, so a read that changes
    /// nothing finds the same revision the next time. Caller holds the terminal lock.
    /// @return The revision.
    [[nodiscard]] uint64_t scrollbarMarksRevision() const;

    /// The stable ids of the primary screen's user-marked lines, reconciled against the grid first.
    ///
    /// Rebuilt by one scan of the grid when the stable-id generation moved since it was last current (a
    /// reflow, a reset, a history-limit change); then rid of ids whose rows left the grid: evicted below
    /// Grid::stableRangeFloor(), or cut off below a page that shrank. A mark placed, erased or moved is
    /// reported as it happens, so a read never looks at a line. const for the reason refreshFoldState() is:
    /// reconciling a cache is not a change of state.
    ///
    /// Caller holds the terminal lock.
    /// @return The reconciled index.
    [[nodiscard]] UserMarkIndex const& userMarks() const;

    /// Places the user mark (vi `mm`) on the logical line the row @p line of the page on display -- the page
    /// the vi cursor walks -- belongs to, or takes it off where the line carries one.
    ///
    /// A replica's marks are its host's (bindReplica): on the primary screen it asks for the change
    /// (Events::userMarkRequested) and leaves its line alone, which takes the mark when the host's row comes
    /// back -- so a mark is the session's, the same for every client of it, and it follows the host's reflow.
    ///
    /// Caller holds the terminal lock.
    /// @param line The row, on the page on display.
    void toggleUserMark(LineOffset line);

    /// Places or takes off the user mark on the logical line of the primary screen's row @p request names: a
    /// replica's request (Events::userMarkRequested), arriving at the terminal it replicates. One whose
    /// stable-id generation is gone, or whose row has left the grid, is passed over: its id may name another
    /// row now.
    ///
    /// Caller holds the terminal lock.
    /// @param request The row, by its stable id in this terminal's primary grid, and what to do to its line.
    void placeUserMark(UserMarkRequest const& request);

    /// Tells the user-mark index that the logical line headed by @p headStableId gained or lost
    /// LineFlag::UserMark on the primary screen.
    ///
    /// Called through Screen::noteUserMarkChange(): for every mark written through the funnel
    /// Screen::setLogicalLineFlags(), and for every one a row rewritten whole takes or brings
    /// (Screen::noteRowRewritten(): an erase, a daemon mirror's host row).
    ///
    /// Caller holds the terminal lock.
    /// @param headStableId The logical line's head, by stable id.
    /// @param change Whether the mark was placed or removed.
    void userMarkChanged(int64_t headStableId, UserMarkChange change);

    /// A scroll on the primary screen is about to move or drop user-marked rows under their stable ids
    /// (@see RowMove): each mark is held under the id its line moves to.
    ///
    /// Caller holds the terminal lock.
    /// @param move What the scroll does, in the ids as they stand before it.
    void userMarkRowsMoved(RowMove const& move);

    /// Scrolls the viewport so the line @p stableId names is its top row, opening first the collapsed fold
    /// that hides that line or hangs off it.
    ///
    /// What a click on a scrollbar tick does, and a press on the sticky header (scrollToCommandBlockHead()).
    /// Refused, rather than guessed at, for an id read in another stable-id generation: after a reflow it
    /// names some other row. Asks whether the viewport can scroll at all before it touches a fold, so a
    /// reveal that cannot land changes nothing.
    ///
    /// Caller holds the terminal lock.
    /// @param stableId The line's stable id on the primary screen.
    /// @param stableIdGeneration The Grid::stableIdGeneration() @p stableId was read in.
    /// @return Nothing once the line is on top, or as near to it as the scrollback allows; why not
    ///         otherwise, in which case nothing changed.
    [[nodiscard]] std::expected<void, RevealError> revealStableLineAtTop(int64_t stableId,
                                                                         uint64_t stableIdGeneration);

    // }}}

    /// Scrolls the viewport and extends the active selection to the boundary cell.
    ///
    /// Used by the GUI layer's auto-scroll timer when the mouse is dragged outside the terminal window.
    ///
    /// @param direction  Negative = scroll up (into history), positive = scroll down (toward present).
    /// @param lineCount  Number of lines to scroll per tick.
    void performAutoScroll(int direction, LineCount lineCount);

    // {{{ Smooth scrolling API

    /// Applies a pixel delta for smooth scrolling.
    /// Accumulates sub-cell pixel offset; converts to line scrolls when a full cell height is reached.
    ///
    /// @param pixelDelta  The pixel amount to scroll by (positive = scroll up into history).
    /// @return SmoothScrollResult::Applied if the viewport was modified,
    ///         SmoothScrollResult::Disabled if smooth scrolling is off or on alternate screen,
    ///         SmoothScrollResult::InvalidCellSize if the cell pixel height is zero or negative.
    SmoothScrollResult applySmoothScrollPixelDelta(float pixelDelta);

    /// Returns the current sub-cell pixel offset for smooth scrolling.
    [[nodiscard]] float smoothScrollPixelOffset() const noexcept { return _viewport.pixelOffset(); }

    /// Returns 1 when smooth scroll pixel offset is non-zero (extra line needed), 0 otherwise.
    [[nodiscard]] LineCount smoothScrollExtraLines() const noexcept
    {
        return _viewport.pixelOffset() > 0.0f ? LineCount(1) : LineCount(0);
    }

    /// Resets pixel offset to zero.
    void resetSmoothScroll() noexcept;

    // {{{ Momentum scrolling API

    /// Processes a scroll gesture phase event for momentum scrolling.
    ///
    /// @param phase      The gesture phase (Begin/Update/End/Momentum/NoPhase).
    /// @param pixelDelta The pixel scroll amount for this event (positive = scroll up into history).
    /// @param now        Current time point for velocity tracking.
    void handleScrollPhase(ScrollPhase phase, float pixelDelta, std::chrono::steady_clock::time_point now);

    /// Cancels any active momentum scroll animation.
    void cancelMomentumScroll() noexcept;

    /// Returns true if a momentum scroll animation is currently active.
    [[nodiscard]] bool isMomentumScrollActive() const noexcept;

    /// Injects a discrete mouse-wheel notch as a momentum impulse (primary screen only).
    ///
    /// Mouse wheels deliver phase-less notches; without this they would snap the viewport by
    /// whole lines. This converts the notch's pixel distance into an initial velocity so the
    /// viewport glides to rest under (wheel-specific) friction, giving mouse-wheel scrolling the
    /// same smooth feel as a touchpad. Gated on smoothScrolling only (independent of
    /// momentumScrolling). Accumulates with any active wheel glide so rapid notches build up.
    ///
    /// @param pixelDelta Signed pixels for this notch (positive = scroll up into history).
    /// @param now        Current time point for the momentum animation.
    /// @return SmoothScrollResult::Applied if a glide was armed (caller should consume the event),
    ///         SmoothScrollResult::Disabled if smooth scrolling is off or on the alternate screen,
    ///         SmoothScrollResult::InvalidCellSize if the cell pixel height is unknown (0) or the
    ///         accumulated velocity degenerated to zero — in both non-Applied cases the caller must
    ///         fall through to the legacy line-based scroll path.
    SmoothScrollResult injectWheelMomentum(float pixelDelta,
                                           std::chrono::steady_clock::time_point now) noexcept;

    // }}} Momentum scrolling API

    // }}}

    // {{{ Screen Render Proxy
    /// When the display should draw next though nothing asks for a frame: a blink, an animation, a status
    /// line command, or link detection's next scan, as the last frame left it. Takes no lock, and is asked
    /// after every frame.
    /// @return The wait, or nullopt when nothing is due.
    std::optional<std::chrono::milliseconds> nextRender() const;

    /// Sets the terminal's clock to @p now, and advances what runs on it between frames: the cursor's blink
    /// and a momentum glide. The render thread's alone, before and after each frame, and a test's: no other
    /// thread ticks, so what it advances has one owner. Takes the state lock while a glide is armed, so it is
    /// never called with that lock held.
    /// @param now The time of the frame.
    void tick(std::chrono::steady_clock::time_point now) noexcept;
    void tick(std::chrono::milliseconds delta) { tick(currentTime() + delta); }
    // }}}

    // {{{ RenderBuffer synchronization API

    /// Ensures the terminals event loop is interrupted
    /// and the render buffer is refreshed.
    ///
    void breakLoopAndRefreshRenderBuffer();

    /// Refreshes the render buffer.
    /// When this function returns, the back buffer is updated
    /// and it is attempted to swap the back/front buffers.
    /// but the swap has NOT been invoked yet.
    ///
    /// @param locked whether or not the Terminal object's lock is already held by the caller.
    ///
    /// @retval true   front buffer now contains the refreshed render buffer.
    /// @retval false  back buffer contains the refreshed render buffer,
    ///                and RenderDoubleBuffer::swapBuffers() must again
    ///                be successfully invoked to swap back/front buffers
    ///                in order to access the refreshed render buffer.
    ///
    /// @note The current time must have been updated in order to get the
    ///       correct cursor blinking state drawn.
    ///
    /// @see RenderDoubleBuffer::swapBuffers()
    /// @see renderBuffer()
    ///
    bool refreshRenderBuffer(bool locked = false);

    /// Eventually refreshes the render buffer iff
    /// - the screen contents has changed AND refresh rate satisfied,
    /// - viewport has changed, or
    /// - refreshing the render buffer was explicitly requested.
    ///
    /// @param now    the current time
    /// @param locked whether or not the Terminal object's lock is already held by the caller.
    ///
    /// @see RenderDoubleBuffer::swapBuffers()
    /// @see renderBuffer()
    bool ensureFreshRenderBuffer(bool locked = false);

    /// Aquuires read-access handle to front render buffer.
    ///
    /// This also acquires the reader lock and releases it automatically
    /// upon RenderBufferRef destruction.
    ///
    /// @see ensureFreshRenderBuffer()
    /// @see refreshRenderBuffer()
    [[nodiscard]] RenderBufferRef renderBuffer() const { return _renderBuffer.frontBuffer(); }

    [[nodiscard]] RenderBufferState renderBufferState() const noexcept { return _renderBuffer.state; }

    /// Updates the IME preedit-string to be rendered when IME is composing a new input.
    /// Passing an empty string effectively disables IME rendering.
    void updateInputMethodPreeditString(std::string preeditString);
    // }}}

    /// Takes the state lock as a frontend does, to change what the parser thread changes. Until unlock(), the
    /// hold keeps back what must not run under the lock: the input flush, as a PTY write can block while the
    /// parser thread waits for the lock, and with SRM reset the local echo parses, which takes the lock
    /// again; a clipboard paste, a round trip to another process that pastes through a flush; and the opening
    /// of a document, which hands it to another process. Records the calling thread as the lock's holder.
    void lock() { acquireState(HoldBack::Yes); }

    /// Releases the state lock taken by lock(), then sends the input queued meanwhile -- the reports a change
    /// raised, say -- or leaves it to the PTY's reader, as Settings::inputWriter says, and asks for
    /// what the hold kept back. Never throws, as a lock guard's destructor calls it, on unwind too: a failure
    /// is logged.
    void unlock() noexcept;

    /// Takes the state lock to read. A const holder changes nothing, so it holds nothing back.
    void lock() const { acquireState(HoldBack::No); }

    /// Releases the state lock taken to read.
    void unlock() const { releaseState(); }

    /// Runs @p command under the state lock, as lock() takes it, and returns what it returns: for a frontend
    /// that wants a value out of the hold, or that may run where its thread holds the lock already. There,
    /// @p command runs at once under that hold, which then sends what it queues: a frontend hook raised under
    /// the lock may answer synchronously, as the QML scrollbar writes back the offset a scroll announces.
    /// @param command What to run under the lock. It must not take the lock itself.
    /// @return What @p command returns.
    template <typename Command>
        requires std::invocable<Command const&>
    std::invoke_result_t<Command const&> runUnderStateLock(Command const& command)
    {
        if (isStateLockHeldByThisThread())
            return command();

        auto const _ = std::scoped_lock { *this };
        return command();
    }

    [[nodiscard]] ColorPalette const& colorPalette() const noexcept { return _colorPalette; }
    [[nodiscard]] ColorPalette& colorPalette() noexcept { return _colorPalette; }
    [[nodiscard]] ColorPalette& defaultColorPalette() noexcept { return _defaultColorPalette; }

    /// @return The scheme's palette: what resetColorPalette() restores, and what no OSC 4 changes. The
    ///         host's own chrome (the indicator status line's roles) is coloured from it.
    [[nodiscard]] ColorPalette const& defaultColorPalette() const noexcept { return _defaultColorPalette; }

    /// The indicator status line's colors for the focus and the input mode as they are now: the inactive
    /// pair while unfocused; the idle (Insert) pair in every mode when the template draws its mode as a chip
    /// (showsModeChip()), which then carries the mode's colours alone; and the mode's pair otherwise.
    [[nodiscard]] RGBColorPair indicatorStatusLineColors() const;

    /// The indicator status line's pair for @p mode, as the scheme names it.
    /// @param mode The vi input mode.
    /// @return The Insert pair for Insert, the Normal pair for Normal and Hint, and the Visual pair for
    ///         the visual modes.
    [[nodiscard]] RGBColorPair indicatorStatusLineColorsFor(ViMode mode) const noexcept;

    [[nodiscard]] std::vector<ColorPalette> const& savedColorPalettes() const noexcept
    {
        return _savedColorPalettes;
    }

    /// Replaces the palette in effect, announcing the default background when it changes. Not
    /// @c noexcept: the announcement reaches the frontend, which may allocate to post it on.
    void setColorPalette(ColorPalette const& palette);
    void resetColorPalette() { setColorPalette(defaultColorPalette()); }

    /// Re-seeds the palette, and the defaults a reset returns to, from @p colors.
    ///
    /// With DEC mode 2031 set, reports the update only when @p colors differs from the palette in effect.
    /// @param colors The configured palette.
    void resetColorPalette(ColorPalette const& colors);

    void pushColorPalette(size_t slot);
    void popColorPalette(size_t slot);
    void reportColorPaletteStack();

    /// The application's hierarchical context ancestry (OSC 3008), for reading.
    /// Mutated only through applyContextCommand().
    [[nodiscard]] ContextStack const& contexts() const noexcept { return _contexts; }

    /// Mutable access, for a daemon mirror adopting records the host already holds.
    /// Every other caller reads: the ancestry moves through applyContextCommand().
    [[nodiscard]] ContextStack& contexts() noexcept { return _contexts; }

    /// Applies one decoded OSC 3008 command: updates the ancestry, mirrors the new active id into
    /// every screen so freshly written lines are stamped with it, and announces an observable change.
    ContextTransition applyContextCommand(ContextCommand const& command);

    /// Adopts a context record replicated from a daemon host. @see ContextStack::adopt.
    void adoptContext(TerminalContext record);

    /// Replaces the ancestry with @p ids, for the same mirroring reason as @ref adoptContext, and announces
    /// the new active context through Events::contextChanged when it moved.
    ///
    /// Never makes it the context the rows the cursor reaches are stamped with, as applyContextCommand()
    /// does: a mirror's rows carry the contexts the host gave them, and a row the mirror stamped itself --
    /// the one its cursor stood on when the chain moved, or one it reached afterwards -- could differ from
    /// the host's.
    void setContextChain(std::span<ContextId const> ids);

    /// Makes this terminal a replica of one a daemon hosts, @p role saying whether the daemon runs on this
    /// machine: the one switch every locality this terminal judges follows -- the records it adopts
    /// (adoptCommandBlock()), its OSC 3008 contexts and its OSC 7 working directory
    /// (resolveWorkingDirectory()), and the `file://` URLs its application writes (fileUrlLocalHost()).
    ///
    /// A documented attach-style call (AGENT.md, cyclic wiring), not configuration at construction: the
    /// terminal is built around its pty before the mirror that binds to it exists, and only that binding
    /// knows where the daemon runs. Called once, by the mirror; lock-free, for the reason
    /// adoptCommandBlock() gives. The role is published last, and atomically, for fileUrlLocalHost().
    /// @param role What this terminal replicates.
    /// @param identity This machine's identity as the daemon's contexts spell a machine -- a daemon sends no
    ///        machine id as it read it, but one derived from it -- which a ReplicaRole::SameMachine replica
    ///        judges every context against, in place of localIdentity(). Unused by the other roles.
    void bindReplica(ReplicaRole role, HostIdentity identity) noexcept
    {
        _replicaIdentity = std::move(identity);
        _replicaRole.store(role, std::memory_order_release);
    }

    /// The host name the authority of a `file://` URL this terminal's application wrote -- an OSC 8 link's
    /// target -- must name for the URL to be a path on this machine (vtbackend::isLocalHost): this machine's
    /// (localIdentity()). None on a replica of another machine's terminal (ReplicaRole::OtherMachine), whose
    /// application names a path on the daemon's machine whatever authority it writes, the empty one and
    /// `localhost` included.
    ///
    /// Lock-free, unlike the rest of the replica's judgements: the GUI asks it on its own thread, of a link
    /// it already holds. The role is read atomically, and localIdentity() names data fixed at construction.
    /// @return The host name, or nullopt when no `file://` URL names a path here.
    [[nodiscard]] std::optional<std::string_view> fileUrlLocalHost() const noexcept;

    /// Adopts a command-block record replicated from a daemon host, and raises
    /// Events::commandBlockFinished when the store reports the adoption as a finish: the mirror's
    /// counterpart of OSC 133;D, beside adoptContext().
    ///
    /// On a replica of another machine's terminal (ReplicaRole::OtherMachine) the record's working
    /// directory is held Foreign, whatever the daemon judged it: it names a directory there. Its start
    /// instants are moved onto this terminal's clock there, by the difference adoptHostClock() measured.
    ///
    /// Never makes the record the block the rows the cursor reaches are stamped with, as beginCommandBlock()
    /// does: a mirror's rows carry the ids the host gave them, and a row the mirror stamped itself could
    /// differ from the host's.
    ///
    /// Lock-free like adoptContext(): the mirror calls it with the terminal lock already held, so the
    /// event is raised under that lock exactly as the parser thread raises it.
    /// @see Events::commandBlockFinished for what that obliges a listener to do.
    /// @param record The record, under the HOST's id.
    /// @param mode Snapshot for history, which never raises; Live for news.
    void adoptCommandBlock(CommandBlockRecord record, AdoptMode mode);

    /// Heads each of @p blocks that has no head in the primary grid's stable-id generation, by the rule a
    /// reflow re-heads every block by: at its lowest row carrying LineFlag::Marked, else its lowest carrying
    /// LineFlag::OutputStart, else its topmost row unless that is the first row the grid holds -- where a
    /// head that scrolled out of the history looks no different. A block with no such row is left without
    /// one: its rows may yet arrive.
    ///
    /// A daemon mirror's counterpart of the head OSC 133;A records. A record adoptCommandBlock() takes has
    /// no head here -- the host's names a row in the HOST's grid -- and a mirror writes rows straight into
    /// the grid, so once it has written a batch of them it names each block it adopted and each a row it
    /// wrote may head. Only the named blocks are looked for, so a record whose rows never reach this grid
    /// costs a walk once, not on every batch. Heads a write took from their blocks are re-headed first, as
    /// any read of commandBlocks() does.
    ///
    /// Does NOT take the lock, for the reason commandBlocks() gives. Allocates nothing: a mirror calls it for
    /// every update that rewrites a head-marked row, which every keystroke at a prompt does.
    /// @param blocks The blocks to head, in any order, repeated or not; an id with no record, or whose record
    ///        has a head in this generation, is passed over. Sorted in place: the caller's buffer is the
    ///        scratch space the call works in, and holds no particular order afterwards.
    void placeCommandBlockHeads(std::span<CommandBlockId> blocks);

    /// Records each of @p blocks that has no head in the primary grid's stable-id generation as evicted, as
    /// a reflow records a block none of whose rows it found: what a daemon mirror does with a block its host
    /// says has lost its head row. The mirror cannot tell that from a head row that has yet to arrive, and
    /// without it such a block is never headed, so neither its sticky header nor its placeholder shows.
    ///
    /// A block headed in this generation keeps its head: the mirror holds the row the host let go of, and
    /// will see it leave as the host did. Call it before placeCommandBlockHeads(), which then passes the
    /// retired blocks over. Does NOT take the lock, for the reason commandBlocks() gives.
    /// @param blocks The blocks whose host head is gone, in any order; an id with no record is passed over.
    void retireCommandBlockHeads(std::span<CommandBlockId const> blocks);

    /// @return The wall-clock instant this terminal's line birth stamps count from, to the second.
    /// @see lineBirthTime()
    [[nodiscard]] std::chrono::system_clock::time_point sessionEpoch() const noexcept;

    /// Re-anchors line birth stamps on @p epoch, floored to the second as the terminal's own epoch is.
    ///
    /// A documented rebinding seam (AGENT.md, configuration at construction time) with one caller: a
    /// daemon mirror copies the host's stamps verbatim, and they name the right instants only when counted
    /// from the HOST's epoch. Lock-free, for the reason adoptCommandBlock() gives.
    ///
    /// The epoch is held to the range this terminal computes with (heldInstant()) here, whoever produced
    /// it: a birth time adds to it and a stamp subtracts it from this terminal's clock, either of which
    /// overflows near the clock's limits. A value outside the range lands on the edge it overshot.
    /// @param epoch The host's session epoch.
    void adoptSessionEpoch(std::chrono::system_clock::time_point epoch) noexcept;

    /// Measures how far this terminal's wall clock is from a daemon host's, by what the host's read as it
    /// stated @p hostNow. On a replica of another machine's terminal (ReplicaRole::OtherMachine) every
    /// command-block record adopted from here on has its start instants moved by that much
    /// (adoptCommandBlock()), and every row's time is read moved by it (lineBirthTime()), so a running
    /// command's time, a block's start and its rows' times read the same here as on the host however the
    /// two machines' clocks are set. A replica on the daemon's own machine reads the same clock, and moves
    /// nothing.
    ///
    /// A documented rebinding seam (AGENT.md, configuration at construction time) with one caller, the
    /// daemon mirror, which states each new reading as it arrives. The time the reading took to arrive
    /// counts as skew: a few milliseconds over a local socket. Lock-free, for the reason
    /// adoptCommandBlock() gives. Both instants are held to the range this terminal computes with
    /// (heldInstant()), which bounds the difference.
    /// @param hostNow The host's wall clock, as it read it just before it was sent.
    void adoptHostClock(std::chrono::system_clock::time_point hostNow) noexcept;

    /// @p hostInstant, read by a daemon host's clock, as this terminal's clock reads it: moved by the
    /// difference adoptHostClock() measured on a replica of another machine's terminal, else as it is.
    /// @param hostInstant An instant the host's clock read.
    /// @return The same instant on this terminal's clock.
    [[nodiscard]] std::chrono::system_clock::time_point onThisClock(
        std::chrono::system_clock::time_point hostInstant) const noexcept;

    /// Mirrors the active context id into every screen, so lines written next carry it.
    void publishActiveContext() noexcept;

    /// Decides whether OSC 3008 may synthesise OSC-133-equivalent marks in this session.
    [[nodiscard]] MarkArbiter& markArbiter() noexcept { return _markArbiter; }
    [[nodiscard]] MarkArbiter const& markArbiter() const noexcept { return _markArbiter; }

    [[nodiscard]] SemanticBlockTracker& semanticBlockTracker() noexcept { return _semanticBlockTracker; }
    [[nodiscard]] SemanticBlockTracker const& semanticBlockTracker() const noexcept
    {
        return _semanticBlockTracker;
    }

    [[nodiscard]] Screen& currentScreen() noexcept { return *_currentScreen; }
    [[nodiscard]] Screen const& currentScreen() const noexcept { return *_currentScreen; }

    /// The display text and sequences are written to: the current page, or the status line DECSASD
    /// selected. Const on a const terminal.
    template <typename Self>
    [[nodiscard]] auto& activeDisplay(this Self& self) noexcept
    {
        // The constness is spelled out: clang with Ubuntu 24.04's libstdc++ rejects std::forward_like
        // here ("with deduced return type cannot be used before it is defined").
        using Display = std::conditional_t<std::is_const_v<Self>, Screen const, Screen>;
        switch (self._activeStatusDisplay)
        {
            case ActiveStatusDisplay::Main: return static_cast<Display&>(*self._currentScreen);
            case ActiveStatusDisplay::StatusLine:
                return static_cast<Display&>(self._hostWritableStatusLineScreen);
            case ActiveStatusDisplay::IndicatorStatusLine:
                return static_cast<Display&>(self._indicatorStatusScreen);
        }
        core::unreachable();
    }

    [[nodiscard]] SequenceHandler& sequenceHandler() noexcept
    {
        // TODO: avoid double-switch by introducing a `SequenceHandler& sequenceHandler` member.
        switch (_executionMode.load())
        {
            case ExecutionMode::Normal: return activeDisplay();
            case ExecutionMode::BreakAtEmptyQueue:
            case ExecutionMode::Waiting: [[fallthrough]];
            case ExecutionMode::SingleStep: return _traceHandler;
        }
        core::unreachable();
    }

    bool isPrimaryScreen() const noexcept { return _currentScreenType == ScreenType::Primary; }
    bool isAlternateScreen() const noexcept { return _currentScreenType == ScreenType::Alternate; }
    ScreenType screenType() const noexcept { return _currentScreenType; }

    /// Switches the cursor to the primary or the alternate screen's page. The page on display follows as for
    /// any page movement, while DECPCCM is set; what the alternate screen modes show is
    /// setAlternateScreen()'s to say.
    /// @param screenType The screen to switch to.
    void setScreen(ScreenType screenType);

    /// Enters or leaves the alternate screen buffer for one of the DEC private modes 47, 1047 or 1049,
    /// following the cursor-carry and clear policy that alternateScreenBehavior() describes for @p mode.
    /// Modelled on xterm's ToAlternate / FromAlternate: the cursor is a terminal-level entity, so all three
    /// modes carry it into the alternate screen (it does not move), and 47 and 1047 carry it back out, while
    /// mode 1049 returns to the cursor the page kept, its implicit DECRC, having saved it into the DECSC
    /// buffer on the way in.
    ///
    /// The alternate screen is entered from any page, and shown whatever DECPCCM says. Leaving it returns the
    /// cursor to the page it was entered from; with DECPCCM reset the page that was on display before is
    /// shown again, as Windows Terminal does, and with DECPCCM set the display follows the cursor.
    /// @param mode   One of DECMode::UseAlternateScreen (47), OptionalAltScreen (1047) or
    ///               ExtendedAltScreen (1049); other modes violate the precondition.
    /// @param enable true to switch to the alternate buffer, false to switch back to the primary.
    void setAlternateScreen(DECMode mode, bool enable);

    Screen& screenForType(ScreenType type) noexcept
    {
        switch (type)
        {
            case ScreenType::Primary: return *_pages[0];
            case ScreenType::Alternate: return *_pages[AlternateScreenPageIndex.value];
        }
        core::unreachable();
    }

    /// Returns a reference to the screen at the given page index.
    [[nodiscard]] Screen& pageAt(PageIndex index) noexcept { return *_pages[index.value]; }
    [[nodiscard]] Screen const& pageAt(PageIndex index) const noexcept { return *_pages[index.value]; }

    /// Returns the zero-based page index where the cursor is currently active.
    [[nodiscard]] PageIndex cursorPageIndex() const noexcept { return _cursorPage; }

    /// Returns the zero-based page index of the displayed page.
    [[nodiscard]] PageIndex displayedPageIndex() const noexcept { return _displayedPage; }

    /// The page the viewer is actually looking at — the alternate screen while a full-screen
    /// application is running, and whatever DECPCCM last displayed.
    ///
    /// Named here rather than spelled `pageAt(displayedPageIndex())` at each consumer, because
    /// every one of those is a chance to reach for `primaryScreen()` instead and silently answer
    /// about the buffer the editor replaced — which is exactly what `capture-pane` used to do.
    [[nodiscard]] Screen& displayedPage() noexcept { return pageAt(_displayedPage); }
    [[nodiscard]] Screen const& displayedPage() const noexcept { return pageAt(_displayedPage); }

    /// Whether the page on display is the primary screen or another one. Not screenType(), which is the
    /// cursor's page: with DECPCCM reset the two differ. Safe to read without the state lock.
    /// @return The screen type of the page on display.
    [[nodiscard]] ScreenType displayedScreenType() const noexcept { return _displayedScreenType; }

    /// Returns the 1-based page number where the cursor is active (for VT replies).
    [[nodiscard]] int cursorPageNumber() const noexcept { return pageNumberOf(_cursorPage); }

    /// Returns the 1-based page number of the displayed page (for VT replies).
    [[nodiscard]] int displayedPageNumber() const noexcept { return pageNumberOf(_displayedPage); }

    /// The number a VT report gives a page: its index plus one, and 1 for the alternate screen, which is no
    /// DEC page. xterm's DECXCPR "assumes the default page", Windows Terminal's PageManager::Get answers for
    /// its alternate buffer with "a fixed page number of 1", and mlterm regards its alternate edit as page 1.
    /// @param page The page.
    /// @return Its 1-based page number.
    [[nodiscard]] static constexpr int pageNumberOf(PageIndex page) noexcept
    {
        return page == AlternateScreenPageIndex ? 1 : page.value + 1;
    }

    /// Switches the cursor to another page; the page on display follows while DECPCCM is set.
    /// @param target    The page to switch to; clamped to the pages there are.
    /// @param placement Where the cursor stands on @p target.
    void setPage(PageIndex target, PageCursorPlacement placement);

    /// Page movement (NP, PP, PPA, PPR, PPB): moves the cursor to another DEC page. A page beyond the last
    /// one is the last one, and one before the first is the first, as DEC STD 070 §4.7 says. DEC STD 070
    /// defines no alternate screen. Windows Terminal, the one terminal with both pages and an alternate
    /// buffer, keeps page movement on it, a display of one page: NP and PP move the cursor home there
    /// (NextPage/PrecedingPage), and PPA, PPR and PPB leave it (PageManager::MoveTo returns outside the main
    /// buffer). Ignored while DECSASD selects the status line, a one-line
    /// display of its own on which vertical cursor positioning is ignored (DEC STD 070 ch. 14 §2.3).
    /// @param page      The zero-based index of the page to move to.
    /// @param placement Where the cursor stands on that page.
    void movePage(int page, PageCursorPlacement placement);

    /// Whether the cursor is on the alternate screen buffer, as xterm's whichBuf says. Not
    /// isAlternateScreen(), which answers for every page but page 1.
    /// @return Whether the cursor is on the alternate screen's page.
    [[nodiscard]] bool isAlternateScreenBufferInUse() const noexcept
    {
        return _cursorPage == AlternateScreenPageIndex;
    }

    /// Saves the current cursor page index (called by DECSC). Not while DECSASD selects the status line,
    /// which keeps a Cursor Save Buffer of its own (DEC STD 070 ch. 14 §2.2).
    void saveCursorPage();

    /// DECRC's page half: returns the cursor to the page DECSC saved. DEC STD 070 keeps one save buffer, the
    /// page in its position; Contour keeps it on the page DECSC saved (its Screen::_savedCursor, which a
    /// resize of that page carries along), one per screen buffer as xterm and Windows Terminal do, so the
    /// rest of the buffer is that page's. Not while DECSASD selects the status line, which keeps a Cursor
    /// Save Buffer of its own (DEC STD 070 ch. 14 §2.2).
    /// @param receiving The display DECRC was received on.
    /// @return The display whose save buffer DECRC restores: the saved page, or @p receiving on the status
    /// line.
    [[nodiscard]] Screen& restoreCursorPage(Screen& receiving);

    /// Returns the margins of the main display, one set that every page and the alternate screen share.
    [[nodiscard]] Margin& mainDisplayMargin() noexcept { return _mainDisplayMargin; }
    /// @copydoc mainDisplayMargin()
    [[nodiscard]] Margin const& mainDisplayMargin() const noexcept { return _mainDisplayMargin; }

    /// Creates a default margin for the given page size (full-screen, no restriction).
    [[nodiscard]] static Margin makeDefaultMargin(PageSize pageSize) noexcept
    {
        return Margin { .vertical =
                            Margin::Vertical { .from = {}, .to = boxed_cast<LineOffset>(pageSize.lines) - 1 },
                        .horizontal = Margin::Horizontal {
                            .from = {}, .to = boxed_cast<ColumnOffset>(pageSize.columns) - 1 } };
    }

    /// Returns true while a screen crossfade transition runs, at the terminal's clock.
    [[nodiscard]] bool isScreenTransitionActive() const noexcept
    {
        return _screenTransition.active && !_screenTransition.isComplete(currentTime());
    }

    /// Returns the current transition progress in [0, 1], or 1.0 if no transition is active.
    [[nodiscard]] float screenTransitionProgress() const noexcept
    {
        return _screenTransition.progress(currentTime());
    }

    /// Immediately ends any active screen transition.
    void finalizeScreenTransition() noexcept;

    /// Detects cursor position changes and injects animation data into the render buffer.
    ///
    /// @param output  The render buffer whose cursor entry will be updated with
    ///                animateFrom, animationProgress and animateFromColor fields.
    void updateCursorMotionAnimation(RenderBuffer& output);

    /// Applies fade-out/fade-in blending when a screen transition is active.
    ///
    /// @param output  The render buffer whose cells and lines will be color-blended
    ///                according to the current transition phase (fade-out or fade-in).
    void applyScreenTransitionBlending(RenderBuffer& output);

    void setHighlightTimeout(std::chrono::milliseconds timeout) noexcept
    {
        _settings.highlightTimeout = timeout;
    }

    // clang-format off
    [[nodiscard]] Screen const& primaryScreen() const noexcept { return *_pages[0]; }
    [[nodiscard]] Screen& primaryScreen() noexcept { return *_pages[0]; }
    [[nodiscard]] Screen const& alternateScreen() const noexcept { return *_pages[AlternateScreenPageIndex.value]; }
    [[nodiscard]] Screen& alternateScreen() noexcept { return *_pages[AlternateScreenPageIndex.value]; }

    /// Injects the ReGIS text rasterizer into every page so ReGIS text renders through the display's
    /// font engine rather than the built-in embedded font. See Screen::setReGISTextRasterizer.
    ///
    /// Takes the terminal lock: this is called from the GUI thread (a session rebind) while the parser
    /// thread reads @c _regisTextRasterizer under the same lock in Screen::hookReGIS, so an unlocked
    /// write would race the shared_ptr control block.
    void setReGISTextRasterizer(std::shared_ptr<regis::ReGISTextRasterizer> const& rasterizer)
    {
        auto const guard = std::lock_guard { *this };
        for (auto& page: _pages)
            page->setReGISTextRasterizer(rasterizer);
    }
    [[nodiscard]] Screen const& hostWritableStatusLineDisplay() const noexcept { return _hostWritableStatusLineScreen; }
    [[nodiscard]] Screen& hostWritableStatusLineDisplay() noexcept { return _hostWritableStatusLineScreen; }
    [[nodiscard]] Screen const& indicatorStatusLineDisplay() const noexcept { return _indicatorStatusScreen; }
    // clang-format on

    /// Whether a row of the page on display continues the row above it, as a line wrapped at the margin does.
    /// Only the primary page records its wrapped rows.
    /// @param lineNumber The row.
    /// @return true if the row continues the one above it.
    [[nodiscard]] bool isLineWrapped(LineOffset lineNumber) const noexcept
    {
        return displayedScreenType() == ScreenType::Primary && primaryScreen().isLineWrapped(lineNumber);
    }

    [[nodiscard]] CellLocation currentMousePosition() const noexcept { return _currentMousePosition; }

    /// The cell under the pointer, a cell of the page on display (displayedPage()) -- not of the page the
    /// cursor writes to, which DECPCCM reset or a status line DECSASD selected makes another one.
    ///
    /// Not noexcept: the coordinate translation goes through the fold projection, which allocates.
    /// @return The grid cell under the pointer; nullopt while the pointer is on none (@see
    /// sendMouseLeaveEvent).
    [[nodiscard]] std::optional<CellLocation> currentMouseGridPosition() const
    {
        if (_pointerPlacement == PointerPlacement::OnGrid && displayedPage().contains(_currentMousePosition))
            return _viewport.translateScreenToGridCoordinate(_currentMousePosition);
        return std::nullopt;
    }

    [[nodiscard]] CellLocation normalModeCursorPosition() const noexcept
    {
        return _viCommands.cursorPosition;
    }
    void moveNormalModeCursorTo(CellLocation pos) noexcept { _viCommands.moveCursorTo(pos); }
    void addLineOffsetToJumpHistory(LineOffset offset) noexcept
    {
        _viCommands.addLineOffsetToJumpHistory(offset);
    }

    // {{{ cursor management
    CursorDisplay cursorDisplay() const noexcept { return _settings.cursorDisplay; }
    void setCursorDisplay(CursorDisplay display);

    CursorShape cursorShape() const noexcept { return _settings.cursorShape; }
    void setCursorShape(CursorShape shape);

    bool cursorBlinkActive() const noexcept { return cursorBlink().phase == CursorBlinkPhase::Shown; }

    bool cursorCurrentlyVisible() const noexcept
    {
        return isModeEnabled(DECMode::VisibleCursor)
               && (cursorDisplay() == CursorDisplay::Steady || cursorBlinkActive());
    }

    /// Returns the predicted animation progress for a cursor at the given grid position.
    /// Used by RenderBufferBuilder to pre-set animationProgress before cell rendering,
    /// so that Block cursor cell inversion is suppressed during animation.
    [[nodiscard]] float cursorAnimationProgress(CellLocation cursorGridPosition) const noexcept
    {
        auto const now = currentTime();
        if (_cursorMotion.active && !_cursorMotion.isComplete(now))
            return _cursorMotion.progress(now);
        if (cursorGridPosition != _cursorMotion.toPosition
            && _settings.cursorMotionAnimationDuration.count() > 0)
            return 0.0f;
        return 1.0f;
    }

    bool isBlinkOnScreen() const noexcept
    {
        return _lastRenderPassHints.load(std::memory_order_relaxed).containsBlinkingCells;
    }

    std::chrono::steady_clock::time_point lastCursorBlink() const noexcept { return cursorBlink().since; }

    void setCursorBlinkingInterval(std::chrono::milliseconds value) { _settings.cursorBlinkInterval = value; }

    std::chrono::milliseconds cursorBlinkInterval() const noexcept { return _settings.cursorBlinkInterval; }
    // }}}

    // {{{ selection management
    void setWordDelimiters(std::string const& wordDelimiters);
    void setExtendedWordDelimiters(std::string const& wordDelimiters);
    std::u32string const& wordDelimiters() const noexcept { return _settings.wordDelimiters; }

    Selection const* selector() const noexcept { return _selection.get(); }
    Selection* selector() noexcept { return _selection.get(); }
    std::chrono::milliseconds highlightTimeout() const noexcept { return _settings.highlightTimeout; }

    void updateSelectionMatches();

    void clearSelection();

    /// Tests whether some area has been selected.
    bool isSelectionAvailable() const noexcept
    {
        return _selection && _selection->state() != Selection::State::Waiting;
    }
    bool isSelectionInProgress() const noexcept
    {
        return _selection && _selection->state() != Selection::State::Complete;
    }
    bool isSelectionComplete() const noexcept
    {
        return _selection && _selection->state() == Selection::State::Complete;
    }

    /// Tests whether given absolute coordinate is covered by a current selection.
    ///
    /// A multi-cell block (a wide character, or an `OSC 66` text-sizing block) is indivisible: if any
    /// of its cells is selected, all of them are. Highlighting half a glyph would show a selection
    /// the user cannot have meant and cannot correct.
    /// @param screen the screen @p coord belongs to. The render path works on the DISPLAYED page,
    ///               which is not the current screen once page-cursor coupling is reset, and a block
    ///               resolved against the wrong screen highlights the wrong cells.
    [[nodiscard]] bool isSelected(Screen const& screen, CellLocation coord) const noexcept;

    /// Keeps a drag that stays inside one row of tall blocks from becoming a multi-line selection.
    ///
    /// @param anchor  where the drag started.
    /// @param pointer where the pointer is now.
    /// @return @p pointer, with its line snapped back to @p anchor's while both are in the same
    ///         block-row of equally shaped blocks.
    [[nodiscard]] CellLocation clampDragWithinMulticellBlock(CellLocation anchor,
                                                             CellLocation pointer) const noexcept;

    /// Tests whether given line offset is intersecting with selection.
    ///
    /// This is the COARSE test the renderer's trivial-line fast path consults: a line it calls
    /// unselected is drawn uniformly and never asks isSelected(CellLocation) about any of its cells.
    /// It must therefore agree with the per-cell test about blocks -- a tall block reaching down into
    /// this line makes the line selected even when the selection's own range stops above it.
    [[nodiscard]] bool isSelected(LineOffset line) const noexcept;

    /// Tests whether the given cell is covered by the active (vi yank/motion) highlight range.
    /// @param cell The absolute grid coordinate to test.
    /// @return true if a highlight range is active and contains @p cell.
    bool isHighlighted(CellLocation cell) const noexcept;

    /// Tests whether the given grid line intersects the active highlight range.
    /// Mirrors isSelected(LineOffset) so the render fast path can cheaply decide whether a
    /// trivial (uniform-SGR) line must drop to the per-cell path to receive the highlight.
    /// @param line The absolute grid line offset to test.
    /// @return true if a highlight range is active and spans @p line.
    [[nodiscard]] bool isHighlighted(LineOffset line) const noexcept;
    float blinkState() const noexcept { return _slowBlinker.opacity(currentTime(), _settings.blinkStyle); }
    float rapidBlinkState() const noexcept
    {
        return _rapidBlinker.opacity(currentTime(), _settings.blinkStyle);
    }

    /// Sets or resets to a new selection.
    void setSelector(std::unique_ptr<Selection> selector);

    /// Tests whether or not some grid cells are selected.
    bool selectionAvailable() const noexcept { return !!_selection; }

    /// Selects everything the page on display holds: its whole scrollback and the whole page.
    void selectAll();

    /// Whether the left mouse button is currently held down, i.e. a selection drag may be in progress.
    [[nodiscard]] bool leftMouseButtonPressed() const noexcept
    {
        return _leftButtonPress == LeftButtonPress::Selecting;
    }

    bool visualizeSelectedWord() const noexcept { return _settings.visualizeSelectedWord; }
    void setVisualizeSelectedWord(bool enabled) noexcept { _settings.visualizeSelectedWord = enabled; }
    // }}}

    [[nodiscard]] std::string extractSelectionText() const;
    [[nodiscard]] std::string extractLastMarkRange() const;

    /// Where the shell's LIVE prompt sits — the one the user is typing at right now.
    ///
    /// Unlike the block actions (commandBlockText()), which always read the primary screen, this is answered
    /// for the screen actually on display: while an alt-screen application (vim, less) is up there is no
    /// shell prompt to speak of, and saying so is the point.
    ///
    /// Does NOT take the lock — the caller holds it.
    ///
    /// @return The span, or why there is no live prompt.
    [[nodiscard]] std::expected<LivePromptSpan, PromptRegionError> livePromptSpan() const;

    /// When the cursor first reached @p line, by the wall clock this terminal was given.
    ///
    /// Read from the 24-bit stamp the line carries: nullopt for a line the cursor never reached, a line of
    /// a screen that is not stamped (the alternate screen), and a line stamped past the ~194-day horizon,
    /// whose saturated stamp no longer says when. A replica of another machine's terminal reads its host's
    /// stamps on its own clock (@see adoptHostClock).
    ///
    /// Does NOT take the lock: the parser thread writes the stamp under it, so the caller holds the lock for
    /// as long as it holds @p line -- the same contract livePromptSpan() has.
    /// @see Line::bornAt
    /// @param line A line of this terminal's primary screen.
    /// @return The instant, to the second.
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> lineBirthTime(
        Line const& line) const noexcept;

    // {{{ Command blocks (OSC 133, and OSC 3008 standing in for it)

    /// Every shell command block of the primary screen, oldest first.
    ///
    /// Every record's head is current: a read first re-records them all when row identity died since the
    /// last read, and those an erase or a new prompt took from their blocks or a scroll inside margins moved,
    /// so read records through here, never through the member.
    ///
    /// Does NOT take the lock: the parser thread feeds the store under it, so the caller holds the lock
    /// for as long as it reads the store -- the same contract lineBirthTime() has.
    /// @see CommandBlockStore
    /// @see syncCommandBlockHeads
    [[nodiscard]] CommandBlockStore& commandBlocks() noexcept;

    /// @copydoc commandBlocks()
    [[nodiscard]] CommandBlockStore const& commandBlocks() const noexcept;

    /// How much re-recording the blocks' heads has looked at since this terminal was built: every record it
    /// examined, and every grid row it walked. The work behind a commandBlocks() read that found rows
    /// changed, and behind placeCommandBlockHeads().
    ///
    /// A test seam -- nothing in production reads it: what pins the cost of a read, and that the events a
    /// flood of OSC 133 raises leave none of that work to the thread parsing it.
    /// @return The count, which only ever grows.
    [[nodiscard]] uint64_t commandBlockHeadWork() const noexcept { return _commandBlockHeadWork; }

    /// The block the primary screen's row @p gridLine belongs to.
    ///
    /// A row an erase took the block from, while the cursor was elsewhere, belongs to the block of the fold
    /// it lies in: the gutter colours it as that block, and every other consumer sees the same block. Outside
    /// every fold it belongs to the block still unfinished when it lies between that block's head and the
    /// primary cursor, which has passed it; a block gets its fold only once it finishes.
    ///
    /// Does NOT take the lock, for the reason commandBlocks() gives; the fold lookup reads the fold ranges
    /// under the same contract.
    /// @param gridLine A grid row (negative addresses the scrollback).
    /// @return Its record, or nullptr when the row is outside the grid, belongs to no block, or its block's
    ///         record has been evicted from the store.
    [[nodiscard]] CommandBlockRecord const* commandBlockAt(LineOffset gridLine) const;

    /// The rows of one part of the block the primary screen's row @p anchor belongs to.
    ///
    /// Every row is attributed as commandBlockAt() attributes it, the anchor included, so an action names the
    /// block the gutter and the tooltip show there: a row an erase blanked inside a block's fold is that
    /// block's, one outside every fold is the unfinished block's between its head and the cursor, and no
    /// block's elsewhere. The parts are commandBlockRows()'s.
    ///
    /// Does NOT take the lock, for the reason commandBlocks() gives. May walk to the top of the history, so
    /// it runs once per user action, never per frame.
    /// @param anchor A grid row (negative addresses the scrollback).
    /// @param part Which part of the block.
    /// @return The rows, or why there are none.
    [[nodiscard]] std::expected<CommandBlockRows, CommandBlockActionError> commandBlockRowsAt(
        LineOffset anchor, CommandBlockPart part) const;

    /// The row a block's lookup stands on: its lowest row that commandBlockAt() names it for.
    ///
    /// Does NOT take the lock, for the reason commandBlocks() gives. Walks up from the bottom of the page.
    /// @param record The block.
    /// @return The row, or nullopt when none of the block's rows is left in the grid.
    [[nodiscard]] std::optional<LineOffset> commandBlockAnchor(CommandBlockRecord const& record) const;

    /// The rows of one part of the block a target names, on the primary screen.
    ///
    /// Blocks live on the primary screen only, so every target resolves there; Pointer resolves to nothing
    /// while an alternate screen is up, because the pointer is over that screen. The rows are
    /// commandBlockRowsAt()'s for the target's row, less the blank lines that end them -- read up to where
    /// the part ends on them -- though never above the part's first line.
    ///
    /// Does NOT take the lock, for the reason commandBlocks() gives. May walk to the top of the history, so
    /// it runs once per user action, never per frame.
    /// @param target Which block: under the pointer, at the cursor (the vi cursor outside Insert mode), or
    ///               the most recently finished one.
    /// @param part Which part of the block.
    /// @param pinned When non-zero, this block regardless of @p target (a context-menu row's block).
    /// @return The rows, or why there are none.
    [[nodiscard]] std::expected<CommandBlockRows, CommandBlockActionError> commandBlockRows(
        CommandBlockTarget target, CommandBlockPart part, CommandBlockId pinned = {}) const;

    /// Whether the block a target names still has a row on the primary screen: whether an action on it has
    /// anything to act on, asked without reading its rows.
    ///
    /// A row the pointer or the cursor stands on answers at once. A block named by its record (Last, or
    /// @p pinned) answers by its head row while that is on the grid, else by its lowest row, looked for from
    /// the bottom up (commandBlockAnchor()); its own rows are never walked.
    ///
    /// Does NOT take the lock, for the reason commandBlocks() gives.
    /// @param target Which block, as for commandBlockRows().
    /// @param pinned When non-zero, this block regardless of @p target.
    /// @return Whether a row of the block is left, its head's or any other.
    [[nodiscard]] bool hasCommandBlockRows(CommandBlockTarget target, CommandBlockId pinned = {}) const;

    /// Selects one part of a command block as whole lines, completed as a mouse selection would be. Where an
    /// end mark splits a line of the part (CommandBlockRows), the selection is linear from where the part
    /// starts to where it ends, which still takes every row between them whole.
    /// @param target Which block, as for commandBlockRows().
    /// @param part Which part of the block.
    /// @param pinned When non-zero, this block regardless of @p target.
    /// @return Nothing on success; NoBlock while an alternate screen is up (the selection would land on
    ///         the screen on display, not the one holding the block), else why there are no rows. A failure
    ///         leaves the selection as it was.
    [[nodiscard]] std::expected<void, CommandBlockActionError> selectCommandBlock(CommandBlockTarget target,
                                                                                  CommandBlockPart part,
                                                                                  CommandBlockId pinned = {});

    /// The text of one part of a command block, a line per logical line: the rows a soft wrap continues are
    /// joined to their head with the blanks that end them kept (they are content), as the semantic-block
    /// query reply reads them (Grid::logicalLineText()).
    /// @param target Which block, as for commandBlockRows().
    /// @param part Which part of the block.
    /// @param rendition Plain text, or text with the SGR sequences its cells wear (for `less -R`).
    /// @param pinned When non-zero, this block regardless of @p target.
    /// @return The text without a trailing newline, or why there is none.
    [[nodiscard]] std::expected<std::string, CommandBlockActionError> commandBlockText(
        CommandBlockTarget target,
        CommandBlockPart part,
        CaptureRendition rendition,
        CommandBlockId pinned = {}) const;

    /// The record of the block a target names. Last and a pinned id answer from the store, so they keep
    /// answering after the block's rows have left the grid; Pointer and Cursor need a row to stand on.
    /// @param target Which block, as for commandBlockRows().
    /// @param pinned When non-zero, the stored record with this id, even if its rows have left the grid.
    /// @return The record, or nullptr.
    [[nodiscard]] CommandBlockRecord const* targetedCommandBlock(CommandBlockTarget target,
                                                                 CommandBlockId pinned = {}) const;

    /// Drops every row above the head of the prompt the primary cursor is at -- scrollback and page alike.
    /// A head on the page moves to its top, with a running command's output below it; a head in the
    /// scrollback stays where it is and becomes the oldest row kept, the page unchanged.
    ///
    /// The head row stays whole, even where the previous command's output ended on it. The rows leave through
    /// the grid's own bookkeeping, so the eviction count, the stable floor and the fold state follow as on
    /// any eviction; the block records stay, as they do for ED 3.
    ///
    /// Does NOT take the lock -- the caller holds it.
    /// @return Nothing on success; NoBlock while an alternate screen is up or when the cursor's row belongs
    ///         to no block (no shell integration), EmptyPart when the block's prompt has left the grid.
    [[nodiscard]] std::expected<void, CommandBlockActionError> clearToPrompt();

    /// OSC 133;A (or its OSC 3008 stand-in) on the main display: closes or discards the current block per
    /// the store's rules, mints a new one headed at the primary cursor's logical line, and stamps that line.
    void beginCommandBlock();

    /// OSC 133;C (or its OSC 3008 stand-in) on the main display: the current block's command starts, in
    /// the OSC 3008 context active now.
    /// @param commandLine The command line, when one is known.
    /// @param source Where it came from; CommandLineSource::None leaves the record without one.
    void beginCommand(std::optional<std::string> commandLine, CommandLineSource source);

    /// OSC 133;D (or an OSC 3008 `end=` while OSC 3008 owns the marks): the running command finished.
    /// Raises Events::commandBlockFinished when something did finish.
    /// @param exitCode The exit code reported.
    /// @param outcome How it ended, when OSC 3008 said.
    void endCommand(int exitCode, ContextOutcome outcome);

    /// An OSC 3008 command context started while OSC 133 owns the marks: it may be the running command's
    /// own.
    /// @see CommandBlockStore::commandContextStarted
    /// @param started The command context, still on the ancestry.
    void commandContextStarted(ContextId started);

    /// An OSC 3008 command context ended while OSC 133 owns the marks: records HOW the command ended on
    /// the block whose command it was, if any, without closing or announcing anything.
    /// @see CommandBlockStore::enrichOutcome
    /// @param ended The command context that ended.
    /// @param outcome The ENDFIELDs the `end=` carried.
    void enrichCommandOutcome(ContextId ended, ContextOutcome outcome);

    /// The working directory a command starting now runs in: the OSC 3008 ancestry first, then OSC 7,
    /// with its locality.
    /// @see resolveWorkingDirectory, CwdPurpose::Display
    /// @return The snapshot; empty when nothing reported a directory.
    [[nodiscard]] WorkingDirectorySnapshot workingDirectorySnapshot() const;

    /// The working directory for @p purpose: the OSC 3008 ancestry first, then OSC 7, judged against this
    /// machine (localIdentity()) -- as the daemon's contexts spell it on a replica of a terminal on this
    /// machine (ReplicaRole::SameMachine), and not at all on a replica of another machine's
    /// (ReplicaRole::OtherMachine), whose every directory is Foreign: shown, never opened or spawned into.
    ///
    /// Does NOT take the lock: it reads the ancestry and the OSC 7 URL, so the caller holds it.
    /// @see vtbackend::resolveWorkingDirectory, bindReplica
    /// @param purpose What the directory is wanted for.
    /// @return The directory, or nullopt when no source could answer for @p purpose.
    [[nodiscard]] std::optional<ResolvedWorkingDirectory> resolveWorkingDirectory(CwdPurpose purpose) const;

    /// This machine's identity, as the listener states it: what a locality in this terminal's records
    /// means "local" to, and what a daemon mirror holds a host's verdict against.
    /// @see Events::localIdentity, whose contract (data fixed at construction) makes this safe from any
    ///      thread holding the terminal lock.
    /// @return Views valid for the listener's lifetime.
    [[nodiscard]] LocalIdentity localIdentity() const noexcept { return _eventListener.localIdentity(); }
    // }}}

    HyperlinkStorage& hyperlinks() noexcept { return _hyperlinks; }
    HyperlinkStorage const& hyperlinks() const noexcept { return _hyperlinks; }

    /// Tests whether or not the mouse is currently hovering a hyperlink.
    [[nodiscard]] bool isMouseHoveringHyperlink() const noexcept
    {
        return _hoveringHyperlinkId.load().value != 0;
    }

    /// The OSC 8 hyperlink under the pointer as of the last pointer event (press, move, leave); none when
    /// there was none. Safe from any thread, without the lock. A scroll, a screen switch or new output
    /// moves the link under a still pointer without an event, so this can be stale then; a frame asks
    /// hyperlinkIdUnderPointer() instead.
    [[nodiscard]] HyperlinkId hoveringHyperlinkId() const noexcept { return _hoveringHyperlinkId.load(); }

    /// The OSC 8 hyperlink in the cell the pointer is over right now, read from the page on display; none
    /// when the pointer is on no cell or the cell links nowhere. A pure query: nothing is stored or notified.
    ///
    /// The caller holds the lock: the cell is read from the grid, which the parser thread changes. Not
    /// noexcept, for the reason currentMouseGridPosition() gives.
    /// @see hoveringHyperlinkId
    [[nodiscard]] HyperlinkId hyperlinkIdUnderPointer() const;

    /// Retrieves the HyperlinkInfo that is currently being hovered by the mouse, if so,
    /// or a nothing otherwise. Read from the page on display, as hyperlinkIdUnderPointer() is.
    [[nodiscard]] std::shared_ptr<HyperlinkInfo const> tryGetHoveringHyperlink() const noexcept
    {
        if (auto const gridPosition = currentMouseGridPosition())
            return displayedPage().hyperlinkAt(*gridPosition);
        return {};
    }

    /// What the follow gesture, or the context menu, opens at @p gridPosition, without touching the
    /// filesystem. In order: an OSC 8 link; the detected link indexed there, while its row is unchanged
    /// since its scan; nothing, where the index holds the row as it is (isScannedAsItIs()); else the cell's
    /// logical line, scanned and matched now -- a URL is known at once, a path from the probe cache, or
    /// else handed back to be probed.
    ///
    /// A path resolves against the directory of the command that printed its row (localPathBaseAt()) and
    /// the home directory. It is refused on a replica of another machine's terminal (ReplicaRole::
    /// OtherMachine), as the `file://` URLs its application writes are (fileUrlLocalHost()): its paths name
    /// files on the daemon's machine, and one that exists here too is another file. It is refused likewise
    /// where the command that printed the row ran on another machine.
    ///
    /// The caller holds the lock: the grid, the block store and the fold cache are read, which the parser
    /// thread changes as output arrives. One logical line is scanned at most, bounded by the scan budget.
    /// @param gridPosition A cell of the page on display, in grid coordinates.
    /// @return What is there; an empty lookup where nothing is.
    [[nodiscard]] LinkLookup lookUpLinkAt(CellLocation gridPosition) const;

    [[nodiscard]] ExecutionMode executionMode() const noexcept { return _executionMode; }
    void setExecutionMode(ExecutionMode mode);

    /// The modes a key or mouse binding can ask for (MatchModes::Flag), read without the state lock.
    ///
    /// A key reads them as it arrives, and must not wait for the parser thread, which holds that lock for a
    /// whole batch. The vi mode and the execution mode are atomic themselves; the screen, the cursor keys and
    /// keypad modes, the selection and the search are published as one word by whoever changes them.
    /// @return The flags that hold.
    [[nodiscard]] uint8_t bindingModes() const noexcept;

    bool processInputOnce();

    void markScreenDirty() noexcept { _screenDirty = true; }

    [[nodiscard]] uint64_t lastFrameID() const noexcept { return _lastFrameID.load(); }

    // Screen's EventListener implementation
    //
    void requestCaptureBuffer(LineCount lines, bool logical);
    void requestShowHostWritableStatusLine();
    void bell();
    /// Tells the frontend that another page came on display. Drops the selection, which was on the page shown
    /// before, and brings the view to the bottom where autoScrollOnUpdate asks for it. Raised by showPage().
    /// @param type The screen type of the page now on display.
    void bufferChanged(ScreenType type);
    void scrollbackBufferCleared();

    /// Brings everything that names a primary-screen row along after rows above the page's top left the
    /// grid (ED 3, ClearToPrompt): the vi cursor moves with its row and, like the viewport, onto the new top
    /// when its row is gone; a hint session over the history ends; the search highlight and the gutter
    /// hover go; then as scrollbackBufferCleared(). Called only when rows were dropped, or when a mirror's
    /// discarding replay rewrites them all.
    /// @param pageShift How far the page rows moved up (Screen::dropRowsAbove()); zero when only history
    ///                  rows went.
    void rowsDropped(LineCount pageShift);
    void screenUpdated();
    /// As screenUpdated(), for a mirror's full replay: the listener hears Events::screenReplayed().
    void screenReplayed();

    /// Brings a view scrolled back into the history down to the output just applied, as
    /// Settings::autoScrollOnUpdate asks in Insert mode, where the listener follows output
    /// (@see Events::outputFollowing). Only output that scrolled lines of the page on display into its
    /// history since the last call counts (@see Screen::linesScrolledIntoHistory): a prompt, a progress bar
    /// or a clock repainted in place is not new output, and neither is output to a page that is not on
    /// display. Called with the state lock held, where output is applied and before it is published: by the
    /// parse batch, and by a mirror applying a remote screen's update.
    void followOutput();

    /// Runs @p pause with the parser thread's hold of the state lock released, and takes the lock back as
    /// that thread's own. For a linefeed under smooth scrolling (DECSCLM), which shows the output so far and
    /// waits before the next line, while the frame is built under the lock; what the batch asked to send so
    /// far goes out as the lock is released. Taken back as a frontend's, the hold would keep back what the
    /// rest of the batch sends.
    /// @param pause What runs with the lock released.
    void runWithOwnStateLockReleased(std::function<void()> const& pause);

    void renderBufferUpdated();
    [[nodiscard]] FontDef getFontDef();
    void setFontDef(FontDef const& fontDef);
    void copyToClipboard(std::string_view data);

    // {{{ Mouse pointer shape (OSC 22)
    /// @return the CSS name of the shape currently in effect.
    [[nodiscard]] std::string const& pointerShape() const noexcept { return _pointerShapes.back(); }

    /// Replaces the current shape without touching what is beneath it.
    void setPointerShape(std::string shape);

    /// Pushes a shape, remembering the one beneath so a later pop can restore it.
    void pushPointerShape(std::string shape);

    /// Discards the current shape, revealing the one beneath. The bottom of the stack is the
    /// terminal's own default and is never popped -- an application that pops more than it pushed
    /// must not be able to leave the terminal with no shape at all.
    void popPointerShape();

    /// Returns to the terminal's own default shape and notifies with an EMPTY name.
    ///
    /// The empty name is distinct from every shape an application can ask for, and means "the
    /// application is no longer imposing one". A frontend caching the application's choice needs
    /// that signal, or its own screen-type defaults never apply again. Reached by `OSC 22 ;` (the
    /// documented reset), by popping back to the bottom of the stack, and by RIS.
    void resetPointerShape();
    // }}}

    /// Answers an OSC 52 clipboard read (`OSC 52 ; Pc ; ? ST`) by replying with the current clipboard,
    /// base64-encoded, as `OSC 52 ; Pc ; <base64> ST`. Does nothing when Settings::allowClipboardRead is
    /// false (the default), so an application cannot read the clipboard unless the user opts in.
    /// @param pc The selection parameter from the request; an empty Pc is reported back as "s0", xterm's
    ///           default selection.
    void requestClipboardRead(std::string_view pc);

    /// @return the clipboard's current contents, for a protocol that formats its own reply.
    ///
    /// requestClipboardRead() both fetches and answers in OSC 52's shape; OSC 5522 has a reply shape
    /// of its own, so it needs the content without the formatting. Both are gated the same way --
    /// this returns nothing unless Settings::allowClipboardRead is set.
    [[nodiscard]] std::string clipboardContent()
    {
        return _settings.allowClipboardRead ? _eventListener.getClipboard() : std::string {};
    }

    /// The buffer an `OSC 5522` write accumulates into, and whether such a write is open.
    /// @see _kittyClipboardWrite.
    [[nodiscard]] std::string& kittyClipboardWrite() noexcept { return _kittyClipboardWrite; }
    [[nodiscard]] bool& kittyClipboardWriteOpen() noexcept { return _kittyClipboardWriteOpen; }

    void openDocument(std::string_view data);
    /// Has the frontend open a resolved link (Events::openLink()), once the terminal lock is released.
    /// @param link The link; a path only once confirmed.
    void openLink(LinkTarget const& link);
    void inspect();
    void notify(std::string_view title, std::string_view body);
    void showDesktopNotification(DesktopNotification const& notification);
    void discardDesktopNotification(std::string_view identifier);
    void focusTerminalWindow();
    [[nodiscard]] DesktopNotificationManager& desktopNotificationManager() noexcept
    {
        return _desktopNotificationManager;
    }
    void reply(std::string_view text);

    template <typename... Ts>
    void reply(std::string_view message, Ts const&... args)
    {
        reply(std::vformat(message, std::make_format_args(args...)));
    }

    void requestWindowResize(PageSize);
    void requestWindowResize(ImageSize);

    /// Asks the frontend to iconify (minimize) the window, or to restore it. @see XTWINOPS.
    void requestWindowIconify(bool iconify);

    /// Asks the frontend to move the window's top-left corner. @see XTWINOPS.
    void requestWindowMove(WindowPosition position);

    /// Asks the frontend to maximize the window, or to restore it. @see XTWINOPS.
    void requestWindowMaximize(WindowMaximize how);

    /// Asks the frontend for full screen, or out of it. @see XTWINOPS.
    void requestWindowFullScreen(WindowFullScreen how);
    void setApplicationkeypadMode(bool enabled);
    void setBracketedPaste(bool enabled);
    void setCursorStyle(CursorDisplay display, CursorShape shape);
    void setCursorVisibility(bool visible);
    void setGenerateFocusEvents(bool enabled);
    void setMarginBellVolume(BellVolume volume);
    void setMouseProtocol(MouseProtocol protocol, bool enabled);
    void setMouseTransport(MouseTransport transport);
    /// Applies one of the four coordinate-encoding DEC modes (1005/1006/1015/1016), which select a
    /// single mutually-exclusive setting between them: a set selects @p mode's encoding, a reset
    /// clears it only when it is the one currently in effect.
    /// @param mode One of the coordinate-encoding modes; anything else is a precondition violation.
    /// @param enable Whether the mode is being set or reset.
    void setMouseCoordinateMode(DECMode mode, bool enable);
    void setMouseWheelMode(InputGenerator::MouseWheelMode mode);
    void setWarningBellVolume(BellVolume volume);

    /// The RESOLVED mouse-reporting state, as the input generator will actually encode it. This is
    /// not derivable from the DEC mode register: nine mouse modes write these three values, several
    /// of them to the same one, so a set of mode bits cannot say which spelling won.
    /// @{
    [[nodiscard]] std::optional<MouseProtocol> mouseProtocol() const noexcept
    {
        return _inputGenerator.mouseProtocol();
    }
    [[nodiscard]] MouseTransport mouseTransport() const noexcept { return _inputGenerator.mouseTransport(); }
    [[nodiscard]] InputGenerator::MouseWheelMode mouseWheelMode() const noexcept
    {
        return _inputGenerator.mouseWheelMode();
    }
    /// @}
    /// Sets alternate-scroll lines per wheel notch, syncing @ref Settings and the input generator.
    /// @param lines Lines per notch (values below 1 are clamped to 1 at emit time).
    void setMouseWheelScrollMultiplier(LineCount lines);
    void setModifyOtherKeys(int mode);
    [[nodiscard]] int modifyOtherKeys() const noexcept;
    void setWindowTitle(std::string_view title);
    void setTabName(std::string_view title);
    [[nodiscard]] std::string const& windowTitle() const noexcept;

    /// Returns a copy of the raw OS-window title (OSC 0/2), read under _stateMutex.
    ///
    /// Unlike windowTitle() — which returns a reference with no lock — this is safe to call from the
    /// GUI thread: _windowTitle is written on the parser thread under _stateMutex (writeToScreen()
    /// holds the lock across the whole parse, within which setWindowTitle()/restoreWindowTitle() run).
    /// A by-value copy is required; a reference handed back after the lock releases would race the
    /// writer (torn read / use-after-free on string reallocation). Unlike resolvedTabName(), this
    /// ignores TabsNamingMode and always yields the raw title — for the GUI tab-label {WindowTitle}
    /// placeholder, which must not depend on the status-line-derived naming mode.
    /// @return The raw window title.
    [[nodiscard]] std::string resolvedWindowTitle() const;

    /// Sets the application's progress indicator state (OSC 9;4) and announces it.
    ///
    /// Lock-free for the same reason as setWindowTitle() above: the only caller is the OSC 9
    /// dispatch in Screen, which runs inside writeToScreen()'s _stateMutex hold.
    void setProgress(Progress progress);

    /// The program-status records of this terminal (OSC 7501), least recently updated first.
    ///
    /// Does NOT take the lock, for the reason commandBlocks() gives: the parser thread changes the set under
    /// it, so the caller holds the lock for as long as it reads the set.
    [[nodiscard]] ProgramStatusSet const& programStatus() const noexcept { return _programStatus; }

    /// Acts on one decoded OSC 7501 report: sets or clears records, or answers a query with
    /// programStatusDetectionReply().
    ///
    /// Lock-free for the reason setProgress() gives: the only caller is the OSC 7501 dispatch in Screen,
    /// inside writeToScreen()'s _stateMutex hold.
    /// @param report The decoded report.
    void applyProgramStatus(ProgramStatusReport const& report);

    /// The terminal's process exited: drops its working and blocked records, as the spec says, and keeps
    /// done, error and idle.
    ///
    /// Takes the lock. Called by the owner of the PTY when it sees the process end, strictly before it
    /// announces the close -- never for a close it asked for, which ends the terminal and its records with
    /// it.
    void markProcessExited();

    /// The mirror seam: replaces every record with a daemon host's, as setProgress() takes the host's
    /// progress indicator.
    ///
    /// Does NOT take the lock: the caller holds it, as a mirror applying a host's frame does.
    /// @param records The host's records, least recently updated first.
    void replaceProgramStatus(std::vector<ProgramStatusRecord> records);

    /// @return The progress indicator state, WITHOUT taking the lock. Parser thread only; the GUI
    ///         thread must use resolvedProgress().
    [[nodiscard]] Progress const& progress() const noexcept { return _progress; }

    /// Returns a copy of the progress indicator state (OSC 9;4), read under _stateMutex.
    ///
    /// The GUI-thread counterpart to progress(), and required for the same reason
    /// resolvedWindowTitle() is: _progress is written on the parser thread under _stateMutex, so an
    /// unlocked read from the GUI thread would race the writer. A by-value copy rather than a
    /// reference, so the two fields cannot be read from either side of a concurrent update.
    /// @return The progress indicator state.
    [[nodiscard]] Progress resolvedProgress() const;

    [[nodiscard]] std::optional<std::string> tabName() const noexcept;

    /// Resolves the indicator status-line tab label for this terminal under a single _stateMutex hold.
    ///
    /// Returns the explicit tab name if one is set, otherwise the window title when TabsNamingMode::Title
    /// is active, otherwise nullopt. Unlike calling tabName()/getTabsNamingMode()/windowTitle()
    /// separately, this reads _tabName/_windowTitle while holding _stateMutex — they are written on the
    /// parser thread under that same mutex (setTabName()/setWindowTitle()), and this is invoked from the
    /// GUI thread (TerminalSessionManager::updateStatusLine()), so an unlocked read would be a data race
    /// on the underlying std::string (torn read / use-after-free on reallocation).
    [[nodiscard]] std::optional<std::string> resolvedTabName() const;
    [[nodiscard]] bool focused() const noexcept { return _focused; }
    /// Read-only by design: @c Search::query is derived from the other fields, and the setters
    /// are what keep it in step. A mutable handle here is how the matcher and the highlighter came to
    /// disagree in the first place, so there is no longer one.
    [[nodiscard]] Search const& search() const noexcept { return _search; }

    // {{{ hint mode
    /// Activates hint mode, scanning the region @p request asks for.
    void activateHintMode(HintModeRequest request);

    /// Returns true if hint mode is currently active.
    [[nodiscard]] bool isHintModeActive() const noexcept { return _hintModeHandler.isActive(); }

    /// Returns the current hint matches for rendering.
    [[nodiscard]] std::vector<HintMatch> const& hintMatches() const noexcept
    {
        return _hintModeHandler.matches();
    }

    /// Returns the current hint filter prefix.
    [[nodiscard]] std::string const& hintFilter() const noexcept { return _hintModeHandler.currentFilter(); }

    /// Applies hint mode overlay to the render buffer.
    /// @param baseLine  The line offset of the first main-screen line in the render buffer.
    void applyHintOverlay(RenderBuffer& output, LineOffset baseLine) const;

    /// Returns the hint mode handler (for direct access by ViCommands).
    [[nodiscard]] HintModeHandler& hintModeHandler() noexcept { return _hintModeHandler; }

    /// Re-scans hints for the current viewport (called on scroll while hint mode is active).
    /// A no-op under HintScope::Scrollback, whose labels are assigned once and must not move.
    void refreshHints();
    // }}} hint mode

    /// Sets the icon (or tab) title, as `OSC 0` and `OSC 1` do.
    void setIconTitle(std::string_view title);

    /// @return The icon (or tab) title, as reported by `CSI 20 t`.
    [[nodiscard]] std::string const& iconTitle() const noexcept;

    /// Pushes the named titles onto the title stack, as one entry. @see XTPUSHTITLE (`CSI 22 ; Ps t`).
    void saveTitles(TitleKinds kinds);

    /// Pops one entry off the title stack and restores the named titles from it.
    /// @see XTPOPTITLE (`CSI 23 ; Ps t`), and SavedTitles for why one stack rather than two.
    ///
    /// An empty stack leaves both titles alone, as in xterm: popping more than was pushed is not an
    /// error, it simply has nothing to restore.
    void restoreTitles(TitleKinds kinds);

    void setTerminalProfile(std::string const& configProfileName);
    void useApplicationCursorKeys(bool enabled);
    void softReset();
    void hardReset();

    /// The checksum extension (XTCHECKSUM) DECRQCRA currently computes with.
    ///
    /// Terminal-wide rather than per-screen: an application that selects an extension and then
    /// switches to the alternate screen must still get the checksums it asked for.
    [[nodiscard]] ChecksumFlags checksumExtension() const noexcept { return _checksumExtension; }
    void setChecksumExtension(ChecksumFlags flags) noexcept { _checksumExtension = flags; }

    /// The User-Preferred Supplemental Set (UPSS), as assigned by DECAUPSS and reported by DECRQUPSS.
    ///
    /// Terminal-wide rather than per-cursor, for the same reason as checksumExtension() above: the
    /// G-set designations live on the cursor, so a UPSS kept there would be lost to DECSC/DECRC and
    /// to every alternate-screen switch. UPSS is a user preference, not cursor state.
    [[nodiscard]] UserPreferredSupplementalSet userPreferredSupplementalSet() const noexcept
    {
        return _userPreferredSupplementalSet;
    }
    void setUserPreferredSupplementalSet(UserPreferredSupplementalSet const& upss) noexcept
    {
        _userPreferredSupplementalSet = upss;
    }

    void forceRedraw(std::function<void()> const& artificialSleep);
    void discardImage(Image const&);
    void markCellDirty(CellLocation position) noexcept;
    void markRegionDirty(Rect area) noexcept;
    void synchronizedOutput(bool enabled);

    /// Reports the current page size in band, as `CSI 48 ; rows ; cols ; height ; width t`.
    ///
    /// A no-op unless DEC mode 2048 is set. Sent on every resize, and once when the mode is enabled.
    void reportInBandWindowResize();

    /// Keeps the viewer's state on its rows when output scrolls the whole of @p scrolled up: the Normal-mode
    /// cursor, the viewport (while scrolled back, part-row offset or in Normal mode), an open hint session
    /// and the selection move up with the content. Only where @p scrolled is the page on display, which all
    /// of that state is on: a page the cursor writes to with DECPCCM reset, the status line DECSASD selected
    /// and the indicator status line scroll under nobody's view.
    /// @param scrolled The screen whose page scrolled.
    /// @param n The lines scrolled, as the grid reports them.
    void onBufferScrolled(Screen const& scrolled, LineCount n) noexcept;

    /// Pulls the viewport and the Normal-mode cursor back inside the scrollback that still exists.
    ///
    /// Distinct from onBufferScrolled, which SHIFTS them to keep the same rows under the user as new
    /// content arrives. This only refuses to name a row that is gone, which is why it may run on the
    /// paths that must not shift anything: a top-anchored partial DECSTBM region feeds the scrollback
    /// through the same scrollUp while the live area below it does not move.
    ///
    /// Needed at all because block-atomic eviction is the first thing that makes the scrollback
    /// SHRINK while output is being written -- at capacity it used to stay pinned forever. A viewport
    /// left naming an evicted row trips Grid::render's assertion, and in a release build indexes the
    /// ring modulo its size onto unrelated rows.
    void clampToHistory() noexcept;

    void onViewportChanged();

    /// Extends the active selection to the current mouse position after a viewport scroll.
    ///
    /// Called from onViewportChanged() so that wheel-scrolling while the left mouse button
    /// is held automatically extends the selection without requiring mouse movement.
    void extendSelectionAfterScroll();

    /// How this terminal's screens measure a grapheme cluster, per DEC mode 2027; also what the status line
    /// measures and cuts its items by, so a fit it judges is the fit the screen lays out.
    ///
    /// DEC mode 2027 is how an application says it expects whole clusters to be measured. Contour keeps the
    /// mode set by default and always clusters; what the mode gates is narrower -- whether a codepoint
    /// arriving AFTER the first may change how many columns the cluster occupies. Resetting it does not
    /// restore the full pre-clustering behaviour, only this part of it: a terminal that also stopped
    /// segmenting would place every combining mark in its own cell, which is a larger change and is not
    /// what applications resetting the mode are usually after.
    /// @return Whether a late codepoint may revise its cluster's width.
    [[nodiscard]] ClusterWidthPolicy clusterWidthPolicy() const noexcept
    {
        return isModeEnabled(DECMode::Unicode) ? ClusterWidthPolicy::ClusterAware
                                               : ClusterWidthPolicy::FirstCodepoint;
    }

    /// @return The user's home directory, for abbreviating a path to `~`; empty when the environment names
    ///         none.
    [[nodiscard]] std::string_view homeDirectory() const noexcept { return _homeDirectory; }

    /// @return The branch `{Git}` shows for the shell's directory, as last looked up; empty for none. A copy,
    ///         for the lookup publishes the next one from another thread. Reads no file: the indicator status
    ///         line's repaint asks for a lookup, on the Git services' executor, and only after a command
    ///         ended or the shell reported a directory.
    [[nodiscard]] std::string gitBranch() const;

    /// @returns either an empty string or a file:// URL of the last set working directory, as reported.
    [[nodiscard]] std::string const& currentWorkingDirectory() const noexcept
    {
        return _currentWorkingDirectory.url;
    }

    /// Takes @p url as the working directory the application reports (OSC 7), unless its path holds a NUL:
    /// such a report names no directory, not a shorter one, so the previous one stays.
    /// @param url A file:// URL, a bare path, or empty for none.
    void setCurrentWorkingDirectory(std::string const& url);

    void verifyState();

    void applyPageSizeToCurrentBuffer();
    void applyPageSizeToMainDisplay(ScreenType screenType);

    [[nodiscard]] crispy::BufferObjectPtr<char> currentPtyBuffer() const noexcept
    {
        return _currentPtyBuffer;
    }

    /// Returns the buffer currently being parsed by parseFragment(), whose hot end Screen::writeText() moves
    /// past the text it wrote.
    /// @return The buffer, owned by the member that holds it and valid until parsing ends or the buffer is
    ///         replaced. Null while a macro's body is parsed: that is parsed out of a copy of its own, not
    ///         out of any pty buffer. Null outside a parse too, as when queued trace events are flushed:
    ///         their text may lie in the reader's buffer or in writeToScreen()'s, and nothing says which.
    [[nodiscard]] crispy::BufferObject<char>* parsingBuffer() const noexcept
    {
        // A raw pointer rather than a reference to a shared one: the macro's null would then have to be a
        // static, and a static shared_ptr, with its destructor to register, costs a guard on every call.
        if (isMacroRunning()) [[unlikely]]
            return nullptr;
        return _parsingBuffer.get();
    }

    [[nodiscard]] vtbackend::SelectionHelper& selectionHelper() noexcept { return _selectionHelper; }

    [[nodiscard]] Selection::OnSelectionUpdated selectionUpdatedHelper()
    {
        return [this]() {
            onSelectionUpdated();
        };
    }

    void onSelectionUpdated();

    [[nodiscard]] ViInputHandler& inputHandler() noexcept { return _inputHandler; }
    [[nodiscard]] ViInputHandler const& inputHandler() const noexcept { return _inputHandler; }

    /// The Kitty keyboard protocol's state, for a reader holding the state lock or the input lock: every
    /// change takes both. @see changeKeyboardProtocol()
    [[nodiscard]] ExtendedKeyboardInputGenerator const& keyboardProtocol() const noexcept
    {
        return _inputGenerator.keyboardProtocol();
    }

    /// Changes the Kitty keyboard protocol's state, under the input lock as well as the state lock the caller
    /// holds, as every change to what a key is encoded with is. @see changeInputEncoding()
    /// @param change What to do to the protocol's state.
    template <typename Change>
    void changeKeyboardProtocol(Change const& change)
    {
        changeInputEncoding([&](InputGenerator& input) { change(input.keyboardProtocol()); });
    }

    void resetHighlight();

    StatusDisplayType statusDisplayType() const noexcept { return _statusDisplayType; }
    void setStatusDisplay(StatusDisplayType statusDisplayType);

    /// DECSSDT: the host selects the type of status display. None and the indicator are shown at once; the
    /// host-writable status line is asked of the frontend, unless shown already, and shown once it allows it.
    /// Selected it is either way, which DECSASD asks for. Another type leaves the host-writable status line,
    /// should DECSASD have selected it (DEC STD 070 ch. 14 §2.3). @see answerHostWritableStatusLineRequest
    /// @param type The type DECSSDT selects.
    void selectStatusDisplayByHost(StatusDisplayType type);

    /// The user selects the type of status display, shown at once. DEC STD 070 ch. 14 §3 DECSSDT: "The type
    /// of Status Line can also be changed through a Set-Up field", which the user's toggle is; and ch. 14
    /// §2.3: "If the Status Display Type is chosen to be something other than "Host Writable", the Status
    /// Display will be exited", so another type leaves the host-writable status line should DECSASD have
    /// selected it. A type shown for a while and then restored (pushStatusDisplay) is no selection and does
    /// not come here.
    /// @param type The type the user selects.
    void selectStatusDisplayByUser(StatusDisplayType type);

    /// The configuration selects the type of status display, where a reload or a profile switch changed the
    /// configured type: that Set-Up field too, so the user's selection, as selectStatusDisplayByUser() makes
    /// it. While a type is shown for a while (pushStatusDisplay, vi mode's indicator), that stays up, and
    /// @p type is what popStatusDisplay() then restores.
    /// @param type The type the configuration now names.
    void selectConfiguredStatusDisplay(StatusDisplayType type);

    /// Whether DECSASD can select the host-writable status line: the host's last DECSSDT or the user selected
    /// it, or it is the type shown. DEC STD 070 ch. 14 §3 DECSASD: "the Host Writable Status Line must be
    /// selected through DECSSDT for DECSASD to be able to select the Host Writable Status Line"; and DECSSDT:
    /// "The type of Status Line can also be changed through a Set-Up field", which the configuration that
    /// shows a type is.
    /// @return Whether the host-writable status line is selected.
    [[nodiscard]] bool isHostWritableStatusLineSelected() const noexcept;
    void setActiveStatusDisplay(ActiveStatusDisplay activeDisplay);
    constexpr ActiveStatusDisplay activeStatusDisplay() const noexcept { return _activeStatusDisplay; }

    /// Puts @p statusDisplayType up for a while as @p owner's notice, over the type shown, which comes back
    /// when every notice is down again.
    /// @param owner Who puts the notice up; it alone takes it down.
    /// @param statusDisplayType The type shown while the notice is up.
    void pushStatusDisplay(StatusNotice owner, StatusDisplayType statusDisplayType);

    /// Takes @p owner's notice down, if it has one up; the last one down shows again the type shown before
    /// the first.
    /// @param owner Whose notice comes down.
    void popStatusDisplay(StatusNotice owner);

    /// Whether input reaches the application. Needs no lock: both locks are atomic, as a key reads this on
    /// the GUI thread while the parser thread may set KAM.
    /// @return true while neither the host (KAM) nor the user (setUserInputLock) locked the keyboard.
    [[nodiscard]] bool allowInput() const noexcept
    {
        return !isModeEnabled(AnsiMode::KeyboardAction)
               && _userInputLock.load(std::memory_order_relaxed) == UserInputLock::Unlocked;
    }

    /// Locks or unlocks the keyboard as the user does, making the terminal read-only or not, and puts the
    /// indicator up as the lock's notice. The user's own lock, apart from KAM: KAM is the host's, which
    /// DECSTR and RIS reset (DEC STD 070 ch. 6 §6.2), and no host sequence sets or resets this one.
    /// @param lock The user's lock.
    void setUserInputLock(UserInputLock lock);

    /// @return The user's own lock of the keyboard. @see setUserInputLock
    [[nodiscard]] UserInputLock userInputLock() const noexcept
    {
        return _userInputLock.load(std::memory_order_relaxed);
    }

    /// Searches backwards for the word vi's `*` and `#` took from under the cursor: installs @p text as the
    /// pattern (SearchOrigin::WordUnderCursor, so always literal) and moves the viewport to make the match
    /// visible. Every search reads the page on display, the one the viewport shows.
    /// @param text           The word.
    /// @param searchPosition Where to search backwards from.
    /// @return The match; @p searchPosition itself where the word was already the pattern.
    [[nodiscard]] std::optional<CellLocation> searchReverse(std::u32string text, CellLocation searchPosition);
    [[nodiscard]] std::optional<CellLocation> searchReverse(CellLocation searchPosition);

    // Searches from current position the next item downwards.
    [[nodiscard]] std::optional<CellLocation> search(CellLocation searchPosition);

    [[nodiscard]] std::optional<CellLocation> searchNextMatch(CellLocation cursorPosition);
    [[nodiscard]] std::optional<CellLocation> searchPrevMatch(CellLocation cursorPosition);

    /// Installs @p text as the active search pattern, compiled under the search's options.
    ///
    /// A double-clicked word and the word vi's `*` and `#` search for are always literal, whatever the
    /// options say; the options themselves are left as they are.
    /// @param text   The needle. Replaces whatever was being searched for; empty ends the search's query.
    /// @param origin Where it came from; selects which highlight colors its matches take.
    /// @return Whether the pattern or how it is read changed, so callers can skip a redundant search; or why
    ///         the pattern cannot be searched for, which Search::problem then holds too.
    [[nodiscard]] std::expected<SearchChange, SearchPatternError> setNewSearchTerm(std::u32string text,
                                                                                   SearchOrigin origin);

    /// Re-points the active search at @p options, recompiling its pattern where they read it differently.
    /// Does not itself move the viewport or the cursor -- the caller re-runs the search, because only it
    /// knows where from.
    /// @param options The find bar's toggles.
    /// @return Whether they changed; or why the pattern cannot be searched for under them.
    [[nodiscard]] std::expected<SearchChange, SearchPatternError> setSearchOptions(SearchOptions options);

    /// The match of the active search that begins at the normal-mode cursor, on the page on display.
    /// @return Its first cell and last column, or nothing where no match begins there.
    [[nodiscard]] std::optional<CellLocationRange> focusedSearchMatch() const;

    /// Counts the active pattern's matches across the whole page on display, scrollback included, and
    /// locates @p position among them.
    ///
    /// One traversal answers both halves of a "3 of 27" label. It is O(grid), so it is meant to be
    /// called when the pattern or the grid changes -- not per rendered frame.
    ///
    /// @param position Where the caller is standing; reported back as @c SearchMatchTally::ordinal.
    /// @param limit    Stop after this many matches and report @c TallyExactness::Capped.
    /// @return The tally. All-zero and @c Exact when the pattern is empty.
    ///
    /// @note Not const: a pattern too complex to match ends the query (R-f).
    [[nodiscard]] SearchMatchTally tallySearchMatches(CellLocation position,
                                                      size_t limit = DefaultSearchMatchLimit);

    /// Begins counting the active query's matches over the page on display, in steps. @see
    /// continueSearchTally
    /// @param position Where the caller is standing; reported back as @c SearchMatchTally::ordinal.
    /// @param limit    Stop after this many matches and report @c TallyExactness::Capped.
    /// @return The walk, and the page it walks; nothing without a query.
    [[nodiscard]] std::optional<TerminalSearchTally> beginSearchTally(
        CellLocation position, size_t limit = DefaultSearchMatchLimit) const;

    /// Reads one step of @p tally: at least @p budget rows of whole segments. Meant to be called under the
    /// terminal's lock, released between calls. A page switch since the last step restarts the walk on the
    /// page now on display.
    /// @param tally  The walk beginSearchTally() began.
    /// @param budget The rows to read at least.
    /// @return Whether rows remain; SearchPatternError::TooComplex, which also ends the active query
    ///         where the walk counts it (R-f).
    [[nodiscard]] std::expected<WalkProgress, SearchPatternError> continueSearchTally(
        TerminalSearchTally& tally, LineCount budget = SearchWalkStepLines);

    /// searchReverse(), giving up after @p budget rows of whole segments: the per-keystroke probe (R-e). A
    /// match it finds is revealed as searchReverse() reveals one.
    ///
    /// Where the whole page on display fits in the budget, the probe also answers for the rows below
    /// @p from: it finds the last match there, as a finished tally walk would jump to, or proves there is
    /// none anywhere.
    /// @param from   Where to search backwards from.
    /// @param budget The rows after which it gives up.
    /// @return The match found, and whether its absence is proved or merely not yet known.
    [[nodiscard]] SearchProbe probeSearchReverse(CellLocation from, LineCount budget = SearchWalkStepLines);

    void clearSearch();

    // Tests if the grid cell at the given location of the page on display does contain a word delimiter.
    [[nodiscard]] bool wordDelimited(CellLocation position) const noexcept;
    [[nodiscard]] bool wordDelimited(CellLocation position,
                                     std::u32string_view wordDelimiters) const noexcept;

    /// The word at @p position of the page on display, as vi's `*` and `#` search for it.
    /// @param position A cell of the word.
    /// @return The word's text and the cells it spans.
    [[nodiscard]] std::tuple<std::u32string, CellLocationRange> extractWordUnderCursor(
        CellLocation position) const noexcept;

    [[nodiscard]] Settings& factorySettings() noexcept { return _factorySettings; }
    [[nodiscard]] Settings const& factorySettings() const noexcept { return _factorySettings; }

    /// Makes @p configured the state the resets restore: RIS's operating level and status display,
    /// DECSCUSR 0's cursor, and whatever else they read from the factory settings.
    ///
    /// A setter on purpose, because a configuration reload is the feature: it changes what the terminal
    /// is configured as, and a reset returns to that, not to what the terminal was born as. The page size
    /// and the color palette keep the values the resize and resetColorPalette() maintain.
    /// @param configured The settings the terminal would be constructed with now.
    void reconfigureFactorySettings(Settings configured);

    [[nodiscard]] Settings const& settings() const noexcept { return _settings; }
    [[nodiscard]] Settings& settings() noexcept { return _settings; }

    // Renders current visual terminal state to the render buffer.
    //
    // @param output target render buffer to write the current visual state to.
    // @param includeSelection boolean to indicate whether or not to include colorize selection.
    void fillRenderBuffer(RenderBuffer& output, bool includeSelection); // <- acquires the lock

    [[nodiscard]] gsl::span<Function const> activeSequences() const noexcept
    {
        return _supportedVTSequences.activeSequences();
    }

    /// The terminal's sequence table: what is active at the current operating level, and the index
    /// that resolves a sequence against it. @see SupportedSequences::select
    [[nodiscard]] SupportedSequences const& supportedSequences() const noexcept
    {
        return _supportedVTSequences;
    }

    /// The complete VT sequence table, independent of the current operating level. A sequence that is in
    /// here but not in activeSequences() is a real capability of the terminal that is merely gated out at
    /// the present conformance level (set by DECSCL) -- recognised, but deliberately inert.
    [[nodiscard]] gsl::span<Function const> allSequences() const noexcept
    {
        return _supportedVTSequences.allSequences();
    }

    // {{{ VT parser related

    [[nodiscard]] size_t maxBulkTextSequenceWidth() const noexcept;

    [[nodiscard]] TraceHandler const& traceHandler() const noexcept { return _traceHandler; }

    [[nodiscard]] constexpr auto const& parser() const noexcept { return _parser; }
    [[nodiscard]] constexpr auto& parser() noexcept { return _parser; }

    [[nodiscard]] bool usingStdoutFastPipe() const noexcept { return _usingStdoutFastPipe; }

    void hookParser(std::unique_ptr<ParserExtension> parserExtension) noexcept
    {
        _sequenceBuilder.hookParser(std::move(parserExtension));
    }

    constexpr void resetInstructionCounter() noexcept { _instructionCounter = 0; }
    constexpr void incrementInstructionCounter(size_t n = 1) noexcept { _instructionCounter += n; }
    [[nodiscard]] constexpr uint64_t instructionCounter() const noexcept { return _instructionCounter; }
    // }}}

    std::vector<ColumnOffset>& tabs() noexcept { return _tabs; }
    std::vector<ColumnOffset> const& tabs() const noexcept { return _tabs; }

    ImagePool& imagePool() noexcept { return _imagePool; }
    ImagePool const& imagePool() const noexcept { return _imagePool; }

    /// Callback for decoding encoded images (e.g. PNG) to RGBA pixel data.
    ///
    /// @param format   The source image format.
    /// @param data     The raw encoded image data.
    /// @param size     [in/out] For PNG: the size is extracted during decoding.
    ///                 For RGB/RGBA: the size is already known.
    /// @returns Decoded RGBA pixel data, or std::nullopt on failure.
    using ImageDecoderCallback = std::function<std::optional<Image::Data>(
        ImageFormat format, std::span<uint8_t const> data, ImageSize& size)>;

    void setImageDecoder(ImageDecoderCallback decoder) noexcept { _imageDecoder = std::move(decoder); }
    ImageDecoderCallback const& imageDecoder() const noexcept { return _imageDecoder; }

    /// Takes the frontend's answer to the application's request for the host-writable status line.
    ///
    /// Shown puts the line up. Denied keeps it hidden, and has the next return to the main display (DECSASD
    /// 0) hand what the application wrote there to the window title, once, should
    /// Settings::syncWindowTitleWithHostWritableStatusDisplay ask for it then: the setting is read where it
    /// acts, so one reloaded between the answer and that return is the one in force.
    /// @param answer What the user, or the frontend's policy, decided.
    /// @see Events::requestShowHostWritableStatusLine
    void answerHostWritableStatusLineRequest(HostWritableStatusLineAnswer answer);

    /// DECSASD 0, before the main display is selected: hands the host-writable status line's text to the
    /// window title, should a denial have left that owed and the configuration still ask for it.
    /// @see answerHostWritableStatusLineRequest
    void syncWindowTitleWithDeniedStatusLine();

    /// Leaves the status line for the main display, as DECSASD 0 does -- and every other control that exits
    /// the status display: on the way out, a denial's text owed to the window title is handed over
    /// (syncWindowTitleWithDeniedStatusLine), however the line is left.
    void exitStatusLine();

    [[nodiscard]] std::optional<StatusDisplayType> savedStatusDisplayType() const noexcept
    {
        return _savedStatusDisplayType;
    }

    void setSavedStatusDisplayType(std::optional<StatusDisplayType> value) noexcept
    {
        _savedStatusDisplayType = value;
    }

    // {{{ Sixel image configuration
    void setMaxSixelColorRegisters(unsigned value) noexcept { _maxSixelColorRegisters = value; }
    [[nodiscard]] unsigned maxSixelColorRegisters() const noexcept { return _maxSixelColorRegisters; }
    std::shared_ptr<SixelColorPalette>& sixelColorPalette() noexcept { return _sixelColorPalette; }
    [[nodiscard]] bool usePrivateColorRegisters() const noexcept { return _usePrivateColorRegisters; }
    // }}}

    void triggerWordWiseSelectionWithCustomDelimiters(std::string const& delimiters);

    void setStatusLineDefinition(StatusLineDefinition&& definition);
    void resetStatusLineDefinition();

    TabsInfo guiTabsInfoForStatusLine() const
    {
        // The render thread reads this from fillRenderBufferStatusLine() (already under _stateMutex), and
        // the GUI thread writes it from setGuiTabInfoForStatusLine(). Serialize the two on a dedicated,
        // lightweight mutex rather than _stateMutex: the GUI-thread writer would otherwise block on the
        // heavily-contended _stateMutex for the full duration the parser thread holds it during a burst of
        // output, stalling input/redraw. _guiTabInfoMutex is only ever held for this small copy.
        //
        // Lock ordering: the reader nests _guiTabInfoMutex INSIDE _stateMutex (it is already held here).
        // The writer (TerminalSessionManager::updateStatusLine) DOES take _stateMutex — it resolves each
        // session name via resolvedTabName() — but it takes and RELEASES _stateMutex there, then acquires
        // _guiTabInfoMutex separately in setGuiTabInfoForStatusLine(); it never holds _stateMutex while
        // acquiring _guiTabInfoMutex. Because the writer never nests the two the other way round, there is
        // no ABBA inversion. Do NOT move the name resolution inside setGuiTabInfoForStatusLine() (i.e.
        // under _guiTabInfoMutex): that WOULD nest _stateMutex inside _guiTabInfoMutex and deadlock against
        // this reader.
        auto const l = std::lock_guard { _guiTabInfoMutex };
        return _guiTabInfoForStatusLine;
    }
    /// Stores what the `{Tabs}` item shows, and tells the status-line extensions when it changed.
    /// @param info The window's tabs.
    void setGuiTabInfoForStatusLine(TabsInfo&& info);

    TabsNamingMode getTabsNamingMode() const noexcept { return _settings.tabNamingMode; }

  private:
    /// Whether a changed screen is announced to the listener now.
    enum class ScreenNotification : uint8_t
    {
        Skipped = 0, ///< Rendering is suppressed, or a frame waits for a swap that failed again.
        Due,         ///< The listener is told.
    };

    /// What screenUpdated() and screenReplayed() share: marks the screen dirty and retries the swap of a
    /// pending render buffer.
    /// @return Due when the listener is to be told.
    [[nodiscard]] ScreenNotification markScreenChanged();

    /// The frames one repaint of the indicator status line reads, at most one per extension name: what
    /// statusLineExtensionText() reads while updateIndicatorStatusLine() runs.
    struct StatusLineExtensionFrame
    {
        std::vector<std::pair<std::string_view, std::shared_ptr<StatusLineExtensionSnapshot const>>>
            snapshots;
    };

    /// The take-over extension's frame @p snapshot, as the indicator row shows it: its top row with every
    /// image re-created in this terminal's image pool and no hyperlink. Default colours are resolved per
    /// repaint, as they follow the input mode and focus.
    /// @param snapshot A frame of the take-over extension; null before its first.
    /// @return The row; a blank one for no frame.
    [[nodiscard]] Line takeOverRowOf(StatusLineExtensionSnapshot const* snapshot);

    /// Tells the status-line extensions the indicator status line's size, if it changed since they were
    /// last told.
    void resizeStatusLineExtensions();

    /// Tells the extensions the current state of each of @p kinds that has one: the working directory, the
    /// title, the input mode, the tabs, the focus, the progress indicator, the foreground process and the
    /// user variables. With the state lock held.
    /// @param kinds The kinds to replay.
    void replayStatusLineState(StatusLineEventKinds kinds);

    /// @param kind An event kind.
    /// @return The events that tell the terminal's current state of @p kind, in the order to tell them: none
    ///         if it has none to tell, one per remembered variable for user variables, one otherwise.
    [[nodiscard]] std::vector<StatusLineEvent> currentStatusLineEvents(StatusLineEventKind kind) const;

    /// Remembers @p name's latest value for the replay, if it is known already or the bound has room.
    /// @param name The variable's name.
    /// @param base64Value Its value, as the program sent it.
    void rememberUserVar(std::string_view name, std::string_view base64Value);

    /// Replays what statusLineExtensionsPublished() found newly subscribed, if anything. With the state lock.
    void drainStatusLineReplay();

    /// Reacts to a finished status-line command run; called from an executor thread.
    /// @param output Whether the displayed text changed.
    void onStatusLineCommandCompleted(StatusLineCommandOutput output);

    /// Shows @p type, which a host's control chose (RIS, DECSSDT, or the frontend's grant of the
    /// host-writable status line DECSSDT asked for) -- unless a notice is up (pushStatusDisplay), which the
    /// host may not take down: then @p type is what comes back once the last notice is down. DEC STD 070
    /// ch. 14 §2.1.1: "The host may not directly program or affect the Error Line".
    /// @param type The type the host chose.
    void showStatusDisplayOfHost(StatusDisplayType type);

    /// Puts @p page on display, and brings the viewport, the window onto it, back inside its history.
    ///
    /// The one place the page on display changes, and the one that announces it (bufferChanged()), with what
    /// follows the page on display: the wheel mode and the key bindings' AlternateScreen mode. Showing the
    /// page already on display announces nothing. The page coming on display may have less history than the
    /// one it replaces -- every page but the primary has none -- and the frame draws it with the viewport's
    /// offset.
    /// @param page The page to display.
    void showPage(PageIndex page);

    /// What the mouse wheel does over the page on display while no binding claims it: Default -- reported to
    /// an application that enabled mouse tracking, else page 1's history scrolls -- or the cursor keys it
    /// becomes over a page without history (alternate scroll):
    ///
    /// | Mouse protocol | Page on display | ?1007 | Wheel                          |
    /// |----------------|-----------------|-------|--------------------------------|
    /// | enabled        | any             | any   | Default: tracking wins         |
    /// | off            | page 1          | any   | Default: its history scrolls   |
    /// | off            | any other       | reset | Default: alternate scroll off  |
    /// | off            | any other       | set   | cursor keys, in DECCKM's form  |
    ///
    /// @return The wheel mode for the input generator.
    [[nodiscard]] InputGenerator::MouseWheelMode wheelModeForDisplayedPage() const noexcept;

    /// Hands wheelModeForDisplayedPage() to the input generator. Called by each writer of what it derives
    /// from: showPage(), the mouse protocol modes, ?1007 and DECCKM.
    void updateWheelMode();

    /// One batch's birth stamp, in force on the primary screen for exactly as long as this lives.
    ///
    /// Rows the cursor first reaches meanwhile are born with it. When it ends the stamp is withdrawn, so a
    /// row the cursor reaches between batches -- a reset or resize the GUI asked for -- stays unstamped
    /// rather than taking a stale time, and the next batch stamps it.
    /// @see Screen::setLineBirthStamp
    class BatchClock
    {
      public:
        /// Puts @p stamp in force on @p primary.
        /// @param primary The primary screen.
        /// @param stamp The batch's birth stamp: seconds since the session began, plus one.
        BatchClock(Screen& primary, uint32_t stamp) noexcept;

        /// Withdraws the stamp.
        ~BatchClock();

        BatchClock(BatchClock const&) = delete;
        BatchClock(BatchClock&&) = delete;
        BatchClock& operator=(BatchClock const&) = delete;
        BatchClock& operator=(BatchClock&&) = delete;

      private:
        Screen& _primary;
    };

    /// Reads the wall clock ONCE for the batch about to be parsed: no clock read per line.
    /// Requires _stateMutex to be held for as long as the returned scope lives.
    /// @return The scope every row the cursor first reaches during the batch is born in.
    [[nodiscard]] BatchClock sampleBatchClock() noexcept;

    /// Runs one parse batch, the shape every entry point that parses output shares: under _stateMutex and
    /// the batch's clock, @p parse, then what it deferred (local echo) and, outside the lock, screenUpdated()
    /// unless synchronized output holds the frame back.
    /// @param lock  Holds _stateMutex already: the caller takes it, so that processInputOnce() can read ahead
    ///              while another thread holds it. Released once the batch is parsed.
    /// @param parse Parses the batch's bytes; called with _stateMutex held.
    template <typename Mutex, typename Parse>
    void runParseBatch(std::unique_lock<Mutex> lock, Parse const& parse);

    /// Offers a key to hint mode and the Vi input handler, under _stateMutex: their commands write the grid's
    /// line flags, the fold set, the user-mark index, the viewport and the selection, which the parser thread
    /// and a mirror's reactor thread write under that lock too.
    ///
    /// An Insert-mode key goes to the application without waiting for the lock, as no command wants it.
    /// What the commands produce for the PTY, and what they ask of the frontend, goes out once the lock is
    /// released. @see runUnderStateLock().
    /// @param dispatch Offers the key; called with _stateMutex held.
    /// @return What @p dispatch returned: whether hint mode or a Vi command consumed the key.
    template <typename Dispatch>
    [[nodiscard]] Handled offerToViInput(Dispatch const& dispatch);

    /// Runs @p action now, or, while this thread holds the state lock as a frontend does, once that releases
    /// it.
    /// @see lock()
    /// @param action What reaches outside the terminal: a request to the frontend that must not run under the
    ///        lock.
    void runOutsideStateLock(std::function<void()> action);

    /// A macro a DECINVM invoked, with a copy of its body: a hard reset in the body clears the definition.
    struct MacroInvocation
    {
        int id = 0;       ///< The macro, recorded as running while its body does.
        std::string body; ///< What it runs.
    };

    /// Parses @p input through the main parser, running the body of each macro a DECINVM in it invokes right
    /// after that DECINVM -- where DEC STD 070 substitutes the body for it.
    /// @param input The bytes, from the host or from a macro body.
    void parseExpandingMacros(std::string_view input);

    /// Runs a macro's body through the main parser, as the bytes the host would have sent in its place.
    /// @param invocation The macro and its body.
    void runMacro(MacroInvocation const& invocation);

    /// Re-records every block's head after row identity was destroyed (a column resize, a history-limit
    /// change, a reset), and within one generation the heads an erase or a new prompt may have taken from
    /// their blocks (Screen::blockRowChanges()) or a scroll inside margins moved (commandBlockRowsMoved()): a
    /// no-op unless one of those happened since the last call, and it fronts both commandBlocks() overloads,
    /// so no reader ever sees a head from a dead generation or a row its block lost. The same lazy shape as
    /// refreshFoldState(). A head the history let go of since the last call, or one whose block an erase left
    /// no row of, counts as a change of its record (CommandBlockStore::headsLeft()).
    void syncCommandBlockHeads() const noexcept;

    /// The re-recording syncCommandBlockHeads() does, without what it does once the heads are recorded.
    /// @return The blocks it found no row of within one generation, and so recorded as evicted, ascending.
    [[nodiscard]] std::vector<CommandBlockId> reheadCommandBlocks() const noexcept;

    /// Where a block starting at the cursor is headed, as the store records a head.
    struct BlockHeadRow
    {
        int64_t stableId {};    ///< The head row's Grid::stableLineIdOf().
        uint64_t generation {}; ///< The Grid::stableIdGeneration() that id is valid in.
    };

    /// @return The head of the logical line the primary screen's cursor is on.
    [[nodiscard]] BlockHeadRow cursorBlockHead() const noexcept;

    /// The block still unfinished, for a primary-screen row that carries no block and lies in no fold.
    ///
    /// Such a row between that block's head and the primary cursor is the block's: an erase blanked it
    /// while the cursor was elsewhere (ED 1 above the cursor, or ED 2 and a redraw that skips blank rows),
    /// and the cursor has passed it since. Below the cursor a row is not the block's yet.
    /// @param gridLine A row of the primary screen's grid.
    /// @return The newest record while it is unfinished and @p gridLine lies within its reach, else nullptr.
    [[nodiscard]] CommandBlockRecord const* unfinishedBlockReaching(LineOffset gridLine) const;

    /// The primary-screen row a block target resolves to, as commandBlockRows() resolves it.
    /// @param target Which block.
    /// @param pinned When non-zero, this block regardless of @p target.
    /// @return The row, or nullopt when the target names none.
    [[nodiscard]] std::optional<LineOffset> commandBlockAnchor(CommandBlockTarget target,
                                                               CommandBlockId pinned) const;

    /// Hands the primary screen the store's current block to stamp rows with, after a transition that may
    /// have minted it. If minting started the ids over since @p idEpochBefore, every row's block id is taken
    /// away first: a row stamped before would name a block minted after.
    /// @param idEpochBefore CommandBlockStore::idEpoch() as it was before the transition.
    void publishActiveBlock(uint64_t idEpochBefore) noexcept;

    /// Tells the listener of @p change, unless it did nothing. The caller holds _stateMutex.
    /// @param change What a change to _programStatus did.
    void announceProgramStatus(ProgramStatusChange const& change);

    /// Scroll the viewport to the bottom if `settings().autoScrollOnUpdate` is enabled.
    /// Intended for PTY/app-caused code paths only (e.g. key/char forwarding,
    /// scrollback clears). User-initiated transitions should call
    /// `_viewport.forceScrollToBottom()` directly.
    void autoScrollToBottomIfEnabled();

    /// Like `autoScrollToBottomIfEnabled()` but bypasses the `scrollingDisabled()`
    /// check (used e.g. on alt-screen switch, where scrolling is "disabled" on the
    /// target buffer but pixel/offset state still needs to be reset).
    void forceAutoScrollToBottomIfEnabled();

    /// Scrolls the viewport to the bottom in response to *user input* (a key or character
    /// forwarded to the application). Called only for key/char *press and repeat* events, never for
    /// releases: a release is not typed content, and snapping on it would undo a viewport-scroll
    /// shortcut whose press was consumed by the GUI (e.g. Shift+Up) once the protocol reports key
    /// releases to the application (win32-input-mode, Kitty keyboard protocol). Intentionally
    /// independent of `settings().autoScrollOnUpdate`, which gates only *output*-driven scrolling:
    /// typing must always reveal the cursor and the resulting output regardless of that setting.
    /// Honors the viewport's own alt-screen guard (`scrollToBottom()` no-ops when scrolling is
    /// disabled).
    void scrollToBottomOnInput();

    void mainLoop();

    /// Builds a frame of the display's, with the lock held: tells link detection whether the screen changed
    /// since the last one, then builds the render buffer (fillRenderBufferInternal()). Only the display's
    /// frames come here (ensureFreshRenderBuffer(), fillRenderBuffer()); a fill on the parser thread builds
    /// alone, so that detection's work never lands on the parser's path.
    ///
    /// A build with LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE fills on the parser thread only, so it consumes
    /// the screen-changed signal here and detects nothing.
    /// @param output The buffer to build.
    /// @param includeSelection Whether the selection is drawn.
    void fillRenderFrame(RenderBuffer& output, bool includeSelection);

    /// Builds the render buffer, with the lock held. Neither feeds link detection nor consumes the
    /// screen-changed signal. Called on its own where the fill is no frame the display builds: setPage()'s
    /// crossfade snapshot, and the frame synchronized output ends with, which the parser thread builds.
    /// @param output The buffer to build.
    /// @param includeSelection Whether the selection is drawn.
    void fillRenderBufferInternal(RenderBuffer& output, bool includeSelection);

    /// The refresh ensureFreshRenderBuffer() runs, with @p fill building the back buffer when a fill is due.
    /// @param fill Builds the frame into the buffer it is handed.
    /// @return Whether rendering is not suppressed.
    template <std::invocable<RenderBuffer&> Fill>
    bool refreshRenderBufferWith(Fill const& fill);

    /// Hands link detection the frame and publishes its next scan for nextRender(). Under the state lock,
    /// on the display's frames only (fillRenderFrame()).
    /// @param change Whether the screen changed since the previous frame.
    void detectLinks(FrameChange change);
    LineCount fillRenderBufferStatusLine(RenderBuffer& output, bool includeSelection, LineOffset base);
    void updateIndicatorStatusLine();

    /// Asks for gitBranch() for the shell's local directory, when the template shows `{Git}`. Called by
    /// updateIndicatorStatusLine() under the state lock; reads no file -- a lookup runs on the Git services'
    /// executor, and only after endCommand(), adoptCommandBlock() or setCurrentWorkingDirectory() invalidated
    /// the branch, or the directory changed.
    void refreshGitHead();

    void updateCursorVisibilityState() const noexcept;

    /// Shows the cursor and starts its blink over, as a key does.
    /// @param now When the key was pressed.
    void restartCursorBlink(Timestamp now) noexcept
    {
        _cursorBlink.store(CursorBlink { .since = now, .phase = CursorBlinkPhase::Shown }.packed(),
                           std::memory_order_relaxed);
    }
    void updateHoveringHyperlinkState();

    /// hyperlinkIdUnderPointer() for a pointer cell already looked up, so that one pointer event or one frame
    /// translates the pointer once for both kinds of link.
    /// @param pointer The grid cell under the pointer (currentMouseGridPosition()).
    /// @return The OSC 8 link in that cell; none without a cell.
    [[nodiscard]] HyperlinkId hyperlinkIdUnderPointer(std::optional<CellLocation> pointer) const;

    /// The detected link in the cell the pointer is over; none when the pointer is on no cell, or the cell is
    /// in none. An OSC 8 link's cell is in none: the scan does not read it. A pure query, the sibling of
    /// hyperlinkIdUnderPointer(): nothing is stored or notified. The caller holds the lock.
    /// @param pointer The grid cell under the pointer (currentMouseGridPosition()).
    /// @return The link's id.
    [[nodiscard]] LinkId detectedLinkIdUnderPointer(std::optional<CellLocation> pointer) const;

    /// Looks up again which detected link the pointer is over, for the tooltip and the pointer shape
    /// (hoveringLinkId()). Called on a pointer event, and by a frame whose detection changed the index: a
    /// link may have appeared under a pointer that has not moved. Notifies nobody; the caller decides whether
    /// a frame is due. The caller holds the lock.
    /// @param pointer The grid cell under the pointer (currentMouseGridPosition()).
    void refreshHoveredLink(std::optional<CellLocation> pointer);

    /// Changes what the input generator encodes keys with: a mode, the mouse protocol, the Kitty flags. Under
    /// the input lock as well as the state lock the caller holds, because a key is encoded under the input
    /// lock alone, so that typing waits for no parse.
    /// @param change What to do to the generator.
    template <typename Change>
    void changeInputEncoding(Change const& change)
    {
        auto const _ = std::scoped_lock { _inputMutex };
        change(_inputGenerator);
    }

    /// Publishes the binding modes that are not atomic themselves, for bindingModes() to read without the
    /// state lock. Called by every writer of the state they name -- the screen, DECCKM, DECNKM, the
    /// selection and the search -- once it has changed it, under that lock.
    void publishBindingModes() noexcept;

    /// Installs @p pattern, @p options and @p origin as the search, recompiling the query where the pattern
    /// or the options its origin reads it with changed. @see setNewSearchTerm, setSearchOptions
    [[nodiscard]] std::expected<SearchChange, SearchPatternError> installSearch(std::u32string pattern,
                                                                                SearchOptions options,
                                                                                SearchOrigin origin);

    /// Compiles Search::pattern under the options its origin reads it with and the current word delimiters,
    /// into Search::query or, where it does not compile, Search::problem. An empty pattern leaves both empty.
    void compileSearch();

    /// Ends the active query because matching it failed with @p error (R-f), and tells the listener.
    void endSearchQuery(SearchPatternError error);

    /// A search's answer, with a failure to match (R-f) ending the query: Search::problem holds why, and
    /// nothing highlights or steps until the pattern or the options change.
    /// @param result What the page on display answered.
    /// @return The match, or nothing where there is none or the query failed.
    [[nodiscard]] std::optional<CellLocation> settleSearch(
        std::expected<std::optional<CellLocation>, SearchPatternError> result);

    struct TheSelectionHelper: public vtbackend::SelectionHelper
    {
        Terminal* terminal;
        explicit TheSelectionHelper(Terminal* self): terminal { self } {}
        [[nodiscard]] PageSize pageSize() const noexcept override;
        [[nodiscard]] bool wrappedLine(LineOffset line) const noexcept override;
        [[nodiscard]] bool cellEmpty(CellLocation pos) const noexcept override;
        [[nodiscard]] int cellWidth(CellLocation pos) const noexcept override;
        [[nodiscard]] bool cellHoldsText(CellLocation pos) const noexcept override;
    };
    void triggerWordWiseSelection(CellLocation startPos, TheSelectionHelper const& selectionHelper);
    bool handleMouseSelection(Modifiers modifiers, uint8_t clickCount);

    /// Selects the whole lines [@p top, @p bottom] of the current screen, completed in Insert mode.
    /// @param top The first row.
    /// @param bottom The last row; not above @p top.
    void selectLines(LineOffset top, LineOffset bottom);

    /// The directories a relative path printed on @p row resolves against: those of the command that printed
    /// it (localPathBaseFor()), and outside any block the working directory for opening locally.
    ///
    /// The block is pathBlockAt()'s -- commandBlockAt()'s while the primary page is on display, as for every
    /// other action on a row --, so the row an output tail shares with the next prompt resolves against that
    /// prompt's command. Does NOT take the lock, for the reason commandBlocks() gives.
    /// @param row A grid row of the page on display; a row of any other page than the primary one belongs to
    ///            no block.
    /// @return Where to look, first to last; none when there is nowhere. Why, when no path printed there
    ///         names a file on this machine: its command ran on another one, or this terminal replicates
    ///         another machine's.
    [[nodiscard]] std::expected<PathBases, PathRefusal> localPathBaseAt(LineOffset row) const;

    /// The block whose command printed what a path on @p row names: commandBlockAt()'s while the primary page
    /// is on display, and none on any other page, whose rows belong to no block. Does NOT take the lock.
    /// @param row A grid row of the page on display.
    /// @return The block's record, or nullptr.
    [[nodiscard]] CommandBlockRecord const* pathBlockAt(LineOffset row) const;

    /// The filesystem the terminal looks printed paths up in: the one it was constructed with.
    [[nodiscard]] core::platform::FileSystem const& pathFileSystem() const noexcept;

    /// This machine as printed links name it: the home directory, and the host name (localIdentity()) a
    /// `file://` URL of it gives.
    [[nodiscard]] LinkMachine linkMachine() const;

    /// The terminal's own working directory as a path base weighs it (localPathBaseFor()). Does NOT take the
    /// lock.
    /// @return The directory; ReplicaOfAnotherMachine on a replica of another machine's terminal, whose
    ///         paths all name files there.
    [[nodiscard]] std::expected<TerminalDirectory, PathRefusal> terminalDirectory() const;

    /// Tells the block store when the directory the application reports now lies on another machine
    /// (CommandBlockStore::foreignDirectoryReported()). A replica's records carry its host's verdict, so it
    /// tells nothing.
    void recordForeignDirectory();

    /// The terminal as its link detector reads it: the page on display, during the display's frames, with
    /// the state lock held.
    class TerminalLinkSource final: public LinkSource
    {
      public:
        /// @param terminal The terminal read; it owns this source.
        explicit TerminalLinkSource(Terminal& terminal): _terminal { terminal } {}

        /// Starts a frame: what the previous one remembered of the working directories is forgotten, as a
        /// command may have changed them since.
        void beginFrame() noexcept { ++_frame; }

        /// @return The grid of the page on display, which is what the fill draws.
        [[nodiscard]] Grid& linkGrid() override { return _terminal.displayedPage().grid(); }
        /// @return The page on display.
        [[nodiscard]] PageIndex linkPage() const override { return _terminal.displayedPageIndex(); }
        /// @return The grid line at the top of the viewport.
        [[nodiscard]] LineOffset viewportTop() const override { return _terminal._viewport.topLine(); }
        /// Where relative paths printed on @p row resolve, as localPathBaseAt() says, for a row of the page
        /// on display. Each frame resolves the terminal's directory once and each block's bases once, rather
        /// than once per line read.
        /// @param row A grid row of the page on display.
        /// @return The directories, first to last, or why there are none.
        [[nodiscard]] std::expected<PathBases, PathRefusal> pathBasesAt(LineOffset row) const override;
        /// @return The home directory, for `~/`, and the host name, for `file://` URLs; empty where unknown.
        [[nodiscard]] LinkMachine localMachine() const override { return _terminal.linkMachine(); }

      private:
        Terminal& _terminal;
        uint64_t _frame = 0; ///< Bumped by beginFrame().

        /// What pathBasesAt() resolved in one frame, forgotten when the next one asks. Mutable: pathBasesAt()
        /// is a query, and this only spares it work.
        struct Memo
        {
            uint64_t frame = 0; ///< The frame it holds answers for.
            std::optional<std::expected<TerminalDirectory, PathRefusal>> directory; ///< terminalDirectory().
            /// localPathBaseFor() per block, nullptr for the rows of none.
            std::vector<std::pair<CommandBlockRecord const*, std::expected<PathBases, PathRefusal>>> bases;
        };
        mutable Memo _memo;
    };

    /// A detector for the current pattern table and detection policy. Not const: its redraw request reaches
    /// this terminal.
    /// @return The detector; nullptr without detection services, with no clickable pattern to detect, or with
    ///         a policy that detects nothing on any screen.
    [[nodiscard]] std::unique_ptr<LinkDetector> makeLinkDetector();

    /// Replaces the link detector after its configuration changed. The jobs in flight finish against what
    /// they started with, and their results find nobody to adopt them. The new detector's link ids start
    /// over, so the hovered one, which would name another link or none, is forgotten.
    void replaceLinkDetector();

    /// Selects from where @p selector starts through @p to, completed in Insert mode.
    /// @param selector The selection to install, started where it begins.
    /// @param to The cell it extends to.
    void selectThrough(std::unique_ptr<Selection> selector, CellLocation to);

    /// Tests if the text selection should be extended by the given mouse position or not.
    ///
    /// @retval false if either no selection is available, selection is complete, or the new pixel position is
    /// not enough into the next grid cell yet
    /// @retval true otherwise
    bool shouldExtendSelectionByMouse(CellLocation newPosition, PixelCoordinate pixelPosition) const noexcept;

    // Tests if the App mouse protocol is explicitly being bypassed by the user,
    // by pressing a special bypass modifier (usually Shift).
    bool allowBypassAppMouseGrabViaModifier(Modifiers modifiers) const noexcept
    {
        return _settings.mouseProtocolBypassModifiers != Modifier::None
               && modifiers.contains(_settings.mouseProtocolBypassModifiers);
    }

    /// Shared gate for handing a mouse event to the app: input allowed, no bypass modifier
    /// (usually Shift), and Vi Insert mode. The two callers add their distinguishing condition.
    bool mouseInputAllowedInInsertMode(Modifiers currentlyPressedModifier) const noexcept
    {
        return allowInput() && !allowBypassAppMouseGrabViaModifier(currentlyPressedModifier)
               && _inputHandler.mode() == ViMode::Insert;
    }

    bool allowPassMouseEventToApp(Modifiers currentlyPressedModifier) const noexcept
    {
        return _inputGenerator.mouseProtocol().has_value()
               && mouseInputAllowedInInsertMode(currentlyPressedModifier);
    }

    /// No-protocol counterpart to @ref allowPassMouseEventToApp: fires when a wheel notch should
    /// become cursor keys (alternate-scroll), i.e. wheel mode is non-Default (alt screen / ?1007).
    bool allowWheelTranslationToApp(MouseButton button, Modifiers currentlyPressedModifier) const noexcept
    {
        return isMouseWheel(button)
               && _inputGenerator.mouseWheelMode() != InputGenerator::MouseWheelMode::Default
               && mouseInputAllowedInInsertMode(currentlyPressedModifier);
    }

    // Reads from PTY.
    struct PtyReadResult
    {
        vtpty::Pty::ReadResult readResult;
        crispy::BufferObjectPtr<char> buffer; // The buffer that was read into
    };
    [[nodiscard]] std::optional<PtyReadResult> readFromPty();

#ifdef LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE
    /// Whether a passive build has a fill to run: the screen changed since the last one, or the frame last
    /// filled still waits to be swapped in.
    [[nodiscard]] bool isPassiveFillPending() const noexcept;

    /// How long a passive build's reader waits for output: until the refresh interval lets a pending fill
    /// run, or a whole interval while none is pending.
    /// @return The wait, never negative.
    [[nodiscard]] std::chrono::milliseconds passiveReadTimeout() const noexcept;

    /// Runs a passive build's fill on the reader thread, measured against the steady clock.
    ///
    /// The throttle measures the terminal's clock (currentTime()), which the display moves only as it paints
    /// -- and nothing makes it paint while a fill waits. So the clock is first brought forward to the time
    /// the frame is built at, as synchronizedOutput(false) sets it for the frame it builds on the parser
    /// thread.
    void refreshPassively();

    /// Wakes the reader where the frame it filled last waits for the front buffer the display held: the swap
    /// is retried then, rather than polled for. Called on the display's thread as it lets go of the front
    /// buffer, without the state lock: the state is atomic, and Pty::wakeupReader() is the cross-thread
    /// wake-up. A wake-up lost to a race costs one refresh interval, after which the read times out and the
    /// reader retries.
    void onFrontBufferReleased();
#endif

    /// Reads on while another thread holds the terminal's state, rather than waiting for it idle.
    ///
    /// The render thread holds the state for as long as it takes to build a frame. Reading meanwhile
    /// keeps a fast writer from blocking on a full PTY, and the parse that follows covers all of it in
    /// the same hold of the lock. A read lands at the hot end of @p buffer, which is first moved past
    /// everything read so far: that keeps the parser's own later reads from overwriting the batch, and
    /// each run of bytes from one source contiguous. A read that lands further on, because another
    /// writer appended to the buffer meanwhile and moved the hot end, becomes a piece of its own and
    /// ends the read-ahead.
    ///
    /// @param buffer The buffer @p first was read into, at its hot end.
    /// @param first  The batch's first read.
    /// @return The batch in order, starting with @p first: one piece per contiguous run of bytes from
    ///         the same source.
    [[nodiscard]] std::vector<vtpty::Pty::ReadResult> readAhead(crispy::BufferObject<char>& buffer,
                                                                vtpty::Pty::ReadResult const& first);

    /// Hands the bytes readAhead() claimed back, once the state lock is held: moves @p buffer's hot end
    /// back to the batch's start, so the parse moves it on again exactly as for a batch that was never
    /// read ahead.
    ///
    /// Claimed but unparsed bytes belong to this thread, and everything else in the buffer is already
    /// parsed: whatever another writer appended amid or after the batch, it parsed under the state lock
    /// this thread now holds, and nothing refers to a PTY buffer's bytes once they are parsed.
    ///
    /// @param buffer The buffer readAhead() claimed bytes in.
    /// @param batch  What readAhead() returned.
    void releaseReadAhead(crispy::BufferObject<char>& buffer,
                          std::span<vtpty::Pty::ReadResult const> batch) noexcept;

    // private data
    //

    Events& _eventListener;

    /// The user's home directory, for abbreviating and expanding `~` in the paths this terminal
    /// recognizes on screen. Empty when the environment names none.
    std::string _homeDirectory;

    /// Where the paths this terminal recognizes on screen are looked up. @see Terminal()
    core::platform::FileSystem const& _fileSystem;

    /// What passive link detection runs on; nullopt detects nothing. Fixed at construction.
    std::optional<LinkDetectionServices> _linkServices;

    /// The terminal as _linkDetector reads it.
    TerminalLinkSource _linkSource { *this };

    /// Whether a reply the parser raises is flushed to the PTY as soon as the parse lets go of the state
    /// lock, before the parse returns, rather than with the next flush (`$CONTOUR_SYNC_PTY_OUTPUT`). A
    /// test-harness knob: it makes replies observable in the order they were generated. @see reply()
    bool _syncPtyOutput;

    // configuration state
    Settings _factorySettings;
    Settings _settings;

    /// Lock-free mirror of _settings.pageSize for the render thread.
    ///
    /// _settings.pageSize is written under _stateMutex by resizeScreen() (GUI thread) but read every frame
    /// by the render thread (renderImpl() -> totalPageSize()) without that lock. PageSize is two ints, so a
    /// plain read concurrent with the write can tear (columns from one value, lines from another). This
    /// atomic, written in lockstep with _settings.pageSize, gives the render thread a consistent snapshot.
    std::atomic<PageSize> _atomicTotalPageSize { _settings.pageSize };

    // synchronization
    //
    // A Terminal's mutexes are each a crispy::LifetimeAnnotatedMutex, so that ThreadSanitizer forgets their
    // lock orders with the Terminal rather than join them with those of the next one built at its address.

    // TracyLockable, not a bare mutex: this is the lock the parser thread and the render path
    // contend for, so with CONTOUR_TRACY=ON its wait times are what show whether a frame is being
    // spent waiting on the parser -- the one question the GUI-side zones cannot answer alone.
    // Expands to a plain `crispy::LifetimeAnnotatedMutex _stateMutex` when profiling is off.
    // tracy::Lockable exposes lock()/unlock(), so lock()/unlock() above and the std::lock_guard CTAD
    // call sites are unaffected by the change of type.
    mutable TracyLockable(crispy::LifetimeAnnotatedMutex, _stateMutex);

    /// Whether a hold of the state lock keeps back, until it is released, what must not run under it.
    enum class HoldBack : uint8_t
    {
        No = 0, ///< The terminal's own threads, which flush where they always have, and readers.
        Yes,    ///< A frontend's. @see lock()
    };

    /// The thread holding _stateMutex; no thread while it is free. Only ever compared with the calling
    /// thread's id, which only that thread stores, so relaxed suffices. @see isStateLockHeldByThisThread()
    mutable std::atomic<std::thread::id> _stateLockOwner {};

    /// What the current hold keeps back. Written by its holder as it takes the lock, and read only by its
    /// holder: holdsBack() asks for the holder first.
    mutable HoldBack _stateHoldBack = HoldBack::No;

    /// Whether the terminal's own hold of the state lock owes a flush once it is released.
    enum class OwedFlush : uint8_t
    {
        None = 0, ///< Nothing was asked for under it.
        Owed,     ///< A flush was asked for under it, and is sent once it is released. @see releaseOwnState()
    };

    /// Written by flushInput() under the terminal's own hold, and read and cleared as it is released: by the
    /// holder only. A reader's hold asks for no flush, as it changes nothing.
    OwedFlush _owedFlush = OwedFlush::None;

    /// Whether the calling thread holds the state lock.
    [[nodiscard]] bool isStateLockHeldByThisThread() const noexcept
    {
        return _stateLockOwner.load(std::memory_order_relaxed) == std::this_thread::get_id();
    }

    /// Whether the calling thread holds the state lock as a frontend does, which keeps back until it is
    /// released what must not run under it.
    [[nodiscard]] bool holdsBack() const noexcept
    {
        return isStateLockHeldByThisThread() && _stateHoldBack == HoldBack::Yes;
    }

    /// Takes the state lock, and records the calling thread as its holder.
    /// @param holdBack What the hold keeps back.
    void acquireState(HoldBack holdBack) const;

    /// Takes the state lock if it is free, and records the calling thread as its holder.
    /// @param holdBack What the hold keeps back.
    /// @return Whether the lock was taken.
    [[nodiscard]] bool tryAcquireState(HoldBack holdBack) const;

    /// Releases the state lock, and forgets its holder.
    void releaseState() const;

    /// Releases the terminal's own hold of the state lock, then sends what a flush asked for under it, on
    /// this thread: in order with everything else queued, and with no lock held across the write.
    void releaseOwnState();

    /// The state lock as the terminal's own threads take it: the parser thread for a batch, the render thread
    /// for a frame and a glide. It records the holder as lock() does, and keeps back only the PTY write: a
    /// flush asked for under it -- a reply some sequences send on the spot -- is sent by the same thread once
    /// it has let go of the lock (@see releaseOwnState()). Everything else runs where it always has. A
    /// Lockable, so the standard's lock guards take it.
    class OwnStateLock
    {
      public:
        explicit OwnStateLock(Terminal* terminal) noexcept: _terminal { terminal } {}
        void lock() const;
        void unlock() const;
        /// @return Whether the lock was taken.
        [[nodiscard]] bool try_lock() const; // NOLINT(readability-identifier-naming)

      private:
        gsl::not_null<Terminal*> _terminal;
    };

    /// @see OwnStateLock
    OwnStateLock _ownStateLock { this };

    // Guards _inputGenerator: its pending byte queue, and the modes it generates input in. The parser thread
    // queues every reply, and asks for some to be flushed on the spot (DSR 996, the GIP replies, DECLRP, the
    // in-band resize report, CONTOUR_SYNC_PTY_OUTPUT); the GUI thread queues key, mouse, focus and paste
    // input. _stateMutex cannot serve here: the parser thread already holds it when it replies, and it is
    // non-recursive.
    //
    // Held only to queue, to copy the queue's head and to consume what was written -- never across the PTY
    // write, so that a thread that only queues, as a daemon's loop thread does reporting a resize, never
    // waits for a write the application leaves parked. @see _flushMutex
    mutable crispy::LifetimeAnnotatedMutex _inputMutex;

    // Admits one flushInput() at a time from taking the queue's head through consuming what was written, so
    // that no two flushes send the same bytes. Held across the PTY write, which UnixPty::write() turns
    // blocking once a write comes up short; never with _stateMutex held (@see flushInput()), and never across
    // echoLocally(), which parses and can therefore reply.
    crispy::LifetimeAnnotatedMutex _flushMutex;

    /// The terminal's clock: the time the frame being built is built at. Set by the render thread's tick(),
    /// before and after each frame, and by the parser thread where a synchronized update ends, as the frame
    /// it builds there is built at that time. Atomic, as the frames built on both threads read it.
    std::atomic<std::chrono::steady_clock::time_point> _currentTime;

    /// The clocks this terminal reads -- never std::chrono::*::now() directly, so a test drives them all.
    /// @see TerminalClocks
    TerminalClocks _clocks;

    /// What the wall clock read when this terminal was built, floored to the second. Line::bornAt() counts
    /// whole seconds from here, which is what lets a row's time fit the three bytes of padding it has; a
    /// whole-second epoch keeps that time in the second the row was born in, never up to one before it.
    /// Rebindable only through adoptSessionEpoch(), for a daemon mirror.
    std::chrono::system_clock::time_point _sessionEpoch;

    /// How far this terminal's wall clock is ahead of a daemon host's (negative: behind), as last measured
    /// by adoptHostClock(); zero for any other terminal. Added to the instants of every record adopted.
    std::chrono::system_clock::duration _hostClockOffset {};

    /// What this terminal replicates, as the mirror bound it. Atomic, as fileUrlLocalHost() reads it without
    /// the lock. @see bindReplica
    std::atomic<ReplicaRole> _replicaRole { ReplicaRole::None };
    /// This machine's identity as a daemon's replicated contexts spell machines, as the mirror bound it:
    /// what a ReplicaRole::SameMachine replica judges a context against. @see bindReplica
    HostIdentity _replicaIdentity;

    // {{{ PTY and PTY read buffer management
    crispy::BufferObjectPool<char> _ptyBufferPool;
    /// The reader thread's alone: it reads into it unlocked.
    crispy::BufferObjectPtr<char> _currentPtyBuffer;
    /// What writeToScreen() parses out of, under the state lock, on whichever thread calls it. Never the
    /// reader's: its text would land where the reader's next read lands, and one would overwrite the other.
    /// Allocated on the first write.
    crispy::BufferObjectPtr<char> _screenWriteBuffer;
    crispy::BufferObjectPtr<char> _parsingBuffer; // Buffer currently being parsed (set during parseFragment)
    size_t _ptyReadBufferSize;

    /// The thread that last read the PTY: under InputWriter::ReaderThread, the only one that writes to it
    /// once set. No thread until the first read. @see flushInput()
    std::atomic<std::thread::id> _readerThread {};
    std::unique_ptr<vtpty::Pty> _pty;
    // }}}

    // {{{ mouse related state (helpers for detecting double/triple clicks)
    ClickCounter _clickCounter; ///< Multi-click sequences (drag, word, extended word, line).
    vtbackend::CellLocation _currentMousePosition {}; // current mouse position
    /// What every cell position reported to the application is offset by. @see setReportedCellOrigin
    vtbackend::CellLocation _reportedCellOrigin {};
    /// Where the pointer is in device pixels, from the grid's top left, as of the last move or press: what a
    /// DEC locator report gives while DECELR selects device pixels. xterm's pixel origin is its VT window,
    /// internal border included; the grid's is the sounder one, as the cells count from it too.
    PixelCoordinate _currentMousePixelPosition {};
    /// Whether _currentMousePosition is where the pointer stands, or only where it last was on the grid. Off
    /// until a pointer event names a cell: until then the position is only its default, not the pointer's.
    PointerPlacement _pointerPlacement = PointerPlacement::OffGrid;
    vtbackend::PixelCoordinate _lastMousePixelPositionOnLeftClick {};
    LeftButtonPress _leftButtonPress = LeftButtonPress::Released; ///< What the held left button does.
    /// Moves on whenever a selection is installed or cleared, so a release can tell whether the action a
    /// claimed press ran selected anything.
    uint64_t _selectionGeneration = 0;
    uint64_t _claimedPressGeneration = 0; ///< _selectionGeneration as a claimed press found it.
    bool _respectMouseProtocol = true;    // shift-click can disable that, button release sets it back to true
    bool _isAutoScrolling = false;        // suppresses extendSelectionAfterScroll during performAutoScroll
    // }}}

    // {{{ blinking state helpers
    /// Whether the blinking cursor is shown in its current phase.
    enum class CursorBlinkPhase : uint8_t
    {
        Hidden = 0,
        Shown,
    };

    /// The cursor's blink: whether it is shown, and since when.
    struct CursorBlink
    {
        Timestamp since; ///< When the phase began: the last blink, or the key that restarted it.
        CursorBlinkPhase phase = CursorBlinkPhase::Hidden; ///< Whether the cursor is shown in it.

        /// @return Both in one integer, the time doubled and the phase in its lowest bit, for one atomic.
        [[nodiscard]] int64_t packed() const noexcept
        {
            return (since.time_since_epoch().count() * 2) + (phase == CursorBlinkPhase::Shown ? 1 : 0);
        }

        /// @param word What packed() made.
        /// @return The blink @p word holds.
        [[nodiscard]] static CursorBlink unpacked(int64_t word) noexcept
        {
            return CursorBlink { .since = Timestamp { Timestamp::duration { word >> 1 } },
                                 .phase =
                                     (word & 1) != 0 ? CursorBlinkPhase::Shown : CursorBlinkPhase::Hidden };
        }
    };

    /// The cursor's blink, packed (@see CursorBlink::packed()): one word, as a key restarts the blink on the
    /// GUI thread and the render thread advances it as it ticks, neither under the state lock. Kept apart, a
    /// tick that read the time before the key wrote both would flip the phase the key had just set, hiding
    /// the cursor for a whole interval right after the key.
    mutable std::atomic<int64_t> _cursorBlink;

    /// @return The cursor's blink, as last published.
    [[nodiscard]] CursorBlink cursorBlink() const noexcept
    {
        return CursorBlink::unpacked(_cursorBlink.load(std::memory_order_relaxed));
    }

    /// Computes blink opacity based on the configured BlinkStyle.
    struct BlinkerState
    {
        std::chrono::milliseconds period {};

        /// Returns opacity in [0, 1] based on the given blink style.
        [[nodiscard]] float opacity(std::chrono::steady_clock::time_point now,
                                    BlinkStyle style) const noexcept
        {
            auto const elapsed = std::chrono::duration<float>(now.time_since_epoch());
            auto const p = std::chrono::duration<float>(period);
            auto const smooth =
                (1.0f + std::cos(2.0f * std::numbers::pi_v<float> * elapsed.count() / p.count())) / 2.0f;
            switch (style)
            {
                case BlinkStyle::Classic: return smooth >= 0.5f ? 1.0f : 0.0f;
                case BlinkStyle::Smooth: return smooth;
                case BlinkStyle::Linger: return std::pow(smooth, 1.0f / 3.0f);
            }
            return smooth;
        }

        /// Returns the time until the next classic toggle transition.
        [[nodiscard]] std::chrono::milliseconds nextToggleIn(
            std::chrono::steady_clock::time_point now) const noexcept
        {
            using namespace std::chrono;
            auto const elapsed = duration_cast<milliseconds>(now.time_since_epoch());
            auto const halfPeriod = period / 2;
            auto const inPeriod = elapsed % period;
            return (inPeriod < halfPeriod) ? (halfPeriod - inPeriod) : (period - inPeriod);
        }
    };
    BlinkerState _slowBlinker { .period = std::chrono::milliseconds { 1000 } };
    BlinkerState _rapidBlinker { .period = std::chrono::milliseconds { 600 } };
    // }}}

    // {{{ Animation state (see Animation.h for base types and easing functors)
    // }}}

    // {{{ Screen transition state (crossfade between primary/alternate screens)
    /// Holds state for an ongoing crossfade transition between screen buffers.
    struct ScreenTransitionState: crispy::AnimationState<crispy::LinearEasing>
    {
        std::vector<RenderCell> snapshotCells {};
        std::optional<RenderCursor> snapshotCursor {};
    };
    /// Guarded by _stateMutex. Started where the screens switch (the parser thread), advanced and ended by
    /// the render-buffer fill (the render thread, or the parser thread where a synchronized update ends), and
    /// ended on a resize (the GUI thread). nextRender() reads _animationsEnd instead.
    ScreenTransitionState _screenTransition;
    // }}}

    // {{{ Cursor motion animation state
    /// Holds state for an ongoing cursor motion animation between grid cells.
    struct CursorMotionState: crispy::AnimationState<crispy::EaseOutCubic>
    {
        FractionalCellLocation fromPosition {}; ///< Between cells where the motion took over from another.
        CellLocation toPosition {};
        RGBColor fromColor {}; ///< Cursor color at the animation source position.
        RGBColor toColor {};   ///< Cursor color at the animation target position.
    };
    /// Guarded by _stateMutex. Started, advanced and ended by the render-buffer fill, on whichever thread
    /// builds the frame, and cancelled where the page switches (the parser thread) or resizes (the GUI
    /// thread). nextRender() reads _animationsEnd instead.
    CursorMotionState _cursorMotion;

    // {{{ Line flash state
    /// A row flashed where a jump landed (@see scrollToCommandBlockHead), named by its stable id so that the
    /// flash stays on that row as the view scrolls.
    struct LineFlashState: crispy::AnimationState<crispy::LinearEasing>
    {
        int64_t stableId = 0;           ///< The row.
        uint64_t stableIdGeneration {}; ///< The Grid::stableIdGeneration() @ref stableId was read in.
        RGBColor color {};              ///< The band's colour: its block's, as the gutter's band has it.
    };

    /// How long a line flash takes to fade: iTerm2's, which is long enough to catch the eye and over before
    /// the user reads on.
    static constexpr auto LineFlashDuration = std::chrono::milliseconds { 750 };

    /// A line flash's opacity as it starts. Under the row's text rather than over it, so a band in the text's
    /// own colour -- an uncoloured success -- must stay faint enough to read through.
    static constexpr auto LineFlashPeakAlpha = 0.4f;

    /// Guarded by _stateMutex. Started by a jump (the GUI thread), advanced and ended by the render-buffer
    /// fill. nextRender() reads _animationsEnd instead.
    LineFlashState _lineFlash;
    // }}}

    /// When the last of the cursor motion, the screen transition and the line flash ends, as the state lock
    /// last left them; min() while none runs. Atomic, for nextRender() to read on the render thread without
    /// that lock.
    std::atomic<std::chrono::steady_clock::time_point> _animationsEnd =
        std::chrono::steady_clock::time_point::min();

    /// Publishes _animationsEnd after a change to either animation. Under the state lock.
    void publishAnimationsEnd() noexcept;
    // }}}

    /// When link detection's next scan is due, as the last frame of the display's left it; max() while none
    /// is. Atomic, for nextRender() to read on the render thread without the state lock, under which a reload
    /// replaces the detector.
    std::atomic<std::chrono::steady_clock::time_point> _linkScanDue =
        std::chrono::steady_clock::time_point::max();

    // {{{ Momentum scrolling state
    /// Tracks velocity during a touchpad scroll gesture for momentum estimation.
    struct VelocityTracker
    {
        static constexpr size_t MaxSamples = 5;
        struct Sample
        {
            std::chrono::steady_clock::time_point time {};
            float pixelDelta = 0.0f;
        };
        std::array<Sample, MaxSamples> samples {};
        size_t count = 0;
        size_t writeIndex = 0;

        void reset() noexcept;
        void addSample(std::chrono::steady_clock::time_point time, float pixelDelta) noexcept;
        [[nodiscard]] float computeVelocity() const noexcept;
    };

    /// Holds state for an active momentum (inertia) scroll animation.
    /// Physics tuning for a momentum glide. Each scroll source (touchpad, mouse wheel) is one row of
    /// this data rather than a branch in the code — adding a source is a new @c MomentumTuning value,
    /// not another `if` scattered across the momentum methods.
    struct MomentumTuning
    {
        float frictionDecayPerSecond; ///< Velocity multiplier per second (v *= f^dt); smaller = faster stop.
        float minVelocityThreshold;   ///< Absolute px/s floor to stop at; 0 disables this rule.
        float stopVelocityFractionOfSeed; ///< Stop below this fraction of the seed velocity; 0 disables this
                                          ///< rule.
    };

    /// Touchpad inertia: gentle friction, stops at an absolute floor (classic macOS/iOS feel).
    static constexpr MomentumTuning TouchpadMomentumTuning {
        .frictionDecayPerSecond = 0.05f,    ///< ~95% decay per second.
        .minVelocityThreshold = 10.0f,      ///< px/s below which momentum stops.
        .stopVelocityFractionOfSeed = 0.0f, ///< unused for touchpad.
    };

    // Mouse-wheel glide: a discrete notch is turned into an inertial glide by seeding an initial
    // velocity and letting friction bleed it out. Two properties are wanted:
    //   1. The glide settles quickly and crisply (front-loaded ease-out feel), not floaty.
    //   2. It lands at the notch's *intended* distance, identically regardless of how many notches
    //      accumulated — a consistent feel independent of scroll speed.
    // With v(t) = v0 * f^t, the distance from v0 down to a stop velocity v_stop is (v0 - v_stop)/(-ln f).
    // Stopping at a fixed FRACTION of the seed velocity makes both the settle time and the landed
    // fraction independent of v0 (hence of the notch count). The seed velocity is
    //   v0 = distance * (-ln f) * WheelSeedCorrection,
    // where WheelSeedCorrection cancels the fixed fractional shortfall plus the discrete integrator's
    // per-frame overshoot so the glide lands on the intended distance (~1px).
    /// Mouse-wheel inertia: aggressive friction (~300ms crisp glide), stops at a fraction of the seed.
    static constexpr MomentumTuning WheelMomentumTuning {
        .frictionDecayPerSecond = 0.0005f,
        .minVelocityThreshold = 0.0f,        ///< unused for the wheel.
        .stopVelocityFractionOfSeed = 0.10f, ///< cuts the long low-velocity tail.
    };
    /// Seed-velocity correction so a wheel glide lands on the intended distance (see derivation above).
    static constexpr float WheelSeedCorrection = 1.0326f;

    struct ScrollMomentumState
    {
        bool active = false;
        MomentumTuning tuning = TouchpadMomentumTuning; ///< Physics for the active glide (source-specific).
        float velocity = 0.0f;        ///< pixels/second (positive = scroll up into history).
        float initialVelocity = 0.0f; ///< |seed velocity|; anchors the fraction-of-seed stop rule.
        std::chrono::steady_clock::time_point lastUpdate {};

        /// True once the glide's velocity has decayed below its (source-specific) stop threshold.
        [[nodiscard]] bool shouldStop() const noexcept;

        static constexpr float StartThreshold = 50.0f; ///< Touchpad: px/s required to start momentum.
    };

    /// Guarded by _stateMutex, as the view a glide moves is: armed on the GUI thread, advanced by the render
    /// thread's tick(), and cancelled by whichever thread resets the view.
    ScrollMomentumState _scrollMomentum;
    VelocityTracker _scrollVelocityTracker;

    /// Whether a glide is armed: _scrollMomentum.active, published for what reads it without the state lock.
    /// tick() takes that lock to advance a glide only while this is set, so a frame without one takes none;
    /// nextRender() and the GUI thread's wheel handling read it as well.
    std::atomic<bool> _isGliding = false;

    /// Starts (or reseeds) a momentum glide at @p velocity with the given @p tuning and wakes the
    /// render loop. Shared by the touchpad (handleScrollPhase) and mouse-wheel (injectWheelMomentum)
    /// entry points so the "arm state + kick loop" sequence lives in one place.
    /// @param velocity Seed velocity in px/s (positive = scroll up into history).
    /// @param tuning   Source-specific physics (friction + stop rule).
    /// @param now      Current time point.
    void armMomentum(float velocity,
                     MomentumTuning const& tuning,
                     std::chrono::steady_clock::time_point now) noexcept;

    /// Moves the view on by the armed glide's velocity over the time since its last step, and slows it. Ends
    /// the glide once it falls below its stop threshold or no longer moves the view. Under the state lock.
    /// @param now The time of the frame the step is for.
    void advanceMomentumGlide(std::chrono::steady_clock::time_point now);
    // }}}

    // {{{ Displays this terminal manages
    std::vector<std::unique_ptr<Screen>> _pages; ///< 16 pages: page 0 = primary, page 15 = alt screen
    PageIndex _cursorPage { 0 };                 ///< Page where cursor/VT output goes
    PageIndex _displayedPage { 0 };              ///< Page shown to user (== _cursorPage when DECPCCM set)
    /// Where paging stood when the alternate screen was last entered, so that leaving it resumes there.
    struct PagesBeforeAlternateScreen
    {
        /// The page the cursor was on; leaving returns the cursor there, as Windows Terminal's ?1049 restores
        /// the cursor's page.
        PageIndex cursor {};
        /// The page on display; leaving with DECPCCM reset shows it again: Windows Terminal's cursor restore
        /// moves its visible page only while DECPCCM is set.
        PageIndex displayed {};
    };
    PagesBeforeAlternateScreen _pagesBeforeAlternateScreen {};
    /// Which slot of _savedCursorPage the current screen uses: 1 for the alternate page, 0 for any
    /// primary page. Keyed on page identity, not _currentScreenType, so a primary VT420 page reached
    /// via PPA/NP/PP still resolves to the primary slot.
    [[nodiscard]] size_t savedCursorPageSlot() const noexcept
    {
        return isAlternateScreenBufferInUse() ? 1 : 0;
    }

    /// DECSC-saved cursor page, kept separately per screen (index 0 = primary, 1 = alternate) so the
    /// two screens' saved cursors never cross. Defaults are each screen's own page, so a DECRC with no
    /// prior DECSC stays put.
    std::array<PageIndex, 2> _savedCursorPage = NoSavedCursorPages;
    /// What _savedCursorPage holds before any DECSC, and after RIS and DECSTR: each screen's own page.
    static constexpr auto NoSavedCursorPages =
        std::array<PageIndex, 2> { PageIndex { 0 }, AlternateScreenPageIndex };
    Screen _hostWritableStatusLineScreen;
    Screen _indicatorStatusScreen;
    gsl::not_null<Screen*> _currentScreen;
    Viewport _viewport;
    /// The page on display's Screen::linesScrolledIntoHistory() as followOutput() last read it, or as the
    /// page came on display (showPage()): what it scrolled since is the output to follow. Under the state
    /// lock.
    uint64_t _displayedPageLinesFollowed = 0;
    StatusLineDefinition _indicatorStatusLineDefinition;

    // {{{ tabs info
    TabsInfo _guiTabInfoForStatusLine;
    /// When the currently open synchronized-output block began.
    ///
    /// The steady clock rather than `_currentTime`: the terminal's own clock only advances when a frame
    /// is painted, and a display that has stopped painting is precisely the state this deadline
    /// exists to break out of. Atomic because the parser thread writes it and the render thread
    /// reads it, the same reason `_renderBufferUpdateEnabled` is. Only meaningful while that flag
    /// is clear.
    std::atomic<std::chrono::steady_clock::time_point> _synchronizedOutputSince {};

    /// Guards _guiTabInfoForStatusLine. Dedicated (not _stateMutex) so the GUI-thread writer
    /// (setGuiTabInfoForStatusLine) does not block on the heavily-contended _stateMutex held by the parser
    /// thread during output bursts. Held only for the small copy in the accessor/mutator.
    crispy::LifetimeAnnotatedMutex mutable _guiTabInfoMutex;
    // }}}

    // {{{ selection states
    std::unique_ptr<Selection> _selection;
    TheSelectionHelper _selectionHelper;
    TheSelectionHelper _extendedSelectionHelper;
    TheSelectionHelper _customSelectionHelper;
    // }}}

    // {{{ Render buffer state
    /// Boolean, indicating whether the terminal's screen buffer contains updates to be rendered.
    mutable std::atomic<uint64_t> _changes { 0 };
    // Atomic because both threads touch it without a common lock: the parser thread sets it from
    // screenUpdated(), which runs OUTSIDE _stateMutex on purpose (it calls back into the GUI), while
    // the GUI/render thread reads and clears it in readFromPty()/fillRenderFrame(), the latter for link
    // detection.
    std::atomic<bool> _screenDirty = false; // TODO: just inc _changes and delete this instead.
    /// The time between two frames. Set by the frontend as the display's refresh rate is known, and read by
    /// the render thread as it asks when the next frame is due and by the parser thread as it refreshes the
    /// render buffer, none under the state lock. @see RelaxedAtomic
    RelaxedAtomic<RefreshInterval> _refreshInterval;
#ifdef LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE
    /// Wakes the reader to swap in a frame the display held up, as it lets go (@see onFrontBufferReleased).
    RenderDoubleBuffer _renderBuffer { [this] { onFrontBufferReleased(); } };
#else
    RenderDoubleBuffer _renderBuffer {};
#endif
    std::atomic<uint64_t> _lastFrameID = 0;
    /// What the last frame built found on screen. Atomic: written by the fill on whichever thread builds the
    /// frame, and read by nextRender() on the render thread without the state lock.
    std::atomic<RenderPassHints> _lastRenderPassHints {};
    // }}}

    InputMethodData _inputMethodData {};

    /// Guards _inputMethodData. The GUI thread writes the composition as it changes, without the state lock,
    /// which composing does not wait for, as typing does not; the fill copies it under that lock.
    mutable crispy::LifetimeAnnotatedMutex _inputMethodMutex;

    /// @return A copy of the composition, taken under _inputMethodMutex.
    [[nodiscard]] InputMethodData inputMethodData() const;
    std::atomic<HyperlinkId> _hoveringHyperlinkId = HyperlinkId {};
    /// The detected link under the pointer as of the last pointer event, or the last frame that changed the
    /// index (@see hoveringLinkId, refreshHoveredLink).
    std::atomic<LinkId> _hoveringLinkId = LinkId {};
    Modifiers
        _heldModifiers {}; ///< The chord the frontend last reported held. Read and written under the lock.
    std::atomic<bool> _renderBufferUpdateEnabled = true; // for "Synchronized Updates" feature
    std::optional<HighlightRange> _highlightRange = std::nullopt;
    SupportedSequences _supportedVTSequences;

    /// The binding modes that are not atomic themselves -- the screen, DECCKM, DECNKM, the selection and the
    /// search -- as publishBindingModes() last read them. @see bindingModes()
    std::atomic<uint8_t> _publishedBindingModes = 0;

    // Execution Trace mode
    std::atomic<ExecutionMode> _executionMode = ExecutionMode::Normal;
    crispy::LifetimeAnnotatedMutex _breakMutex;
    std::condition_variable _breakCondition;
    TraceHandler _traceHandler;

    /// contains the pixel size of a single cell, or area(cellPixelSize_) == 0 if unknown.
    ImageSize _cellPixelSize;

    /// Where and how the window sits on the screen. Owned by the frontend, which pushes it in; the
    /// terminal only reports it back. @see setWindowState().
    WindowState _windowState {};

    ColorPalette _defaultColorPalette;
    ColorPalette _colorPalette;
    std::vector<ColorPalette> _savedColorPalettes;
    size_t _lastSavedColorPalette = 0;

    /// Whether this terminal currently has focus. Seeded from Settings::focused (NOT defaulted here, so
    /// there is one source of truth) and thereafter written only by sendFocus{In,Out}Event.
    bool _focused;

    /// The terminal identity DA1/DA2 report, and the conformance level in force. Both seeded from
    /// Settings::terminalId (NOT defaulted here, so there is one source of truth) and thereafter
    /// written only by setTerminalId / setOperatingLevel — DECSCL for the level, a config reload for
    /// the identity.
    ///
    /// Seeding matters beyond tidiness: a terminal nobody reconfigures after construction used to
    /// report VT525 whatever its settings said, so every daemon-hosted session ignored its profile's
    /// `terminal_id` outright. The GUI was immune only because configureTerminal() assigns it.
    VTType _terminalId;
    VTType _operatingLevel;
    ControlTransmissionMode _c1TransmissionMode = ControlTransmissionMode::S7C1T;

    /// xterm title-mode features (XTSMTITLE / XTRMTITLE), one bit per TitleModeFeature. Default is all
    /// clear (xterm's DEF_TITLE_MODES), i.e. plain UTF-8 title set and query.
    std::bitset<TitleModeFeatureCount> _titleModes {};

    std::unordered_map<int, std::string> _macros;

    /// The macros running right now, outermost first: one invoking the next runs it in place.
    std::vector<int> _runningMacros;

    /// The macro a DECINVM just invoked, until the main parser returns after that DECINVM to run it.
    std::optional<MacroInvocation> _pendingMacroInvocation;

    /// Whether the macro bodies in _runningMacros were voided by clearMacros(): each ends where it stands.
    bool _areRunningMacrosVoid = false;

    /// The bytes of macro bodies the latest invocation from the stream has run, of
    /// MaxMacroBytesPerInvocation.
    size_t _macroBytesRun = 0;

    /// Local echo (SRM, reset) that was produced while the parser was running, and so could not be
    /// parsed on the spot. Written and drained only under _stateMutex. @see echoLocally().
    std::string _pendingLocalEcho;

    /// What was asked of the frontend while a frontend held _stateMutex, to be asked once that releases it.
    /// Written and taken only under _stateMutex. @see runOutsideStateLock(), unlock().
    std::vector<std::function<void()>> _outsideStateLock;

    /// The strings DECUDK programmed, by key. Written by the parser thread (DECUDK, RIS) under _inputMutex,
    /// and read under it by the GUI thread as a function key is pressed, which does not wait for the state
    /// lock: an unordered map being rewritten cannot be read alongside. @see sendKeyEvent(), udkString().
    std::unordered_map<int, std::string> _userDefinedKeys;
    bool _udkLocked = false;

    LocatorState _locatorState;
    /// The locator's buttons held down, as DECLRP's Pb names them (right 1, middle 2, left 4): what a report
    /// says of them after its event. Kept on every press and release, the locator on or off.
    uint8_t _locatorButtonsHeld = 0;
    std::unordered_map<int, DRCSCharset> _drcsCharsets;
    std::unordered_map<std::string, int> _drcsDesignatorMap; ///< Dscs designator → font number

    Modes _modes;
    std::map<DECMode, std::vector<bool>> _savedModes; //!< saved DEC modes

    /// The margins of the main display (DECSTBM, DECSLRM): one set, which the Screen of every page, the
    /// alternate screen's included, points at. DEC STD 070 keeps one top, bottom, left and right margin for
    /// the terminal (ch. 5 §3), and xterm, Windows Terminal, foot, vte and kitty share theirs between the
    /// main and the alternate screen. The status lines keep their own.
    Margin _mainDisplayMargin;
    Margin _hostWritableScreenMargin;
    Margin _indicatorScreenMargin;

    unsigned _maxSixelColorRegisters = 256;
    /// The canvas size an application asked for via XTSMGRAPHICS, unclamped, if it ever did.
    /// Empty means "follow the ceiling" -- the state the terminal starts in and that a
    /// reset-to-default returns it to. See maxImageSize().
    std::optional<ImageSize> _negotiatedImageCanvasSize;
    std::shared_ptr<SixelColorPalette> _sixelColorPalette;
    ImagePool _imagePool;

    /// Clipboard data accumulated across the `wdata` packets of one `OSC 5522` write, and whether
    /// such a write is open. Terminal-level rather than per-screen: an application may switch screens
    /// between chunks, and the transmission is the terminal's, not any one screen's.
    std::string _kittyClipboardWrite {};
    bool _kittyClipboardWriteOpen = false;

    /// The mouse pointer shape stack (`OSC 22`), innermost last. Never empty: the bottom entry is
    /// the terminal's default.
    std::vector<std::string> _pointerShapes { std::string(pointer_shape::DefaultName) };

    /// Whether the bottom of @c _pointerShapes holds a shape the APPLICATION set, rather than the
    /// terminal's own default. A pop landing there restores the former and signals a reset only for
    /// the latter. @see popPointerShape.
    bool _pointerShapeBaseSetByApplication = false;
    ImageDecoderCallback _imageDecoder;

    std::vector<ColumnOffset> _tabs;

    /// Switched by the parser thread under the state lock, and read without it by the renderer each frame.
    /// @see RelaxedAtomic
    RelaxedAtomic<ScreenType> _currentScreenType = ScreenType::Primary;
    /// The screen type of the page on display, published by showPage() under the state lock, and read without
    /// it by the frontend: its wheel path, its scrollbar and its pointer shape. @see RelaxedAtomic
    RelaxedAtomic<ScreenType> _displayedScreenType = ScreenType::Primary;
    /// Set by DECSSDT and the frontend under the state lock, and read without it by the render thread as it
    /// asks when the indicator's clock is due next. @see RelaxedAtomic
    RelaxedAtomic<StatusDisplayType> _statusDisplayType = StatusDisplayType::None;

    /// Whether a denied host-writable status line still owes its text to the window title.
    /// @see answerHostWritableStatusLineRequest
    enum class DeniedStatusLineTitle : uint8_t
    {
        Settled = 0, ///< Nothing owed: no denial, or its one sync has happened.
        Owed,        ///< Denied; the next DECSASD 0 syncs, if the configuration asks for it then.
    };
    DeniedStatusLineTitle _deniedStatusLineTitle = DeniedStatusLineTitle::Settled;

    /// Records @p type as the selected type of status display, the host's or the user's, and leaves the
    /// host-writable status line for the main display, should DECSASD have selected it, when @p type is
    /// another. DEC STD 070 ch. 14 §2.3: "If the Status Display Type is chosen to be something other than
    /// "Host Writable", the Status Display will be exited".
    /// @param type The type selected.
    void recordStatusDisplaySelection(StatusDisplayType type);

    /// The type of status display the host's last DECSSDT, or the user since, selected; RIS resets it, DECSTR
    /// keeps it (DEC STD 070 ch. 14 §2.3). It differs from the type shown, _statusDisplayType, while the
    /// host-writable status line waits for the frontend's answer or was denied, and while the frontend shows
    /// a type it did not select.
    /// @see isHostWritableStatusLineSelected, selectStatusDisplayByUser
    StatusDisplayType _selectedStatusDisplayType = StatusDisplayType::None;

    /// The type shown before the first of the notices up (_statusNotices); nothing while none is up.
    std::optional<StatusDisplayType> _savedStatusDisplayType = std::nullopt;
    /// The owners of the notices up. @see pushStatusDisplay
    StatusNotices _statusNotices {};
    /// The user's own lock of the keyboard (setUserInputLock); atomic, as allowInput() is read lock-free.
    std::atomic<UserInputLock> _userInputLock { UserInputLock::Unlocked };
    ActiveStatusDisplay _activeStatusDisplay = ActiveStatusDisplay::Main;

    Search _search;

    /// XTCHECKSUM state; reset to Settings::checksumExtension by DECSTR and RIS alike.
    ChecksumFlags _checksumExtension = _settings.checksumExtension;

    /// UPSS state (DECAUPSS/DECRQUPSS); reset to Settings::userPreferredSupplementalSet by DECSTR
    /// and RIS alike, matching xterm -- whose ReallyReset() restores the charsets unconditionally,
    /// i.e. on a soft reset as well as a hard one.
    UserPreferredSupplementalSet _userPreferredSupplementalSet = _settings.userPreferredSupplementalSet;

    FileUrl _currentWorkingDirectory {};

    unsigned _maxImageRegisterCount = 256;
    bool _usePrivateColorRegisters = false;

    bool _usingStdoutFastPipe = false;

    // Hyperlink related
    //
    HyperlinkStorage _hyperlinks {};

    std::string _windowTitle {};

    /// The application's progress indicator state (OSC 9;4). @see setProgress(), resolvedProgress().
    Progress _progress {};

    /// The program-status records (OSC 7501). Terminal-global, not per screen: an alternate-screen switch
    /// and DECSTR leave it alone; RIS empties it. Written under _stateMutex. @see programStatus().
    ProgramStatusSet _programStatus {};

    /// The icon (or tab) title. Set by `OSC 0` and `OSC 1`, reported by `CSI 20 t`.
    std::string _iconTitle {};

    /// The title stack, deepest entry first. @see SavedTitles, saveTitles(), restoreTitles().
    std::vector<SavedTitles> _savedTitles {};

    std::optional<std::string> _tabName {};

    struct ModeDependantSequenceHandler
    {
        Terminal& terminal;
        void executeControlCode(char controlCode)
        {
            terminal.sequenceHandler().executeControlCode(controlCode);
        }
        void processSequence(Sequence const& sequence)
        {
            terminal.sequenceHandler().processSequence(sequence);
        }
        void processAPC(std::string_view body) { terminal.sequenceHandler().processAPC(body); }
        void writeText(char32_t codepoint) { terminal.sequenceHandler().writeText(codepoint); }
        void writeText(std::string_view codepoints, size_t cellCount)
        {
            terminal.sequenceHandler().writeText(codepoints, cellCount);
        }
        void writeTextEnd() { terminal.sequenceHandler().writeTextEnd(); }
        [[nodiscard]] size_t maxBulkTextSequenceWidth() const noexcept
        {
            return terminal.maxBulkTextSequenceWidth();
        }
    };

    struct TerminalInstructionCounter
    {
        Terminal& terminal;
        void operator()() noexcept { terminal.incrementInstructionCounter(); }
        void operator()(size_t increment) noexcept { terminal.incrementInstructionCounter(increment); }
    };

    using StandardSequenceBuilder = SequenceBuilder<ModeDependantSequenceHandler, TerminalInstructionCounter>;

    StandardSequenceBuilder _sequenceBuilder;
    vtparser::Parser<StandardSequenceBuilder, false> _parser;
    uint64_t _instructionCounter = 0;

    InputGenerator _inputGenerator {};

    ViCommands _viCommands;
    ViInputHandler _inputHandler;

    /// Bridges Terminal to HintModeHandler::Executor.
    struct HintModeExecutor: HintModeHandler::Executor
    {
        Terminal& terminal;
        std::optional<ViMode> previousViMode;

        explicit HintModeExecutor(Terminal& t): terminal { t } {}
        void onHintSelected(HintMatch const& match, HintAction action) override;
        void onHintModeEntered() override;
        void onHintModeExited() override;
        void requestRedraw() override;
    };
    HintModeExecutor _hintModeExecutor { *this };
    HintModeHandler _hintModeHandler { _hintModeExecutor };

    /// The scope the active hint session was activated with; decides whether a scroll re-scans.
    HintScope _hintScope = HintScope::Visible;

    /// Gathers the rows hint mode scans under @p scope, extended past the labelable range to whole
    /// logical lines so a pattern wrapped across the boundary is still matched in full.
    [[nodiscard]] HintScanArea collectHintScanArea(HintScope scope, LineCount scrollbackLimit) const;

    /// One logical line read for the link scanner, and where a given cell is in it.
    struct LogicalLineScan
    {
        ScanText text;         ///< The logical line, as the patterns see it.
        ScanPosition position; ///< The requested cell in @ref text.
    };

    /// Reads the logical line holding @p gridPosition on the page on display, at most one scan budget around
    /// it. The caller holds the lock.
    /// @param gridPosition A cell of the page on display.
    /// @return The ScanText that holds the cell, and the cell in it; nullopt off the grid.
    [[nodiscard]] std::optional<LogicalLineScan> scanLogicalLineAt(CellLocation gridPosition) const;

    SemanticBlockTracker _semanticBlockTracker;

    /// Every shell command block of the primary screen. Limits and clocks are fixed at construction: a
    /// session configured differently is a different session, so there is no setter. Cleared by RIS.
    ///
    /// Mutable for the reason _foldState is: syncCommandBlockHeads() reconciles it lazily from const readers.
    mutable CommandBlockStore _commandBlocks { _settings.commandBlockLimits,
                                               _clocks.steady,
                                               _clocks.wall,
                                               [this](CommandBlockChange const& change) {
                                                   _eventListener.commandBlockChanged(change);
                                               } };

    /// @see commandBlockHeadWork
    mutable uint64_t _commandBlockHeadWork = 0;

    /// Everything scrollbarMarkInputs() is a function of, as scrollbarMarksRevision() last saw it.
    struct ScrollbarMarksInputs
    {
        uint64_t blockRevision = 0;      ///< CommandBlockStore::revision()
        uint64_t userMarkRevision = 0;   ///< UserMarkIndex::revision()
        uint64_t foldRevision = 0;       ///< FoldState::revision()
        uint64_t stableIdGeneration = 0; ///< What the heads' and marks' ids mean
        int64_t stableFloor = 0;         ///< Eviction moves every row up
        PageIndex displayedPage {};      ///< canRevealScrollback(): the page on display

        bool operator==(ScrollbarMarksInputs const&) const = default;
    };

    /// @see scrollbarMarksRevision
    mutable ScrollbarMarksInputs _scrollbarMarksInputs {};

    /// @see scrollbarMarksRevision
    mutable uint64_t _scrollbarMarksRevision = 0;

    /// The Grid::stableIdGeneration() the heads in _commandBlocks were last recorded in.
    mutable uint64_t _commandBlockHeadsGeneration = 0;

    /// The primary screen's Screen::blockRowChanges() when the heads in _commandBlocks were last looked at.
    mutable uint64_t _commandBlockHeadsRowChanges = 0;

    /// The primary grid's Grid::stableRangeFloor() when the heads in _commandBlocks were last looked at: a
    /// head between it and the floor now has left the history since.
    mutable int64_t _commandBlockHeadsFloor = 0;

    /// The stable ids whose contents a scroll moved or dropped since the heads were last looked at, as one
    /// span (@see commandBlockRowsMoved): the heads within it are looked at again. Mutable because the lazy
    /// sync consumes it.
    mutable std::optional<StableIdSpan> _commandBlockRowsMoved;

    /// The application's hierarchical context ancestry (OSC 3008), and the records the scrollback still
    /// points at. Its limits are seeded from the settings here and replaced only by a configuration
    /// reload (ContextStack::setLimits).
    ///
    /// Deliberately NOT reset by RIS or DECSTR. @see hardReset().
    ContextStack _contexts { _settings.contextLimits };

    /// Which of OSC 133 and OSC 3008 owns this session's semantic marks. Like the ancestry itself,
    /// deliberately NOT reset by RIS or DECSTR: it describes the shells attached to the pty, not the
    /// screen, and a program that emits RIS must not be able to flip it.
    MarkArbiter _markArbiter { _settings.contextMarkPolicy };

    // {{{ Output folding
    mutable FoldState _foldState;

    /// The Grid::stableIdGeneration() _foldState was last reconciled against (@see refreshFoldState).
    ///
    /// Its OWN counter, and deliberately not _foldRangesGeneration: that one is a cache stamp written
    /// by foldRanges() on every miss, so anything that merely reads the ranges after a reflow -- a
    /// resize snapping the Vi cursor, a mouse move over the gutter mid-drag -- would bring it up to
    /// date and the reconciliation below would conclude nothing had happened. The ids in _foldState
    /// would then survive a reflow that gave them to different rows, and the wrong block would show as
    /// collapsed.
    mutable uint64_t _foldStateGeneration = 0;

    /// The scrollback floor _foldState was last pruned against, or nullopt before the first
    /// reconciliation. What makes refreshFoldState() cheap enough to front every read: the floor moves
    /// only when history is evicted, so between two evictions the reconciliation is two comparisons.
    mutable std::optional<int64_t> _foldStateFloor;

    /// Everything the fold ranges are a function of.
    ///
    /// The grid's own identity: a generation bump means row identity was destroyed wholesale, and
    /// stableBase advances on every scroll, which is exactly when a mark may have moved into or out of
    /// reach. A struct with a defaulted operator== rather than a hand-written conjunction, for the
    /// reason FoldProjectionKey gives: a fifth input is then a field rather than a term someone has to
    /// remember to add to the comparison.
    ///
    /// The floor is named EXPLICITLY even though a trim currently cannot happen without stableBase
    /// moving in the same Grid::scrollUp call. That is a coincidence of where clampHistory() is
    /// called from, not a property of the cache, and the failure it would hide -- ranges whose head
    /// has been evicted, handed to foldContaining() and to the gutter -- is silent.
    struct FoldRangesKey
    {
        uint64_t generation = 0;
        int64_t stableBase = 0;
        int64_t stableFloor = 0;
        uint64_t markRevision = 0;

        [[nodiscard]] bool operator==(FoldRangesKey const&) const noexcept = default;
    };

    // Cached, because computeFoldRanges walks the scrollback: valid until the grid moves underneath it.
    // Empty key means never computed -- an all-zero key would compare equal to a fresh grid's.
    mutable std::vector<FoldRange> _foldRanges;
    mutable std::optional<FoldRangesKey> _foldRangesKey;

    /// Bumped whenever a shell mark is stamped or cleared (@see invalidateFoldRanges).
    uint64_t _semanticMarkRevision = 0;

    /// Everything the projection is a function of. Cached as one key rather than rebuilt per call,
    /// because foldProjection() hands out a SPAN over the buffer below: recomputing on each call would
    /// reallocate it and dangle the span a caller is still holding.
    ///
    /// Held as an optional for the reason FoldRangesKey is: nullopt says never computed, where an
    /// all-zero key would compare equal to the one a fresh grid produces.
    struct FoldProjectionKey
    {
        uint64_t generation = 0;
        int64_t stableBase = 0;
        int64_t stableFloor = 0;
        int scrollOffset = 0;
        int pageLines = 0;
        uint64_t foldRevision = 0;
        uint64_t markRevision = 0;
        int extraLines = 0;
        bool foldingApplies = false;

        [[nodiscard]] bool operator==(FoldProjectionKey const&) const noexcept = default;
    };

    void ensureFoldProjection() const;

    /// The stable id of grid line 0 on the page on display, the grid the vi cursor and the viewport move
    /// over.
    ///
    /// That page's own grid, not the primary one: Vi normal mode runs on the alternate screen too,
    /// where the primary's addressableTop() is negative by its whole scrollback while the alternate has
    /// none -- clamping a `k` against that bound walks the cursor off the top of a grid with no rows
    /// there. Nor the cursor's page: hiddenIntervals() name the rows of the page on display, in its ids.
    /// hiddenIntervals() is empty on every page but the primary one (nothing there folds), so every walk
    /// written against this base correctly degenerates to the plain arithmetic.
    ///
    /// @return The base to add a LineOffset to when working in id space.
    [[nodiscard]] int64_t currentStableBase() const noexcept
    {
        return displayedPage().grid().stableLineIdOf(LineOffset(0));
    }

    /// Which part of @p range's column the row @p id carries.
    ///
    /// Split out of foldMarkerAt() because the gutter has the range in hand already: looking it up a
    /// second time per row to ask what to draw there would be one binary search too many. Takes the
    /// stable id rather than the grid line for the same reason -- whoever found the range derived it on
    /// the way there.
    ///
    /// @param range The fold covering @p id.
    /// @param id The stable id of the row to describe.
    /// @return The marker, or FoldMarker::None for a row a COLLAPSED fold hides.
    [[nodiscard]] FoldMarker markerIn(FoldRange const& range, int64_t id) const noexcept;

    /// The fold in @p ranges covering @p id, if one does.
    ///
    /// The lookup foldContaining() is written in terms of, exposed separately for the gutter: it walks a
    /// page of rows against the same range list, and re-validating the range cache per row would be a
    /// frame-constant check repeated fifty times.
    ///
    /// @param ranges The fold ranges, most recent first (@see foldRanges).
    /// @param id The stable id to look up.
    /// @return The covering fold, or nullopt when none reaches that id.
    [[nodiscard]] static std::optional<FoldRange> foldCovering(std::span<FoldRange const> ranges,
                                                               int64_t id) noexcept;

    /// Moves the Vi cursor out of a block that is no longer drawn underneath it.
    void snapViCursorOutOfFolds();

    /// Restores the invariants a change to the fold set breaks, and the single funnel every mutator of
    /// _foldState ends in.
    ///
    /// Hiding rows does two things at once that nothing else repairs: it can leave the Vi cursor on a
    /// line that is no longer drawn, and it shrinks the scrollable range out from under a viewport
    /// already scrolled past the new top. Both are corrected here so that a sixth mutator has one call
    /// to make rather than a checklist to remember.
    void onFoldStateChanged();

    /// Collapses the most recently FINISHED command's fold, whichever page is on display: the engine of
    /// collapseLastFold(), and of the automatic collapse at a new prompt, which the primary screen's shell
    /// drives whatever is being shown.
    /// @return Whether there was one to collapse.
    bool collapseNewestFold();

    /// Whether a verb that can hide output may run: the one place Settings::foldingAvailability is
    /// enforced, for the vi `z` keys, the actions, the gutter and auto-collapse alike.
    [[nodiscard]] bool isFoldingAvailable() const noexcept
    {
        return _settings.foldingAvailability == FoldingAvailability::Enabled;
    }

    /// Where one collapsed fold's label is drawn: on which grid line, after how many columns of text, saying
    /// what. Shared by the painting (fillFoldLabels()) and the click (collapsedFoldLabelAt()).
    struct FoldLabelPlacement
    {
        FoldRange range;         ///< The collapsed fold.
        LineOffset gridLine;     ///< The row the label is drawn on: the head's logical line's last drawn row.
        ColumnCount textColumns; ///< How many columns of that row hold text.
        std::u32string label;    ///< The label, as collapsedFoldLabel() spells it.
    };

    /// The labels of the collapsed folds on display, each where it is drawn.
    /// @return One per collapsed fold whose head's line has a drawn row; empty while nothing is collapsed.
    [[nodiscard]] std::vector<FoldLabelPlacement> foldLabelPlacements() const;

    /// The facts that decide the block-column glyph beside @p gridLine (blockColumnGlyph()), for the gutter's
    /// painting and its press alike. Primary-screen rows only.
    /// @param gridLine A grid line of the primary screen.
    /// @return The row's fold marker, head state, user mark and its block's outcome (with exit status on).
    [[nodiscard]] BlockColumnFacts blockColumnFactsAt(LineOffset gridLine) const;

    /// blockColumnFactsAt() for a row whose stable id and covering fold the caller has already looked up:
    /// the gutter's walk and the press, which need the fold for their own decisions too.
    /// @param gridLine A grid line of the primary screen.
    /// @param id Its stable id.
    /// @param range The fold covering @p id (foldCovering()), if one does.
    /// @return As blockColumnFactsAt().
    [[nodiscard]] BlockColumnFacts blockColumnFactsAt(LineOffset gridLine,
                                                      int64_t id,
                                                      std::optional<FoldRange> const& range) const;

    /// Fills @p output's gutter: per drawn row its timestamp, line number and block-column glyph, each at
    /// its own negative column, as GutterLayout places them and appendGutterCells() paints them.
    /// @param output The buffer being built.
    /// @param baseLine The screen row the main page starts at -- non-zero with a top status line, and
    ///                 the same shift RenderBufferBuilder applies to every cell it emits.
    void fillGutter(RenderBuffer& output, LineOffset baseLine) const;

    /// Fills @p output's annotations with the `⋯ N lines` label of every collapsed fold on display, after
    /// the text of the fold's prompt line -- on the last row of that line where a long command wrapped it.
    /// @param output The buffer being built.
    /// @param baseLine As for fillGutter().
    void fillFoldLabels(RenderBuffer& output, LineOffset baseLine) const;

    /// How many screen rows down the main page rows are drawn: the whole page, or fewer where collapsed
    /// folds hide more rows than the history can backfill and the rest of the page is blank.
    /// @return The number of drawn rows, as a screen row: every row above it is drawn.
    [[nodiscard]] LineOffset drawnRowLimit() const;

    /// The cursor the user is looking at, in grid coordinates: the Vi cursor in a Vi mode, the terminal
    /// cursor otherwise. What the cursor animation follows and relative line numbers count from.
    /// @return A position on the current screen's grid.
    [[nodiscard]] CellLocation gridCursorPosition() const noexcept;

    mutable std::vector<LineOffset> _foldProjection;

    /// The hidden runs the projection was built from, kept rather than discarded: the fold-aware Vi
    /// motions ask for them on every keystroke, and they are a function of the very same cache key.
    mutable std::vector<HiddenInterval> _hiddenIntervals;

    mutable LineCount _hiddenLineCount = LineCount(0);
    mutable std::optional<FoldProjectionKey> _foldProjectionKey;

    /// The grid line the pointer hovers in the gutter, if any (@see setGutterHoverLine).
    std::optional<LineOffset> _gutterHoverLine;
    // }}}

    // {{{ Sticky command header

    /// Builds this frame's sticky command header into @p output when stickyHeaderFor() decides on one. The
    /// block it names reaches stickyHeaderBlock() with the frame, once swapBuffers() puts it in front.
    ///
    /// Its cost does not grow with the block's output: the head comes from the record's cached position, the
    /// input row from a walk of at most MaxPromptScanLines rows down from the head, and the copy is one row.
    ///
    /// @param output The buffer being built.
    /// @param baseLine The screen row the main page starts at, as for fillGutter().
    void fillStickyHeader(RenderBuffer& output, LineOffset baseLine) const;

    /// Fills @p output's line flash while one fades on a drawn row of the primary page, and ends the flash
    /// once it has faded out.
    /// @param output The buffer being built.
    /// @param baseLine The screen row the main page starts at, as for fillGutter().
    void fillLineFlash(RenderBuffer& output, LineOffset baseLine);

    /// What one frame's sticky header is decided from, and what the fill reads back from it rather than
    /// looking it up a second time.
    struct GatheredStickyHeader
    {
        StickyHeaderInput input;                    ///< What stickyHeaderFor() decides on.
        CommandBlockRecord const* record = nullptr; ///< The top row's block; set exactly when input.block is.
        /// The row the block's command was typed on; set exactly when its head is StickyHeadPosition::Above.
        std::optional<LineOffset> inputLine;
    };

    /// What stickyHeaderFor() reads about this frame, gathered without scanning the output: one lookup of
    /// the top row's block, one of its head, and one walk down its prompt.
    ///
    /// Not noexcept: the top row's block is commandBlockAt()'s, which may build the fold ranges.
    /// @return The decision's input, the record and the input row; the input's block is nullopt when no
    ///         block could get a header here (@see stickyHeaderPossible), when the top row belongs to no
    ///         known block, or to one whose head this grid cannot place yet (a daemon mirror's record before
    ///         its rows).
    [[nodiscard]] GatheredStickyHeader stickyHeaderInput() const;

    // }}}

    // {{{ Scrollbar marks
    /// The user-marked lines, by stable id (@see userMarks). Mutable for the reason _foldState is: the
    /// const read reconciles it against the grid.
    mutable UserMarkIndex _userMarks;

    /// How many changes userMarkChanged() has heard of, in any generation. While none, no line carries a
    /// mark, and userMarks() skips the rescan a generation bump would otherwise cost.
    uint64_t _userMarkChanges = 0;
    // }}}

    DesktopNotificationManager _desktopNotificationManager;

    /// The status-line extensions; null without extension services. Fixed at construction; what it forwards
    /// to may change, which statusLineExtensionsReplaced() is told of.
    IStatusLineExtensions* _extensions;

    /// Where the extensions' workers tell of a published frame; null without extension services. Attached
    /// to this terminal in the constructor, revoked in the destructor.
    StatusLinePublishSink* _publishSink;

    /// The event kinds newly subscribed to whose current state the extensions are yet to be told. Set by
    /// statusLineExtensionsPublished() on an extension worker, which reads no terminal state; drained by the
    /// next frame refresh, under the state lock (@see replayStatusLineState).
    std::atomic<StatusLineEventKinds::value_type> _extensionReplay = 0;

    /// What every hook posts through. Refreshed by statusLineExtensionsPublished(), on an extension worker;
    /// subscribed to nothing, forever, without extension services.
    StatusLineEventGate _extensionEvents;

    /// A user variable a program set, and its latest value as the program sent it (base64).
    using RememberedUserVar = std::pair<std::string, std::string>;

    /// What the extensions are told again when they newly follow it, since no host repeats it: the latest
    /// value of each user variable a program set, in the order first set, and the foreground process its
    /// host last told of (empty for none). Kept only with extension services -- without them each hook
    /// skips it in one untaken branch. Guarded by the state lock; no worker reads it.
    std::vector<RememberedUserVar> _userVars;
    std::string _foregroundProcessName;

    /// The font metrics the extensions were last told; empty until the host first tells them. Guarded by
    /// the state lock.
    StatusLineFontMetrics _statusLineFont {};

    /// How many distinct names _userVars holds at most: StatusLineExtensionServices::maxUserVars; 0
    /// without extension services. Fixed at construction.
    std::size_t _maxRememberedUserVars;

    /// The extension drawing the whole indicator row instead of its template; read at construction and by
    /// statusLineExtensionsReplaced(). Guarded by the state lock once constructed.
    std::optional<std::string> _takeOverName;

    /// The take-over frame the indicator row was last built from, and that row: an unchanged frame is not
    /// re-created in the image pool on every repaint. Guarded by the state lock.
    std::shared_ptr<StatusLineExtensionSnapshot const> _takeOverSource;
    Line _takeOverRow;

    /// The status-line size the extensions were last told. Guarded by the state lock.
    PageSize _extensionSurfaceSize {};

    /// The row a mirror's host drew, shown as the indicator status line instead of this terminal's own;
    /// none but in a mirror. Guarded by the state lock. @see setMirroredIndicatorStatusLine()
    std::optional<Line> _mirroredIndicatorRow;

    /// The frames of the repaint in progress; null outside updateIndicatorStatusLine(). Guarded by the state
    /// lock; mutable because statusLineExtensionText() is a const read that fills it.
    mutable StatusLineExtensionFrame* _extensionFrame = nullptr;

    /// Runs the indicator's `{Command}` items off thread; null without command services. Keep this the last
    /// data member but _linkDetector: its refresh callback touches _eventListener, _pty, _changes and
    /// _renderBuffer, and being destroyed before them guarantees no callback fires once the members declared
    /// before it are gone (runs still in flight keep their own state alive and merely find nobody to notify).
    std::unique_ptr<StatusLineCommandCache> _statusLineCommands;

    /// Looks up the `{Git}` branch off thread; null without Git services, which then shows none. Declared
    /// after what its redraw request touches, as _statusLineCommands is, for the same reason.
    std::unique_ptr<GitHeadCache> _gitHead;

    /// Finds links in output once it goes quiet; null without detection services, with no clickable pattern,
    /// or with detection off on every screen. Replaced, never changed, by setLinkPatterns() and
    /// setLinkDetectionPolicy(), under the state lock. Keep this the LAST data
    /// member, so that it is destroyed first: its redraw request reaches _eventListener, _pty, _changes and
    /// _renderBuffer, and ~LinkDetector cuts that request off before any of them goes (jobs still in flight
    /// keep their own state alive and merely find nobody to notify).
    std::unique_ptr<LinkDetector> _linkDetector;
};

} // namespace vtbackend

// {{{ fmt formatter specializations
template <>
struct std::formatter<vtbackend::TraceHandler::PendingSequence>: std::formatter<std::string>
{
    auto format(vtbackend::TraceHandler::PendingSequence const& pendingSequence, auto& ctx) const
    {
        std::string value;
        if (auto const* p = std::get_if<vtbackend::Sequence>(&pendingSequence))
            value = std::format("{}", p->text());
        else if (auto const* p = std::get_if<vtbackend::TraceHandler::CodepointSequence>(&pendingSequence))
            value = std::format("\"{}\"", core::escape(p->text));
        else if (auto const* p = std::get_if<char32_t>(&pendingSequence))
            value = std::format("'{}'", unicode::convert_to<char>(*p));
        else
            core::unreachable();

        return formatter<std::string>::format(value, ctx);
    }
};

template <>
struct std::formatter<vtbackend::AnsiMode>: std::formatter<std::string>
{
    auto format(vtbackend::AnsiMode mode, auto& ctx) const
    {
        return formatter<std::string>::format(toString(mode), ctx);
    }
};

template <>
struct std::formatter<vtbackend::DECMode>: std::formatter<std::string>
{
    auto format(vtbackend::DECMode mode, auto& ctx) const
    {
        return formatter<std::string>::format(toString(mode), ctx);
    }
};

template <>
struct std::formatter<vtbackend::DynamicColorName>: formatter<std::string_view>
{
    template <typename FormatContext>
    auto format(vtbackend::DynamicColorName value, FormatContext& ctx) const
    {
        using vtbackend::DynamicColorName;
        string_view name;
        switch (value)
        {
            case DynamicColorName::DefaultForegroundColor: name = "DefaultForegroundColor"; break;
            case DynamicColorName::DefaultBackgroundColor: name = "DefaultBackgroundColor"; break;
            case DynamicColorName::TextCursorColor: name = "TextCursorColor"; break;
            case DynamicColorName::MouseForegroundColor: name = "MouseForegroundColor"; break;
            case DynamicColorName::MouseBackgroundColor: name = "MouseBackgroundColor"; break;
            case DynamicColorName::HighlightForegroundColor: name = "HighlightForegroundColor"; break;
            case DynamicColorName::HighlightBackgroundColor: name = "HighlightBackgroundColor"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtbackend::ExecutionMode>: formatter<std::string_view>
{
    auto format(vtbackend::ExecutionMode value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::ExecutionMode::Normal: name = "NORMAL"; break;
            case vtbackend::ExecutionMode::Waiting: name = "WAITING"; break;
            case vtbackend::ExecutionMode::SingleStep: name = "SINGLE STEP"; break;
            case vtbackend::ExecutionMode::BreakAtEmptyQueue: name = "BREAK AT EMPTY"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

// }}}
