// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/CommandBlockId.hpp>
#include <vtbackend/core/Hyperlink.hpp>
#include <vtbackend/core/Image.hpp>
#include <vtbackend/core/RowMove.hpp>
#include <vtbackend/core/Search.hpp>
#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/graphics/KittyGraphics.hpp>
#include <vtbackend/graphics/MessageParser.hpp>
#include <vtbackend/grid/CellUtil.hpp>
#include <vtbackend/grid/Grid.hpp>
#include <vtbackend/screen/Cursor.hpp>
#include <vtbackend/screen/SearchQuery.hpp>
#include <vtbackend/screen/SearchTallyWalk.hpp>
#include <vtbackend/shell/Folding.hpp>
#include <vtbackend/shell/LivePromptMarks.hpp>
#include <vtbackend/shell/PromptRegion.hpp>
#include <vtbackend/vt/Capabilities.hpp>
#include <vtbackend/vt/Charset.hpp>
#include <vtbackend/vt/KittyClipboard.hpp>
#include <vtbackend/vt/SemanticPrompt.hpp>
#include <vtbackend/vt/Sequence.hpp>
#include <vtbackend/vt/TextSizing.hpp>
#include <vtbackend/vt/VTType.hpp>

#include <vtparser/ParserExtension.hpp>

#include <crispy/Size.hpp>

#include <core/Utils.hpp>
#include <core/log/LogStore.hpp>

#include <libunicode/grapheme_segmenter.h>
#include <libunicode/utf8.h>
#include <libunicode/width.h>

#include <gsl/pointers>

