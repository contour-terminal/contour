// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Animation.h>
#include <vtbackend/Bidi.h>
#include <vtbackend/ColorPalette.h>
#include <vtbackend/Cursor.h>
#include <vtbackend/DesktopNotification.h>
#include <vtbackend/Grid.h>
#include <vtbackend/HintModeHandler.h>
#include <vtbackend/Hyperlink.h>
#include <vtbackend/InputGenerator.h>
#include <vtbackend/InputHandler.h>
#include <vtbackend/PointerShape.h>
#include <vtbackend/RenderBuffer.h>
#include <vtbackend/Selector.h>
#include <vtbackend/SemanticBlockTracker.h>
#include <vtbackend/Sequence.h>
#include <vtbackend/SequenceBuilder.h>
#include <vtbackend/Settings.h>
#include <vtbackend/ShellIntegration.h>
#include <vtbackend/StatusLineBuilder.h>
#include <vtbackend/ViCommands.h>
#include <vtbackend/ViInputHandler.h>
#include <vtbackend/Viewport.h>
#include <vtbackend/logging.h>
#include <vtbackend/primitives.h>

#include <vtparser/Parser.h>

#include <vtpty/Pty.h>

#include <crispy/BufferObject.h>
#include <crispy/assert.h>
#include <crispy/defines.h>

#include <libunicode/bidi.h>

#include <gsl/pointers>

