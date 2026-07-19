#pragma once

#include <vtbackend/Charset.h>
#include <vtbackend/ColorPalette.h>
#include <vtbackend/InputGenerator.h> // Modifier
#include <vtbackend/RectangularAreaChecksum.h>
#include <vtbackend/VTType.h>
#include <vtbackend/primitives.h>

#include <chrono>
#include <map>

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

    MaxHistoryLineCount maxHistoryLineCount;
    ImageSize maxImageSize { Width(800), Height(600) };
    unsigned maxImageRegisterCount = 256;
    bool goodImageProtocol = false;

    /// Whether an application may read the clipboard via OSC 52 (`OSC 52 ; Pc ; ? ST`). Disabled by
    /// default: clipboard reading is a well-known exfiltration vector, so it is opt-in. Writing the
    /// clipboard via OSC 52 is unaffected by this. @see Terminal::requestClipboardRead.
    bool allowClipboardRead = false;

    /// Whether DEC mode 2027 (grapheme clustering) starts out set. While set, a codepoint arriving
    /// after the first may revise how many columns its grapheme cluster occupies.
    /// @see vtbackend::ClusterWidthPolicy.
    bool graphemeClustering = true;
    StatusDisplayType statusDisplayType = StatusDisplayType::None;
    StatusDisplayPosition statusDisplayPosition = StatusDisplayPosition::Bottom;
    struct
    {
        std::string left { "{VTType} │ {InputMode:Bold,Color=#C0C030}{SearchPrompt:Left= │ }"
                           "{TraceMode:Bold,Color=#FFFF00,Left= │ }{ProtectedMode:Bold,Left= │ }" };
        std::string middle { "{Title:Left= « ,Right= » ,Color=#20c0c0}" };
        std::string right { "{HistoryLineCount:Faint,Color=#c0c0c0} │ {Clock:Bold} " };
    } indicatorStatusLine;
    bool syncWindowTitleWithHostWritableStatusDisplay = true;
    CursorDisplay cursorDisplay = CursorDisplay::Steady;
    CursorShape cursorShape = CursorShape::Block;
    BlinkStyle blinkStyle = BlinkStyle::Smooth;
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

    std::chrono::milliseconds cursorBlinkInterval = std::chrono::milliseconds { 500 };
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

    /// When true, PTY/app-caused updates (key input forwarded to the PTY, buffer
    /// switches, scrollback clears, etc.) force the viewport to snap to the
    /// bottom. When false, the viewport stays wherever the user parked it.
    /// User-initiated transitions (e.g. leaving Vi mode) are not affected.
    bool autoScrollOnUpdate = true;

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
    size_t ptyBufferObjectSize = 1024lu * 1024lu;
    // Configures the size of the PTY read buffer.
    // Changing this value may result in better or worse throughput performance.
    //
    // This value must be integer-devisable by 16.
    size_t ptyReadBufferSize = 4096;
    std::u32string wordDelimiters;
    std::u32string extendedWordDelimiters;
    Modifiers mouseProtocolBypassModifiers = Modifier::Shift;
    Modifiers mouseBlockSelectionModifiers = Modifier::Control;
    LineOffset copyLastMarkRangeOffset = LineOffset(0);
    bool visualizeSelectedWord = true;
    std::chrono::milliseconds highlightTimeout = std::chrono::milliseconds { 150 };
    bool highlightDoubleClickedWord = true;
    // TODO: ^^^ make also use of it. probably rename to how VScode has named it.

    struct PrimaryScreen
    {
        bool allowReflowOnResize = true;
    };
    PrimaryScreen primaryScreen;

    bool fromSearchIntoInsertMode = true;
    bool isInsertAfterYank = false;

    TabsNamingMode tabNamingMode = TabsNamingMode::Indexing;

    // TODO: we could configure also the number of lines of the host writable statusline and indicator
    // statusline.
};

} // namespace vtbackend
