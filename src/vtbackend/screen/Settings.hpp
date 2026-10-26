#pragma once

#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/core/LinkDecoration.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/input/InputGenerator.hpp> // Modifier
#include <vtbackend/links/LinkDetectionPolicy.hpp>
#include <vtbackend/links/LinkPatterns.hpp>
#include <vtbackend/screen/GutterSettings.hpp>
#include <vtbackend/shell/CommandBlock.hpp> // CommandBlockStoreLimits
#include <vtbackend/shell/Folding.hpp>      // FoldJumpBehavior, FoldingAvailability
#include <vtbackend/shell/MarkArbiter.hpp>
#include <vtbackend/shell/StickyHeader.hpp>
#include <vtbackend/vt/Charset.hpp>
#include <vtbackend/vt/RectangularAreaChecksum.hpp>
#include <vtbackend/vt/VTType.hpp>

#include <vtparser/BulkScan.hpp>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>

namespace vtbackend
{

// TODO : use boxed type
struct RefreshRate
{
    double value { 24 };
};

struct RefreshInterval
{
    std::chrono::milliseconds value;

    explicit RefreshInterval(RefreshRate rate): value { static_cast<long long>(1000.0 / rate.value) } {}
};

enum class TabsNamingMode : uint8_t
{
    Indexing,
    Title
};

/// The most bytes taken from the PTY in one read by default, and so the most parsed in one hold of the
/// terminal's lock: the batch Alacritty holds its lock for. The configuration's `read_buffer_size`
/// defaults to it too, so a host that builds Settings without a configuration reads alike.
inline constexpr size_t DefaultPtyReadBufferSize = 65536;

/// What a sixel image does at the bottom margin: DECSDM (DEC mode 80) as the terminal powers up and as RIS
/// restores it.
enum class SixelScrolling : uint8_t
{
    Enabled = 0, ///< The page scrolls up to make room for the image: DECSDM reset.
    Disabled,    ///< The image stops at the bottom margin: DECSDM set (DECMode::NoSixelScrolling).
};

/// A value the render thread reads without the terminal's state lock -- as it ticks, and asks when the next
/// frame is due -- while the parser thread or the frontend writes it under that lock. Each read and write is
/// a relaxed atomic one, and a copy copies the value, so what holds it stays a value type.
template <typename T>
class RelaxedAtomic
{
  public:
    /// @param value The value to start with.
    constexpr RelaxedAtomic(T value) noexcept: _value { value } {}

    RelaxedAtomic(RelaxedAtomic const& other) noexcept: _value { other.get() } {}

    RelaxedAtomic& operator=(RelaxedAtomic const& other) noexcept
    {
        set(other.get());
        return *this;
    }

    RelaxedAtomic& operator=(T value) noexcept
    {
        set(value);
        return *this;
    }

    ~RelaxedAtomic() = default;

    /// @return The value.
    [[nodiscard]] T get() const noexcept { return _value.load(std::memory_order_relaxed); }

    /// @param value The value to hold from now on.
    void set(T value) noexcept { _value.store(value, std::memory_order_relaxed); }

    /// @return The value, so that the setting reads as the value it holds.
    operator T() const noexcept { return get(); }

  private:
    std::atomic<T> _value;
};

/// Which thread writes to the PTY the input another thread queues: a frontend's keys, mouse reports and
/// pastes, and what its hold of the terminal lock raised. @see Terminal::flushInput(), Terminal::unlock()
enum class InputWriter : uint8_t
{
    /// The thread queuing it writes it, or the thread releasing the lock it was queued under: for a terminal
    /// one thread drives, as a test drives one.
    QueueingThread = 0,
    /// The PTY's reader writes it, woken by the thread queuing it, which then never parks on a write the
    /// application leaves unread: a GUI's thread, or a daemon's event loop. Until the reader first reads,
    /// the queuing thread writes it.
    ReaderThread,
};

/// Terminal settings, enabling hardware reset to be easier implemented.
struct Settings
{
    VTType terminalId = VTType::VT525;
    ColorPalette colorPalette; // NB: The default color palette can be taken from the factory settings.

    // Set of DEC modes that are frozen and cannot be changed by the application.
    std::map<vtbackend::DECMode, bool> frozenModes;