#include <algorithm>
#include <atomic>
#include <concepts>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vtbackend
{

class SixelImageBuilder;
class Terminal;
struct CommandBlockRecord;
struct Settings;

namespace regis
{
    struct ReGISContext;
    class ReGISRasterizer;
    class ReGISTextRasterizer;
    class ReGISEvents;
} // namespace regis

// {{{ TODO: move me somewhere more appropriate
// XTSMGRAPHICS (xterm extension)
// CSI ? Pi ; Pa ; Pv S
namespace XtSmGraphics
{
    enum class Item : uint8_t
    {
        NumberOfColorRegisters = 1,
        SixelGraphicsGeometry = 2,
        ReGISGraphicsGeometry = 3,
    };

    enum class Action : uint8_t
    {
        Read = 1,
        ResetToDefault = 2,
        SetToValue = 3,
        ReadLimit = 4
    };

    using Value = std::variant<std::monostate, unsigned, ImageSize>;
} // namespace XtSmGraphics

/// TBC - Tab Clear
///
/// This control function clears tab stops.
enum class HorizontalTabClear : uint8_t
{
    /// Ps = 0 (default)
    AllTabs,

    /// Ps = 3
    UnderCursor,
};

/// Input: CSI 16 t
///
///  Input: CSI 14 t (for text area size)
///  Input: CSI 14; 2 t (for full window size)
/// Output: CSI 14 ; width ; height ; t
/// The area an XTWINOPS report is asked about.
enum class RequestPixelSize : uint8_t // TODO: rename RequestPixelSize to RequestArea?
{
    /// A single character cell. `CSI 16 t`.
    CellArea,

    /// The grid of cells the application writes to. `CSI 14 t`, `CSI 18 t`.
    TextArea,

    /// The window, including whatever chrome the frontend draws around the text area. `CSI 14 ; 2 t`.
    WindowArea,

    /// The screen the window is displayed on -- xterm's "display". `CSI 15 t`, `CSI 19 t`.
    ScreenArea,
};

/// DECRQSS - Request Status String
enum class RequestStatusString : uint8_t
{
    SGR,
    DECSCL,
    DECSCUSR,
    DECSCA,
    DECSACE,
    DECELF,
    DECLFKC,
    DECSMKR,
    DECSTBM,
    DECSLRM,
    DECSLPP,
    DECSCPP,
    DECSNLS,
    DECSASD,
    DECSSDT,
};

/// Renders @p color as the X11 color specification a color query is answered with.
///
/// Each of Contour's 8-bit channels is widened to the 16 bits X11 specifies by repeating its byte, so
/// 0xAB is reported as "abab" -- exactly as xterm reports a color it was given as "rgb:ab/ab/ab".
///
/// The digits are lower-case, as xterm's are: an application comparing the answer to the specification
/// it sent (as esctest does, and as any round-tripping application would) reads upper-case digits as a
/// different color.
[[nodiscard]] inline std::string colorSpecification(RGBColor const& color)
{
    auto const widen = [](uint8_t channel) {
        return (static_cast<unsigned>(channel) << 8) | channel;
    };
    return std::format("rgb:{:04x}/{:04x}/{:04x}", widen(color.red), widen(color.green), widen(color.blue));
}

enum class ApplyResult : uint8_t
{
    Ok,
    Invalid,
    Unsupported,
};

/// Where the text cursor goes once an image has been placed on the grid. Each image protocol defines
/// its own answer, so the protocol's handler picks one rather than the renderer guessing from the image.
enum class ImageCursorPolicy : uint8_t
{
    /// The cursor stays where it was (kitty `C=1`, ReGIS, GIP without `u`).
    Stay,
    /// The cursor moves to the image's left column, on the line below it (Sixel, GIP `u`, iTerm2).
    BelowImage,
    /// The cursor moves right past the image, on its last row (kitty without `C=1`), walking down by
    /// index so a DECSTBM region scrolls at its bottom margin. The right edge is the one text wraps at:
    /// the right margin inside a DECLRMM band, else the last page column. The spec leaves a cursor pushed
    /// off the screen to the implementation:
    /// - DECAWM set: it wraps once to the next line's first column (the band's left margin), scrolling at
    ///   the bottom margin, as kitty's screen.c and ghostty do.
    /// - DECAWM reset: it is clamped to the edge column and adds no wrap row, so it never scrolls the
    ///   image away. A deliberate divergence from kitty and ghostty, which wrap regardless of DECAWM:
    ///   with autowrap off nothing else in Contour moves the cursor onto the next line either.
    AdvancePastImage,
};
// }}}

/**
 * Terminal Screen.
 *
 * Implements the all VT command types and applies all instruction
 * to an internal screen buffer, maintaining width, height, and history,
 * allowing the object owner to control which part of the screen (or history)
 * to be viewn.
 */
class Screen final: public SequenceHandler, public capabilities::StaticDatabase
{
  public:
    /// @param terminal            reference to the terminal this display belongs to.
    /// @param margin              margin of this display.
    /// @param pageSize            page size of this display. This is passed because it does not necessarily
    ///                            need to match the terminal's main display page size.
    /// @param reflowOnResize      whether or not to perform virtual line text reflow on resuze.
    /// @param historyLimits       how deep this screen's scrollback is guaranteed to be, and how deep
    ///                            it may ever get (@see HistoryLimits).
    /// @param name                name of this screen, used for logging purposes.
    Screen(Terminal& terminal,
           gsl::not_null<Margin*> margin,
           PageSize pageSize,
           bool reflowOnResize,
           HistoryLimits historyLimits,
           std::string_view name);

    /// A screen with no headroom in its scrollback -- the status lines and the non-primary pages,
    /// which pass a bare depth (usually none at all).
    Screen(Terminal& terminal,
           gsl::not_null<Margin*> margin,
           PageSize pageSize,
           bool reflowOnResize,
           MaxHistoryLineCount maxHistoryLineCount,
           std::string_view name):
        Screen(terminal, margin, pageSize, reflowOnResize, HistoryLimits::plain(maxHistoryLineCount), name)
    {
    }

    Screen(Screen const&) = delete;
    Screen& operator=(Screen const&) = delete;
    Screen(Screen&&) noexcept = delete;
    Screen& operator=(Screen&&) noexcept = delete;
    ~Screen() override; // SequenceHandler is the only virtual base

    [[nodiscard]] Cursor& cursor() noexcept { return _cursor; }
    [[nodiscard]] Cursor const& cursor() const noexcept { return _cursor; }
    [[nodiscard]] Cursor const& savedCursorState() const noexcept { return _savedCursor; }
    void resetSavedCursorState() { _savedCursor = {}; }

    using StaticDatabase::numericCapability;
    [[nodiscard]] unsigned numericCapability(capabilities::Code cap) const override;

    [[nodiscard]] LineFlags lineFlags(LineOffset line) const noexcept { return _grid.lineAt(line).flags(); }

    // {{{ SequenceHandlers
    void writeText(char32_t codepoint) override;
    void writeText(std::string_view text, size_t cellCount) override;
    void writeTextEnd() override;
    void executeControlCode(char controlCode) override;
    void processSequence(Sequence const& seq) override;
    void processAPC(std::string_view body) override;

  private:
    /// Handles one kitty graphics command (the APC body after its leading 'G').
    void processKittyGraphics(std::string_view body);

    /// Answers a kitty graphics command, unless it asked to be left alone (`q=`).
    void replyKittyGraphics(kitty_graphics::Command const& command, std::string_view status);

    /// The animation frames of one kitty image.
    ///
    /// Frame 1 is the transmitted image itself: the protocol has no separate command to send it, and
    /// every stored image therefore has one frame from the moment it exists.
    struct KittyAnimation
    {
        /// One frame: its pixels and how long it is shown.
        struct Frame
        {
            std::shared_ptr<Image const> image;
            int32_t gapMilliseconds = 0;
        };

        std::vector<Frame> frames; ///< Frame N of the wire is index N-1 here.
        uint32_t currentFrame = 1; ///< Which frame a placement displays.
        kitty_graphics::AnimationState state = kitty_graphics::AnimationState::Unset;
        uint32_t loopCount = 0; ///< 0 unspecified, 1 forever, n>1 loop n-1 times.

        /// @return the image of 1-based frame @p number, or nullptr if there is no such frame.
        [[nodiscard]] std::shared_ptr<Image const> frameImage(uint32_t number) const
        {
            return number != 0 && number <= frames.size() ? frames[number - 1].image : nullptr;
        }

        /// @return the image a placement of this animation shows now.
        [[nodiscard]] std::shared_ptr<Image const> currentImage() const { return frameImage(currentFrame); }
    };

    /// Places @p image at the cursor, sized per the command's `c=`/`r=` or derived from its pixels.
    void renderKittyImage(kitty_graphics::Command const& command, std::shared_ptr<Image const> const& image);

    /// Moves the cursor past a placed image as ImageCursorPolicy::AdvancePastImage describes.
    /// @param topLeft the image's top-left cell, where the cursor was when it was placed.
    /// @param rendered the extent of the image that is on the page.
    void advanceCursorPastImage(CellLocation topLeft, GridSize rendered);

    /// Builds or edits one animation frame (`a=f`), compositing the transmitted rectangle onto a copy
    /// of the base frame the command names.
    void transmitKittyFrame(kitty_graphics::Command const& command);

    /// Applies animation control (`a=a`): play state, loop count, gap, and which frame is current.
    void controlKittyAnimation(kitty_graphics::Command const& command);

    /// @return the animation for @p imageId, creating it with its root frame, or nullptr if no image
    ///         is stored under that id.
    [[nodiscard]] KittyAnimation* kittyAnimationFor(uint32_t imageId);

    /// @return the number of frames the image stored under @p imageId has: one, its root, until a
    ///         frame is added, and zero if there is no such image.
    [[nodiscard]] uint32_t kittyFrameCount(uint32_t imageId) const;

    /// @return the image of 1-based frame @p number of @p imageId, or nullptr if there is none.
    ///         Unlike kittyAnimationFor(), this creates no animation, so a command may consult it
    ///         before it has decided to succeed.
    [[nodiscard]] std::shared_ptr<Image const> kittyFrameImage(uint32_t imageId, uint32_t number) const;

    /// Replaces every on-screen fragment of @p previous with the same fragment of @p next.
    ///
    /// Works from the grid rather than from a recorded position, so it cannot drift as the cells
    /// scroll, are inserted or deleted, are erased or are reflowed -- if the image is no longer
    /// there, there is nothing to find and nothing happens.
    void showKittyFrame(std::shared_ptr<Image const> const& previous,
                        std::shared_ptr<Image const> const& next);

    /// @return decoded bytes held by stored kitty images and their animation frames.
    [[nodiscard]] size_t storedKittyBytes() const;

    /// @return decoded bytes held by the kitty image stored under @p imageId and its animation frames.
    [[nodiscard]] size_t storedKittyBytesOf(uint32_t imageId) const;

    /// Clears the on-screen fragments of @p imageId, whichever of its frames the cells hold.
    void removeKittyPlacementsOf(uint32_t imageId);

    /// Validates the transmission parameters of @p command against what this terminal implements.
    /// @return the wire status to answer with, or nullopt when the command is acceptable.
    ///
    /// Shared by the transmission path and by `a=q`, which exists precisely to let an application
    /// discover what a transmission would do without performing one.
    [[nodiscard]] static std::optional<std::string_view> validateKittyTransmission(
        kitty_graphics::Command const& command) noexcept;

    /// Handles a kitty graphics `a=d`, honouring the case of its `d=` target: a lower-case target
    /// removes placements only, an upper-case one additionally frees the transmitted image data.
    /// @return nothing, or the wire status to answer with when the `d=` target is not implemented.
    [[nodiscard]] std::expected<void, std::string_view> deleteKittyGraphics(
        kitty_graphics::Command const& command);

    /// Removes every image fragment on the page, leaving the text sharing those cells untouched.
    ///
    /// Fragments do not record which protocol placed them, so this clears Sixel images too, as it
    /// always has.
    void removeAllKittyPlacements();

    /// Calls @p update on every cell of the page holding an image fragment that @p selects, and
    /// marks the cell dirty.
    ///
    /// Placements are not tracked separately: a placement IS the image fragments in the cells it
    /// covers, so every operation on one is a walk of the page.
    void updateKittyFragments(std::predicate<ImageFragment const&> auto const& selects,
                              std::invocable<CellProxy&, ImageFragment const&> auto const& update);

    /// Drops all kitty graphics and clipboard protocol state. Called on RIS, which must not leave a
    /// half-open transmission for the next application to inherit.
    void resetKittyState() noexcept;

    /// Handles one `OSC 22` (kitty pointer shape protocol) request.
    [[nodiscard]] ApplyResult processPointerShape(std::string_view payload);

    /// Handles one `OSC 5522` (kitty clipboard protocol) packet.
    [[nodiscard]] ApplyResult processKittyClipboard(std::string_view payload);

    /// Lays out one `OSC 66` (kitty text sizing) request.
    [[nodiscard]] ApplyResult processTextSizing(std::string_view payload);

    /// Applies one `OSC 3008` (hierarchical context signalling) sequence.
    [[nodiscard]] ApplyResult processHierarchicalContext(std::string_view payload);

    /// Applies one `OSC 7501` (program status) report: a rejected one changes nothing and is logged.
    [[nodiscard]] ApplyResult processProgramStatus(std::string_view payload);

    /// Stamps the OSC-133-equivalent marks an OSC 3008 transition stands in for, when the arbiter says
    /// this session's marks are OSC 3008's to stamp, and feeds the command block store once OSC 3008 owns
    /// the session. While OSC 133 owns it, a command context's end only enriches the matching block.
    /// @see MarkArbiter
    void synthesizeSemanticMarks(ContextTransition const& transition, ContextCommand const& command);

    /// Erases the whole multi-cell block that @p position belongs to, however many rows and columns
    /// it covers, and wherever within it @p position happens to be.
    void eraseMulticellBlockAt(CellLocation position);

    /// Erases every multi-cell block overlapping @p line between @p from and @p to inclusive.
    ///
    /// A block cannot survive a sideways shift. insertChars()/deleteChars() move one line's cells and
    /// know nothing about MulticellContinuation, so a shift carries a head sideways while the rows it
    /// owns stay put -- leaving cells multicellBlockAt() can no longer resolve and that no later erase
    /// can reach. Destroying each overlapping block whole beforehand is the same rule writing over one
    /// follows, and the only way a shift can leave the grid consistent.
    ///
    /// @param line the line whose cells are about to move.
    /// @param from leftmost column the shift touches.
    /// @param to rightmost column the shift touches, inclusive.
    void eraseMulticellBlocksInRange(LineOffset line, ColumnOffset from, ColumnOffset to);

    /// @return whether @p cell could be part of a multi-cell block, and so is worth resolving.
    ///
    /// A cheap screen against multicellBlockAt(), whose walk costs several out-of-line grid lookups.
    /// False here is conclusive: none of the three states its walk can begin from is present.
    [[nodiscard]] static bool cellCouldBelongToMulticell(ConstCellProxy cell) noexcept;

    /// Writes @p text as a single cell block @p columns wide and @p scale cells tall.
    void writeSizedText(std::u32string_view codepoints, uint8_t columns, CellScale const& cellScale);

    /// Dispatches one OSC 1337 payload to the iTerm2 extension it names.
    void processITerm2(std::string_view payload);

    /// Answers `OSC 1337 ; Capabilities` with the features Contour actually has.
    void reportITerm2Capabilities();

    /// Hands `OSC 1337 ; SetUserVar=<name>=<base64 value>` to the terminal.
    /// @param argument What follows `SetUserVar=`.
    void setITerm2UserVar(std::string_view argument);

    /// Draws an `OSC 1337 ; File=...` inline image.
    void renderITerm2InlineImage(std::string_view arguments);

  public:
    // }}}

    /// @return the multi-cell block covering @p position, or nullopt when that cell stands alone.
    ///
    /// One authority for "which cells belong together", so that everything treating a block as
    /// indivisible -- erasing it, highlighting it -- agrees on its extent.
    [[nodiscard]] std::optional<MulticellBlock> multicellBlockAt(CellLocation position) const noexcept;

    void writeTextFromExternal(std::string_view text);

    /// Renders the full screen by passing every grid cell to the callback.
    ///
    /// @param extraLines  Additional lines to render beyond the page size (e.g. for smooth scrolling).
    /// @param rows        The exact rows to draw, top first, when they are not the contiguous page --
    ///                    @see Grid::render, which documents the folding case this exists for.
    template <typename Renderer>
    RenderPassHints render(Renderer&& render,
                           ScrollOffset scrollOffset = {},
                           HighlightSearchMatches highlightSearchMatches = HighlightSearchMatches::Yes,
                           LineCount extraLines = LineCount(0),
                           std::span<LineOffset const> rows = {}) const
    {
        return _grid.render(
            std::forward<Renderer>(render), scrollOffset, highlightSearchMatches, extraLines, rows);
    }

    /// Renders the full screen as text into the given string. Each line will be terminated by LF.
    [[nodiscard]] std::string renderMainPageText() const;

    /// Takes a screenshot by outputting VT sequences needed to render the current state of the screen.
    ///
    /// @note Only the screenshot of the current buffer is taken, not both (main and alternate).
    ///
    /// It moves the cursor to no absolute position and clears nothing, so it draws wherever the replaying
    /// cursor stands. That is what the state dump needs: inspect() prints it between lines of its own, to
    /// standard error on a failure and to standard output and `screen-state-dump.vt` from
    /// TerminalDisplay::inspect(), and a cursor-home there would jump to the top of the console it is
    /// printed on, so the state dump writes it as it is, `screen-state-dump.vt` included. `screenshot.vt`
    /// has no such surroundings: the ScreenshotVT action writes it there behind a prefix that resets the
    /// rendition, the character set, origin mode and the scroll region, homes the cursor and clears the
    /// display, so that file replays on its own.
    ///
    /// A text-sizing block is written as the OSC 66 that makes it, from its head down. One whose head has
    /// scrolled above the page cannot be made again: its lower part on the page replays as blank cells.
    ///
    /// @returns The page's rows as text and renditions, separated by CRLF and with none after the last, so
    ///          that replaying it from the top-left cell of a page of the same size draws the page again
    ///          without scrolling it.
    [[nodiscard]] std::string screenshot(std::function<std::string(LineOffset)> const& postLine = {}) const;

    void crlf() { linefeed(margin().horizontal.from); }
    void crlfIfWrapPending();

    // {{{ VT API
    void linefeed(); // LF

    void clearToBeginOfLine();
    void clearToEndOfLine();
    void clearLine();

    void clearToBeginOfScreen();
    void clearToEndOfScreen();
    void clearScreen();

    /// Drops every row above @p head (@see Grid::dropRowsAbove) and moves the cursor, the cursor DECSC
    /// saved and the cell last written up with the rows they stood on (never above the page top).
    /// @param head The row to keep at the top.
    /// @return How far the page scrolled up.
    [[nodiscard]] LineCount dropRowsAbove(LineOffset head);

    /// Marks a prompt start (OSC 133;A, or its OSC 3008 stand-in) on the cursor's logical line, voiding an
    /// earlier prompt's input border (PromptEnd) still on it and every right prompt an earlier prompt marked.
    void setMark();

    // Erase, sparing cells that carry @p protectedFlag. The default (CharacterProtected) is DEC/DECSCA
    // protection, so the DECSEL/DECSED/DECSERA handlers erase selectively with no argument. The regular
    // ED/EL/ECH erases pass CharacterProtectedISO to spare ISO 6429 (SPA/EPA) guarded cells instead.

    // DECSEL
    void selectiveEraseToBeginOfLine(CellFlag protectedFlag = CellFlag::CharacterProtected);
    void selectiveEraseToEndOfLine(CellFlag protectedFlag = CellFlag::CharacterProtected);
    void selectiveEraseLine(LineOffset line, CellFlag protectedFlag = CellFlag::CharacterProtected);

    // DECSED
    void selectiveEraseToBeginOfScreen(CellFlag protectedFlag = CellFlag::CharacterProtected);
    void selectiveEraseToEndOfScreen(CellFlag protectedFlag = CellFlag::CharacterProtected);
    void selectiveEraseScreen(CellFlag protectedFlag = CellFlag::CharacterProtected);

    void selectiveEraseArea(Rect area, CellFlag protectedFlag = CellFlag::CharacterProtected);

    void selectiveErase(LineOffset line,
                        ColumnOffset begin,
                        ColumnOffset end,
                        CellFlag protectedFlag = CellFlag::CharacterProtected);
    [[nodiscard]] bool containsProtectedCharacters(
        LineOffset line,
        ColumnOffset begin,
        ColumnOffset end,
        CellFlag protectedFlag = CellFlag::CharacterProtected) const;

    void eraseCharacters(ColumnCount n);  // ECH
    void insertCharacters(ColumnCount n); // ICH
    void deleteCharacters(ColumnCount n); // DCH
    void deleteColumns(ColumnCount n);    // DECDC
    void insertLines(LineCount n);        // IL
    void insertColumns(ColumnCount n);    // DECIC

    void copyArea(Rect sourceArea, int page, CellLocation targetTopLeft, int targetPage);

    // DEC Multi-Page Navigation (VT420)
    void nextPage(int count);                   ///< NP — Move to next page(s), cursor to home.
    void previousPage(int count);               ///< PP — Move to previous page(s), cursor to home.
    void pagePositionAbsolute(int page);        ///< PPA — Move to absolute page, preserve cursor.
    void pagePositionRelative(int count);       ///< PPR — Move forward by count pages, preserve cursor.
    void pagePositionBackward(int count);       ///< PPB — Move backward by count pages, preserve cursor.
    void requestDisplayedExtent();              ///< DECRQDE — Report displayed page extent.
    void requestUserPreferredSupplementalSet(); ///< DECRQUPSS — Report the User-Preferred Supplemental Set.

    void eraseArea(int top, int left, int bottom, int right);

    void fillArea(char32_t ch, int top, int left, int bottom, int right);

    void deleteLines(LineCount n); // DL

    void backIndex();    // DECBI
    void forwardIndex(); // DECFI

    void moveCursorTo(LineOffset line, ColumnOffset column); // CUP
    void moveCursorBackward(ColumnCount n);                  // CUB
    void moveCursorDown(LineCount n);                        // CUD
    void moveCursorForward(ColumnCount n);                   // CUF
    void moveCursorToBeginOfLine();                          // CR
    void moveCursorToColumn(ColumnOffset n);                 // CHA
    void moveCursorToLine(LineOffset n);                     // VPA
    void moveCursorToNextLine(LineCount n);                  // CNL
    void moveCursorToNextTab();                              // HT
    void moveCursorToPrevLine(LineCount n);                  // CPL
    void moveCursorUp(LineCount n);                          // CUU

    void cursorBackwardTab(TabStopCount count);        // CBT
    void cursorForwardTab(TabStopCount count);         // CHT
    void backspace();                                  // BS
    void horizontalTabClear(HorizontalTabClear which); // TBC
    void horizontalTabSet();                           // HTS

    void index();        // IND
    void reverseIndex(); // RI

    void setScrollSpeed(int speed); // DECSSCLS
    void deviceStatusReport();      // DSR
    void reportCursorPosition();    // CPR

    /// DECMSR, the reply to `CSI ? 62 n` -- the room left for macro definitions, in units of 16 bytes.
    void reportMacroSpace();

    /// DECCKSR (`CSI ? 63 ; Pid n`) -- the checksum of the macro memory.
    /// @param requestId The id the request was tagged with, carried back in the reply.
    void reportMacroSpaceChecksum(unsigned requestId);
    void reportExtendedCursorPosition(); // DECXCPR
    void reportCursorInformation();      // DECCIR
    void reportColorPaletteUpdate();
    void selectConformanceLevel(VTType level);
    void requestDynamicColor(DynamicColorName name);
    void requestCapability(capabilities::Code code);
    void requestCapability(std::string_view name);
    void sendDeviceAttributes();
    void sendTerminalId();

    /// Sets the current working directory as file:// URL.
    void setCurrentWorkingDirectory(std::string const& url); // OSC 7

    void hyperlink(std::string id, std::string uri);                   // OSC 8
    void notify(std::string const& title, std::string const& content); // OSC 777

    void captureBuffer(LineCount lineCount, bool logicalLines);

    /// Replies the empty terminating PM chunk that closes every capture reply, and nothing else.
    ///
    /// This is what a *refused* capture owes its client: the protocol has no way to say "no reply is
    /// coming", and a client reading until the empty chunk would otherwise block forever.
    /// @see docs/vt-extensions/buffer-capture.md
    void replyCaptureBufferEnd();

    void setForegroundColor(Color color);
    void setBackgroundColor(Color color);
    void setUnderlineColor(Color color);
    void setGraphicsRendition(GraphicsRendition rendition);
    void screenAlignmentPattern();
    void applicationKeypadMode(bool enable);
    void designateCharset(CharsetTable table, CharsetId charset);

    /// Attempts to handle an unmatched ESC sequence as SCS designation.
    /// Returns true if the sequence was recognized as SCS (including DRCS two-byte designators).
    [[nodiscard]] bool tryHandleSCS(Sequence const& seq);
    void singleShiftSelect(CharsetTable table);
    void requestPixelSize(RequestPixelSize area);
    void requestCharacterSize(RequestPixelSize area);
    void sixelImage(ImageSize pixelSize, Image::Data&& rgbaData);

    /// Publishes the persistent ReGIS canvas to the grid as an overlay image. Invoked when a ReGIS
    /// DCS string drew anything.
    /// @param pixelSize The canvas pixel dimensions.
    /// @param rgbaData The canvas RGBA pixels (a copy; the persistent canvas is left intact).
    void regisImage(ImageSize pixelSize, Image::Data&& rgbaData);

    /// Commits the current ReGIS canvas to the grid via @ref regisImage if anything was drawn.
    void commitReGIS();

    /// Injects the rasterizer used for ReGIS text glyphs, replacing the built-in embedded font.
    ///
    /// The display layer calls this with a text_shaper-backed implementation so ReGIS text renders
    /// through the terminal's real font engine; passing nullptr restores the embedded default.
    /// @param rasterizer The text rasterizer to use (shared with the display).
    void setReGISTextRasterizer(std::shared_ptr<regis::ReGISTextRasterizer> rasterizer) noexcept
    {
        _regisTextRasterizer = std::move(rasterizer);
    }
    void requestStatusString(RequestStatusString value);
    void requestTabStops();
    void resetDynamicColor(DynamicColorName name);
    void setDynamicColor(DynamicColorName name, RGBColor color);
    void inspect();
    void smGraphics(XtSmGraphics::Item item, XtSmGraphics::Action action, XtSmGraphics::Value value);
    // }}}

    std::shared_ptr<Image const> uploadImage(ImageFormat format, ImageSize imageSize, Image::Data&& pixmap);

    /// Uploads an image to the named image pool, decoding PNG to RGBA if needed.
    void uploadImage(std::string const& name, ImageFormat format, ImageSize imageSize, Image::Data&& pixmap);

    /**
     * Renders an image onto the screen.
     *
     * @p image the image to be rendered.
     * @p topLeft Screen coordinate to start rendering the top/left corner of the image.
     * @p gridSize Screen grid size to span the image into.
     * @p imageOffset Offset into the image, in pixels, to start rendering from.
     * @p imageSize the source image's pixel size, or zero. A non-zero size selects the VT340 sixel-band
     * math for where ImageCursorPolicy::BelowImage puts the cursor.
     * @p alignmentPolicy render the image using the given image alignment policy.
     * @p resizePolicy render the image using the given image resize policy.
     * @p autoScroll whether the screen scrolls to draw the rows that do not fit below @p topLeft. This
     * is independent of @p cursorPolicy: the scroll happens with any policy (it leaves the cursor on
     * the page's last line), and only the final placement of the cursor depends on the policy.
     * @p cursorPolicy where the text cursor goes once the image is placed.
     * @p layer whether the image is drawn below, above, or in place of the cells' text.
     */
    void renderImage(std::shared_ptr<Image const> image,
                     CellLocation topLeft,
                     GridSize gridSize,
                     PixelCoordinate imageOffset,
                     ImageSize imageSize,
                     ImageAlignment alignmentPolicy,
                     ImageResize resizePolicy,
                     bool autoScroll,
                     ImageCursorPolicy cursorPolicy,
                     ImageLayer layer = ImageLayer::Replace);

    /// Renders a previously uploaded named image onto the grid at the current cursor position.
    void renderImageByName(std::string const& name,
                           GridSize gridSize,
                           PixelCoordinate imageOffset,
                           ImageSize imageSize,
                           ImageAlignment alignmentPolicy,
                           ImageResize resizePolicy,
                           bool autoScroll,
                           bool requestStatus,
                           ImageCursorPolicy cursorPolicy,
                           ImageLayer layer = ImageLayer::Replace);

    /// Renders an image from raw pixel data (upload + render in one step).
    /// @return true on success, false if decoding failed.
    [[nodiscard]] bool renderImage(ImageFormat format,
                                   ImageSize imageSize,
                                   Image::Data&& pixmap,
                                   GridSize gridSize,
                                   ImageAlignment alignmentPolicy,
                                   ImageResize resizePolicy,
                                   bool autoScroll,
                                   ImageCursorPolicy cursorPolicy,
                                   ImageLayer layer = ImageLayer::Replace);

    /// Removes a named image from the image pool.
    void releaseImage(std::string const& name);

    void inspect(std::string const& message, std::ostream& os) const;

    // for DECSC and DECRC
    void requestAnsiMode(unsigned int mode);
    void requestDECMode(unsigned int mode);

    [[nodiscard]] PageSize pageSize() const noexcept { return _grid.pageSize(); }

    [[nodiscard]] constexpr Margin margin() const noexcept { return *_margin; }
    [[nodiscard]] constexpr Margin& margin() noexcept { return *_margin; }

    [[nodiscard]] bool isFullHorizontalMargins() const noexcept
    {
        return margin().horizontal.to.value + 1 == pageSize().columns.value;
    }

    /// How many columns of text may go straight into the cursor's line, from the cursor towards the
    /// right margin, without the per-codepoint path. The one owner of every condition that depends on
    /// where the cursor stands and what its line holds; the modes and charsets, which hold for a whole
    /// run of text, are the text writer's own business.
    /// @return 0 while a wrap is pending, on a line holding a cell that spans, or right of the right
    ///         margin; otherwise the columns from the cursor up to the right margin, the margin's own
    ///         column left out.
    [[nodiscard]] size_t bulkWritableColumns() const noexcept
    {
        // A pending wrap moves the text to the next line, which nothing below looks at. At a real
        // deferred wrap the cursor stands on the right margin, where the answer would be 0 anyway; a
        // margin widened under the wrap, or a DECRC after a resize, leaves it pending further left.
        if (_cursor.wrapPending)
            return 0;

        // A line holding a cell that reaches past its own column or row -- a wide character's or a
        // scaled block's continuation, a scaled block's head -- or an image is written cell by cell:
        // only the per-codepoint path erases the rest of a cell it overwrites part of. Any other line
        // takes the bulk path, whatever its renditions and on whichever screen. @see LineShape
        if (!currentLine().isBulkWritable())
            return 0;

        // Right of the right margin -- DECLRMM lets CUP put the cursor there -- the margin no longer
        // bounds the text, and `margin - cursor` would go negative.
        auto const column = _cursor.position.column;
        auto const rightMargin = margin().horizontal.to;
        if (column > rightMargin)
            return 0;

        return unbox<size_t>(rightMargin - column);
    }

    [[nodiscard]] bool isCursorInsideMargins() const noexcept;

    /// Tests whether the cursor sits within the active left/right margins.
    ///
    /// This mirrors xterm's `!(IsLeftRightMode && !ScrnIsColInMargins)` guard: when DECLRMM is off the
    /// horizontal margins span the whole page, so this is trivially true; when DECLRMM is on it reports
    /// whether the cursor column lies within the margin band. Vertical scrolling is confined to that
    /// band, so IND/RI/LF and friends consult this before deciding to scroll.
    /// @return true if the cursor column is within the horizontal margins.
    [[nodiscard]] bool isCursorInsideHorizontalMargins() const noexcept;

    [[nodiscard]] constexpr CellLocation realCursorPosition() const noexcept { return _cursor.position; }

    [[nodiscard]] constexpr CellLocation logicalCursorPosition() const noexcept
    {
        if (!_cursor.originMode)
            return realCursorPosition();
        else
            return CellLocation { .line = _cursor.position.line - margin().vertical.from,
                                  .column = _cursor.position.column - margin().horizontal.from };
    }

    [[nodiscard]] constexpr CellLocation origin() const noexcept
    {
        if (!_cursor.originMode)
            return {};

        return CellLocation { .line = margin().vertical.from, .column = margin().horizontal.from };
    }

    /// Returns identity if DECOM is disabled (default), but returns translated coordinates if DECOM is
    /// enabled.
    [[nodiscard]] CellLocation toRealCoordinate(CellLocation pos) const noexcept
    {
        if (!_cursor.originMode)
            return pos;
        else
            return CellLocation { .line = pos.line + margin().vertical.from,
                                  .column = pos.column + margin().horizontal.from };
    }

    [[nodiscard]] LineOffset applyOriginMode(LineOffset line) const noexcept
    {
        if (!_cursor.originMode)
            return line;
        else
            return line + margin().vertical.from;
    }

    [[nodiscard]] ColumnOffset applyOriginMode(ColumnOffset column) const noexcept
    {
        if (!_cursor.originMode)
            return column;
        else
            return column + margin().horizontal.from;
    }

    /// Clamps given coordinates, respecting DECOM (Origin Mode).
    [[nodiscard]] CellLocation clampCoordinate(CellLocation coord) const noexcept
    {
        if (_cursor.originMode)
            return clampToOrigin(coord);
        else
            return clampToScreen(coord);
    }

    /// Clamps given logical coordinates to margins as used in when DECOM (origin mode) is enabled.
    [[nodiscard]] CellLocation clampToOrigin(CellLocation coord) const noexcept
    {
        return CellLocation { .line = std::clamp(coord.line, LineOffset { 0 }, margin().vertical.to),
                              .column =
                                  std::clamp(coord.column, ColumnOffset { 0 }, margin().horizontal.to) };
    }

    [[nodiscard]] LineOffset clampedLine(LineOffset line) const noexcept
    {
        return std::clamp(line, LineOffset(0), boxed_cast<LineOffset>(_grid.pageSize().lines) - 1);
    }

    [[nodiscard]] ColumnOffset clampedColumn(ColumnOffset column) const noexcept
    {
        return std::clamp(column, ColumnOffset(0), boxed_cast<ColumnOffset>(_grid.pageSize().columns) - 1);
    }

    [[nodiscard]] CellLocation clampToScreen(CellLocation coord) const noexcept
    {
        return CellLocation { .line = clampedLine(coord.line), .column = clampedColumn(coord.column) };
    }

    [[nodiscard]] CellLocation clampToMargin(CellLocation pos) const noexcept
    {
        return CellLocation { .line = std::clamp(pos.line, margin().vertical.from, margin().vertical.to),
                              .column =
                                  std::clamp(pos.column, margin().horizontal.from, margin().horizontal.to) };
    }

    // Tests if given coordinate is within the visible screen area.
    //
    // Both bounds are exclusive at the top: the last addressable column is columns-1, exactly as the
    // last addressable line is lines-1. The column test used to be `<=`, which accepted a coordinate
    // one cell past the right edge -- the value verifyState() aborts on and any caller using this to
    // guard an index would have read out of bounds with.
    [[nodiscard]] bool contains(CellLocation coord) const noexcept
    {
        return LineOffset(0) <= coord.line && coord.line < boxed_cast<LineOffset>(pageSize().lines)
               && ColumnOffset(0) <= coord.column
               && coord.column < boxed_cast<ColumnOffset>(pageSize().columns);
    }

    /// Finds the first literal match of @p searchText at or after @p startPosition.
    ///
    /// A convenience over a literal SearchQuery, for callers with a needle and no query.
    /// @param searchText    The needle. An empty needle matches nothing.
    /// @param startPosition Where to begin; a match starting exactly here is returned.
    /// @param mode          The case policy. @see caseComparisonFor.
    [[nodiscard]] std::optional<CellLocation> search(std::u32string_view searchText,
                                                     CellLocation startPosition,
                                                     SearchCaseSensitivity mode) const;

    /// Finds the first literal match of @p searchText at or before @p startPosition. @see search
    [[nodiscard]] std::optional<CellLocation> searchReverse(std::u32string_view searchText,
                                                            CellLocation startPosition,
                                                            SearchCaseSensitivity mode) const;

    /// Counts @p searchText's literal matches over the whole grid, scrollback included, and locates
    /// @p position among them.
    ///
    /// Counted by a SearchTallyWalk drained at once, over the very matcher stepping uses, so the count can
    /// never disagree with what stepping through the matches actually visits -- including the overlapping
    /// ones, which navigation reaches and a non-overlapping count would hide.
    ///
    /// @param searchText The needle.
    /// @param position   Reported back as @c ordinal when a match starts exactly there.
    /// @param mode       The case policy.
    /// @param limit      Stop after this many and report @c TallyExactness::Capped.
    [[nodiscard]] SearchMatchTally tallyMatches(std::u32string_view searchText,
                                                CellLocation position,
                                                SearchCaseSensitivity mode,
                                                size_t limit) const;

    /// Finds the first match of @p query at or after @p startPosition, walking forward segment by segment.
    /// @param query         The compiled pattern.
    /// @param startPosition Where to begin; a match starting exactly here is returned. A column past the
    ///                      row's end begins at the next row.
    /// @return The match's first cell, nothing where there is none, or SearchPatternError::TooComplex.
    [[nodiscard]] std::expected<std::optional<CellLocation>, SearchPatternError> searchFrom(
        SearchQuery const& query, CellLocation startPosition) const;

    /// Finds the last match of @p query at or before @p startPosition, walking backward segment by segment.
    /// @param query         The compiled pattern.
    /// @param startPosition Where to begin; a match starting exactly here is returned.
    /// @return The match's first cell, nothing where there is none, or SearchPatternError::TooComplex.
    [[nodiscard]] std::expected<std::optional<CellLocation>, SearchPatternError> searchReverseFrom(
        SearchQuery const& query, CellLocation startPosition) const;

    /// searchReverseFrom(), reading whole segments while @p read admits them: the keystroke probe's bound, so
    /// typing never walks a deep scrollback, nor feeds a slow regex much.
    /// @param query         The compiled pattern.
    /// @param startPosition Where to begin; a match starting exactly here is returned.
    /// @param read          What the read may take; exhausted() afterwards where it stopped short.
    /// @return The match's first cell, nothing where there is none within the budget, or
    ///         SearchPatternError::TooComplex.
    [[nodiscard]] std::expected<std::optional<CellLocation>, SearchPatternError> searchReverseFrom(
        SearchQuery const& query, CellLocation startPosition, SearchReadBudget& read) const;

    /// The match of @p query that begins at @p start.
    /// @param query The compiled pattern.
    /// @param start A cell of the grid.
    /// @param memo  Where a regex's matches of the segment are remembered (Terminal::searchMatchMemo).
    /// @return The match's first cell and last column, nothing where no match begins there, or
    ///         SearchPatternError::TooComplex.
    [[nodiscard]] std::expected<std::optional<CellLocationRange>, SearchPatternError> matchStartingAt(
        std::shared_ptr<SearchQuery const> const& query, CellLocation start, SearchMatchMemo& memo) const;

    [[nodiscard]] CellProxy usePreviousCell() noexcept
    {
        return useCellAt(_lastCursorPosition.line, _lastCursorPosition.column);
    }

    /// Re-points _currentLine at the row the cursor is on, and hands that row what is in effect -- the
    /// context and command block being written, and the time the cursor first reached it -- wherever it
    /// carries none yet.
    ///
    /// The stamp lives HERE because this is the tree's single "the cursor is now on this line" funnel
    /// -- fifteen call sites, and provably exhaustive, because _currentLine would already be stale if
    /// any path skipped it. Stamping per CODEPOINT instead would be the obvious design and the wrong
    /// one: this runs once per cursor line change, so the per-character write path is untouched.
    ///
    /// Fill-once: reaching a row is not writing it. A row the cursor merely revisits -- `clear` homing onto
    /// the page's top row, a program stepping back up over earlier output -- keeps the context, block and
    /// time it was born with; only an explicit transition relabels a row.
    /// @see setActiveContextId, setActiveBlockId, setLineBirthStamp
    void updateCursorIterator() noexcept
    {
        _currentLine = &_grid.lineAt(_cursor.position.line);
        _currentLine->adoptProvenance(_stamp);
    }

    /// The OSC 3008 context the lines the cursor reaches are stamped with, where they carry none yet.
    ///
    /// A mirror of Terminal's ancestry rather than a read through it: Terminal fans the active id out
    /// to all 18 screens whenever it moves (@see Terminal::applyContextCommand), which costs two stores
    /// per command and keeps this funnel free of the two pointer chases a lookup would add. It also
    /// keeps Screen drivable in a test without a populated stack behind it.
    ///
    /// Also re-stamps (and dirties) the line the cursor is ALREADY on, not just the ones it moves to next:
    /// a context is announced before the output it describes is written, and typically at column 0 of a
    /// line the cursor is already sitting on -- so waiting for the next cursor-line change would attribute
    /// that whole first line to the previous context. All of its LOGICAL line: output that did not end in a
    /// newline leaves the cursor on the last row of a wrapped line, and reflow hands every chunk the head's
    /// context, so restamping the cursor's row alone would hand what the new context writes there back to
    /// the old one on the next resize. Re-stamping a line that already holds output is last-writer-wins,
    /// which is the only defensible answer when one line spans two contexts. Zero is ignored for the stamp,
    /// as Line::adoptContext() ignores it.
    /// @param id The context now in effect.
    void setActiveContextId(ContextId id) noexcept;

    /// The birth stamp rows the cursor reaches from now on are given.
    ///
    /// Set by Terminal for the length of one PTY batch, and back to zero when the batch ends, on the
    /// primary screen only: the alternate screen and the status lines never show a time, so their rows stay
    /// unstamped and are never dirtied for one. Zero between batches is what keeps a row the cursor reaches
    /// THEN -- a reset or resize the GUI asked for -- from taking the previous batch's time. Such a row is
    /// stamped here instead, by the next batch: the line the cursor is ALREADY on is stamped when it has no
    /// stamp yet, because this is the first batch it is seen in.
    ///
    /// That line is looked up by the cursor's row rather than through the cached _currentLine: this runs on
    /// the primary page while another page may be current, and nothing that happened to the primary grid
    /// meanwhile may leave the stamp written into a row that has moved.
    /// @see Line::bornAt
    /// @param secondsPlusOne Seconds since the session began, plus one; zero stamps nothing.
    void setLineBirthStamp(uint32_t secondsPlusOne) noexcept
    {
        _stamp.bornAt = secondsPlusOne;
        _grid.lineAt(_cursor.position.line).stampBornAt(secondsPlusOne);
    }

    /// The command block the rows the cursor reaches are stamped with, where they belong to none yet.
    ///
    /// A mirror of the store's current id rather than a read through it, for the reason
    /// setActiveContextId() gives. Like it, re-stamps (and dirties) the line the cursor is ALREADY on --
    /// all of its LOGICAL line: a prompt that starts on the last row of a wrapped output line marks that
    /// logical line's head, and reflow hands every chunk the head's id, so stamping the cursor's row alone
    /// would hand the prompt back to the previous block on the next resize. Last writer wins. Zero is
    /// ignored for the stamp, as Line::adoptBlock() ignores it.
    /// @param id The block now being written.
    void setActiveBlockId(CommandBlockId id) noexcept;

    /// Takes every row's command block id away: the store's ids started over, so an id a row was stamped
    /// with would name a block minted after it. A walk over the whole grid, scrollback included --
    /// affordable, as it happens once every 2^32 - 1 blocks.
    /// @see CommandBlockStore::idEpoch
    void forgetBlockIds() noexcept;

    /// The context that wrote the line at @p line, walking UP through blank lines.
    ///
    /// A line the cursor never moved onto -- one cleared by ED while the cursor stayed put -- carries
    /// no stamp of its own, and a run of blank lines below some output reads to a user as belonging to
    /// that output. Bounded by the page height so the walk cannot run away into a scrollback of blank
    /// lines; this is a query-time cost only, paid at most once per visible row.
    [[nodiscard]] ContextId contextIdAt(LineOffset line) const noexcept
    {
        auto const limit = unbox<int>(pageSize().lines);
        auto const topMost = boxed_cast<LineOffset>(-_grid.historyLineCount());
        for (auto steps = 0; steps < limit; ++steps)
        {
            auto const probe = line - steps;
            if (probe < topMost)
                break;
            if (auto const id = _grid.lineAt(probe).contextId(); !!id)
                return id;
        }
        return {};
    }

    [[nodiscard]] Line& currentLine() noexcept { return *_currentLine; }

    [[nodiscard]] Line const& currentLine() const noexcept { return *_currentLine; }

    [[nodiscard]] CellProxy useCurrentCell() noexcept
    {
        return currentLine().useCellAt(_cursor.position.column);
    }

    /// Gets a CellProxy to the cell relative to screen origin (top left, 1:1).
    [[nodiscard]] CellProxy at(LineOffset line, ColumnOffset column) noexcept
    {
        return _grid.useCellAt(line, column);
    }
    [[nodiscard]] CellProxy useCellAt(LineOffset line, ColumnOffset column) noexcept
    {
        return _grid.lineAt(line).useCellAt(column);
    }

    /// Gets a CellProxy to the cell relative to screen origin (top left, 1:1).
    [[nodiscard]] CellProxy at(LineOffset line, ColumnOffset column) const noexcept
    {
        return _grid.at(line, column);
    }

    [[nodiscard]] CellProxy at(CellLocation p) noexcept { return useCellAt(p.line, p.column); }
    [[nodiscard]] CellProxy useCellAt(CellLocation p) noexcept { return useCellAt(p.line, p.column); }
    [[nodiscard]] CellProxy at(CellLocation p) const noexcept { return _grid.at(p.line, p.column); }

    /// Finds the next marker right after the given line position.
    ///
    /// @paramn startLine the line number of the current cursor (1..N) for screen area, or
    ///                   (0..-N) for savedLines area
    /// @return cursor position relative to screen origin (1, 1), that is, if line Number os >= 1, it's
    ///         in the screen area, and in the savedLines area otherwise.
    [[nodiscard]] std::optional<LineOffset> findMarkerDownwards(LineOffset startLine) const;

    /// Finds the previous marker right next to the given line position.
    ///
    /// @paramn startLine the line number of the current cursor (1..N) for screen area, or
    ///                   (0..-N) for savedLines area
    /// @return cursor position relative to screen origin (1, 1), that is, if line Number os >= 1, it's
    ///         in the screen area, and in the savedLines area otherwise.
    [[nodiscard]] std::optional<LineOffset> findMarkerUpwards(LineOffset startLine) const;

    /// Where the shell's LIVE prompt sits in the grid — the one the user is typing at right now.
    ///
    /// Reads the OSC 133 marks alone, so no shell needs to opt into DEC mode 2034 for this to work.
    ///
    /// Does NOT take the terminal lock — the caller holds it.
    ///
    /// @return The span, or why there is no live prompt (a command is running, no shell integration, or
    ///         the prompt scrolled out of reach).
    [[nodiscard]] std::expected<LivePromptSpan, PromptRegionError> livePromptSpan() const;

    /// The foldable output regions of the finished commands in the scrollback, most recent first.
    ///
    /// Reads the OSC 133 marks alone, like livePromptSpan() above, so folding works for any shell that
    /// speaks plain OSC 133 -- DEC mode 2034 is a separate, opt-in reader channel and is not required.
    ///
    /// Does NOT take the terminal lock; the caller holds it, the same contract livePromptSpan() has.
    ///
    /// @param maxScanLines How far up the scrollback to walk (@see MaxFoldScanLines).
    /// @return The ranges, most recent first; empty when nothing in reach is foldable.
    [[nodiscard]] std::vector<FoldRange> foldRanges(size_t maxScanLines) const;

    void scrollUp(LineCount n) { scrollUp(n, margin()); }
    void scrollDown(LineCount n) { scrollDown(n, margin()); }
    void scrollLeft(ColumnCount n);
    void scrollRight(ColumnCount n);
    void unscroll(LineCount n);

    void verifyState() const;

    [[nodiscard]] Grid const& grid() const noexcept { return _grid; }
    [[nodiscard]] Grid& grid() noexcept { return _grid; }

    /// How many times a row of a command block was changed in what it tells its head, in place: an erase took
    /// the row out of the block or a mark a head is found by (BlockHeadLineFlags) off it, or a new prompt
    /// relabelled it to its own block.
    ///
    /// What makes Terminal::syncCommandBlockHeads() look at the heads again while the stable-id generation
    /// stands still: a head row an erase blanked, or a new prompt took, is no row of its block any more.
    [[nodiscard]] uint64_t blockRowChanges() const noexcept { return _blockRowChanges; }

    /// How many rows the explicit transitions (setActiveContextId(), setActiveBlockId()) have looked at on
    /// this screen since it was built.
    ///
    /// A test seam -- nothing in production reads it: what pins the cost of a transition on a long logical
    /// line.
    /// @return The count, which only ever grows.
    [[nodiscard]] uint64_t restampWork() const noexcept { return _restampWork; }

    /// Counts in blockRowChanges() a change made in place to a row of command block @p block, when it took
    /// the row out of the block or changed which of the marks a head is found by (BlockHeadLineFlags) it
    /// carries: the row may have headed the block.
    ///
    /// An erase counts its own. A daemon mirror writes the host's rows straight into the grid, past the
    /// erase and the relabelling that would have counted them, so it counts each row it writes: its blocks
    /// are then re-headed after the host's erase or new prompt as the host's own were.
    /// @param block The block the row belonged to before the change; a row of none counts nothing.
    /// @param flags The flags the row carried before the change.
    /// @param row The row after the change.
    void noteBlockRowChange(CommandBlockId block, LineFlags flags, Line const& row) noexcept;

    /// Tells the user-mark index (Terminal::userMarkChanged()) that row @p line gained or lost
    /// LineFlag::UserMark, if it did, on the primary screen.
    ///
    /// setLogicalLineFlags(), the funnel a mark is written through, reports its own. A row rewritten whole
    /// passes no funnel -- an erase resets it, a daemon mirror writes the host's row over it -- so each such
    /// rewrite reports through noteRowRewritten().
    /// @param line The row; a mark sits on its logical line's head.
    /// @param before The flags the row carried before the change.
    void noteUserMarkChange(LineOffset line, LineFlags before) noexcept;

    /// Tells everything that follows a row in place what rewriting row @p line whole did to it: the command
    /// blocks (noteBlockRowChange()) and the user-mark index (noteUserMarkChange()).
    ///
    /// The one call each whole-row rewrite makes once it is done: an erase that resets the row, and a daemon
    /// mirror writing a host row over it.
    /// @param line The row, as rewritten.
    /// @param block The block the row belonged to before the rewrite.
    /// @param before The flags the row carried before the rewrite.
    void noteRowRewritten(LineOffset line, CommandBlockId block, LineFlags before) noexcept;

    /// Changes how much scrollback the grid keeps.
    ///
    /// Through the screen, not the grid, because the change turns the ring back to its zero and regrows it,
    /// which moves every row: the cached cursor line is re-pointed at once, as after a resize. It is read
    /// whether or not this screen is current -- the batch clock stamps the primary page's cursor row while
    /// the alternate screen is up.
    /// @param historyLimits The new limits.
    void setHistoryLimits(HistoryLimits historyLimits)
    {
        _grid.setHistoryLimits(historyLimits);
        updateCursorIterator();
    }

    /// @returns true iff given absolute line number is wrapped, false otherwise.
    [[nodiscard]] bool isLineWrapped(LineOffset lineNumber) const noexcept
    {
        return _grid.isLineWrapped(lineNumber);
    }

    [[nodiscard]] bool isCellEmpty(CellLocation position) const noexcept
    {
        return _grid.lineAt(position.line).cellEmptyAt(position.column);
    }

    [[nodiscard]] bool compareCellTextAt(CellLocation position, char32_t codepoint) const noexcept
    {
        auto cell = _grid.at(position.line, position.column);
        return CellUtil::compareText(cell, codepoint);
    }

    [[nodiscard]] std::string cellTextAt(CellLocation position) const noexcept
    {
        auto cell = _grid.at(position.line, position.column);
        return cell.toUtf8();
    }

    [[nodiscard]] CellFlags cellFlagsAt(CellLocation position) const noexcept
    {
        auto cell = _grid.at(position.line, position.column);
        return cell.flags();
    }

    [[nodiscard]] Color cellForegroundColorAt(CellLocation position) const noexcept
    {
        auto cell = at(position);
        return cell.foregroundColor();
    }

    [[nodiscard]] Color cellBackgroundColorAt(CellLocation position) const noexcept
    {
        auto cell = at(position);
        return cell.backgroundColor();
    }

    [[nodiscard]] LineFlags lineFlagsAt(LineOffset line) const noexcept { return _grid.lineAt(line).flags(); }

    void enableLineFlags(LineOffset lineOffset, LineFlags flags, bool enable) noexcept
    {
        // changingLineAt, because a logical line's head follows wrapped rows into the SCROLLBACK:
        // marking one changes a row nothing scrolled, which the delta scan would otherwise never
        // revisit. @see Grid::changingLineAt.
        _grid.changingLineAt(lineOffset).setFlag(flags, enable);
    }

    /// Sets or clears the semantic marks @p flags on the LOGICAL line that @p line belongs to.
    ///
    /// The one way these marks may be written. They name a logical line — the line the shell wrote, or the
    /// line the user put a Vi mark on — and never the physical piece a wrap happened to chop it into.
    /// Stamping a continuation is exactly where they cannot survive: see HeadOnlyLineFlags, and the
    /// widening resize that rebuilds a joined logical line from its head alone.
    ///
    /// Out of line because it reaches into Terminal, which is incomplete here: every mark in the tree
    /// passes through this one funnel -- OSC 133's four, and vi's `mm` -- and the fold ranges derived from
    /// the shell's (ShellMarkLineFlags) have to be invalidated when one lands. A user mark derives none.
    void setLogicalLineFlags(LineOffset line, LineFlags flags, bool enable) noexcept;

    /// Which of the two column-carrying semantic marks a @ref markLogicalLineAtCursorWithColumn call
    /// is recording.
    ///
    /// OSC 133's ;B and ;D each remember a COLUMN as well as a flag -- where the prompt handed the
    /// line over, and where the command's output stopped -- and both are meaningless without it. An
    /// enum rather than two functions because the two differ only in which pair they write, and
    /// because a third such mark would be an enumerator rather than a third near-copy.
    enum class ColumnMark : uint8_t
    {
        PromptEnd = 0, ///< OSC 133;B: the border between what the shell wrote and what the user types.
        CommandEnd,    ///< OSC 133;D: where the command's output actually stopped.
    };

    /// Stamps @p mark onto the head of the logical line the cursor is on, together with the cursor's
    /// LOGICAL column.
    ///
    /// Goes through the same funnel setLogicalLineFlags() does, and for the same reason: both marks
    /// are in ShellMarkLineFlags, so both have to invalidate the fold ranges derived from them.
    void markLogicalLineAtCursorWithColumn(ColumnMark mark) noexcept;

    /// Whether the LOGICAL line that @p line belongs to carries any of @p flags.
    [[nodiscard]] bool isLogicalLineFlagEnabled(LineOffset line, LineFlags flags) const noexcept
    {
        return isLineFlagEnabledAt(_grid.logicalLineHead(line), flags);
    }

    /// Stamps the semantic marks @p flags onto the LOGICAL line the cursor is on.
    void markLogicalLineAtCursor(LineFlags flags) noexcept
    {
        setLogicalLineFlags(cursor().position.line, flags, true);
    }

    /// Whether the physical line @p line carries any of @p flags.
    [[nodiscard]] bool isLineFlagEnabledAt(LineOffset line, LineFlags flags) const noexcept
    {
        return _grid.lineAt(line).isFlagEnabled(flags);
    }

    [[nodiscard]] std::string lineTextAt(LineOffset line,
                                         bool stripLeadingSpaces = true,
                                         bool stripTrailingSpaces = true) const noexcept
    {
        return _grid.lineAt(line).toUtf8Trimmed(stripLeadingSpaces, stripTrailingSpaces);
    }

    /// The text of columns [@p begin, @p end) of @p line with exactly one codepoint per grid column -- a
    /// wide character's continuation cell becomes a space.
    ///
    /// Indexing by COLUMN is the point: a caller that wants a range would otherwise slice a whole-line
    /// string afterwards, and by then the columns have been encoded and a wide or non-BMP character no
    /// longer occupies one unit. @ref Line::toUtf8 clamps both bounds.
    ///
    /// @param line  The line to read.
    /// @param begin First column to include.
    /// @param end   One past the last column to include.
    /// @return That column range's text.
    [[nodiscard]] std::string lineTextColumnAlignedAt(LineOffset line,
                                                      ColumnOffset begin,
                                                      ColumnOffset end) const noexcept
    {
        return _grid.lineAt(line).toUtf8(begin, end, ContinuationCell::Pad);
    }

    [[nodiscard]] bool isLineEmpty(LineOffset line) const noexcept { return _grid.lineAt(line).empty(); }

    [[nodiscard]] uint8_t cellWidthAt(CellLocation position) const noexcept
    {
        return _grid.lineAt(position.line).cellWidthAt(position.column);
    }

    /// Whether @p codepoint, written next, would join the grapheme cluster written last rather than start a
    /// cell of its own -- a combining mark, a variation selector, a joiner and what it joins --, as
    /// writeText() decides it. Asked ahead of the write, of the codepoint before any charset mapping.
    /// @param codepoint The codepoint.
    /// @return true where writeText() would add @p codepoint to the cluster written last.
    [[nodiscard]] bool joinsLastCluster(char32_t codepoint) const noexcept;

    [[nodiscard]] LineCount historyLineCount() const noexcept { return _grid.historyLineCount(); }

    /// How many lines this screen's output has scrolled into its history since the screen was made: by the
    /// whole page or by a top-anchored region, as Grid::scrollUp() reports them. Only ever grows; the
    /// terminal follows output by its difference on the page on display (@see Terminal::followOutput).
    [[nodiscard]] uint64_t linesScrolledIntoHistory() const noexcept { return _linesScrolledIntoHistory; }

    [[nodiscard]] HyperlinkId hyperlinkIdAt(CellLocation position) const noexcept
    {
        auto cell = at(position);
        return cell.hyperlink();
    }

    [[nodiscard]] std::shared_ptr<HyperlinkInfo const> hyperlinkAt(CellLocation pos) const noexcept;

    void applyAndLog(Function const& function, Sequence const& seq);
    [[nodiscard]] ApplyResult apply(Function const& function, Sequence const& seq);

    void fail(std::string const& message) const;

    void hardReset();

    /// Clears any active ISO 6429 (SPA/EPA) guarded-area protection. Called from both the hard reset
    /// (RIS) and the soft reset (DECSTR), mirroring xterm's unconditional reset of `protected_mode`.
    /// The per-cell DECSCA flag is cleared separately, by the SGR reset. @see SPA, EPA, DECSCA.
    void resetProtection() noexcept { _isoProtectionActive = false; }

    void applyPageSizeToMainDisplay(PageSize pageSize);

    void saveCursor();
    void restoreCursor();
    void restoreCursor(Cursor const& savedCursor);

    /// Takes @p cursor over as this screen's cursor -- its position, rendition, character sets, origin mode
    /// and pending wrap -- as a switch of screen that carries the cursor across hands it on.
    /// @param cursor The cursor to take over; its position is held to the page.
    void adoptCursor(Cursor const& cursor) noexcept;

    /// How this screen measures a grapheme cluster, per DEC mode 2027.
    /// @return whether a late codepoint may revise its cluster's width.
    [[nodiscard]] ClusterWidthPolicy clusterWidthPolicy() const noexcept;

    void restoreGraphicsRendition();
    void saveGraphicsRendition();

    void reply(std::string_view text);

    template <typename... Ts>
    void reply(std::string_view message, Ts const&... args)
    {
        reply(std::vformat(message, std::make_format_args(args...)));
    }

  private:
    void writeTextInternal(char32_t codepoint);

    /// Whether writeSoftFontGlyph() put a glyph on the screen.
    enum class SoftFontGlyph : uint8_t
    {
        Absent,  ///< Nothing was written; the character takes the ordinary path.
        Written, ///< The glyph is in the cell and the cursor has advanced.
    };

    /// Writes @p sourceCodepoint as a glyph of the DRCS soft font @p font, the font the charset
    /// invoked for the next graphic character names (Charsets::activeDRCSFont()), when it defines one.
    ///
    /// The caller asks activeDRCSFont() first, an inline load, so that the text paths pay a call only
    /// when a soft font is invoked at all.
    ///
    /// The single-shift state (SS2/SS3) is consumed only when a glyph was written, so a character
    /// that falls back to the ordinary path is still mapped through the single-shifted set.
    /// @param font            The soft font's number.
    /// @param sourceCodepoint The character as received, before charset mapping.
    /// @return Whether a glyph was written.
    [[nodiscard]] SoftFontGlyph writeSoftFontGlyph(int font, char32_t sourceCodepoint);

    /// Whether the modes and charsets let ASCII go straight into the line's arrays: no insert mode,
    /// no left/right margins, and every ASCII character mapping to itself (CharsetMapping::isPlainUSASCII()).
    /// The half of the question that holds for a whole run of text; bulkWritableColumns() is the other.
    [[nodiscard]] bool canBulkWriteAscii() const noexcept;

    /// Whether an ASCII stretch of @p length bytes may be written in bulk where the cursor stands now,
    /// inside a run for which canBulkWriteAscii() holds.
    ///
    /// Asks bulkWritableColumns() again, what Terminal::maxBulkTextSequenceWidth() asked for the whole
    /// run when it began, because the text written before the stretch can change the answer. The
    /// screen may have taken more columns than the parser's scan counted for that text: the scan
    /// measures a wide emoji followed by VS15 as one column, while the screen keeps the two the emoji
    /// already took, since applyClusterWidthChange() never narrows a cluster on screen. The stretch
    /// then no longer fits before the margin, and a bulk write would drop the letters that cross it.
    /// That text may also have left a wrap pending or put a wide character on the line.
    [[nodiscard]] bool canBulkWriteAsciiStretch(size_t length) const noexcept;

    /// Writes the interior of an ASCII stretch straight into the current line's arrays and moves the
    /// cursor past it. Lean, because canBulkWriteAsciiStretch() has ruled out all that writeText() sees
    /// to before a run of nothing but ASCII: no wrap can be pending, the line holds no wide character
    /// to overwrite half of, and the interior ends left of the margin. The run's end -- the parsing
    /// buffer's hot end, the instruction counter -- is writeText()'s.
    void writeAsciiInterior(std::string_view interior);

    /// Whether the ASCII stretches of a run may go into the line in bulk.
    enum class AsciiStretches : uint8_t
    {
        PerCodepoint, ///< The modes or the charsets need every codepoint looked at (canBulkWriteAscii()).
        InBulk,       ///< The long ones may (forEachBulkAsciiInterior).
    };

    /// Writes a run of text through the grapheme segmenter, one codepoint at a time, apart from the
    /// interiors of long ASCII stretches when @p stretches lets those go into the line in bulk.
    /// @param text      The run.
    /// @param cellCount How many columns the parser measured the run to take.
    /// @param stretches Whether the run's ASCII stretches may be written in bulk.
    /// @return The offset into @p text where the last interior written in bulk ends, if any was.
    [[nodiscard]] std::optional<size_t> writeSegmentedText(std::string_view text,
                                                           size_t cellCount,
                                                           AsciiStretches stretches);

    /// Grapheme segmentation carried from one codepoint of a run of text to the next.
    struct GraphemeRun
    {
        unicode::grapheme_segmenter_state segmenter {}; ///< Where segmentation stands.
        char32_t previousCodepoint = 0;                 ///< The codepoint segmented last; 0 before any.
    };

    /// The segmentation state after the cell the previous write ended in, replayed from that cell's
    /// codepoints, so that a cluster continues across writes: 👨 written by one, then the ZWJ in the
    /// next, joins the 👨. Not the parser's state, which its text scan has already advanced past the
    /// text being written: reusing it would segment that text twice.
    /// @return An empty run if that cell is off the page or its line is blank.
    [[nodiscard]] GraphemeRun seedGraphemeStateFromLastCell() const noexcept;

    /// Where the next codepoint of a run of text goes.
    enum class ClusterPlacement : uint8_t
    {
        JoinsLastCluster, ///< No grapheme cluster breaks before it: into the cell written last.
        StartsCluster,    ///< A grapheme cluster breaks before it: into a cell of its own.
    };

    /// Segments @p codepoint, already mapped through the charsets, as the next codepoint of @p run, and
    /// advances @p run past it. Holds the one segmentation rule both per-codepoint writers follow.
    /// @return Whether @p codepoint starts a grapheme cluster or joins the one written last.
    [[nodiscard]] static ClusterPlacement placeInRun(char32_t codepoint, GraphemeRun& run) noexcept;

    /// Adds @p codepoint to the grapheme cluster the previous write ended in, and claims the columns
    /// that widens it by.
    void appendToLastCluster(char32_t codepoint);

    /// Writes @p text one codepoint at a time through the grapheme segmenter, continuing @p run.
    /// @param text Whole characters: decoding starts afresh.
    /// @param run  The run's segmentation state, carried on to whatever the run writes next.
    void writeTextPerCodepoint(std::string_view text, GraphemeRun& run);

    /// Erases the row at @p line in place and starts it afresh: blank in @p attributes, carrying @p flags --
    /// with the offset of each of them it already carried -- and no context, block or birth time.
    ///
    /// The one way Screen resets a row it keeps; a row the grid recycles as it scrolls in is a new row, and
    /// the grid resets that itself. If the cursor is on the row, it is re-stamped at once with the context,
    /// block and time in effect: a `\r\e[K` that finalises a progress line or redraws a prompt leaves the
    /// cursor where it was, and updateCursorIterator(), which stamps a row on arrival, runs only on a change
    /// of row -- so the row would otherwise carry no context, no block and no time. A progress line therefore
    /// reads the time of its last redraw. Outside a batch the birth stamp is zero and stamps nothing, which
    /// leaves the time to the next batch; a row erased with the cursor elsewhere is stamped when the cursor
    /// reaches it. An erase that takes a row out of its command block, or a head mark off it, counts in
    /// blockRowChanges(); one that takes a vi mark off it is reported to the user-mark index; one that
    /// changes the shell marks a row carries drops the cached fold ranges.
    /// @param line The row to erase.
    /// @param flags The flags it is left with.
    /// @param attributes The rendition its blank cells take.
    /// @return The erased row, for a caller that goes on to write it.
    Line& eraseRowInPlace(LineOffset line, LineFlags flags, GraphicsAttributes attributes) noexcept;

    /// Has what holds this grid's stable ids follow a scroll the grid is about to do that moves or drops
    /// rows' contents under their ids: the marks the shell put on the prompt in progress here, and, on the
    /// primary screen, the command block heads on those rows, looked at again, a collapsed fold, which
    /// follows its prompt line, and a user mark, which follows its line.
    /// @param move What the scroll will do (@see Grid::rowMoveOfScrollUp), asked before it does it; nullopt
    ///        for a scroll that moves no row's content away from its id.
    void followRowMove(std::optional<RowMove> const& move);

    /// followRowMove() for scrollUp(@p n, ..., @p margin), which is about to happen. A function of its own so
    /// that the move it asks the grid for is not a temporary of scrollUp(), the line feed's path.
    /// @param n      The scroll's line count.
    /// @param margin The region it scrolls.
    void followScrollUp(LineCount n, Margin margin);

    /// Ends the wrap from row @p line into the row below it, which then starts a line of its own: what an
    /// erase does that empties the end of @p line (EL 0, ED 0), or all of it above the row below (ED 1).
    /// xterm's ClearRight and ClearBufRows clear LINEWRAPPED the same way. Nothing happens on the page's
    /// last row, whose next row is not on the page.
    /// @param line The row whose wrap ends.
    void endWrapBelow(LineOffset line) noexcept;

    /// Takes from the cursor's row what an ED that reaches across it from edge to edge takes from every row
    /// it erases whole, beyond the cells EL has already blanked: its line rendition, which DEC STD 070's ED
    /// sets to single width (p. 5-156), and its vi mark. EL alone leaves both: it changes what a line says,
    /// not which line it is.
    void forgetCursorRowErasedWhole() noexcept;

    /// Relabels every row of the cursor's logical line with the ids @p transition carries: what an explicit
    /// transition (setActiveContextId(), setActiveBlockId()) does to the line it happens on. A row it takes
    /// from another command block counts in blockRowChanges(): that block's head may have been on it.
    ///
    /// From the cursor's row up, and no further than the first row that carries the ids already: a
    /// transition relabels its line whole, and a row reached since takes the ids in force, so the rows above
    /// such a row carry them too. A transition that hands out what the line carries -- the ;C after the ;A
    /// that began its block, a context fanned out to every screen -- so looks at one row, however long the
    /// line.
    /// @param transition The ids the transition hands out; a zero id leaves the rows' own.
    void restampCursorLogicalLine(LineProvenance const& transition) noexcept;

    void configureCurrentLineSize(LineFlags enabled);

    void advanceCursorAfterWrite(ColumnCount n) noexcept;

    void clearAllTabs();
    void clearTabUnderCursor();
    void setTabUnderCursor();

    /// Applies LF but also moves cursor to given column @p column.
    void linefeed(ColumnOffset column);

    void writeCharToCurrentAndAdvance(char32_t codepoint) noexcept;

    /// What becomes of a character about to be written at the cursor.
    enum class CharacterPlacement : uint8_t
    {
        Dropped, ///< It is not written at all.
        Written, ///< It is written at the cursor, which may have moved to make room for it.
    };

    /// Makes room for a character @p width columns wide that does not fit between the cursor and the
    /// margin. With autowrap it moves on to the next row and leaves the cells it passed over as
    /// CellFlag::WrapPadding, as foot does; without, it is dropped, as xterm drops it (measured). At the
    /// start of its row it fits nowhere better and is written there.
    /// @param width The character's width in columns.
    /// @return What becomes of the character.
    [[nodiscard]] CharacterPlacement placeWideCharacter(int width) noexcept;
    void clearAndAdvance(int oldWidth, int newWidth) noexcept;

    /// Claims or releases columns after a grapheme cluster's width was revised by a codepoint that
    /// joined it late (a variation selector).
    ///
    /// Anchored on @c _lastCursorPosition -- the cluster head -- not on the cursor, which has already
    /// moved past it. Does nothing unless the cursor still sits immediately after that head, so an
    /// intervening cursor move or scroll cannot make this touch an unrelated cell.
    void applyClusterWidthChange(int delta) noexcept;

    /// @return The rightmost column that may still be written on the cursor's line: the right margin
    ///         when the cursor is inside a DECLRMM band, otherwise the last page column.
    [[nodiscard]] ColumnOffset lastWritableColumn() const noexcept;

    void scrollUp(LineCount n, GraphicsAttributes sgr, Margin margin);
    void scrollUp(LineCount n, Margin margin);
    void scrollDown(LineCount n, Margin margin);
    void insertChars(LineOffset lineOffset, ColumnCount columnsToInsert);
    void deleteChars(LineOffset lineOffset, ColumnOffset column, ColumnCount columnsToDelete);

    /// Sets the current column to given logical column number.
    void setCurrentColumn(ColumnOffset n);

    /// Places the cursor at the absolute @p column, clamped to the page.
    ///
    /// Unlike setCurrentColumn(), this does not apply origin mode: it is for callers that have already
    /// computed a real column (a tab stop, the page's first column), which must not have the left margin
    /// added to it a second time. @see cursorBackwardTab().
    void setCurrentAbsoluteColumn(ColumnOffset column);

    [[nodiscard]] std::unique_ptr<ParserExtension> hookSTP(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookSixel(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookReGIS(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookDECAUPSS(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookDECDLD(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookDECDMAC(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookDECRQSS(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookDECUDK(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookXTGETTCAP(Sequence const& seq);

    [[nodiscard]] std::unique_ptr<ParserExtension> hookGoodImageProtocol(Sequence const& seq);
    void handleGipUpload(Message message);
    void handleGipRender(Message const& message);
    void handleGipRelease(Message const& message);
    void handleGipOneshot(Message message);
    void handleGipQuery();
    void replyGipStatus(int statusCode);

    [[nodiscard]] std::optional<Image::Data> decodePng(std::span<uint8_t const> data, ImageSize& size) const;

    void processShellIntegration(Sequence const& seq);

    /// Marks the cursor's logical line as where a command's output starts (LineFlag::OutputStart), as OSC
    /// 133;C or an OSC 3008 command start says, and notes that the shell marks output starts.
    void markOutputStart();

    /// The command line the user typed at the live prompt, read off the grid at OSC 133;C.
    ///
    /// Only possible when the shell marked where its prompt ended (;B): the text from that column up to
    /// the cursor is what was typed, less each continuation prompt the shell marked with a ;B of its own
    /// and each right prompt beside it.
    /// Must run BEFORE the ;C's OutputStart is stamped -- the live-prompt scan reads a line carrying
    /// OutputStart as "a command is running" and finds no prompt.
    /// @return The typed text, trailing blanks dropped, or nullopt without ;B or when nothing was typed.
    [[nodiscard]] std::optional<std::string> recoverCommandLine() const;

    /// The live prompt a column change leaves as the shell drew it, and the names its lines go by until then.
    struct UnreflowedPrompt
    {
        /// Its first row; it reaches to the bottom of the page.
        LineOffset firstLine;

        /// The row each of its logical lines begins on, and the name LivePromptMarks knows the line by.
        std::vector<std::pair<LineOffset, StableLine>> lines {};
    };

    /// The live prompt, if a column change is to leave it as the shell drew it rather than reflow it.
    ///
    /// The shell redraws its prompt at the new width, moving up as many rows as it drew before; reflowed
    /// under it as well, the prompt spans other rows than the shell counts, and the redraw leaves doubled or
    /// misplaced fragments of it, or erases the output above it. So it is not reflowed, as kitty does it
    /// (prevent_current_prompt_from_rewrapping) -- unless the shell said it does not redraw it
    /// (OSC 133;A;redraw=0), the prompt began above the page, where kitty does not look for it, or it shares
    /// its first logical line with output that did not end that line. Nor before the shell has marked an
    /// output start (@see OutputStartMarking): kitty's own integration always does, but a shell that sends
    /// only ;A would have a running command's output taken for its prompt, and cut rather than reflowed.
    /// @return The prompt, or nullopt when the column change reflows everything.
    [[nodiscard]] std::optional<UnreflowedPrompt> unreflowedPrompt() const;

    /// Hands what LivePromptMarks holds for the lines of @p prompt over to the names the resize just gave
    /// them, each column it marked moving with its cell.
    /// @param prompt The prompt, as unreflowedPrompt() found it before the resize.
    /// @param moved How far the resize moved the prompt's rows on the page.
    /// @param width The resize's change of width.
    void renameUnreflowedPromptLines(UnreflowedPrompt const& prompt, LineOffset moved, WidthChange width);

    /// Whether this screen is the main display -- the primary or the alternate screen -- where the shell's
    /// command cycle runs, and so whose OSC 133 and OSC 3008 command transitions feed the command block
    /// store. Not a DEC page other than the first, nor a status line. Rows are stamped with blocks on the
    /// primary screen alone, whichever screen fed the store.
    [[nodiscard]] bool isMainDisplay() const noexcept;

    void handleSemanticBlockQuery(Sequence const& seq);

    /// Answers an SBQUERY with the blocks it found: each one's record, and the text of its own rows.
    /// @param newestFirst The blocks, newest first; the reply lists them oldest first, and reports no data
    ///        when there are none.
    void replyWithBlocks(std::span<CommandBlockRecord const* const> newestFirst);

    gsl::not_null<Terminal*> _terminal;
    gsl::not_null<Settings*> _settings;
    gsl::not_null<Margin*> _margin;
    Grid _grid;

    Cursor _cursor {};
    /// What DECSC saved here, but the page (Terminal::_savedCursorPage): DECRC reads only the buffer of the
    /// page its screen buffer's last DECSC was received on, and every other page keeps its own, unread until
    /// a DECSC there, because a resize reflows a page's saved cursor with its grid. A status line's is its
    /// own.
    Cursor _savedCursor {};

    /// What a row the cursor reaches is handed where it carries nothing yet: the OSC 3008 context and the
    /// command block being written, and the batch's birth stamp.
    /// @see updateCursorIterator, setActiveContextId, setActiveBlockId, setLineBirthStamp
    LineProvenance _stamp {};

    /// @see blockRowChanges
    uint64_t _blockRowChanges = 0;

    /// @see restampWork
    uint64_t _restampWork = 0;

    /// Scratch for the OSC 3008 decoder, owned here so the per-prompt hot path allocates once per
    /// session rather than once per sequence. @see parseContextSequence.
    std::string _contextScratch {};

    /// What the shell said about the lines of the prompt in progress that no row records, for
    /// recoverCommandLine(): where a right prompt begins (OSC 133;P;k=r), and whether the input ends with
    /// its line (OSC 133;I). setMark() forgets it.
    LivePromptMarks _livePromptMarks {};

    /// Whether the shell redraws the prompt in progress when the window is resized, as its OSC 133;A said.
    /// @see unreflowedPrompt
    PromptRedraw _promptRedraw = PromptRedraw::OnResize;

    /// Whether the shell has marked a command's output start here since the last hard reset.
    /// @see unreflowedPrompt, markOutputStart
    OutputStartMarking _outputStartMarking = OutputStartMarking::Unseen;

    /// Payload accumulated across a chunked kitty graphics transmission (`m=1`), plus the command
    /// that opened it -- the continuation chunks carry no control data of their own.
    std::string _kittyChunkedPayload {};
    std::optional<kitty_graphics::Command> _kittyChunkedCommand {};

    /// Images transmitted by a kitty graphics command but not yet displayed, keyed by their `i=` id.
    std::unordered_map<uint32_t, std::shared_ptr<Image const>> _kittyImages {};

    std::unordered_map<uint32_t, KittyAnimation> _kittyAnimations {};

    // NOTE: the `OSC 5522` write transmission lives on Terminal, not here: an application may switch
    // screens (DECSASD, or a page change) between chunks, and a per-screen buffer would drop the
    // chunks that landed elsewhere while still answering DONE. @see Terminal::kittyClipboardWrite.

    GraphicsAttributes _savedGraphicsRenditions {};

    CellLocation _lastCursorPosition {};

    /// @see linesScrolledIntoHistory()
    uint64_t _linesScrolledIntoHistory = 0;

    Line* _currentLine = nullptr;
    std::unique_ptr<SixelImageBuilder> _sixelImageBuilder;

    // Persistent ReGIS interpreter state and canvas: ReGIS state and pixels survive across DCS
    // strings, so these live on the Screen and a fresh parser references them each hook.
    std::unique_ptr<regis::ReGISContext> _regisContext;
    std::unique_ptr<regis::ReGISRasterizer> _regisCanvas;
    std::shared_ptr<regis::ReGISTextRasterizer> _regisTextRasterizer;
    std::unique_ptr<regis::ReGISEvents> _regisEvents;

#ifdef LIBTERMINAL_LOG_TRACE
    std::atomic<bool> _logCharTrace = true;
    std::string _pendingCharTraceLog;
#endif

    std::string_view _name;

    /// Controls whether DECCARA/DECRARA operate on the full rectangle (true) or stream (false).
    bool _rectangularAttributeMode = true;

    /// Whether ISO 6429 guarded-area protection (SPA/EPA) is in effect. Set by SPA, and left set by
    /// EPA (which only stops guarding *new* cells); cleared only by a reset. @see resetProtection.
    bool _isoProtectionActive = false;

    /// Whether the regular erases (ED/EL/ECH) must spare CellFlag::CharacterProtectedISO cells.
    /// True only while ISO protection is active; when false those erases take their fast paths. DECSCA
    /// (CellFlag::CharacterProtected) is honoured by the selective erases instead, never here.
    [[nodiscard]] bool eraseSkipsProtectedCells() const noexcept { return _isoProtectionActive; }

    // VT525 keyboard settings that Contour has nothing to act on but must still remember and report
    // through DECRQSS: it stores the parameter it was given and hands it back verbatim. @see DECELF,
    // DECLFKC and DECSMKR in requestStatusString(). TODO: wire these to real keyboard behaviour.
    int _enableLocalFunctions = 0;    ///< DECELF (`CSI Pn + q`).
    int _localFunctionKeyControl = 0; ///< DECLFKC (`CSI Pn * }`).
    int _modifierKeyReporting = 0;    ///< DECSMKR (`CSI Pn + r`).
};

inline void Screen::scrollUp(LineCount n, Margin margin)
{
    scrollUp(n, cursor().graphicsRendition, margin);
}

// isContiguousToCurrentLine is implemented in Screen.cpp because it needs
// access to Terminal which is only forward-declared in this header.

} // namespace vtbackend

// {{{ fmt formatter
template <>
struct std::formatter<vtbackend::RequestStatusString>: formatter<std::string_view>
{
    auto format(vtbackend::RequestStatusString value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::RequestStatusString::SGR: name = "SGR"; break;
            case vtbackend::RequestStatusString::DECSCL: name = "DECSCL"; break;
            case vtbackend::RequestStatusString::DECSCUSR: name = "DECSCUSR"; break;
            case vtbackend::RequestStatusString::DECSCA: name = "DECSCA"; break;
            case vtbackend::RequestStatusString::DECSACE: name = "DECSACE"; break;
            case vtbackend::RequestStatusString::DECELF: name = "DECELF"; break;
            case vtbackend::RequestStatusString::DECLFKC: name = "DECLFKC"; break;
            case vtbackend::RequestStatusString::DECSMKR: name = "DECSMKR"; break;
            case vtbackend::RequestStatusString::DECSTBM: name = "DECSTBM"; break;
            case vtbackend::RequestStatusString::DECSLRM: name = "DECSLRM"; break;
            case vtbackend::RequestStatusString::DECSLPP: name = "DECSLPP"; break;
            case vtbackend::RequestStatusString::DECSCPP: name = "DECSCPP"; break;
            case vtbackend::RequestStatusString::DECSNLS: name = "DECSNLS"; break;
            case vtbackend::RequestStatusString::DECSASD: name = "DECSASD"; break;
            case vtbackend::RequestStatusString::DECSSDT: name = "DECSSDT"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtbackend::Sequence>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(vtbackend::Sequence const& seq, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "{}", seq.text());
    }
};
// }}}
