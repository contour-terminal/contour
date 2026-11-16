// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/CursorShape.hpp>
#include <core/tui/Error.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace core::tui
{

/// @brief RGB color representation.
struct RgbColor
{
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    /// Componentwise equality.
    [[nodiscard]] constexpr bool operator==(RgbColor const&) const noexcept = default;
};

/// @brief User-defined literal for creating RgbColor from a hex value.
///
/// Usage: `0xFF6600_rgb` creates RgbColor{255, 102, 0}.
[[nodiscard]] consteval RgbColor operator""_rgb(unsigned long long hex) noexcept
{
    return RgbColor { .r = static_cast<std::uint8_t>((hex >> 16) & 0xFF),
                      .g = static_cast<std::uint8_t>((hex >> 8) & 0xFF),
                      .b = static_cast<std::uint8_t>(hex & 0xFF) };
}

/// @brief Linearly interpolates between two RGB colors.
///
/// @param a The start color.
/// @param b The end color.
/// @param t Interpolation parameter in [0, 1].
/// @return The interpolated color.
[[nodiscard]] constexpr RgbColor lerpColor(RgbColor a, RgbColor b, float t) noexcept
{
    auto const lerp = [](std::uint8_t x, std::uint8_t y, float s) noexcept -> std::uint8_t {
        auto const val = static_cast<float>(x) + ((static_cast<float>(y) - static_cast<float>(x)) * s);
        if (val < 0.0f)
            return 0;
        if (val > 255.0f)
            return 255;
        return static_cast<std::uint8_t>(val);
    };
    return { .r = lerp(a.r, b.r, t), .g = lerp(a.g, b.g, t), .b = lerp(a.b, b.b, t) };
}

/// @brief Color representation: default, 256-color index, or true color (RGB).
using Color = std::variant<std::monostate, std::uint8_t, RgbColor>;

/// @brief Underline style for terminal output (SGR 4:n).
enum class UnderlineStyle : std::uint8_t
{
    None = 0,   ///< No underline.
    Single = 1, ///< Single underline (SGR 4:1, same as SGR 4).
    Double = 2, ///< Double underline (SGR 4:2).
    Curly = 3,  ///< Curly/wavy underline (SGR 4:3).
    Dotted = 4, ///< Dotted underline (SGR 4:4).
    Dashed = 5, ///< Dashed underline (SGR 4:5).
};

/// @brief Text styling attributes for terminal output.
struct Style
{
    Color fg;                                             ///< Foreground color.
    Color bg;                                             ///< Background color.
    bool bold = false;                                    ///< Bold text.
    bool italic = false;                                  ///< Italic text.
    bool underline = false;                               ///< Underlined text (simple SGR 4).
    bool strikethrough = false;                           ///< Strikethrough text.
    bool dim = false;                                     ///< Dim/faint text.
    bool inverse = false;                                 ///< Inverse/reverse video.
    UnderlineStyle underlineStyle = UnderlineStyle::None; ///< Extended underline style (SGR 4:n).
    Color underlineColor;                                 ///< Underline color (SGR 58;2;r;g;b or 58;5;idx).
};

class TerminalOutput;

/// @brief RAII guard for synchronized terminal output.
///
/// Uses CSI ?2026h/l (synchronized output mode) to prevent tearing.
/// Both ends flush: the constructor flushes what was composed BEFORE the region and then writes
/// the begin sequence, and the destructor flushes what was composed INSIDE it and then writes the
/// end sequence. So the region carries its own bytes and only its own, whether the guard was made
/// by @c TerminalOutput::syncGuard() or constructed directly.
///
/// Both go to the @c TerminalOutput the guard was made from, through the same
/// @c writeToDestination() every other byte takes, so a retargeted output (a test capture, a
/// pipe, a second terminal) is bracketed by its own sequences and the process's standard output
/// is left alone. It is also why the guard carries no native handle, and this header no
/// `<windows.h>` type.
///
/// **Only a terminal is bracketed.** The guard asks the output's @c TerminalOutput::isTerminal()
/// once, when it is made, and on any other destination writes no sequence at all while still
/// flushing at both ends. So a caller takes a guard unconditionally, and piped output carries the
/// frame without the markers, as it carries text without colour.
class SyncGuard
{
  public:
    /// @brief Constructs a no-op guard (no synchronized output).
    SyncGuard() noexcept = default;

    /// @brief Begins synchronized output mode on @p output.
    /// @param output The output to bracket; it must outlive the guard.
    explicit SyncGuard(TerminalOutput& output);

    /// @brief Flushes the bracketed output and ends synchronized output mode.
    ~SyncGuard();

    SyncGuard(SyncGuard const&) = delete;
    auto operator=(SyncGuard const&) -> SyncGuard& = delete;
    SyncGuard(SyncGuard&&) noexcept;
    auto operator=(SyncGuard&&) noexcept -> SyncGuard&;

  private:
    /// Flushes the region's bytes and, on a terminal, ends synchronized output mode.
    void end() noexcept;

    TerminalOutput* _output = nullptr; ///< The guarded output, or nullptr for a no-op guard.
    bool _bracketed = false;           ///< Whether the destination is a terminal, so the mode was begun.
};

/// @brief What is behind a @c TerminalOutput.
///
/// A terminal reads what is written and can answer a query; a simulation (@c MockTerminalOutput)
/// keeps its own model of the screen and answers nothing. @c Terminal reads this to decide whether
/// to set up its input and ask questions -- it used to tell by RTTI, which a guest built with
/// `-fno-rtti` does not have.
enum class OutputKind : std::uint8_t
{
    Device,     ///< Bytes reach a terminal, or whatever stands in for one: a pipe, a file, a capture.
    Simulation, ///< The output models the screen itself, and no terminal reads its bytes.
};

/// @brief Handles styled terminal output, cursor control, and screen management.
///
/// Buffers output internally and flushes on demand. Supports SGR styling,
/// cursor movement, alt screen, synchronized output, double-width/height lines,
/// and sixel image output.
///
/// All public methods are virtual to allow subclassing for testing (e.g., MockTerminalOutput).
class TerminalOutput
{
  public:
    TerminalOutput() = default;
    virtual ~TerminalOutput() = default;

    TerminalOutput(TerminalOutput const&) = delete;
    auto operator=(TerminalOutput const&) -> TerminalOutput& = delete;
    TerminalOutput(TerminalOutput&&) = delete;
    auto operator=(TerminalOutput&&) -> TerminalOutput& = delete;

    /// @brief Initializes the terminal output by querying terminal dimensions.
    /// @return Success or IoError.
    [[nodiscard]] virtual auto initialize() -> VoidResult;

    /// @brief Writes styled text at the current cursor position.
    /// @param text The plain text to write (no VT sequences).
    /// @param style The style to apply.
    virtual void writeText(std::string_view text, Style const& style = {});

    /// @brief Writes raw text without styling.
    /// @param text The text to write directly to the buffer.
    virtual void writeRaw(std::string_view text);

    /// @brief Moves the cursor to an absolute position.
    /// @param row Row (1-based).
    /// @param col Column (1-based).
    virtual void moveTo(int row, int col);

    /// @brief Moves the cursor up by n rows.
    virtual void moveUp(int n = 1);

    /// @brief Moves the cursor down by n rows.
    virtual void moveDown(int n = 1);

    /// @brief Moves the cursor left by n columns.
    virtual void moveLeft(int n = 1);

    /// @brief Moves the cursor right by n columns.
    virtual void moveRight(int n = 1);

    /// @brief Moves the cursor to column 0 (carriage return).
    virtual void carriageReturn();

    /// @brief Moves the cursor down one row, scrolling if at the bottom.
    virtual void linefeed();

    /// @brief Clears from the cursor to the end of the display (CSI J).
    virtual void clearToEndOfDisplay();

    /// @brief Sends a request for cell pixel dimensions (CSI 16 t).
    virtual void requestCellSize();

    /// @brief Sends a request for cursor position (CSI 6 n).
    virtual void requestCursorPosition();

    /// @brief Sends a DEC Private Mode request (DECRQM): CSI ? mode $ p.
    ///
    /// The terminal responds with CSI ? mode ; status $ y (DecModeReport).
    /// @param mode The DEC private mode number to query.
    virtual void requestDecMode(int mode);

    /// @brief Sends a Primary Device Attributes query (DA1): CSI c.
    ///
    /// The terminal responds with CSI ? p1 ; p2 ; ... c (DeviceAttributesReport).
    virtual void requestDeviceAttributes();

    /// @brief Resets every SGR attribute to the terminal's default (`CSI m`), as
    ///        @c writeText does after styled text; the bytes are @c buildSgrReset's.
    virtual void resetStyle();

    /// @brief Clears the entire current line.
    virtual void clearLine();

    /// @brief Clears from cursor to end of line.
    virtual void clearToEndOfLine();

    /// @brief Clears from cursor to start of line.
    virtual void clearToStartOfLine();

    /// @brief Deletes @p n lines at the cursor row, pulling the lines below up (DL, `CSI n M`).
    /// @param n Number of lines to delete; nothing is written when it is not positive.
    virtual void deleteLines(int n);

    /// @brief Clears the entire screen.
    virtual void clearScreen();

    /// @brief Clears the scrollback buffer.
    virtual void clearScrollback();

    /// @brief Enters the alternate screen buffer.
    virtual void enterAltScreen();

    /// @brief Leaves the alternate screen buffer.
    virtual void leaveAltScreen();

    /// @brief Creates a synchronized output guard.
    ///
    /// While the guard is alive, output is batched to prevent tearing. On a destination that is
    /// not a terminal (@c isTerminal() is false) the guard writes no synchronized-output sequence,
    /// and only flushes.
    /// @return An RAII SyncGuard object.
    [[nodiscard]] virtual auto syncGuard() -> SyncGuard;

    /// @brief Sets the current line to double-width (ESC #6).
    virtual void setDoubleWidth();

    /// @brief Sets the current line to double-height top half (ESC #3).
    virtual void setDoubleHeightTop();

    /// @brief Sets the current line to double-height bottom half (ESC #4).
    virtual void setDoubleHeightBottom();

    /// @brief Sets the current line to single-width (ESC #5).
    virtual void setSingleWidth();

    /// @brief Disables text reflow mode (DEC mode 2028l — Contour extension).
    virtual void disableReflow();

    /// @brief Enables text reflow mode (DEC mode 2028h — Contour extension).
    virtual void enableReflow();

    /// @brief Shows the cursor (CSI ?25h).
    virtual void showCursor();

    /// @brief Hides the cursor (CSI ?25l).
    virtual void hideCursor();

    /// @brief Saves the cursor position (ESC 7).
    virtual void saveCursor();

    /// @brief Restores the cursor position (ESC 8).
    virtual void restoreCursor();

    /// @brief Sets the cursor shape using DECSCUSR (CSI Ps SP q).
    /// @param shape The desired cursor shape.
    virtual void setCursorShape(CursorShape shape);

    /// @brief Sets the scroll region to the given rows (1-based, inclusive).
    /// @param top First row of the scroll region.
    /// @param bottom Last row of the scroll region.
    virtual void setScrollRegion(int top, int bottom);

    /// @brief Resets the scroll region to the full screen (CSI r).
    virtual void resetScrollRegion();

    /// @brief Writes raw sixel image data to the terminal.
    /// @param sixelData The sixel-encoded image data (without DCS/ST framing).
    virtual void writeSixel(std::string_view sixelData);

    /// @brief Copies text to the system clipboard using OSC 52.
    ///
    /// This works in terminals that support OSC 52 (most modern terminals).
    /// The text is base64-encoded and sent via the OSC 52 escape sequence.
    /// @param text The text to copy to the clipboard.
    virtual void copyToClipboard(std::string_view text);

    /// @brief Opens an OSC 8 hyperlink; text written until endHyperlink() is clickable.
    ///
    /// Terminals without OSC 8 support ignore the sequence, so no capability
    /// probe is required.
    /// @param url The link target. Should be an absolute URI.
    /// @param id OSC 8 `id=` value; empty for none. Runs sharing an id form one logical link,
    ///           which matters when a cell-diffing renderer emits one visual link as several runs.
    ///           Deliberately not defaulted: a default argument is not inherited, so which one
    ///           applied would depend on the static type of the call.
    virtual void beginHyperlink(std::string_view url, std::string_view id);

    /// @brief Closes the hyperlink opened by beginHyperlink().
    virtual void endHyperlink();

    /// @brief Writes styled text wrapped in an OSC 8 hyperlink.
    /// @param text The visible label.
    /// @param url The link target.
    /// @param style Styling applied to the label.
    virtual void writeHyperlink(std::string_view text, std::string_view url, Style const& style = {});

    /// @brief Unscrolls (restores) lines from the scrollback buffer.
    ///
    /// Uses the Kitty unscroll extension (CSI Ps + T) to restore content
    /// that was previously scrolled into the scrollback buffer.
    /// Supported by: Kitty, Contour, mintty.
    ///
    /// @param n Number of lines to restore from scrollback.
    /// @note On unsupported terminals, this sequence is silently ignored.
    virtual void unscroll(int n);

    /// @brief Detects terminal capabilities that require query/response I/O.
    ///
    /// Must be called after raw mode is enabled (ECHO off) to prevent
    /// response bytes from being echoed to the screen.
    /// Currently detects unscroll support via XTVERSION query.
    virtual void detectCapabilities();

    /// @brief Checks if the terminal supports the unscroll extension.
    ///
    /// Detects support by checking for known terminals:
    /// - Kitty: KITTY_WINDOW_ID environment variable
    /// - Contour: TERMINAL_NAME=contour environment variable
    /// - mintty: TERM_PROGRAM=mintty environment variable
    ///
    /// @return true if unscroll is likely supported.
    [[nodiscard]] virtual bool supportsUnscroll() const noexcept;

    /// @brief Flushes the internal buffer to stdout.
    virtual void flush();

    /// @brief Whether the destination this writes to is a terminal.
    ///
    /// The default destination is the process's standard output, and the default answer is what
    /// the operating system says about it. A subclass that retargets @c writeToDestination()
    /// answers for its own destination, so a caller deciding whether to emit colour, progress or
    /// synchronised output asks the output it actually holds rather than the process.
    ///
    /// @return true when the destination is a terminal.
    [[nodiscard]] virtual bool isTerminal() const noexcept;

    /// @brief What is behind this output.
    ///
    /// @return @c OutputKind::Device, unless a subclass models the screen itself.
    [[nodiscard]] virtual auto kind() const noexcept -> OutputKind;

    /// @brief Where a simulation's cursor is, which it can say without being asked over the wire.
    ///
    /// @return The cursor as (row, column), both 1-based, as a cursor position report has them; or
    ///         nothing, for an output with a terminal behind it, whose cursor only that terminal knows.
    [[nodiscard]] virtual auto simulatedCursorPosition() const noexcept -> std::optional<std::pair<int, int>>;

    /// @brief Returns the terminal width in columns.
    [[nodiscard]] virtual auto columns() const noexcept -> int;

    /// @brief Returns the terminal height in rows.
    [[nodiscard]] virtual auto rows() const noexcept -> int;

    /// @brief Updates the cached terminal dimensions.
    virtual void updateDimensions();

    /// @brief Records that inline content room has been reserved on the terminal.
    ///
    /// Called by Screen::clearAndRelease() to communicate to the next Screen's
    /// first flushInline() render that room already exists on screen and LF
    /// reservation can be skipped.
    ///
    /// @param rows Number of rows of reserved room.
    void setInlineRoomReserved(int rows) noexcept { _inlineRoomReserved = rows; }

    /// @brief Consumes and returns any previously reserved inline room.
    ///
    /// Returns the number of rows of room previously reserved via
    /// setInlineRoomReserved(), then resets the value to 0 (one-shot).
    ///
    /// @return Number of reserved rows, or 0 if none.
    [[nodiscard]] int consumeInlineRoom() noexcept
    {
        auto const room = _inlineRoomReserved;
        _inlineRoomReserved = 0;
        return room;
    }

  protected:
    /// @brief Writes fully composed output bytes to the destination.
    ///
    /// The default destination is the process's standard output. Subclasses
    /// override this to retarget the stream (e.g. to an arbitrary file
    /// descriptor, or to a capture buffer in tests) without reimplementing
    /// any of the escape-sequence composition above.
    ///
    /// @param bytes The buffered bytes to emit.
    virtual void writeToDestination(std::string_view bytes);

  private:
    /// The guard's begin and end sequences take the same destination as every other byte, which
    /// is what this friendship is for and all it is used for.
    friend class SyncGuard;

    std::string _buffer; ///< Output buffer for batching writes.
    int _cols = 80;
    int _rows = 24;
    bool _unscrollSupported = false; ///< Cached unscroll support detection.
    int _inlineRoomReserved = 0;     ///< Rows of inline room reserved by previous Screen.

    /// @brief Appends SGR (Select Graphic Rendition) sequences for the given style.
    void appendSgr(Style const& style);

    /// @brief Appends the SGR reset sequence.
    void appendSgrReset();
};

} // namespace core::tui