    // total page size available to this terminal.
    // This page size may differ from the main displays (primary/alternate screen) page size If
    // some other display is shown along with it (e.g. below the main display).
    PageSize pageSize = PageSize { LineCount(25), ColumnCount(80) };

    HistoryLimits historyLimits;

    /// The user's home directory, for abbreviating and expanding `~` in the paths this terminal shows and
    /// recognizes, as the composition root reads it (the GUI's QDir::homePath(), which the pane header
    /// abbreviates with too). Empty takes the environment's `HOME`.
    std::string homeDirectory {};

    ImageSize maxImageSize { Width(800), Height(600) };
    unsigned maxImageRegisterCount = 256;

    /// Largest total size, in decoded bytes, of the kitty graphics images and animation frames a
    /// screen holds for later display.
    ///
    /// Bounds an attack driven straight from the wire: image ids are 32-bit, so without a quota an
    /// application can park 2^32 distinct images in the terminal. A transmission or frame that would
    /// exceed it is refused with `ENOSPC` rather than silently evicting an image the application still
    /// intends to place.
    size_t kittyImageStorageQuota = static_cast<size_t>(128 * 1024 * 1024);
    bool goodImageProtocol = false;

    /// Whether sixel images scroll the page, as the terminal starts and as RIS returns it: a configured
    /// setting, like the VT340's Graphics Set-Up feature of that name and xterm's `sixelScrolling` resource,
    /// which DECSDM changes only until the next RIS. DECSTR leaves the mode alone.
    SixelScrolling sixelScrolling = SixelScrolling::Enabled;

    /// Whether an application may read the clipboard via OSC 52 (`OSC 52 ; Pc ; ? ST`). Disabled by
    /// default: clipboard reading is a well-known exfiltration vector, so it is opt-in. Writing the
    /// clipboard via OSC 52 is unaffected by this. @see Terminal::requestClipboardRead.
    bool allowClipboardRead = false;

    /// Whether DEC mode 2027 (grapheme clustering) starts out set. While set, a codepoint arriving
    /// after the first may revise how many columns its grapheme cluster occupies.
    /// @see vtbackend::ClusterWidthPolicy.
    bool graphemeClustering = true;
    StatusDisplayType statusDisplayType = StatusDisplayType::None;
    /// Read by the renderer each frame. @see RelaxedAtomic
    RelaxedAtomic<StatusDisplayPosition> statusDisplayPosition = StatusDisplayPosition::Bottom;
    struct
    {
        std::string left { "{VTType} │ {InputMode:Bold,Color=#C0C030}"
                           "{TraceMode:Bold,Color=#FFFF00,Left= │ }{ProtectedMode:Bold,Left= │ }"
                           // Collapses to nothing without a privilege or machine boundary.
                           "{Context:Left= │ }" };
        std::string middle { "{Title:Left= « ,Right= » ,Color=#20c0c0}" };
        std::string right { "{HistoryLineCount:Faint,Color=#c0c0c0} │ {Clock:Bold} " };
    } indicatorStatusLine;
    bool syncWindowTitleWithHostWritableStatusDisplay = true;
    /// Read by the render thread as it ticks. @see RelaxedAtomic
    RelaxedAtomic<CursorDisplay> cursorDisplay = CursorDisplay::Steady;
    CursorShape cursorShape = CursorShape::Block;
    BellVolume warningBellVolume = BellVolume::High;
    BellVolume marginBellVolume = BellVolume::Off;
    /// Read by the render thread as it asks when the next frame is due. @see RelaxedAtomic
    RelaxedAtomic<BlinkStyle> blinkStyle = BlinkStyle::Smooth;
    ScreenTransitionStyle screenTransitionStyle = ScreenTransitionStyle::Fade;
    std::chrono::milliseconds screenTransitionDuration { 250 };
    std::chrono::milliseconds cursorMotionAnimationDuration { 80 };

    bool usePrivateColorRegisters = false;

    /// The checksum extension (XTCHECKSUM) the terminal starts with, and returns to on a soft or
    /// hard reset. Zero -- the default -- is DEC-compatible.
    ///
    /// This mirrors xterm's `checksumExtension` resource. It exists because the conformance suites
    /// need a terminal whose checksums they can interpret (esctest, for one, cannot pass any
    /// terminal that leaves this at its default), yet neither suite ever sends XTCHECKSUM itself.
    ChecksumFlags checksumExtension {};