#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <queue>
#include <span>
#include <stack>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vtbackend
{

class Screen;

/// Result of applying a smooth-scroll pixel delta.
enum class SmoothScrollResult : uint8_t
{
    Applied,         ///< Pixel delta was applied to the viewport.
    Disabled,        ///< Smooth scrolling is disabled or not applicable (alternate screen).
    InvalidCellSize, ///< Cell pixel size is zero or negative; cannot compute scroll.
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

/// Helping information to visualize IME text that has not been comitted yet.
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
    void set(AnsiMode mode, bool enabled) { _ansi.set(static_cast<size_t>(mode), enabled); }

    bool set(DECMode mode, bool enabled)
    {
        if (_decFrozen[static_cast<size_t>(mode)])
        {
            errorLog()("Attempt to change frozen DEC mode {}. Ignoring.", mode);
            return false;
        }
        _dec.set(static_cast<size_t>(mode), enabled);
        return true;
    }

    [[nodiscard]] bool enabled(AnsiMode mode) const noexcept { return _ansi[static_cast<size_t>(mode)]; }
    [[nodiscard]] bool enabled(DECMode mode) const noexcept { return _dec[static_cast<size_t>(mode)]; }

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

  private:
    std::bitset<32> _ansi;                                              // AnsiMode
    std::bitset<static_cast<size_t>(DECMode::DECModeCount)> _dec;       // DECMode
    std::bitset<static_cast<size_t>(DECMode::DECModeCount)> _decFrozen; // DECMode
    std::map<DECMode, std::vector<bool>> _savedModes;                   // saved DEC modes
};
// }}}

struct Search
{
    std::u32string pattern;
    ScrollOffset initialScrollOffset {};
    bool initiatedByDoubleClick = false;
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

enum class WrapPending : uint8_t
{
    Yes,
    No,
};

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
    gsl::not_null<Terminal*> _terminal;
    PendingSequenceQueue _pendingSequences = {};
};

struct TabsInfo
{
    struct Tab
    {
        std::optional<std::string> name;
        Color color;
    };

    std::vector<Tab> tabs;
    size_t activeTabPosition = 1;
};

/// Terminal API to manage input and output devices of a pseudo terminal, such as keyboard, mouse, and screen.
///
/// With a terminal being attached to a Process, the terminal's screen
/// gets updated according to the process' outputted text,
/// whereas input to the process can be send high-level via the various
/// send(...) member functions.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding) // TODO
class Terminal
{
  public:
    class Events
    {
      public:
        virtual ~Events() = default;

        virtual void requestCaptureBuffer(LineCount /*lines*/, bool /*logical*/) {}
        virtual void bell() {}
        virtual void bufferChanged(ScreenType) {}
        virtual void renderBufferUpdated() {}
        virtual void screenUpdated() {}
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
        virtual void inspect() {}
        virtual void notify(std::string_view /*title*/, std::string_view /*body*/) {}
        virtual void showDesktopNotification(DesktopNotification const& /*notification*/) {}
        virtual void discardDesktopNotification(std::string_view /*identifier*/) {}
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
        virtual void setTerminalProfile(std::string const& /*configProfileName*/) {}
        virtual void discardImage(Image const&) {}
        virtual void inputModeChanged(ViMode /*mode*/) {}
        virtual void updateHighlights() {}
        virtual void playSound(Sequence::Parameters const&) {}
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
        FontDef getFontDef() override { return {}; }
        void setFontDef(FontDef const& /*fontSpec*/) override {}
        void copyToClipboard(std::string_view /*data*/) override {}
        void setPointerShape(std::string_view /*cssName*/) override {}
        void openDocument(std::string_view /*fileOrUrl*/) override {}
        void inspect() override {}
        void notify(std::string_view /*title*/, std::string_view /*body*/) override {}
        void showDesktopNotification(DesktopNotification const& /*notification*/) override {}
        void discardDesktopNotification(std::string_view /*identifier*/) override {}
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
        void setTerminalProfile(std::string const& /*configProfileName*/) override {}
        void discardImage(Image const&) override {}
        void inputModeChanged(ViMode /*mode*/) override {}
        void updateHighlights() override {}
        void playSound(Sequence::Parameters const&) override {}
        void cursorPositionChanged() override {}
        void onScrollOffsetChanged(ScrollOffset) override {}
    };

    Terminal(Events& eventListener,
             std::unique_ptr<vtpty::Pty> pty,
             Settings factorySettings,
             std::chrono::steady_clock::time_point now /* = std::chrono::steady_clock::now()*/);
    ~Terminal() = default;

    void start();

    void setRefreshRate(RefreshRate refreshRate);
    void setLastMarkRangeOffset(LineOffset value) noexcept;

    void setMaxHistoryLineCount(MaxHistoryLineCount maxHistoryLineCount);
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
    static constexpr int MaxMacroCount = 64;
    static constexpr int MaxMacroRecursionDepth = 16;

    /// Defines a macro with the given ID and body.
    void defineMacro(int id, bool deleteAll, std::string body);

    /// Invokes a previously defined macro by ID (queues for deferred execution).
    void invokeMacro(int id);

    /// Processes any pending macro invocations queued by invokeMacro().
    void processPendingMacros();

    /// Clears all defined macros.
    void clearMacros() noexcept { _macros.clear(); }

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
    [[nodiscard]] std::optional<std::string> udkString(int keyId) const noexcept
    {
        if (auto const it = _userDefinedKeys.find(keyId); it != _userDefinedKeys.end())
            return it->second;
        return std::nullopt;
    }

    /// Checks if a UDK is defined for the given function key and returns its string.
    /// @param key the Key enum value (e.g., Key::F6)
    /// @return the programmed string or nullopt if no UDK is defined.
    [[nodiscard]] std::optional<std::string> udkStringForKey(Key key) const noexcept;

    /// Clears all user-defined keys.
    void clearUDKs() noexcept
    {
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

    struct LocatorState
    {
        bool enabled = false;
        bool oneShot = false;
        LocatorCoordUnit coordUnit = LocatorCoordUnit::CharacterCells;
        bool reportButtonDown = true;
        bool reportButtonUp = true;
    };

    /// Enables or disables DEC Locator reporting.
    void setLocatorMode(int ps, int pu) noexcept;

    /// Selects which locator events generate reports.
    void selectLocatorEvents(std::span<int const> params) noexcept;

    /// Sends a DEC Locator report (DECLRP) to the PTY.
    void sendLocatorReport(int event, int button, int row, int col);

    /// Requests the current locator position.
    void requestLocatorPosition();

    /// Returns the current locator state.
    [[nodiscard]] LocatorState const& locatorState() const noexcept { return _locatorState; }

    /// Resets locator state (called by soft reset).
    void resetLocator() noexcept { _locatorState = {}; }

    /// Called from sendMousePressEvent/sendMouseReleaseEvent when locator mode is active.
    bool handleLocatorMouseEvent(int button, bool press, CellLocation pos);
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

    /// Visual layout of the line at grid offset @p line, for the frame being rendered.
    ///
    /// Returns a neutral left-to-right layout when the line is outside the range laid out for this
    /// frame, or when bidirectional reordering is switched off (BDSM reset).
    [[nodiscard]] BidiLineLayout const& bidiLayoutAt(LineOffset line) const noexcept;

    /// Whether the cursor currently sits inside a right-to-left run.
    [[nodiscard]] bool cursorIsInRightToLeftRun() const noexcept;

    // {{{ Bidirectional text
    /// Selects the character path, per SCP (`CSI Ps SP k`).
    /// @param direction nullopt restores the terminal's own default.
    void setCharacterPath(std::optional<unicode::Bidi_Direction> direction) noexcept
    {
        _characterPath = direction;
    }

    /// The character path last selected by SCP, or nullopt for the terminal default.
    [[nodiscard]] std::optional<unicode::Bidi_Direction> characterPath() const noexcept
    {
        return _characterPath;
    }

    /// Whether the terminal reorders bidirectional text itself.
    ///
    /// This is BDSM: set means implicit (we reorder), reset means explicit (the application already
    /// did, and we must draw what we were given).
    [[nodiscard]] bool bidiReorderingEnabled() const noexcept
    {
        return isModeEnabled(AnsiMode::BiDirectionalSupport);
    }

    /// The base direction to impose on every paragraph, or nullopt to autodetect each from its own
    /// first strong character (UAX#9 P2/P3).
    ///
    /// Autodetection wins when it is enabled, because it is the more specific request: SCP and
    /// DECRLM state a default, `CSI ? 2501 h` says to derive the direction from the text instead.
    [[nodiscard]] std::optional<unicode::Bidi_Direction> bidiParagraphDirection() const noexcept
    {
        if (isModeEnabled(DECMode::BidiAutodetectParagraph))
            return std::nullopt;
        if (_characterPath.has_value())
            return _characterPath;
        if (isModeEnabled(DECMode::RightToLeftMode))
            return unicode::Bidi_Direction::Right_To_Left;
        return unicode::Bidi_Direction::Left_To_Right;
    }
    // }}}
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
    [[nodiscard]] constexpr bool contains(CellLocation coord) const noexcept
    {
        return LineOffset(0) <= coord.line && coord.line < boxed_cast<LineOffset>(_settings.pageSize.lines)
               && ColumnOffset(0) <= coord.column
               && coord.column <= boxed_cast<ColumnOffset>(_settings.pageSize.columns);
    }

    [[nodiscard]] bool isCursorInViewport() const noexcept
    {
        return viewport().isLineVisible(currentScreen().cursor().position.line);
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

    /// Retrieves the time point this terminal instance has been spawned.
    [[nodiscard]] std::chrono::steady_clock::time_point currentTime() const noexcept { return _currentTime; }

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

    // Returns number of lines for the currently displayed status line,
    // or 0 if status line is currently not displayed.
    [[nodiscard]] LineCount statusLineHeight() const noexcept
    {
        switch (_statusDisplayType)
        {
            case StatusDisplayType::None: return LineCount(0);
            case StatusDisplayType::Indicator: return _indicatorStatusScreen.pageSize().lines;
            case StatusDisplayType::HostWritable: return _hostWritableStatusLineScreen.pageSize().lines;
        }
        crispy::unreachable();
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

    // {{{ input proxy
    using Timestamp = std::chrono::steady_clock::time_point;
    Handled sendKeyEvent(Key key, KeyboardModifiers modifiers, KeyboardEventType eventType, Timestamp now);
    Handled sendCharEvent(char32_t ch,
                          uint32_t physicalKey,
                          KeyboardModifiers modifiers,
                          KeyboardEventType eventType,
                          Timestamp now);
    Handled sendMousePressEvent(Modifiers modifiers,
                                MouseButton button,
                                PixelCoordinate pixelPosition,
                                bool uiHandledHint);
    void sendMouseMoveEvent(Modifiers modifiers,
                            CellLocation newPosition,
                            PixelCoordinate pixelPosition,
                            bool uiHandledHint);
    Handled sendMouseReleaseEvent(Modifiers modifiers,
                                  MouseButton button,
                                  PixelCoordinate pixelPosition,
                                  bool uiHandledHint);
    bool sendFocusInEvent();
    bool sendFocusOutEvent();
    void sendPaste(std::string_view text); // Sends verbatim text in bracketed mode to application.
    void sendPasteFromClipboard(unsigned count, bool strip)
    {
        _eventListener.pasteFromClipboard(count, strip);
    }
    void sendRawInput(std::string_view text);

    void inputModeChanged(ViMode mode) { _eventListener.inputModeChanged(mode); }
    void updateHighlights() { _eventListener.updateHighlights(); }
    void playSound(vtbackend::Sequence::Parameters const& params) { _eventListener.playSound(params); }

    /// Notifies the frontend that the application assigned a window-frame (tab) color (DECAC item 2).
    /// @param color The color to paint the frame/tab with.
    void setWindowFrameColor(RGBColor color) { _eventListener.setWindowFrameColor(color); }
    /// Notifies the frontend that the window-frame (tab) color was reset to the host default.
    void resetWindowFrameColor() { _eventListener.resetWindowFrameColor(); }

    bool applicationCursorKeys() const noexcept { return _inputGenerator.applicationCursorKeys(); }
    bool applicationKeypad() const noexcept { return _inputGenerator.applicationKeypad(); }

    bool hasInput() const noexcept;
    void flushInput();

    std::string_view peekInput() const noexcept { return _inputGenerator.peek(); }
    // }}}

    /// Writes a given VT-sequence to screen.
    void writeToScreen(std::string_view vtStream);

    /// Echoes @p bytes onto our own screen, as SRM (reset) asks for.
    ///
    /// The bytes cannot always be parsed on the spot: flushInput() is also reached from *inside* the
    /// parser, by every sequence handler that replies, and the parser is not safe to re-enter. Those
    /// bytes are deferred to processPendingLocalEcho() instead, which runs once the parse has unwound.
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
    void writeToScreenInternal(std::string_view vtStream);

    /// Writes a given VT-sequence to screen - but without acquiring the lock (must be already acquired).
    /// This version of the function is used to write to the status line and should not be used by the shell.
    void writeToScreenInternal(Screen& screen, std::string_view vtStream);

    // viewport management
    [[nodiscard]] Viewport& viewport() noexcept { return _viewport; }
    [[nodiscard]] Viewport const& viewport() const noexcept { return _viewport; }

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
    std::optional<std::chrono::milliseconds> nextRender() const;

    /// Updates the internal clock to the given time point,
    /// and ensures internal time-dependant state is updated.
    void tick(std::chrono::steady_clock::time_point now) noexcept;
    void tick(std::chrono::milliseconds delta) { tick(_currentTime + delta); }
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

    void lock() const { _stateMutex.lock(); }
    void unlock() const { _stateMutex.unlock(); }

    [[nodiscard]] ColorPalette const& colorPalette() const noexcept { return _colorPalette; }
    [[nodiscard]] ColorPalette& colorPalette() noexcept { return _colorPalette; }
    [[nodiscard]] ColorPalette& defaultColorPalette() noexcept { return _defaultColorPalette; }

    [[nodiscard]] std::vector<ColorPalette> const& savedColorPalettes() const noexcept
    {
        return _savedColorPalettes;
    }

    void setColorPalette(ColorPalette const& palette) noexcept;
    void resetColorPalette() noexcept { setColorPalette(defaultColorPalette()); }
    void resetColorPalette(ColorPalette const& colors);

    void pushColorPalette(size_t slot);
    void popColorPalette(size_t slot);
    void reportColorPaletteStack();

    [[nodiscard]] ShellIntegration& shellIntegration() noexcept { return *_shellIntegration; }
    [[nodiscard]] ShellIntegration const& shellIntegration() const noexcept { return *_shellIntegration; }
    void setShellIntegration(std::unique_ptr<ShellIntegration> newShellIntegration)
    {
        _shellIntegration = std::move(newShellIntegration);
    }

    [[nodiscard]] SemanticBlockTracker& semanticBlockTracker() noexcept { return _semanticBlockTracker; }
    [[nodiscard]] SemanticBlockTracker const& semanticBlockTracker() const noexcept
    {
        return _semanticBlockTracker;
    }

    [[nodiscard]] Screen& currentScreen() noexcept { return *_currentScreen; }
    [[nodiscard]] Screen const& currentScreen() const noexcept { return *_currentScreen; }

    [[nodiscard]] Screen& activeDisplay() noexcept
    {
        switch (_activeStatusDisplay)
        {
            case ActiveStatusDisplay::Main: return *_currentScreen;
            case ActiveStatusDisplay::StatusLine: return _hostWritableStatusLineScreen;
            case ActiveStatusDisplay::IndicatorStatusLine: return _indicatorStatusScreen;
        }
        crispy::unreachable();
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
        crispy::unreachable();
    }

    bool isPrimaryScreen() const noexcept { return _currentScreenType == ScreenType::Primary; }
    bool isAlternateScreen() const noexcept { return _currentScreenType == ScreenType::Alternate; }
    ScreenType screenType() const noexcept { return _currentScreenType; }
    void setScreen(ScreenType screenType);

    /// Enters or leaves the alternate screen buffer for one of the DEC private modes 47, 1047 or 1049,
    /// following the cursor-carry and clear policy that alternateScreenBehavior() describes for @p mode.
    /// Modelled on xterm's ToAlternate / FromAlternate: the cursor is a terminal-level entity, so modes
    /// 47 and 1047 carry it across the switch (it does not move), while mode 1049 lets each page keep
    /// its own cursor as an implicit DECSC/DECRC.
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
        crispy::unreachable();
    }

    /// Returns a reference to the screen at the given page index.
    [[nodiscard]] Screen& pageAt(PageIndex index) noexcept { return *_pages[index.value]; }
    [[nodiscard]] Screen const& pageAt(PageIndex index) const noexcept { return *_pages[index.value]; }

    /// Returns the zero-based page index where the cursor is currently active.
    [[nodiscard]] PageIndex cursorPageIndex() const noexcept { return _cursorPage; }

    /// Returns the zero-based page index of the displayed page.
    [[nodiscard]] PageIndex displayedPageIndex() const noexcept { return _displayedPage; }

    /// Returns the 1-based page number where the cursor is active (for VT replies).
    [[nodiscard]] int cursorPageNumber() const noexcept { return _cursorPage.value + 1; }

    /// Returns the 1-based page number of the displayed page (for VT replies).
    [[nodiscard]] int displayedPageNumber() const noexcept { return _displayedPage.value + 1; }

    /// Switches the active cursor page. See Phase 2 for full implementation.
    void setPage(PageIndex target, bool moveCursorHome);

    /// Saves the current cursor page index (called by DECSC).
    void saveCursorPage();

    /// Restores the previously saved cursor page index (called by DECRC).
    void restoreCursorPage();

    /// Returns the margin for the page where the cursor is currently active.
    [[nodiscard]] Margin& currentPageMargin() noexcept { return _pageMargins[_cursorPage.value]; }
    [[nodiscard]] Margin const& currentPageMargin() const noexcept { return _pageMargins[_cursorPage.value]; }

    /// Creates a default margin for the given page size (full-screen, no restriction).
    [[nodiscard]] static Margin makeDefaultMargin(PageSize pageSize) noexcept
    {
        return Margin { .vertical =
                            Margin::Vertical { .from = {}, .to = boxed_cast<LineOffset>(pageSize.lines) - 1 },
                        .horizontal = Margin::Horizontal {
                            .from = {}, .to = boxed_cast<ColumnOffset>(pageSize.columns) - 1 } };
    }

    /// Returns true if a screen crossfade transition is currently active.
    [[nodiscard]] bool isScreenTransitionActive() const noexcept { return _screenTransition.active; }

    /// Returns the current transition progress in [0, 1], or 1.0 if no transition is active.
    [[nodiscard]] float screenTransitionProgress() const noexcept
    {
        return _screenTransition.progress(_currentTime);
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
    [[nodiscard]] Screen const& indicatorStatusLineDisplay() const noexcept { return _indicatorStatusScreen; }
    // clang-format on

    [[nodiscard]] bool isLineWrapped(LineOffset lineNumber) const noexcept
    {
        return isPrimaryScreen() && primaryScreen().isLineWrapped(lineNumber);
    }

    [[nodiscard]] CellLocation currentMousePosition() const noexcept { return _currentMousePosition; }

    [[nodiscard]] std::optional<CellLocation> currentMouseGridPosition() const noexcept
    {
        if (_currentScreen->contains(_currentMousePosition))
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

    bool cursorBlinkActive() const noexcept { return _cursorBlinkState; }

    bool cursorCurrentlyVisible() const noexcept
    {
        return isModeEnabled(DECMode::VisibleCursor)
               && (cursorDisplay() == CursorDisplay::Steady || _cursorBlinkState);
    }

    /// Returns the predicted animation progress for a cursor at the given grid position.
    /// Used by RenderBufferBuilder to pre-set animationProgress before cell rendering,
    /// so that Block cursor cell inversion is suppressed during animation.
    [[nodiscard]] float cursorAnimationProgress(CellLocation cursorGridPosition) const noexcept
    {
        if (_cursorMotion.active && !_cursorMotion.isComplete(_currentTime))
            return _cursorMotion.progress(_currentTime);
        if (cursorGridPosition != _cursorMotion.toPosition
            && _settings.cursorMotionAnimationDuration.count() > 0)
            return 0.0f;
        return 1.0f;
    }

    bool isBlinkOnScreen() const noexcept { return _lastRenderPassHints.containsBlinkingCells; }

    std::chrono::steady_clock::time_point lastCursorBlink() const noexcept { return _lastCursorBlink; }

    constexpr void setCursorBlinkingInterval(std::chrono::milliseconds value)
    {
        _settings.cursorBlinkInterval = value;
    }

    constexpr std::chrono::milliseconds cursorBlinkInterval() const noexcept
    {
        return _settings.cursorBlinkInterval;
    }
    // }}}

    // {{{ selection management
    void setWordDelimiters(std::string const& wordDelimiters);
    void setExtendedWordDelimiters(std::string const& wordDelimiters);
    std::u32string const& wordDelimiters() const noexcept { return _settings.wordDelimiters; }

    Selection const* selector() const noexcept { return _selection.get(); }
    Selection* selector() noexcept { return _selection.get(); }
    std::chrono::milliseconds highlightTimeout() const noexcept { return _settings.highlightTimeout; }

    void updateSelectionMatches();

    template <typename RenderTarget>
    void renderSelection(RenderTarget renderTarget) const
    {
        if (!_selection)
            return;

        if (isPrimaryScreen())
            vtbackend::renderSelection(*_selection,
                                       [&](CellLocation pos) { renderTarget(pos, primaryScreen().at(pos)); });
        else
            vtbackend::renderSelection(
                *_selection, [&](CellLocation pos) { renderTarget(pos, alternateScreen().at(pos)); });
    }

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
    float blinkState() const noexcept { return _slowBlinker.opacity(_currentTime, _settings.blinkStyle); }
    float rapidBlinkState() const noexcept
    {
        return _rapidBlinker.opacity(_currentTime, _settings.blinkStyle);
    }

    /// Sets or resets to a new selection.
    void setSelector(std::unique_ptr<Selection> selector);

    /// Tests whether or not some grid cells are selected.
    bool selectionAvailable() const noexcept { return !!_selection; }

    /// Selects everything the terminal holds: the whole scrollback and the whole screen.
    void selectAll();

    /// Whether the left mouse button is currently held down, i.e. a selection drag may be in progress.
    [[nodiscard]] bool leftMouseButtonPressed() const noexcept { return _leftMouseButtonPressed; }

    bool visualizeSelectedWord() const noexcept { return _settings.visualizeSelectedWord; }
    void setVisualizeSelectedWord(bool enabled) noexcept { _settings.visualizeSelectedWord = enabled; }
    // }}}

    [[nodiscard]] std::string extractSelectionText() const;
    [[nodiscard]] std::string extractLastMarkRange() const;

    /// The most recently finished shell command, reconstructed from the OSC 133 marks its shell left.
    ///
    /// Always read from the PRIMARY screen — that is where the history and the shell's prompt live — so it
    /// keeps answering while an alt-screen app (vim, less) is on top.
    ///
    /// @return The block, or nullopt when the scrollback holds no finished command.
    [[nodiscard]] std::optional<CommandBlockText> lastCommandBlock() const;

    /// Where the shell's LIVE prompt sits — the one the user is typing at right now.
    ///
    /// Unlike lastCommandBlock(), this is answered for the screen actually on display: while an alt-screen
    /// application (vim, less) is up there is no shell prompt to speak of, and saying so is the point.
    ///
    /// Does NOT take the lock — the caller holds it, the same contract lastCommandBlock() has.
    ///
    /// @return The span, or why there is no live prompt.
    [[nodiscard]] std::expected<LivePromptSpan, PromptRegionError> livePromptSpan() const;

    HyperlinkStorage& hyperlinks() noexcept { return _hyperlinks; }
    HyperlinkStorage const& hyperlinks() const noexcept { return _hyperlinks; }

    /// Tests whether or not the mouse is currently hovering a hyperlink.
    [[nodiscard]] bool isMouseHoveringHyperlink() const noexcept
    {
        return _hoveringHyperlinkId.load().value != 0;
    }

    /// Retrieves the HyperlinkInfo that is currently behing hovered by the mouse, if so,
    /// or a nothing otherwise.
    [[nodiscard]] std::shared_ptr<HyperlinkInfo const> tryGetHoveringHyperlink() const noexcept
    {
        if (auto const gridPosition = currentMouseGridPosition())
            return _currentScreen->hyperlinkAt(*gridPosition);
        return {};
    }

    /// Returns the local filesystem path under the mouse cursor, if any.
    [[nodiscard]] std::optional<std::string> localPathAtMousePosition() const;

    [[nodiscard]] ExecutionMode executionMode() const noexcept { return _executionMode; }
    void setExecutionMode(ExecutionMode mode);

    bool processInputOnce();

    void markScreenDirty() noexcept { _screenDirty = true; }

    [[nodiscard]] uint64_t lastFrameID() const noexcept { return _lastFrameID.load(); }

    // Screen's EventListener implementation
    //
    void requestCaptureBuffer(LineCount lines, bool logical);
    void requestShowHostWritableStatusLine();
    void bell();
    void bufferChanged(ScreenType);
    void scrollbackBufferCleared();
    void screenUpdated();
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
#if defined(__APPLE__) || defined(_MSC_VER)
        reply(std::vformat(message, std::make_format_args(args...)));
#else
        reply(std::vformat(message, std::make_format_args(args...)));
#endif
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
    void setMouseProtocol(MouseProtocol protocol, bool enabled);
    void setMouseTransport(MouseTransport transport);
    void setMouseWheelMode(InputGenerator::MouseWheelMode mode);
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
    [[nodiscard]] Search& search() noexcept { return _search; }
    [[nodiscard]] Search const& search() const noexcept { return _search; }

    // {{{ hint mode
    /// Activates hint mode by scanning visible lines for regex matches.
    void activateHintMode(std::vector<HintPattern> const& patterns, HintAction action);

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
    void onBufferScrolled(LineCount n) noexcept;

    void onViewportChanged();

    /// Extends the active selection to the current mouse position after a viewport scroll.
    ///
    /// Called from onViewportChanged() so that wheel-scrolling while the left mouse button
    /// is held automatically extends the selection without requiring mouse movement.
    void extendSelectionAfterScroll();

    /// @returns either an empty string or a file:// URL of the last set working directory.
    [[nodiscard]] std::string const& currentWorkingDirectory() const noexcept
    {
        return _currentWorkingDirectory;
    }

    void setCurrentWorkingDirectory(std::string text) { _currentWorkingDirectory = std::move(text); }

    void verifyState();

    void applyPageSizeToCurrentBuffer();
    void applyPageSizeToMainDisplay(ScreenType screenType);

    [[nodiscard]] crispy::buffer_object_ptr<char> currentPtyBuffer() const noexcept
    {
        return _currentPtyBuffer;
    }

    /// Returns the buffer currently being parsed by parseFragment().
    /// This is used to ensure buffer_fragment holds the correct buffer reference
    /// when creating TrivialLineBuffer entries during parsing.
    [[nodiscard]] crispy::buffer_object_ptr<char> parsingBuffer() const noexcept
    {
        return _parsingBuffer ? _parsingBuffer : _currentPtyBuffer;
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

    [[nodiscard]] ExtendedKeyboardInputGenerator& keyboardProtocol() noexcept
    {
        return _inputGenerator.keyboardProtocol();
    }

    void resetHighlight();

    StatusDisplayType statusDisplayType() const noexcept { return _statusDisplayType; }
    void setStatusDisplay(StatusDisplayType statusDisplayType);
    void setActiveStatusDisplay(ActiveStatusDisplay activeDisplay);
    constexpr ActiveStatusDisplay activeStatusDisplay() const noexcept { return _activeStatusDisplay; }

    void pushStatusDisplay(StatusDisplayType statusDisplayType);
    void popStatusDisplay();

    bool allowInput() const noexcept { return !isModeEnabled(AnsiMode::KeyboardAction); }

    void setAllowInput(bool enabled);

    // Sets the current search term to the given text and
    // moves the viewport accordingly to make sure the given text is visible,
    // or it will not move at all if the input text was not found.
    [[nodiscard]] std::optional<CellLocation> searchReverse(std::u32string text, CellLocation searchPosition);
    [[nodiscard]] std::optional<CellLocation> searchReverse(CellLocation searchPosition);

    // Searches from current position the next item downwards.
    [[nodiscard]] std::optional<CellLocation> search(CellLocation searchPosition);

    [[nodiscard]] std::optional<CellLocation> searchNextMatch(CellLocation cursorPosition);
    [[nodiscard]] std::optional<CellLocation> searchPrevMatch(CellLocation cursorPosition);

    bool setNewSearchTerm(std::u32string text, bool initiatedByDoubleClick);
    void clearSearch();

    // Tests if the grid cell at the given location does contain a word delimiter.
    [[nodiscard]] bool wordDelimited(CellLocation position) const noexcept;
    [[nodiscard]] bool wordDelimited(CellLocation position,
                                     std::u32string_view wordDelimiters) const noexcept;

    [[nodiscard]] std::tuple<std::u32string, CellLocationRange> extractWordUnderCursor(
        CellLocation position) const noexcept;

    [[nodiscard]] Settings& factorySettings() noexcept { return _factorySettings; }
    [[nodiscard]] Settings const& factorySettings() const noexcept { return _factorySettings; }
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

    bool syncWindowTitleWithHostWritableStatusDisplay() const noexcept
    {
        return _syncWindowTitleWithHostWritableStatusDisplay;
    }

    void setSyncWindowTitleWithHostWritableStatusDisplay(bool value) noexcept
    {
        _syncWindowTitleWithHostWritableStatusDisplay = value;
    }

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
        // no AB-BA inversion. Do NOT move the name resolution inside setGuiTabInfoForStatusLine() (i.e.
        // under _guiTabInfoMutex): that WOULD nest _stateMutex inside _guiTabInfoMutex and deadlock against
        // this reader.
        auto const l = std::lock_guard { _guiTabInfoMutex };
        return _guiTabInfoForStatusLine;
    }
    void setGuiTabInfoForStatusLine(TabsInfo&& info)
    {
        auto const l = std::lock_guard { _guiTabInfoMutex };
        _guiTabInfoForStatusLine = std::move(info);
    }

    TabsNamingMode getTabsNamingMode() const noexcept { return _settings.tabNamingMode; }

  private:
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
    void fillRenderBufferInternal(RenderBuffer& output, bool includeSelection);

    /// Recomputes _bidiPageLayout for the grid lines about to be rendered.
    void updateBidiPageLayout(ScrollOffset scrollOffset, LineCount extraLines);
    LineCount fillRenderBufferStatusLine(RenderBuffer& output, bool includeSelection, LineOffset base);
    void updateIndicatorStatusLine();
    void updateCursorVisibilityState() const noexcept;
    void updateHoveringHyperlinkState();

    struct TheSelectionHelper: public vtbackend::SelectionHelper
    {
        Terminal* terminal;
        explicit TheSelectionHelper(Terminal* self): terminal { self } {}
        [[nodiscard]] PageSize pageSize() const noexcept override;
        [[nodiscard]] bool wrappedLine(LineOffset line) const noexcept override;
        [[nodiscard]] bool cellEmpty(CellLocation pos) const noexcept override;
        [[nodiscard]] int cellWidth(CellLocation pos) const noexcept override;
    };
    void triggerWordWiseSelection(CellLocation startPos, TheSelectionHelper const& selectionHelper);
    bool handleMouseSelection(Modifiers modifiers);

    /// Tests if the text selection should be extended by the given mouse position or not.
    ///
    /// @retval false if either no selection is available, selection is complete, or the new pixel position is
    /// not enough into the next grid cell yet
    /// @retval true otherwise
    bool shouldExtendSelectionByMouse(CellLocation newPosition, PixelCoordinate pixelPosition) const noexcept;

    // Tests if the App mouse protocol is explicitly being bypassed by the user,
    // by pressing a special bypass modifier (usualy Shift).
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
        crispy::buffer_object_ptr<char> buffer; // The buffer that was read into
    };
    [[nodiscard]] std::optional<PtyReadResult> readFromPty();

    // Writes partially or all input data to the PTY buffer object and returns a string view to it.
    [[nodiscard]] std::string_view lockedWriteToPtyBuffer(std::string_view data);

    // private data
    //

    Events& _eventListener;

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
    std::mutex mutable _stateMutex;

    // terminal clock
    std::chrono::steady_clock::time_point _currentTime;

    // {{{ PTY and PTY read buffer management
    crispy::buffer_object_pool<char> _ptyBufferPool;
    crispy::buffer_object_ptr<char> _currentPtyBuffer;
    crispy::buffer_object_ptr<char>
        _parsingBuffer; // Buffer currently being parsed (set during parseFragment)
    size_t _ptyReadBufferSize;
    std::unique_ptr<vtpty::Pty> _pty;
    // }}}

    // {{{ mouse related state (helpers for detecting double/triple clicks)
    std::chrono::steady_clock::time_point _lastClick {};
    unsigned int _speedClicks = 0;
    vtbackend::CellLocation _currentMousePosition {}; // current mouse position
    vtbackend::PixelCoordinate _lastMousePixelPositionOnLeftClick {};
    bool _leftMouseButtonPressed = false; // tracks left-mouse button pressed state (used for cell selection).
    bool _respectMouseProtocol = true;    // shift-click can disable that, button release sets it back to true
    bool _isAutoScrolling = false;        // suppresses extendSelectionAfterScroll during performAutoScroll
    // }}}

    // {{{ blinking state helpers
    mutable std::chrono::steady_clock::time_point _lastCursorBlink;
    mutable unsigned _cursorBlinkState = 1;

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
    struct ScreenTransitionState: AnimationState<LinearEasing>
    {
        std::vector<RenderCell> snapshotCells {};
        std::optional<RenderCursor> snapshotCursor {};
    };
    ScreenTransitionState _screenTransition;
    // }}}

    // {{{ Cursor motion animation state
    /// Holds state for an ongoing cursor motion animation between grid cells.
    struct CursorMotionState: AnimationState<EaseOutCubic>
    {
        CellLocation fromPosition {};
        CellLocation toPosition {};
        RGBColor fromColor {}; ///< Cursor color at the animation source position.
        RGBColor toColor {};   ///< Cursor color at the animation target position.
    };
    CursorMotionState _cursorMotion;
    // }}}

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

    ScrollMomentumState _scrollMomentum;
    VelocityTracker _scrollVelocityTracker;

    /// Starts (or reseeds) a momentum glide at @p velocity with the given @p tuning and wakes the
    /// render loop. Shared by the touchpad (handleScrollPhase) and mouse-wheel (injectWheelMomentum)
    /// entry points so the "arm state + kick loop" sequence lives in one place.
    /// @param velocity Seed velocity in px/s (positive = scroll up into history).
    /// @param tuning   Source-specific physics (friction + stop rule).
    /// @param now      Current time point.
    void armMomentum(float velocity,
                     MomentumTuning const& tuning,
                     std::chrono::steady_clock::time_point now) noexcept;
    // }}}

    // {{{ Displays this terminal manages
    std::vector<std::unique_ptr<Screen>> _pages; ///< 16 pages: page 0 = primary, page 15 = alt screen
    PageIndex _cursorPage { 0 };                 ///< Page where cursor/VT output goes
    PageIndex _displayedPage { 0 };              ///< Page shown to user (== _cursorPage when DECPCCM set)
    /// Which slot of _savedCursorPage the current screen uses: 1 for the alternate page, 0 for any
    /// primary page. Keyed on page identity, not _currentScreenType, so a primary VT420 page reached
    /// via PPA/NP/PP still resolves to the primary slot.
    [[nodiscard]] size_t savedCursorPageSlot() const noexcept
    {
        return _cursorPage == AlternateScreenPageIndex ? 1 : 0;
    }

    /// DECSC-saved cursor page, kept separately per screen (index 0 = primary, 1 = alternate) so the
    /// two screens' saved cursors never cross. Defaults are each screen's own page, so a DECRC with no
    /// prior DECSC stays put.
    std::array<PageIndex, 2> _savedCursorPage { PageIndex { 0 }, AlternateScreenPageIndex };
    Screen _hostWritableStatusLineScreen;
    Screen _indicatorStatusScreen;
    gsl::not_null<Screen*> _currentScreen;
    Viewport _viewport;
    StatusLineDefinition _indicatorStatusLineDefinition;

    // {{{ tabs info
    TabsInfo _guiTabInfoForStatusLine;
    /// Guards _guiTabInfoForStatusLine. Dedicated (not _stateMutex) so the GUI-thread writer
    /// (setGuiTabInfoForStatusLine) does not block on the heavily-contended _stateMutex held by the parser
    /// thread during output bursts. Held only for the small copy in the accessor/mutator.
    std::mutex mutable _guiTabInfoMutex;
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
    bool _screenDirty = false; // TODO: just inc _changes and delete this instead.
    RefreshInterval _refreshInterval;
    RenderDoubleBuffer _renderBuffer {};
    std::atomic<uint64_t> _lastFrameID = 0;
    RenderPassHints _lastRenderPassHints {};
    // }}}

    InputMethodData _inputMethodData {};
    std::atomic<HyperlinkId> _hoveringHyperlinkId = HyperlinkId {};
    std::atomic<bool> _renderBufferUpdateEnabled = true; // for "Synchronized Updates" feature
    std::optional<HighlightRange> _highlightRange = std::nullopt;
    SupportedSequences _supportedVTSequences;

    // Execution Trace mode
    std::atomic<ExecutionMode> _executionMode = ExecutionMode::Normal;
    std::mutex _breakMutex;
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

    VTType _terminalId = VTType::VT525;
    VTType _operatingLevel = VTType::VT525;
    ControlTransmissionMode _c1TransmissionMode = ControlTransmissionMode::S7C1T;

    /// xterm title-mode features (XTSMTITLE / XTRMTITLE), one bit per TitleModeFeature. Default is all
    /// clear (xterm's DEF_TITLE_MODES), i.e. plain UTF-8 title set and query.
    std::bitset<TitleModeFeatureCount> _titleModes {};

    std::unordered_map<int, std::string> _macros;
    std::queue<std::string> _pendingMacroInvocations;
    int _macroRecursionDepth = 0;

    /// Local echo (SRM, reset) that was produced while the parser was running, and so could not be
    /// parsed on the spot. Written and drained only under _stateMutex. @see echoLocally().
    std::string _pendingLocalEcho;

    std::unordered_map<int, std::string> _userDefinedKeys;
    bool _udkLocked = false;

    LocatorState _locatorState;
    std::unordered_map<int, DRCSCharset> _drcsCharsets;
    std::unordered_map<std::string, int> _drcsDesignatorMap; ///< Dscs designator → font number

    Modes _modes;

    /// Visual layout of the lines laid out for the current frame, and the grid offset of its first
    /// entry. Recomputed per frame in fillRenderBufferInternal(); deliberately not cached across
    /// frames, since every grid mutation, reflow and scroll would have to invalidate it.
    BidiPageLayout _bidiPageLayout {};
    LineOffset _bidiPageLayoutTop {};

    /// Character path selected by SCP; nullopt means the terminal's own default.
    std::optional<unicode::Bidi_Direction> _characterPath = std::nullopt;
    std::map<DECMode, std::vector<bool>> _savedModes; //!< saved DEC modes

    /// Per-page screen margins for DEC multi-page support.
    /// Each page has its own independent margin (DECSTBM/DECSLRM apply per-page).
    /// This excludes all status lines, title lines, etc.
    std::array<Margin, MaxPageCount> _pageMargins;
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

    ScreenType _currentScreenType = ScreenType::Primary;
    StatusDisplayType _statusDisplayType = StatusDisplayType::None;
    bool _syncWindowTitleWithHostWritableStatusDisplay = false;
    std::optional<StatusDisplayType> _savedStatusDisplayType = std::nullopt;
    ActiveStatusDisplay _activeStatusDisplay = ActiveStatusDisplay::Main;

    Search _search;

    CursorDisplay _cursorDisplay = CursorDisplay::Steady;
    CursorShape _cursorShape = CursorShape::Block;

    /// XTCHECKSUM state; reset to Settings::checksumExtension by DECSTR and RIS alike.
    ChecksumFlags _checksumExtension = _settings.checksumExtension;

    /// UPSS state (DECAUPSS/DECRQUPSS); reset to Settings::userPreferredSupplementalSet by DECSTR
    /// and RIS alike, matching xterm -- whose ReallyReset() restores the charsets unconditionally,
    /// i.e. on a soft reset as well as a hard one.
    UserPreferredSupplementalSet _userPreferredSupplementalSet = _settings.userPreferredSupplementalSet;

    std::string _currentWorkingDirectory = {};

    unsigned _maxImageRegisterCount = 256;
    bool _usePrivateColorRegisters = false;

    bool _usingStdoutFastPipe = false;

    // Hyperlink related
    //
    HyperlinkStorage _hyperlinks {};

    std::string _windowTitle {};

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
        void onHintSelected(std::string const& matchedText,
                            HintAction action,
                            CellLocation start,
                            CellLocation end) override;
        void onHintModeEntered() override;
        void onHintModeExited() override;
        void requestRedraw() override;
    };
    HintModeExecutor _hintModeExecutor { *this };
    HintModeHandler _hintModeHandler { _hintModeExecutor };

    std::unique_ptr<ShellIntegration> _shellIntegration;
    SemanticBlockTracker _semanticBlockTracker;

    DesktopNotificationManager _desktopNotificationManager;
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
            value = std::format("\"{}\"", crispy::escape(p->text));
        else if (auto const* p = std::get_if<char32_t>(&pendingSequence))
            value = std::format("'{}'", unicode::convert_to<char>(*p));
        else
            crispy::unreachable();

        return formatter<std::string>::format(value, ctx);
    }
};

template <>
struct std::formatter<vtbackend::AnsiMode>: std::formatter<std::string>
{
    auto format(vtbackend::AnsiMode mode, auto& ctx) const
    {
        return formatter<std::string>::format(to_string(mode), ctx);
    }
};

template <>
struct std::formatter<vtbackend::DECMode>: std::formatter<std::string>
{
    auto format(vtbackend::DECMode mode, auto& ctx) const
    {
        return formatter<std::string>::format(to_string(mode), ctx);
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