    /// The User-Preferred Supplemental Set (UPSS) the terminal starts with, and returns to on a soft
    /// or hard reset. DEC Supplemental Graphic is the DEC power-up default.
    ///
    /// This mirrors xterm's `DFT_UPSS`/`preferLatin1` pair: xterm lets a resource pick between DEC
    /// Supplemental Graphic and ISO Latin-1, and so does this.
    UserPreferredSupplementalSet userPreferredSupplementalSet = DefaultUserPreferredSupplementalSet;

    /// Read by the render thread as it ticks. @see RelaxedAtomic
    RelaxedAtomic<std::chrono::milliseconds> cursorBlinkInterval = std::chrono::milliseconds { 500 };

    /// How long synchronized output (DECSET 2026) may suppress rendering before the terminal draws
    /// anyway.
    ///
    /// The mode exists so an application can present a frame atomically, and honouring it is
    /// correct. Honouring it without limit is not: an application that opens a block and then
    /// stalls, blocks on a read, or dies before closing it freezes the display with no recovery
    /// path, because nothing else in the terminal will ask for a repaint while it is open. Other
    /// terminals cap it for the same reason and the mode's own proposal recommends a timeout.
    ///
    /// 150ms is well clear of a legitimate frame -- measured synchronized blocks from notcurses
    /// average 34.9ms, two refresh intervals -- so this bounds the pathological case without
    /// truncating the normal one. It is deliberately not a throughput knob: lowering it does not
    /// make a well-behaved application render faster.
    std::chrono::milliseconds synchronizedOutputTimeout { 150 };
    RefreshRate refreshRate = { 30.0 };

    // Defines the time to wait before the terminal executes the line feed (LF) command.
    // This is used to implement the DECSCLM (slow scroll) mode.
    std::chrono::milliseconds smoothLineScrolling { 100 };

    /// Enables pixel-based smooth scrolling instead of line-jump scrolling.
    bool smoothScrolling = true;

    /// Enables momentum (inertia) scrolling for touchpad gestures.
    bool momentumScrolling = true;

    /// Lines one wheel notch scrolls during alternate-scroll (wheel -> cursor keys).
    /// Mirrors the frontend history scroll multiplier; values below 1 are treated as 1.
    LineCount mouseWheelScrollMultiplier { LineCount(1) };

    /// When true, application-caused updates bring a viewport scrolled back into the history down to the
    /// bottom: output that adds lines (@see Terminal::followOutput), buffer switches, scrollback clears.
    /// When false, the viewport stays wherever the user parked it. Typing brings it down either way, and
    /// user-initiated transitions (e.g. leaving Vi mode) are not affected.
    ///
    /// Off by default, as in VTE, ghostty, WezTerm and Windows Terminal: a view taken back to read the
    /// history is the user's until they type. xterm's scrollTtyOutput is the one reference that defaults on.
    bool autoScrollOnUpdate = false;

    /// What the strip left of the grid shows. The window geometry must agree: the gutter's width --
    /// gutterLayoutFor(gutter).totalColumns cells -- is taken out of the space available to cells, so a
    /// page fitted for one layout and a renderer drawing another put the grid in two different places.
    /// @see GutterSettings
    GutterSettings gutter {};

    /// The sticky command header and the evicted-output placeholder (profile key `sticky_header`).
    ///
    /// Read afresh on every frame, so a profile switch or a config reload shows on the next one.
    StickyHeaderSettings stickyHeader {};

    /// OSC 3008 hierarchical context signalling.
    ///
    /// Whether the sequence is read at all is ONE flag, honoured at the point the sequence enters the
    /// emulation; the two below then describe only what to do with an ancestry that exists. @see
    /// ContextSignalling for why the gate is here rather than folded into each of them.
    ContextSignalling contextSignalling = ContextSignalling::Enabled;
    ContextStackLimits contextLimits {};
    ContextMarkPolicy contextMarkPolicy = ContextMarkPolicy::WhenAlone;
    ContextTintScope contextTintScope = ContextTintScope::Boundaries;

    /// Whether a finished command's output is folded away as soon as the next prompt starts, so
    /// that only the command running now is shown in full. Off by default: folding output the user
    /// did not ask to fold is a strong preference, not a sensible default.
    bool autoCollapseFoldOnNewCommand = false;

    /// What a targeted Vi-mode jump -- a search hit, a mark, jump history -- does when its target sits
    /// inside a collapsed fold. Expanding by default, because silently skipping a match the user just
    /// searched for is worse than revealing the block it was found in.
    FoldJumpBehavior foldJumpBehavior = FoldJumpBehavior::Expand;

    /// Whether folding is available at all (`folding.enabled`). Every fold verb that can hide output reads
    /// it, so the vi `z` keys, the frontend's actions and the automatic collapse are refused alike.
    FoldingAvailability foldingAvailability = FoldingAvailability::Enabled;

    /// How many shell command blocks (OSC 133 / OSC 3008) the session remembers. Read once: the store takes
    /// it at construction, so a changed value applies to sessions opened afterwards.
    /// @see CommandBlockStore
    CommandBlockStoreLimits commandBlockLimits {};

    /// Whether the terminal considers itself focused at birth. Birth state, not a runtime knob: it is
    /// read once by the constructor and thereafter only sendFocus{In,Out}Event writes the flag.
    ///
    /// A host that multiplexes several terminals and routes focus between them sets this false, so
    /// focus is granted rather than assumed — a terminal born focused but never told otherwise would
    /// render an active cursor and an active indicator status line forever, and would never send its
    /// application the DECSET 1004 focus-out it is due. A host with a single terminal that never moves
    /// focus leaves it true.
    bool focused = true;

    // Size in bytes per PTY Buffer Object.
    //
    // Defaults to 1 MB, that's roughly 10k lines when column count is 100.
    size_t ptyBufferObjectSize = 1024zu * 1024zu;
    // The most bytes taken from the PTY in one read, and so the most parsed in one hold of the terminal's
    // lock. The terminal rounds it up to a power of two.
    size_t ptyReadBufferSize = DefaultPtyReadBufferSize;
    /// @see InputWriter
    InputWriter inputWriter = InputWriter::QueueingThread;
    /// Whether the VT parser may skip its state machine for runs of bytes it would only pass on.
    /// Always Enabled in production. Disabled is the per-byte reference the tests compare against.
    vtparser::BulkScan parserBulkScan = vtparser::BulkScan::Enabled;
    std::u32string wordDelimiters = U" ";         ///< What ends a word a double click selects.
    std::u32string extendedWordDelimiters = U" "; ///< What ends a word a triple click selects.
    Modifiers mouseProtocolBypassModifiers = Modifier::Shift;
    Modifiers mouseBlockSelectionModifiers = Modifier::Control;
    /// The link pattern table hint mode and link detection scan with: the built-ins merged with the profile's
    /// hint_patterns, compiled once when the profile is applied.
    std::shared_ptr<LinkPatterns const> linkPatterns = LinkPatterns::builtins();
    /// Which links automatic detection finds on which screen (link_detection). Hint mode and OSC 8 links are
    /// not governed by it.
    LinkDetectionPolicy linkDetection {};
    LinkDecorationMode linkDecorationMode = LinkDecorationMode::Always; ///< hyperlink_decoration.mode.
    /// The modifiers of the FollowHyperlink mouse binding; none when there is no such binding.
    std::optional<Modifiers> linkFollowModifiers = Modifiers { Modifier::Control };
    LineOffset copyLastMarkRangeOffset = LineOffset(0);
    /// Whether a double-clicked word's other occurrences are highlighted too, by making the word the
    /// search pattern (the profile's `highlight_word_and_matches_on_double_click`).
    bool visualizeSelectedWord = true;
    std::chrono::milliseconds highlightTimeout = std::chrono::milliseconds { 150 };

    struct PrimaryScreen
    {
        bool allowReflowOnResize = true;
    };
    PrimaryScreen primaryScreen;

    bool isInsertAfterYank = false;

    TabsNamingMode tabNamingMode = TabsNamingMode::Indexing;

    // TODO: we could configure also the number of lines of the host writable statusline and indicator
    // statusline.
};

} // namespace vtbackend
