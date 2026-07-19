// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/ControlCode.h>
#include <vtbackend/DesktopNotification.h>
#include <vtbackend/InputGenerator.h>
#include <vtbackend/MessageParser.h>
#include <vtbackend/RectangularAreaChecksum.h>
#include <vtbackend/Screen.h>
#include <vtbackend/SixelParser.h>
#include <vtbackend/SoAClusterWriter.h>
#include <vtbackend/Terminal.h>
#include <vtbackend/VTType.h>
#include <vtbackend/VTWriter.h>
#include <vtbackend/logging.h>
#include <vtbackend/regis/ReGISContext.h>
#include <vtbackend/regis/ReGISParser.h>
#include <vtbackend/regis/ReGISRasterizer.h>
#include <vtbackend/regis/ReGISTextRasterizer.h>

#include <crispy/App.h>
#include <crispy/Comparison.h>
#include <crispy/algorithm.h>
#include <crispy/base64.h>
#include <crispy/escape.h>
#include <crispy/size.h>
#include <crispy/times.h>
#include <crispy/utils.h>

#include <libunicode/convert.h>
#include <libunicode/emoji_segmenter.h>
#include <libunicode/grapheme_segmenter.h>
#include <libunicode/utf8_grapheme_segmenter.h>
#include <libunicode/word_segmenter.h>

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstring>
#include <iostream>
#include <iterator>
#include <optional>
#include <ranges>
#include <sstream>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>

#if defined(_WIN32)
    #include <Windows.h>
#endif

using namespace std::string_view_literals;

using crispy::escape;
using crispy::for_each;
using crispy::times;
using crispy::toHexString;

using gsl::span;

using std::accumulate;
using std::array;
using std::clamp;
using std::endl;
using std::fill;
using std::function;
using std::get;
using std::holds_alternative;
using std::make_shared;
using std::make_unique;
using std::monostate;
using std::move;
using std::next;
using std::nullopt;
using std::optional;
using std::ostringstream;
using std::pair;
using std::prev;
using std::ref;
using std::rotate;
using std::shared_ptr;
using std::string;
using std::string_view;
using std::tuple;
using std::unique_ptr;
using std::vector;
using std::views::iota;

namespace vtbackend
{

auto constexpr inline ColorPaletteUpdateDsrRequestId = 996;
auto constexpr inline ColorPaletteUpdateDsrReplyId = 997;

auto constexpr inline TabWidth = ColumnCount(8);

auto const inline vtCaptureBufferLog = logstore::category("vt.ext.capturebuffer",
                                                          "Capture Buffer debug logging.",
                                                          logstore::category::state::Disabled,
                                                          logstore::category::visibility::Hidden);

#define SAFE_SYS_CALL(x)                \
    while ((x) == -1 && errno == EINTR) \
    {                                   \
    }

namespace // {{{ helper
{
    constexpr bool isLightColor(RGBColor color) noexcept
    {
        return ((5 * color.green) + (2 * color.red) + color.blue) > 8 * 128;
    }

    template <typename Rep, typename Period>
    inline void sleep_for(std::chrono::duration<Rep, Period> const& rtime)
    {
#if defined(USE_SLEEP_FOR_FROM_STD_CHRONO)
        std::this_thread::sleep_for(rtime);
#else
        if (rtime.count() <= 0)
            return;
    #if defined(_WIN32)
        auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(rtime);
        Sleep(ms.count());
    #else
        auto const s = std::chrono::duration_cast<std::chrono::seconds>(rtime);
        auto const ns = std::chrono::duration_cast<std::chrono::nanoseconds>(rtime - s);
        struct ::timespec ts = { .tv_sec = static_cast<std::time_t>(s.count()),
                                 .tv_nsec = static_cast<long>(ns.count()) };
        SAFE_SYS_CALL(::nanosleep(&ts, &ts));
    #endif
#endif
    }

    std::string vtSequenceParameterString(GraphicsAttributes const& sgr)
    {
        std::string output;
        // TODO: See if we can use VTWriter here instead (might need a little refactor).
        // std::stringstream os;
        // auto vtWriter = VTWriter(os);
        // vtWriter.setForegroundColor(sgr.foregroundColor);
        // vtWriter.setBackgroundColor(sgr.backgroundColor);

        auto const sgrSep = [&]() {
            if (!output.empty())
                output += ';';
        };
        auto const sgrAdd = [&](unsigned value) {
            sgrSep();
            output += std::to_string(value);
        };
        auto const sgrAddStr = [&](string_view value) {
            sgrSep();
            output += value;
        };
        auto const sgrAddSub = [&](unsigned value) {
            sgrSep();
            output += std::to_string(value);
        };

        if (isIndexedColor(sgr.foregroundColor))
        {
            auto const colorValue = getIndexedColor(sgr.foregroundColor);
            if (static_cast<unsigned>(colorValue) < 8)
                sgrAdd(30 + static_cast<unsigned>(colorValue));
            else
            {
                sgrAdd(38);
                sgrAddSub(5);
                sgrAddSub(static_cast<unsigned>(colorValue));
            }
        }
        // A default foreground is already implied by the leading `0` (reset) DECRQSS prepends, so it is
        // not spelled out -- xterm's SGR report lists only the attributes that actually differ.
        else if (isBrightColor(sgr.foregroundColor))
            sgrAdd(90 + static_cast<unsigned>(getBrightColor(sgr.foregroundColor)));
        else if (isRGBColor(sgr.foregroundColor))
        {
            auto const rgb = getRGBColor(sgr.foregroundColor);
            sgrAdd(38);
            sgrAddSub(2);
            sgrAddSub(static_cast<unsigned>(rgb.red));
            sgrAddSub(static_cast<unsigned>(rgb.green));
            sgrAddSub(static_cast<unsigned>(rgb.blue));
        }

        if (isIndexedColor(sgr.backgroundColor))
        {
            auto const colorValue = getIndexedColor(sgr.backgroundColor);
            if (static_cast<unsigned>(colorValue) < 8)
                sgrAdd(40 + static_cast<unsigned>(colorValue));
            else
            {
                sgrAdd(48);
                sgrAddSub(5);
                sgrAddSub(static_cast<unsigned>(colorValue));
            }
        }
        // As with the foreground: a default background is implied by the reset and left unspoken.
        else if (isBrightColor(sgr.backgroundColor))
            sgrAdd(100 + getBrightColor(sgr.backgroundColor));
        else if (isRGBColor(sgr.backgroundColor))
        {
            auto const& rgb = getRGBColor(sgr.backgroundColor);
            sgrAdd(48);
            sgrAddSub(2);
            sgrAddSub(static_cast<unsigned>(rgb.red));
            sgrAddSub(static_cast<unsigned>(rgb.green));
            sgrAddSub(static_cast<unsigned>(rgb.blue));
        }

        if (isRGBColor(sgr.underlineColor))
        {
            auto const& rgb = getRGBColor(sgr.underlineColor);
            sgrAdd(58);
            sgrAddSub(2);
            sgrAddSub(static_cast<unsigned>(rgb.red));
            sgrAddSub(static_cast<unsigned>(rgb.green));
            sgrAddSub(static_cast<unsigned>(rgb.blue));
        }

        // TODO: sgr.styles;
        auto constexpr Masks = array {
            pair { CellFlag::Bold, "1"sv },
            pair { CellFlag::Faint, "2"sv },
            pair { CellFlag::Italic, "3"sv },
            pair { CellFlag::Underline, "4"sv },
            pair { CellFlag::Blinking, "5"sv },
            pair { CellFlag::RapidBlinking, "6"sv },
            pair { CellFlag::Inverse, "7"sv },
            pair { CellFlag::Hidden, "8"sv },
            pair { CellFlag::CrossedOut, "9"sv },
            pair { CellFlag::DoublyUnderlined, "4:2"sv },
            pair { CellFlag::CurlyUnderlined, "4:3"sv },
            pair { CellFlag::DottedUnderline, "4:4"sv },
            pair { CellFlag::DashedUnderline, "4:5"sv },
            pair { CellFlag::Framed, "51"sv },
            // TODO(impl or completely remove): pair{CellFlag::Encircled, ""sv},
            pair { CellFlag::Overline, "53"sv },
        };

        for (auto const& mask: Masks)
            if (sgr.flags & mask.first)
                sgrAddStr(mask.second);

        return output;
    }

    template <typename T, typename U>
    std::optional<boxed::boxed<T, U>> decr(std::optional<boxed::boxed<T, U>> v)
    {
        if (v.has_value())
            --*v;
        return v;
    }

    // optional<CharsetTable> getCharsetTableForCode(std::string const& intermediate)
    // {
    //     if (intermediate.size() != 1)
    //         return nullopt;
    //
    //     char const code = intermediate[0];
    //     switch (code)
    //     {
    //         case '(':
    //             return {CharsetTable::G0};
    //         case ')':
    //         case '-':
    //             return {CharsetTable::G1};
    //         case '*':
    //         case '.':
    //             return {CharsetTable::G2};
    //         case '+':
    //         case '/':
    //             return {CharsetTable::G3};
    //         default:
    //             return nullopt;
    //     }
    // }

    std::optional<int> toNumber(string const* value)
    {
        if (!value)
            return std::nullopt;
        return crispy::to_integer<10, int>(string_view(*value));
    }

    optional<ImageAlignment> toImageAlignmentPolicy(string const* value, ImageAlignment defaultValue)
    {
        if (!value)
            return defaultValue;

        if (value->size() != 1)
            return nullopt;

        switch (value->at(0))
        {
            case '1': return ImageAlignment::TopStart;
            case '2': return ImageAlignment::TopCenter;
            case '3': return ImageAlignment::TopEnd;
            case '4': return ImageAlignment::MiddleStart;
            case '5': return ImageAlignment::MiddleCenter;
            case '6': return ImageAlignment::MiddleEnd;
            case '7': return ImageAlignment::BottomStart;
            case '8': return ImageAlignment::BottomCenter;
            case '9': return ImageAlignment::BottomEnd;
            default: return nullopt;
        }
    }

    optional<ImageResize> toImageResizePolicy(string const* value, ImageResize defaultValue)
    {
        if (!value)
            return defaultValue;

        if (value->size() != 1)
            return nullopt;

        switch (value->at(0))
        {
            case '0': return ImageResize::NoResize;
            case '1': return ImageResize::ResizeToFit;
            case '2': return ImageResize::ResizeToFill;
            case '3': return ImageResize::StretchToFill;
            default: return nullopt;
        }
    }

    optional<ImageFormat> toImageFormat(string const* value)
    {
        auto constexpr DefaultFormat = ImageFormat::Auto;

        if (value)
        {
            if (value->size() == 1)
            {
                switch (value->at(0))
                {
                    case '1': return ImageFormat::Auto;
                    case '2': return ImageFormat::RGB;
                    case '3': return ImageFormat::RGBA;
                    case '4': return ImageFormat::PNG;
                    default: return nullopt;
                }
            }
            else
                return nullopt;
        }
        else
            return DefaultFormat;
    }

    /// Resolves Auto format to a concrete format by inspecting the data.
    ///
    /// @param data  The raw image data to inspect.
    /// @param size  The declared image dimensions (may be zero for self-describing formats).
    /// @return The detected format, or std::nullopt if the format cannot be determined.
    std::optional<ImageFormat> resolveAutoFormat(std::span<uint8_t const> data, ImageSize size)
    {
        // Check PNG magic bytes.
        static constexpr uint8_t PngMagic[] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
        if (data.size() >= sizeof(PngMagic) && std::memcmp(data.data(), PngMagic, sizeof(PngMagic)) == 0)
            return ImageFormat::PNG;

        // Detect RGB/RGBA from data size when dimensions are known.
        if (*size.width > 0 && *size.height > 0)
        {
            auto const pixelCount = static_cast<size_t>(*size.width) * *size.height;
            if (data.size() == pixelCount * 4)
                return ImageFormat::RGBA;
            if (data.size() == pixelCount * 3)
                return ImageFormat::RGB;
        }

        return std::nullopt;
    }

    ImageLayer toImageLayer(string const* value)
    {
        if (!value || value->empty())
            return ImageLayer::Replace;

        switch (value->at(0))
        {
            case '0': return ImageLayer::Below;
            case '1': return ImageLayer::Replace;
            case '2': return ImageLayer::Above;
            default: return ImageLayer::Replace;
        }
    }

    /// Common parameters shared by GIP render and oneshot operations.
    struct GipRenderParams
    {
        LineCount screenRows {};
        ColumnCount screenCols {};
        Width imageWidth {};
        Height imageHeight {};
        ImageAlignment alignmentPolicy { ImageAlignment::MiddleCenter };
        ImageResize resizePolicy { ImageResize::NoResize };
        ImageLayer layer { ImageLayer::Replace };
        bool autoScroll {};
        bool updateCursor {};
        bool requestStatus {};
    };

    GipRenderParams parseGipRenderParams(Message const& message)
    {
        return GipRenderParams {
            .screenRows = LineCount::cast_from(toNumber(message.header("r")).value_or(0)),
            .screenCols = ColumnCount::cast_from(toNumber(message.header("c")).value_or(0)),
            .imageWidth = Width::cast_from(toNumber(message.header("w")).value_or(0)),
            .imageHeight = Height::cast_from(toNumber(message.header("h")).value_or(0)),
            .alignmentPolicy = toImageAlignmentPolicy(message.header("a"), ImageAlignment::MiddleCenter)
                                   .value_or(ImageAlignment::MiddleCenter),
            .resizePolicy = toImageResizePolicy(message.header("z"), ImageResize::NoResize)
                                .value_or(ImageResize::NoResize),
            .layer = toImageLayer(message.header("L")),
            .autoScroll = message.header("l") != nullptr,
            .updateCursor = message.header("u") != nullptr,
            .requestStatus = message.header("s") != nullptr,
        };
    }
} // namespace

// }}}

Screen::~Screen() = default;

Screen::Screen(Terminal& terminal,
               gsl::not_null<Margin*> margin,
               PageSize pageSize,
               bool reflowOnResize,
               MaxHistoryLineCount maxHistoryLineCount,
               std::string_view name):
    _terminal { &terminal },
    _settings { &terminal.settings() },
    _margin { margin },
    _grid { pageSize, reflowOnResize, maxHistoryLineCount },
    _name { name }
{
    updateCursorIterator();
}

unsigned Screen::numericCapability(capabilities::Code cap) const
{
    using namespace capabilities::literals;

    switch (cap)
    {
        case "li"_tcap: return unbox<unsigned>(pageSize().lines);
        case "co"_tcap: return unbox<unsigned>(pageSize().columns);
        case "it"_tcap: return unbox<unsigned>(TabWidth);
        default: return StaticDatabase::numericCapability(cap);
    }
}

void Screen::verifyState() const
{
    Require(*_cursor.position.column < *pageSize().columns);
    Require(*_cursor.position.line < *pageSize().lines);

    // verify cursor positions
    [[maybe_unused]] auto const clampedCursorPos = clampToScreen(_cursor.position);
    if (_cursor.position != clampedCursorPos)
    {
        fail(std::format("Cursor {} does not match clamp to screen {}.", _cursor.position, clampedCursorPos));
        // FIXME: the above triggers on tmux vertical screen split (cursor.column off-by-one)
    }

    _grid.verifyState();
}

void Screen::fail(std::string const& message) const
{
    inspect(message, std::cerr);
    abort();
}

void Screen::hardReset()
{
    _grid.reset();
    _cursor = {};
    _lastCursorPosition = {};
    resetProtection();
    resetKittyState();
    updateCursorIterator();
}

void Screen::applyPageSizeToMainDisplay(PageSize mainDisplayPageSize)
{
    auto cursorPosition = _cursor.position;

    // Only reset margins when the page size actually changes (during a resize),
    // not during a simple page switch where the grid size already matches.
    auto const sizeChanged = (_grid.pageSize() != mainDisplayPageSize);

    // Ensure correct screen buffer size for the buffer we've just switched to.
    cursorPosition = _grid.resize(mainDisplayPageSize, cursorPosition, _cursor.wrapPending);
    cursorPosition = clampCoordinate(cursorPosition);

    if (sizeChanged)
    {
        *_margin = Margin {
            .vertical = Margin::Vertical { .from = {}, .to = mainDisplayPageSize.lines.as<LineOffset>() - 1 },
            .horizontal =
                Margin::Horizontal { .from = {}, .to = mainDisplayPageSize.columns.as<ColumnOffset>() - 1 }
        };
    }

    if (_cursor.position.column < boxed_cast<ColumnOffset>(mainDisplayPageSize.columns))
        _cursor.wrapPending = false;

    // update (last-)cursor position
    _cursor.position = cursorPosition;
    _lastCursorPosition = cursorPosition;
    updateCursorIterator();

    // TODO: find out what to do with DECOM mode. Reset it to?

    verifyState();
}

void Screen::advanceCursorAfterWrite(ColumnCount n) noexcept
{
    assert(_cursor.position.column.value + n.value <= margin().horizontal.to.value + 1);
    //  + 1 here because `to` is inclusive.

    if (_cursor.position.column.value + n.value < pageSize().columns.value)
        _cursor.position.column.value += n.value;
    else
    {
        _cursor.position.column.value += n.value - 1;
        _cursor.wrapPending = true;
    }
}

void Screen::writeText(string_view text, size_t cellCount)
{
#if defined(LIBTERMINAL_LOG_TRACE)
    if (vtTraceSequenceLog)
        vtTraceSequenceLog()(
            "[{}] text: ({} bytes, {} cells): \"{}\"", _name, text.size(), cellCount, escape(text));

    // Do not log individual characters, as we already logged the whole string above
    _logCharTrace = false;
    auto const _ = crispy::finally { [&]() { _logCharTrace = true; } };
#endif

    assert(cellCount <= static_cast<size_t>(pageSize().columns.value - _cursor.position.column.value));

    // Fast path: bulk-write ASCII directly into SoA arrays.
    // Only for pure ASCII content — non-ASCII needs the per-codepoint grapheme segmenter
    // to handle multi-codepoint clusters (emoji + VS16, ZWJ sequences, etc.).
    auto const isPureAscii =
        std::ranges::all_of(text, [](char ch) { return static_cast<unsigned char>(ch) < 0x80; });
    if (isPureAscii && !_terminal->isModeEnabled(AnsiMode::Insert) && isFullHorizontalMargins()
        && _cursor.charsets.isSelected(CharsetId::USASCII) && !_cursor.charsets.activeDRCSFont().has_value())
    {
        crlfIfWrapPending();

        auto const columnsAvailable =
            static_cast<size_t>(pageSize().columns.value - _cursor.position.column.value);

        auto& line = currentLine();
        auto& soa = line.materializedStorage();
        auto const startCol = static_cast<size_t>(_cursor.position.column.value);

        // If the cursor is at a wide-char continuation cell, erase the left half
        // of the wide character before overwriting. The bulk SoA write path does
        // not handle this, unlike the per-character writeCharToCurrentAndAdvance.
        if (startCol > 0 && soa.sgr[startCol].flags.contains(CellFlag::WideCharContinuation))
        {
            auto prevCell = line.useCellAt(_cursor.position.column - 1);
            prevCell.reset(_cursor.graphicsRendition);
        }

        auto const written = writeTextToSoA(soa,
                                            startCol,
                                            text,
                                            _cursor.graphicsRendition,
                                            _cursor.hyperlink,
                                            /*asciiHint=*/true);

        // Advance parsing buffer hot-end to match baseline's tryEmplaceChars behavior.
        // This reduces bytesAvailable(), forcing the next readFromPty() to allocate a
        // fresh buffer. Without this, the next PTY read reuses the same large buffer,
        // batching the subsequent output (e.g., shell prompt) into the same chunk —
        // preventing the display from refreshing between them.
        if (auto const buf = _terminal->parsingBuffer(); buf)
            buf->advanceHotEndUntil(text.data() + text.size());

        // Update last cursor position for grapheme cluster continuation
        _lastCursorPosition = _cursor.position;

        // Advance cursor
        if (written > 0)
        {
            if (static_cast<int>(written) < static_cast<int>(columnsAvailable))
            {
                _cursor.position.column += ColumnOffset::cast_from(written);
            }
            else if (_terminal->isModeEnabled(DECMode::AutoWrap))
            {
                _cursor.position.column = ColumnOffset::cast_from(static_cast<int>(startCol + written) - 1);
                _cursor.wrapPending = true;
            }
            else
            {
                _cursor.position.column = boxed_cast<ColumnOffset>(pageSize().columns) - 1;
            }

            // Note: parser's lastCodepointHint is not updated here (private).
            // REP (CSI Ps b) may not work correctly after bulk writes.
            // TODO: Add a public setter if REP-after-bulk-text is needed.
        }
        _terminal->resetInstructionCounter();
        _terminal->markCellDirty(_cursor.position);
        return;
    }

    // Slow path: decode UTF-8 and process per-codepoint with grapheme segmentation.
    // We use a LOCAL grapheme state here — the parser's scan_text has already advanced
    // the parser's graphemeSegmenterState and lastCodepointHint past this text.
    // Reusing the parser state would double-process the grapheme segmentation,
    // corrupting ZWJ emoji sequences and other multi-codepoint clusters.
    {
        // Reconstruct the grapheme state from the preceding cell's codepoints.
        // This is needed for cross-call clusters (e.g., 👨 written in one call,
        // then ZWJ in the next — the ZWJ must join with the preceding 👨).
        char32_t prevCodepoint = 0;
        unicode::grapheme_segmenter_state localGraphemeState {};
        if (_lastCursorPosition.column >= ColumnOffset(0) && _lastCursorPosition.line >= LineOffset(0)
            && _lastCursorPosition.line < boxed_cast<LineOffset>(pageSize().lines))
        {
            auto const& prevLine = grid().lineAt(_lastCursorPosition.line);
            auto const prevCol = unbox<size_t>(_lastCursorPosition.column);
            // Blank lines have no preceding codepoints to seed grapheme state with.
            if (prevCol < unbox<size_t>(prevLine.size()) && !prevLine.isBlank())
            {
                auto const& prevStorage = prevLine.storage();
                auto const prevProxy = ConstCellProxy(prevStorage, prevCol);
                auto const cpCount = prevProxy.codepointCount();
                // Replay all codepoints from the preceding cell to build correct state
                // (needed for GB11: Extended_Pictographic Extend* ZWJ × Extended_Pictographic)
                for (size_t i = 0; i < cpCount; ++i)
                {
                    auto const cp = prevProxy.codepoint(i);
                    if (!prevCodepoint)
                        unicode::grapheme_process_init(cp, localGraphemeState);
                    else
                        unicode::grapheme_process_breakable(cp, localGraphemeState);
                    prevCodepoint = cp;
                }
            }
        }

        for (auto const rawCp: unicode::convert_to<char32_t>(text))
        {
            crlfIfWrapPending();

            // DRCS check: if the active charset is a DRCS font, render as image
            if (auto const drcsFont = _cursor.charsets.activeDRCSFont(); drcsFont.has_value())
            {
                (void) _cursor.charsets.map(rawCp); // Advance single-shift state
                if (auto const* charset = _terminal->drcsCharset(*drcsFont); charset != nullptr)
                {
                    auto const charPos = static_cast<int>(rawCp);
                    if (auto const glyphIt = charset->glyphs.find(charPos); glyphIt != charset->glyphs.end())
                    {
                        auto const fgColor = vtbackend::apply(_terminal->colorPalette(),
                                                              _cursor.graphicsRendition.foregroundColor,
                                                              ColorTarget::Foreground,
                                                              ColorMode::Normal);
                        auto rasterizedImage = _terminal->createDRCSImage(glyphIt->second, fgColor);
                        auto cell = useCellAt(_cursor.position.line, _cursor.position.column);
                        cell.write(_cursor.graphicsRendition, U' ', 1, _cursor.hyperlink);
                        cell.setImageFragment(
                            rasterizedImage,
                            CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) });
                        _lastCursorPosition = _cursor.position;
                        prevCodepoint = U' ';
                        advanceCursorAfterWrite(ColumnCount(1));
                        _terminal->markCellDirty(_lastCursorPosition);
                        continue;
                    }
                }
            }

            auto const cp = _cursor.charsets.map(rawCp);

            if (!prevCodepoint)
            {
                unicode::grapheme_process_init(cp, localGraphemeState);
                writeCharToCurrentAndAdvance(cp);
            }
            else if (unicode::grapheme_process_breakable(cp, localGraphemeState))
            {
                writeCharToCurrentAndAdvance(cp);
            }
            else
            {
                auto const widthChange = usePreviousCell().appendCharacter(cp, clusterWidthPolicy());
                applyClusterWidthChange(widthChange);
                _terminal->markCellDirty(_lastCursorPosition);
            }

            prevCodepoint = cp;
            _terminal->resetInstructionCounter();
        }
    }
}

void Screen::writeTextEnd()
{
#if defined(LIBTERMINAL_LOG_TRACE)
    // Do not log individual characters, as we already logged the whole string above
    if (!_pendingCharTraceLog.empty())
    {
        if (vtTraceSequenceLog)
            vtTraceSequenceLog()("[{}] text: \"{}\"", _name, _pendingCharTraceLog);

        _pendingCharTraceLog.clear();
    }
#endif

    // Writing text wraps lines and scrolls the page, so a text run is held to the same invariants a
    // sequence is. Verified once per run rather than once per codepoint: the invariants describe the
    // page, not any single cell, and checking them per codepoint would make a debug build unusable.
    // Compiled out unless CONTOUR_VERIFY_STATE.
    _terminal->verifyState();
}

void Screen::writeTextFromExternal(std::string_view text)
{
#if defined(LIBTERMINAL_LOG_TRACE)
    if (vtTraceSequenceLog)
        vtTraceSequenceLog()("external text: \"{}\"", text);
#endif

    for (char32_t const ch: unicode::convert_to<char32_t>(text))
        writeTextInternal(ch);
}

void Screen::crlfIfWrapPending()
{
    if (_cursor.wrapPending
        && _terminal->isModeEnabled(DECMode::AutoWrap)) // && !_terminal->isModeEnabled(DECMode::TextReflow))
    {
        bool const lineWrappable = currentLine().wrappable();
        crlf();
        if (lineWrappable)
            currentLine().setFlag(LineFlags { LineFlag::Wrappable, LineFlag::Wrapped }, true);
    }
}

void Screen::writeText(char32_t codepoint)
{
#if defined(LIBTERMINAL_LOG_TRACE)
    if (vtTraceSequenceLog && _logCharTrace.load())
        _pendingCharTraceLog += unicode::convert_to<char>(codepoint);
#endif

    return writeTextInternal(codepoint);
}

void Screen::writeTextInternal(char32_t sourceCodepoint)
{
    crlfIfWrapPending();

    // Check if the active charset is a DRCS font
    if (auto const drcsFont = _cursor.charsets.activeDRCSFont(); drcsFont.has_value())
    {
        if (auto const* charset = _terminal->drcsCharset(*drcsFont); charset != nullptr)
        {
            auto const charPos = static_cast<int>(sourceCodepoint);
            if (auto const glyphIt = charset->glyphs.find(charPos); glyphIt != charset->glyphs.end())
            {
                // Resolve foreground color for the glyph
                auto const fgColor = vtbackend::apply(_terminal->colorPalette(),
                                                      _cursor.graphicsRendition.foregroundColor,
                                                      ColorTarget::Foreground,
                                                      ColorMode::Normal);

                // Create an RGBA image from the monochrome DRCS glyph
                auto rasterizedImage = _terminal->createDRCSImage(glyphIt->second, fgColor);

                // Write a space as placeholder, attach the DRCS image, then advance cursor.
                {
                    auto cell = useCellAt(_cursor.position.line, _cursor.position.column);
                    cell.write(_cursor.graphicsRendition, U' ', 1, _cursor.hyperlink);
                    cell.setImageFragment(rasterizedImage,
                                          CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) });
                    _lastCursorPosition = _cursor.position;
                    advanceCursorAfterWrite(ColumnCount(1));
                    _terminal->markCellDirty(_lastCursorPosition);
                }
                (void) _cursor.charsets.map(sourceCodepoint); // Advance single-shift state
                _terminal->resetInstructionCounter();
                return;
            }
        }
        // Fall through to normal rendering if glyph not found
    }

    char32_t const codepoint = _cursor.charsets.map(sourceCodepoint);

    // Reconstruct grapheme segmentation state from the preceding cell rather than
    // using the parser's state. The parser's scan_text may have already advanced
    // lastCodepointHint and graphemeSegmenterState past this codepoint (when the
    // bulk text path partially processes bytes then falls back to the FSM),
    // causing double-processing that corrupts ZWJ emoji sequences.
    char32_t precedingChar = 0;
    unicode::grapheme_segmenter_state graphemeState {};
    if (_lastCursorPosition.column >= ColumnOffset(0) && _lastCursorPosition.line >= LineOffset(0)
        && _lastCursorPosition.line < boxed_cast<LineOffset>(pageSize().lines))
    {
        auto const& prevLine = grid().lineAt(_lastCursorPosition.line);
        auto const prevCol = unbox<size_t>(_lastCursorPosition.column);
        if (prevCol < unbox<size_t>(prevLine.size()) && !prevLine.isBlank())
        {
            auto const& prevStorage = prevLine.storage();
            auto const prevProxy = ConstCellProxy(prevStorage, prevCol);
            auto const cpCount = prevProxy.codepointCount();
            for (size_t i = 0; i < cpCount; ++i)
            {
                auto const cp = prevProxy.codepoint(i);
                if (!precedingChar)
                    unicode::grapheme_process_init(cp, graphemeState);
                else
                    unicode::grapheme_process_breakable(cp, graphemeState);
                precedingChar = cp;
            }
        }
    }

    if (!precedingChar)
    {
        unicode::grapheme_process_init(codepoint, graphemeState);
        writeCharToCurrentAndAdvance(codepoint);
    }
    else if (unicode::grapheme_process_breakable(codepoint, graphemeState))
    {
        writeCharToCurrentAndAdvance(codepoint);
    }
    else
    {
        auto const widthChange = usePreviousCell().appendCharacter(codepoint, clusterWidthPolicy());
        applyClusterWidthChange(widthChange);
        _terminal->markCellDirty(_lastCursorPosition);
    }

    _terminal->resetInstructionCounter();
}

void Screen::writeCharToCurrentAndAdvance(char32_t codepoint) noexcept
{
    // IRM: shift existing cells right to make room for the new character
    if (_terminal->isModeEnabled(AnsiMode::Insert))
    {
        auto const width = ColumnCount::cast_from(unicode::width(codepoint));
        insertChars(realCursorPosition().line, width);
    }

    Line& line = currentLine();

    auto cell = line.useCellAt(_cursor.position.column);

#if defined(LINE_AVOID_CELL_RESET)
    bool const consecutiveTextWrite = _terminal->instructionCounter() == 1;
    if (!consecutiveTextWrite)
        cell.reset();
#endif

    // A cell claimed by a scaled block on a line ABOVE belongs to that block; writing here destroys
    // it, exactly as writing into a horizontal continuation does.
    if (cell.isFlagEnabled(CellFlag::MulticellContinuation))
        eraseMulticellBlockAt(_cursor.position);

    if (cell.isFlagEnabled(CellFlag::WideCharContinuation) && _cursor.position.column > ColumnOffset(0))
    {
        // Writing into the middle of a multi-column cell destroys the whole cell, not just the
        // column being written -- half a glyph is not a thing that can be drawn.
        //
        // This used to step back exactly ONE column, which is correct only because a wide character
        // is exactly two columns. A text-sizing block (OSC 66) can be up to 49, and clearing its
        // second column while leaving the head and the rest behind leaves a corrupt block. Walk back
        // to the head and clear the entire run, as kitty's nuke_multicell_char_at() does.
        eraseMulticellBlockAt(_cursor.position);
    }

    auto const oldWidth = cell.width();

    cell.write(_cursor.graphicsRendition,
               codepoint,
               static_cast<uint8_t>(unicode::width(codepoint)),
               _cursor.hyperlink);

    _lastCursorPosition = _cursor.position;

    clearAndAdvance(oldWidth, cell.width());

    // TODO: maybe move selector API up? So we can make this call conditional,
    //       and only call it when something is selected?
    //       Alternatively we could add a boolean to make this callback
    //       conditional, something like: setReportDamage(bool);
    //       The latter is probably the easiest.
    _terminal->markCellDirty(_cursor.position);
}

ClusterWidthPolicy Screen::clusterWidthPolicy() const noexcept
{
    // DEC mode 2027 is how an application says it expects whole clusters to be measured. Contour
    // keeps the mode set by default and always clusters; what the mode gates is narrower -- whether
    // a codepoint arriving AFTER the first may change how many columns the cluster occupies.
    //
    // NOTE: resetting 2027 does not restore the full pre-clustering behaviour, only this part of it.
    // A terminal that also stopped segmenting would place every combining mark in its own cell, which
    // is a larger change and is not what applications resetting the mode are usually after.
    return _terminal->isModeEnabled(DECMode::Unicode) ? ClusterWidthPolicy::ClusterAware
                                                      : ClusterWidthPolicy::FirstCodepoint;
}

void Screen::applyClusterWidthChange(int delta) noexcept
{
    if (delta == 0)
        return;

    // The cluster head is where the last write landed, NOT the cursor -- the cursor has already moved
    // past it. Anchoring on _cursor.position here would claim or release the wrong columns.
    auto const head = _lastCursorPosition;

    // The head has to be addressable before its width can be read back, let alone put back.
    if (head.line < LineOffset(0) || head.line >= boxed_cast<LineOffset>(pageSize().lines)
        || head.column < ColumnOffset(0) || head.column >= boxed_cast<ColumnOffset>(pageSize().columns))
        return;

    auto& line = _grid.lineAt(head.line);
    auto headCell = line.useCellAt(head.column);
    auto const newWidth = static_cast<int>(headCell.width());
    auto const oldWidth = newWidth - delta;

    // appendCodepointToCluster has ALREADY committed the revised width to the head cell, so every
    // path that declines to carry the change through has to put the old width back. Leaving it would
    // let a cell claim a column that holds no continuation of it: the renderer draws the glyph across
    // its neighbour, and eraseMulticellBlockAt later wipes both.
    auto const abandon = [&]() noexcept {
        headCell.setWidth(static_cast<uint8_t>(oldWidth));
    };

    // A cluster already on screen may only ever GROW. terminal-unicode-core is explicit about the
    // asymmetry: VS16 "will force the grapheme cluster's width to be 2, which may possibly cause
    // reflowing", whereas VS15 "will NOT change the underlying width but only change the display to
    // prefer textual non-colored presentation".
    //
    // The reason is mechanical rather than stylistic. Giving a column back would mean undoing work
    // that is already committed -- un-wrapping a line that has wrapped, un-scrolling content that has
    // left the screen -- so a terminal that narrows an on-screen cluster ends up with behaviour no
    // application can predict.
    //
    // The measurement in appendCodepointToCluster still reports the narrowing honestly; the screen
    // simply declines to act on it. The variation selector stays part of the cluster, so the run
    // segmenter still resolves the run to text presentation and the glyph is drawn uncolored -- which
    // is the whole of what VS15 is specified to do.
    if (delta < 0)
    {
        abandon();
        return;
    }

    // Only revise a cluster the cursor is still sitting immediately after. Anything else -- an
    // intervening CUP, a scroll that moved the head, a resize, or a deferred wrap that has already
    // carried the cursor to the next line -- means this is no longer a live cluster, and the "next"
    // cell is not ours to take. In the normal sequential flow the cursor IS the next cell, so it is
    // free by construction.
    if (head.line != _cursor.position.line
        || _cursor.position.column != head.column + ColumnOffset::cast_from(oldWidth))
    {
        abandon();
        return;
    }

    // Growing must not run past the right margin (or the page edge). Rather than reflow a cell that
    // is already committed -- which would invalidate damage tracking, selections and hyperlink spans
    // -- abandon the promotion and leave the cluster at its original width.
    if (head.column + ColumnOffset::cast_from(newWidth - 1) > lastWritableColumn())
    {
        abandon();
        return;
    }

    // Insert mode sized its shift from the first codepoint alone, so it is short by exactly the
    // columns the cluster just gained.
    if (_terminal->isModeEnabled(AnsiMode::Insert))
        insertChars(head.line, ColumnCount::cast_from(delta));

    // The continuation inherits the HEAD cell's pen, not the current one: the SGR may have changed
    // between the base codepoint and the variation selector that widened it.
    auto const sgr = headCell.graphicsAttributes().with(CellFlag::WideCharContinuation);
    for (int i = oldWidth; i < newWidth; ++i)
    {
        line.useCellAt(head.column + ColumnOffset::cast_from(i)).reset(sgr, headCell.hyperlink());
        _terminal->markCellDirty(head + ColumnOffset::cast_from(i));
    }

    // Advancing past the last writable column is the deferred-wrap case, exactly as in
    // clearAndAdvance: the cursor stays where it is and the wrap happens when the next character
    // arrives. The grow guard above only proves the cluster ITSELF fits; a cluster ending flush
    // against the last column still leaves the cursor one past it. Moving there unconditionally
    // breaks the `column < pageSize().columns` invariant verifyState() asserts, and in a build
    // without it the next write indexes the line's storage out of bounds.
    auto const landing = _cursor.position.column + ColumnOffset::cast_from(delta);
    if (landing <= lastWritableColumn())
        _cursor.position.column = landing;
    else if (_terminal->isModeEnabled(DECMode::AutoWrap))
        _cursor.wrapPending = true;
}

ColumnOffset Screen::lastWritableColumn() const noexcept
{
    // Inside the left/right band the last writable column is the right margin itself; on the full
    // page it is the last page column. The band form once carried an extra `- 1`, which made autowrap
    // fire one column early -- the char destined for the right margin wrapped instead. Extracted so
    // that the arithmetic exists exactly once.
    bool const cursorInsideMargin =
        _terminal->isModeEnabled(DECMode::LeftRightMargin) && isCursorInsideMargins();
    return cursorInsideMargin ? margin().horizontal.to : boxed_cast<ColumnOffset>(pageSize().columns) - 1;
}

void Screen::clearAndAdvance(int oldWidth, int newWidth) noexcept
{
    // Columns strictly to the right of the cursor that are still writable.
    auto const cellsAvailable = *(lastWritableColumn() - _cursor.position.column);

    auto const sgr = newWidth > 1 ? _cursor.graphicsRendition.with(CellFlag::WideCharContinuation)
                                  : _cursor.graphicsRendition;
    auto& line = currentLine();
    for (int i = 1; i < std::min(std::max(oldWidth, newWidth), cellsAvailable); ++i)
        line.useCellAt(_cursor.position.column + i).reset(sgr, _cursor.hyperlink);

    if (newWidth == std::min(newWidth, cellsAvailable))
        _cursor.position.column += ColumnOffset::cast_from(newWidth);
    else if (_terminal->isModeEnabled(DECMode::AutoWrap))
        _cursor.wrapPending = true;
}

std::string Screen::screenshot(function<string(LineOffset)> const& postLine) const
{
    auto result = std::stringstream {};
    auto writer = VTWriter(result);

    for (int const line: iota(0, *pageSize().lines))
    {
        writer.write(_grid.lineAt(LineOffset(line)));
        if (postLine)
            writer.write(postLine(LineOffset(line)));
        writer.crlf();
    }

    return result.str();
}

optional<LineOffset> Screen::findMarkerUpwards(LineOffset startLine) const
{
    // XXX startLine is an absolute history line coordinate
    if (historyLineCount() == 0)
        return nullopt;
    if (*startLine <= -*historyLineCount())
        return nullopt;

    startLine = std::min(startLine, boxed_cast<LineOffset>(pageSize().lines - 1));

    for (LineOffset i = startLine - 1; i >= -boxed_cast<LineOffset>(historyLineCount()); --i)
        if (_grid.lineAt(i).marked())
            return { i };

    return nullopt;
}

optional<LineOffset> Screen::findMarkerDownwards(LineOffset startLine) const
{
    if (historyLineCount() == 0)
        return nullopt;

    auto const top = std::clamp(startLine,
                                -boxed_cast<LineOffset>(historyLineCount()),
                                +boxed_cast<LineOffset>(pageSize().lines) - 1);

    auto const bottom = LineOffset(0);

    for (LineOffset i = top + 1; i <= bottom; ++i)
        if (_grid.lineAt(i).marked())
            return { i };

    return nullopt;
}

// {{{ tabs related

void Screen::clearAllTabs()
{
    _terminal->tabs().clear();
}

void Screen::clearTabUnderCursor()
{
    // populate tabs vector in case of default tab width is used (until now).
    if (_terminal->tabs().empty() && *TabWidth != 0)
        for (auto column = boxed_cast<ColumnOffset>(TabWidth);
             column < boxed_cast<ColumnOffset>(pageSize().columns);
             column += boxed_cast<ColumnOffset>(TabWidth))
            _terminal->tabs().emplace_back(column - 1);

    // erase the specific tab underneath
    for (auto i = begin(_terminal->tabs()); i != end(_terminal->tabs()); ++i)
    {
        if (*i == realCursorPosition().column)
        {
            _terminal->tabs().erase(i);
            break;
        }
    }
}

void Screen::setTabUnderCursor()
{
    _terminal->tabs().emplace_back(realCursorPosition().column);
    sort(begin(_terminal->tabs()), end(_terminal->tabs()));
}
// }}}

// {{{ others

void Screen::moveCursorTo(LineOffset line, ColumnOffset column)
{
    auto const [originAppliedLine, originAppliedColumn] = [&]() {
        if (!_cursor.originMode)
            return pair { line, column };
        else
            return pair { line + margin().vertical.from, column + margin().horizontal.from };
    }();

    _cursor.wrapPending = false;
    _cursor.position =
        clampToScreen(CellLocation { .line = originAppliedLine, .column = originAppliedColumn });
    updateCursorIterator();
}

void Screen::linefeed(ColumnOffset newColumn)
{
    // Where the cursor *is* decides whether this scrolls -- asked before the carriage-return half of the
    // line feed moves it. Under LNM (and on the stdout fast pipe) newColumn is the left margin, which would
    // snap the cursor into the band and make the test below vacuously true. xterm keeps the same order:
    // xtermIndex() reads screen->cur_col, and CASE_VMOT applies CarriageReturn only afterwards.
    auto const scrollsOnIndex = isCursorInsideHorizontalMargins();

    _cursor.wrapPending = false;
    _cursor.position.column = newColumn;
    if (unbox(historyLineCount()) > 0)
        _terminal->addLineOffsetToJumpHistory(LineOffset { 1 });
    if (*realCursorPosition().line == *margin().vertical.to)
    {
        // A line feed only scrolls when the cursor is within the left/right margins. Outside that band
        // (DECLRMM on, cursor left of the left margin or right of the right margin) it neither scrolls
        // nor advances past the bottom margin -- xterm's `!ScrnIsColInMargins` guard in xtermIndex(),
        // whose CursorDown() then clamps to the bottom margin, leaving the cursor where it was.
        if (scrollsOnIndex)
        {
            // TODO(perf) if we know that we text is following this LF
            // (i.e. parser state will be ground state),
            // then invoke scrollUpUninitialized instead
            // and make sure the subsequent text write will
            // possibly also reset remaining grid cells in that line
            // if the incoming text did not write to the full line
            scrollUp(LineCount(1), _cursor.graphicsRendition, margin());
        }
    }
    else if (*realCursorPosition().line + 1 < *pageSize().lines)
    {
        // using moveCursorTo() would embrace code reusage,
        // but due to the fact that it's fully recalculating iterators,
        // it may be faster to just incrementally update them.
        // moveCursorTo({logicalCursorPosition().line + 1, margin().horizontal.from});
        _cursor.position.line++;
        updateCursorIterator();
    }
    // Otherwise the cursor is on the last line of the page but *outside* the scrolling region, so
    // there is neither anything to scroll nor anywhere to move: a line feed there does nothing.
    //
    // The page bound is what makes this safe. Only a cursor exactly on the bottom *margin* scrolls,
    // so a cursor below the region used to be incremented unconditionally — and every further line
    // feed walked it further off the end of the page. That is reachable from any application: set a
    // scrolling region, put the cursor below it, and hold down Return. verifyState() catches it in a
    // debug build; in a release build it is simply a cursor pointing outside the grid.
}

void Screen::scrollUp(LineCount n, GraphicsAttributes sgr, Margin margin)
{
    auto const scrollCount = _grid.scrollUp(n, sgr, margin);
    updateCursorIterator();

    // Only notify the viewport/Vi-cursor/selection of a scrollback scroll when the
    // scroll covered the whole page. A scroll confined to a sub-page margin region
    // (e.g. a top-anchored partial DECSTBM region) may still feed lines into
    // scrollback, but the live area below the region did not move, so shifting the
    // viewport, the Normal-mode cursor, or the active selection would be wrong.
    auto const isFullPageMargin =
        margin.horizontal.from.value == 0 && margin.horizontal.to.value + 1 == pageSize().columns.value
        && margin.vertical.from.value == 0 && margin.vertical.to.value + 1 == pageSize().lines.value;
    if (isFullPageMargin)
        _terminal->onBufferScrolled(scrollCount);
}

void Screen::scrollDown(LineCount n, Margin margin)
{
    _grid.scrollDown(n, cursor().graphicsRendition, margin);
    updateCursorIterator();
}

void Screen::unscroll(LineCount n)
{
    _grid.unscroll(n, cursor().graphicsRendition);
    updateCursorIterator();
}

void Screen::setCurrentColumn(ColumnOffset n)
{
    auto const col = _cursor.originMode ? margin().horizontal.from + n : n;
    setCurrentAbsoluteColumn(col);
}

void Screen::setCurrentAbsoluteColumn(ColumnOffset column)
{
    auto const clampedCol =
        std::clamp(column, ColumnOffset(0), boxed_cast<ColumnOffset>(pageSize().columns) - 1);
    _cursor.wrapPending = false;
    _cursor.position.column = clampedCol;
}

void Screen::restoreGraphicsRendition()
{
    _cursor.graphicsRendition = _savedGraphicsRenditions;
}

void Screen::saveGraphicsRendition()
{
    _savedGraphicsRenditions = _cursor.graphicsRendition;
}

string Screen::renderMainPageText() const
{
    return _grid.renderMainPageText();
}
// }}}

// {{{ ops

void Screen::linefeed()
{
    if (_terminal->isModeEnabled(DECMode::SmoothScroll)
        && _terminal->settings().smoothLineScrolling.count() != 0)
    {
        _terminal->unlock();
        auto const _ = crispy::finally([&]() { _terminal->lock(); });
        if (!_terminal->isModeEnabled(DECMode::BatchedRendering))
            _terminal->screenUpdated();
        sleep_for(_terminal->settings().smoothLineScrolling);
    }

    // If coming through stdout-fastpipe, the LF acts like CRLF.
    auto const newColumnOffset =
        _terminal->usingStdoutFastPipe() || _terminal->isModeEnabled(AnsiMode::AutomaticNewLine)
            ? margin().horizontal.from
            : _cursor.position.column;
    linefeed(newColumnOffset);
}

void Screen::backspace()
{
    // BS and CUB are the same movement, and xterm implements them with the same function.
    moveCursorBackward(ColumnCount(1));
}

void Screen::setScrollSpeed(int speed)
{
    if (speed >= 9)
    {
        // Speed value 9 defined by spec to be at maximum speed.
        _terminal->settings().smoothLineScrolling = {};
        return;
    }

    // NB: Match speeds as defined by old DEC VT1xx and VT2xx terminals.
    // See https://github.com/contour-terminal/contour/pull/1212/files#r1344674416
    std::array<float, 9> constexpr NumberOfLinesPerSecond = { {
        3,  // 0
        6,  // 1
        9,  // 2 | defined by spec to be 9 lines per second
        12, // 3
        18, // 4 | defined by spec to be 18 lines per second
        22, // 5
        27, // 6
        31, // 7
        36, // 8
    } };

    auto const index = std::clamp(speed, 0, 8);
    auto const delay = int(1000.0f / NumberOfLinesPerSecond[index]);

    _terminal->settings().smoothLineScrolling = std::chrono::milliseconds { delay };
}

void Screen::deviceStatusReport()
{
    reply("\033[0n");
}

void Screen::reportCursorPosition()
{
    reply("\033[{};{}R", logicalCursorPosition().line + 1, logicalCursorPosition().column + 1);
}

void Screen::reportMacroSpaceChecksum(unsigned requestId)
{
    // DECCKSR. There is no macro memory to checksum, so the checksum of it is zero -- but the reply
    // still carries back the id it was asked with, which is how an application with several requests in
    // flight tells the answers apart.
    reply("\033P{}!~0000\033\\", requestId);
}

void Screen::reportColorPaletteUpdate()
{
    auto constexpr DarkModeHint = 1;
    auto constexpr LightModeHint = 2;

    auto const modeHint =
        isLightColor(_terminal->colorPalette().defaultForeground) ? DarkModeHint : LightModeHint;

    reply("\033[?{};{}n", ColorPaletteUpdateDsrReplyId, modeHint);
    _terminal->flushInput();
}

void Screen::reportExtendedCursorPosition()
{
    auto const pageNum = _terminal->cursorPageNumber();
    reply("\033[?{};{};{}R", logicalCursorPosition().line + 1, logicalCursorPosition().column + 1, pageNum);
}

void Screen::reportCursorInformation()
{
    // DECCIR — Cursor Information Report
    // Response: DCS 1 $ u Pr;Pc;Pp;Srend;Satt;Sflag;Pgl;Pgr;Scss;Sdesig ST
    // See: https://vt100.net/docs/vt510-rm/DECCIR.html
    auto const& cursor = _cursor;
    auto const line = *cursor.position.line + 1;
    auto const column = *cursor.position.column + 1;
    auto const page = _terminal->cursorPageNumber();

    // Srend: visual attributes bitmask (0x40 base)
    // Bit 1=Bold, Bit 2=Underline, Bit 3=Blinking, Bit 4=Inverse
    auto const& flags = cursor.graphicsRendition.flags;
    auto srendBits = 0;
    if (flags.test(CellFlag::Bold))
        srendBits |= 1;
    if (flags.test(CellFlag::Underline))
        srendBits |= 2;
    if (flags.test(CellFlag::Blinking))
        srendBits |= 4;
    if (flags.test(CellFlag::Inverse))
        srendBits |= 8;
    auto const srend = static_cast<char>(0x40 + srendBits);

    // Satt: protection attribute (Bit 1 = DECSCA protection)
    auto const satt =
        flags.test(CellFlag::CharacterProtected) ? static_cast<char>(0x41) : static_cast<char>(0x40);

    // Sflag: Bit 1=DECOM, Bit 2=SS2, Bit 3=SS3, Bit 4=wrap pending
    auto const& charsets = cursor.charsets;
    auto sflagBits = 0;
    if (cursor.originMode)
        sflagBits |= 1;
    if (charsets.tableForNextGraphic() != charsets.selectedTable())
    {
        if (charsets.tableForNextGraphic() == CharsetTable::G2)
            sflagBits |= 2; // SS2 active
        else if (charsets.tableForNextGraphic() == CharsetTable::G3)
            sflagBits |= 4; // SS3 active
    }
    if (cursor.wrapPending)
        sflagBits |= 8;
    auto const sflag = static_cast<char>(0x40 + sflagBits);

    // Pgl: GL charset table index (0=G0, 1=G1, 2=G2, 3=G3)
    auto const pgl = static_cast<int>(charsets.selectedTable());

    // Pgr: GR charset table index (0=G0, 1=G1, 2=G2, 3=G3; defaults to G2 per the VT standard)
    auto const pgr = static_cast<int>(charsets.selectedTableGR());

    // Scss: per-G-set character set size (base 0x40; bit g is set when G(g) holds a 96-charset).
    auto scssBits = 0x40;
    for (auto const gSet: { CharsetTable::G0, CharsetTable::G1, CharsetTable::G2, CharsetTable::G3 })
        if (charsets.is96Charset(gSet))
            scssBits |= 1 << static_cast<int>(gSet);
    auto const scss = static_cast<char>(scssBits);

    // Sdesig: SCS designation final characters for G0 through G3
    auto const sdesig = std::string { charsets.designationOf(CharsetTable::G0),
                                      charsets.designationOf(CharsetTable::G1),
                                      charsets.designationOf(CharsetTable::G2),
                                      charsets.designationOf(CharsetTable::G3) };

    reply("\033P1$u{};{};{};{};{};{};{};{};{};{}\033\\",
          line,
          column,
          page,
          srend,
          satt,
          sflag,
          pgl,
          pgr,
          scss,
          sdesig);
}

void Screen::selectConformanceLevel(VTType level)
{
    _terminal->setOperatingLevel(level);
}

void Screen::sendDeviceAttributes()
{
    // See https://vt100.net/docs/vt510-rm/DA1.html

    auto const id = [&]() -> string_view {
        switch (_terminal->terminalId())
        {
            case VTType::VT100: return "1";
            case VTType::VT220:
            case VTType::VT240: return "62";
            case VTType::VT320:
            case VTType::VT330:
            case VTType::VT340: return "63";
            case VTType::VT420: return "64";
            case VTType::VT510:
            case VTType::VT520:
            case VTType::VT525: return "65";
        }
        return "1"; // Should never be reached.
    }();

    auto da = DeviceAttributes::AnsiColor | DeviceAttributes::AnsiTextLocator
              | DeviceAttributes::CaptureScreenBuffer | DeviceAttributes::Columns132
              | DeviceAttributes::HorizontalScrolling | DeviceAttributes::NationalReplacementCharacterSets
              | DeviceAttributes::RectangularEditing | DeviceAttributes::SelectiveErase
              | DeviceAttributes::RegisGraphics | DeviceAttributes::SixelGraphics
              | DeviceAttributes::SoftCharacterSet | DeviceAttributes::StatusDisplay
              | DeviceAttributes::TechnicalCharacters | DeviceAttributes::TextMacros
              | DeviceAttributes::UserDefinedKeys | DeviceAttributes::Windowing
              | DeviceAttributes::ClipboardExtension;
    if (_terminal->settings().goodImageProtocol)
        da = da | DeviceAttributes::GoodImageProtocol;

    // Filter out extensions that are required at the current operating level.
    // Required extensions are implied by the conformance level and should not be listed.
    da = filterRequiredExtensions(da, _terminal->operatingLevel());

    auto const attrs = to_params(da);

    reply("\033[?{};{}c", id, attrs);
}

void Screen::sendTerminalId()
{
    // Note, this is "Secondary DA".
    // It requests for the terminalID

    // terminal protocol type
    auto const pp = static_cast<unsigned>(_terminal->terminalId());

    // version number
    // TODO: (PACKAGE_VERSION_MAJOR * 100 + PACKAGE_VERSION_MINOR) * 100 + PACKAGE_VERSION_MICRO
    auto constexpr Pv =
        (((LIBTERMINAL_VERSION_MAJOR * 100) + LIBTERMINAL_VERSION_MINOR) * 100) + LIBTERMINAL_VERSION_PATCH;

    // ROM cardridge registration number (always 0)
    auto constexpr Pc = 0;

    reply("\033[>{};{};{}c", pp, Pv, Pc);
}

// {{{ ED

void Screen::clearToEndOfScreen()
{
    // Under ISO protection a regular ED must spare the ISO-guarded cells -- which is the selective
    // erase sparing CharacterProtectedISO, so delegate rather than duplicate the per-cell skip logic.
    if (eraseSkipsProtectedCells())
    {
        selectiveEraseToEndOfScreen(CellFlag::CharacterProtectedISO);
        return;
    }

    clearToEndOfLine();

    for (auto const lineOffset: iota(unbox(_cursor.position.line) + 1, unbox(pageSize().lines)))
    {
        Line& line = _grid.lineAt(LineOffset::cast_from(lineOffset));
        line.reset(_grid.defaultLineFlags(), _cursor.graphicsRendition);
    }
}

void Screen::clearToBeginOfScreen()
{
    if (eraseSkipsProtectedCells())
    {
        selectiveEraseToBeginOfScreen(CellFlag::CharacterProtectedISO);
        return;
    }

    clearToBeginOfLine();

    for (auto const lineOffset: iota(0, *_cursor.position.line))
    {
        Line& line = _grid.lineAt(LineOffset::cast_from(lineOffset));
        line.reset(_grid.defaultLineFlags(), _cursor.graphicsRendition);
    }
}

void Screen::clearScreen()
{
    // Under ISO protection the guarded cells must stay put, so we cannot scroll the page into
    // history; erase the non-ISO-guarded cells in place instead (matching xterm's ED 2 under protection).
    if (eraseSkipsProtectedCells())
    {
        selectiveEraseScreen(CellFlag::CharacterProtectedISO);
        return;
    }

    // Instead of *just* clearing the screen, and thus, losing potential important content,
    // we scroll up by RowCount number of lines, so move it all into history, so the user can scroll
    // up in case the content is still needed.
    //
    // ED 2 erases the WHOLE screen regardless of any DECSTBM/DECSLRM scrolling region (the region is
    // ignored -- xterm), so scroll the full page into history, not just the current margin band.
    scrollUp(_grid.pageSize().lines, Terminal::makeDefaultMargin(_grid.pageSize()));
}
// }}}

void Screen::eraseCharacters(ColumnCount n)
{
    // Spec: https://vt100.net/docs/vt510-rm/ECH.html
    // It's not clear from the spec how to perform erase when inside margin and number of chars to be erased
    // would go outside margins.
    // TODO: See what xterm does ;-)

    // erase characters from current column to the right
    auto const columnsAvailable = pageSize().columns - boxed_cast<ColumnCount>(realCursorPosition().column);
    auto const clampedN = unbox<long>(clamp(n, ColumnCount(1), columnsAvailable));

    // Under ISO protection the ISO-guarded cells within the run must survive, so route through the
    // selective erase sparing CharacterProtectedISO, exactly as ED/EL do.
    if (eraseSkipsProtectedCells())
    {
        auto const endColumn = ColumnOffset::cast_from(_cursor.position.column.value + clampedN);
        selectiveErase(
            _cursor.position.line, _cursor.position.column, endColumn, CellFlag::CharacterProtectedISO);
        return;
    }

    auto& line = currentLine();
    for (int i = 0; i < clampedN; ++i)
        line.useCellAt(_cursor.position.column + i).reset(_cursor.graphicsRendition);
}

// {{{ DECSEL

void Screen::selectiveEraseToEndOfLine(CellFlag protectedFlag)
{
    if (isFullHorizontalMargins() && _cursor.position.column.value == 0)
        selectiveEraseLine(_cursor.position.line, protectedFlag);
    else
        selectiveErase(_cursor.position.line,
                       _cursor.position.column,
                       ColumnOffset::cast_from(pageSize().columns),
                       protectedFlag);
}

void Screen::selectiveEraseToBeginOfLine(CellFlag protectedFlag)
{
    if (isFullHorizontalMargins() && _cursor.position.column.value == pageSize().columns.value)
        selectiveEraseLine(_cursor.position.line, protectedFlag);
    else
        selectiveErase(_cursor.position.line, ColumnOffset(0), _cursor.position.column + 1, protectedFlag);
}

void Screen::selectiveEraseLine(LineOffset line, CellFlag protectedFlag)
{
    if (containsProtectedCharacters(
            line, ColumnOffset(0), ColumnOffset::cast_from(pageSize().columns), protectedFlag))
    {
        selectiveErase(line, ColumnOffset(0), ColumnOffset::cast_from(pageSize().columns), protectedFlag);
        return;
    }

    auto& targetLine = _grid.lineAt(line);
    targetLine.reset(targetLine.flags(), _cursor.graphicsRendition);

    auto const left = ColumnOffset(0);
    auto const right = boxed_cast<ColumnOffset>(pageSize().columns - 1);
    auto const area = Rect { .top = unbox<Top>(line),
                             .left = unbox<Left>(left),
                             .bottom = unbox<Bottom>(line),
                             .right = unbox<Right>(right) };
    _terminal->markRegionDirty(area);
}

void Screen::selectiveErase(LineOffset line, ColumnOffset begin, ColumnOffset end, CellFlag protectedFlag)
{
    for (auto col = begin; col < end; ++col)
    {
        auto cell = at(line, col);
        if (!cell.isFlagEnabled(protectedFlag))
            cell.reset(_cursor.graphicsRendition);
    }

    auto const left = begin;
    auto const right = end - 1;
    auto const area = Rect { .top = unbox<Top>(line),
                             .left = unbox<Left>(left),
                             .bottom = unbox<Bottom>(line),
                             .right = unbox<Right>(right) };
    _terminal->markRegionDirty(area);
}

bool Screen::containsProtectedCharacters(LineOffset line,
                                         ColumnOffset begin,
                                         ColumnOffset end,
                                         CellFlag protectedFlag) const
{
    for (auto col = begin; col < end; ++col)
    {
        auto cell = at(line, col);
        if (cell.isFlagEnabled(protectedFlag))
            return true;
    }
    return false;
}
// }}}
// {{{ DECSED

void Screen::selectiveEraseToEndOfScreen(CellFlag protectedFlag)
{
    selectiveEraseToEndOfLine(protectedFlag);

    auto const lineStart = unbox(_cursor.position.line) + 1;
    auto const lineEnd = unbox(pageSize().lines);

    for (auto const lineOffset: iota(lineStart, lineEnd))
        selectiveEraseLine(LineOffset::cast_from(lineOffset), protectedFlag);
}

void Screen::selectiveEraseToBeginOfScreen(CellFlag protectedFlag)
{
    selectiveEraseToBeginOfLine(protectedFlag);

    for (auto const lineOffset: iota(0, *_cursor.position.line))
        selectiveEraseLine(LineOffset::cast_from(lineOffset), protectedFlag);
}

void Screen::selectiveEraseScreen(CellFlag protectedFlag)
{
    for (auto const lineOffset: iota(0, *pageSize().lines))
        selectiveEraseLine(LineOffset::cast_from(lineOffset), protectedFlag);
}
// }}}
// {{{ DECSERA

/// Erases the cells of @p area that are not guarded by @p protectedFlag.
///
/// @param area The area to erase, as zero-based offsets already resolved against origin mode and
///             clamped to the page. @see impl::readRectangularArea().
/// @param protectedFlag The protection flag that spares a cell (DEC by default; ISO for regular erases).
void Screen::selectiveEraseArea(Rect area, CellFlag protectedFlag)
{
    auto const [top, left, bottom, right] = area;
    Require(unbox(right) < unbox(pageSize().columns));
    Require(unbox(bottom) < unbox(pageSize().lines));

    if (top.value > bottom.value || left.value > right.value)
        return;

    for (int y = top.value; y <= bottom.value; ++y)
    {
        for (int x = left.value; x <= right.value; ++x)
        {
            auto cell = grid().lineAt(LineOffset::cast_from(y)).useCellAt(ColumnOffset::cast_from(x));
            if (!cell.isFlagEnabled(protectedFlag))
            {
                cell.writeTextOnly(L' ', 1);
                cell.setHyperlink(HyperlinkId(0));
            }
        }
    }
}
// }}}

// {{{ EL

void Screen::clearToEndOfLine()
{
    if (eraseSkipsProtectedCells())
    {
        selectiveEraseToEndOfLine(CellFlag::CharacterProtectedISO);
        return;
    }

    if (isFullHorizontalMargins() && _cursor.position.column.value == 0)
    {
        currentLine().reset(currentLine().flags(), _cursor.graphicsRendition);
        return;
    }

    auto& current = currentLine();
    // Skip the partial clear if the line is already blank with matching fill attrs:
    // every cell already appears to have the cursor's SGR, so the clear is a no-op
    // and materialization would only allocate without changing state.
    if (current.isBlankWithFillAttrs(_cursor.graphicsRendition))
        return;
    auto& storage = current.materializedStorage();
    auto const from = unbox<size_t>(_cursor.position.column);
    auto const count = unbox<size_t>(pageSize().columns) - from;
    clearRange(storage, from, count, _cursor.graphicsRendition);

    auto const line = _cursor.position.line;
    auto const left = _cursor.position.column;
    auto const right = boxed_cast<ColumnOffset>(pageSize().columns - 1);
    auto const area =
        Rect { .top = Top(*line), .left = Left(*left), .bottom = Bottom(*line), .right = Right(*right) };
    _terminal->markRegionDirty(area);
}

void Screen::clearToBeginOfLine()
{
    if (eraseSkipsProtectedCells())
    {
        selectiveEraseToBeginOfLine(CellFlag::CharacterProtectedISO);
        return;
    }

    auto& currentLineRef = _grid.lineAt(_cursor.position.line);
    if (currentLineRef.isBlankWithFillAttrs(_cursor.graphicsRendition))
        return;
    auto& storage = currentLineRef.materializedStorage();
    auto const count = unbox<size_t>(_cursor.position.column) + 1;
    clearRange(storage, 0, count, _cursor.graphicsRendition);

    auto const line = _cursor.position.line;
    auto const left = ColumnOffset(0);
    auto const right = _cursor.position.column;
    auto const area =
        Rect { .top = Top(*line), .left = Left(*left), .bottom = Bottom(*line), .right = Right(*right) };
    _terminal->markRegionDirty(area);
}

void Screen::clearLine()
{
    if (eraseSkipsProtectedCells())
    {
        selectiveEraseLine(_cursor.position.line, CellFlag::CharacterProtectedISO);
        return;
    }

    currentLine().reset(currentLine().flags(), _cursor.graphicsRendition);

    auto const line = _cursor.position.line;
    auto const left = ColumnOffset(0);
    auto const right = boxed_cast<ColumnOffset>(pageSize().columns - 1);
    auto const area =
        Rect { .top = Top(*line), .left = Left(*left), .bottom = Bottom(*line), .right = Right(*right) };
    _terminal->markRegionDirty(area);
}
// }}}

void Screen::moveCursorToNextLine(LineCount n)
{
    // CNL is cursor-down followed by a carriage return (xterm's CursorNextLine). moveCursorDown() clamps
    // at the bottom margin (or the page edge when the cursor starts below the region), and the carriage
    // return snaps to the left margin -- so CNL never scrolls and never leaves the scroll region.
    moveCursorDown(n);
    moveCursorToBeginOfLine();
}

void Screen::moveCursorToPrevLine(LineCount n)
{
    // CPL is the mirror of CNL: cursor-up clamped at the top margin, then a carriage return.
    moveCursorUp(n);
    moveCursorToBeginOfLine();
}

void Screen::insertCharacters(ColumnCount n)
{
    if (isCursorInsideMargins())
        insertChars(realCursorPosition().line, n);
}

/// Inserts @p n characters at given line @p lineNo.

void Screen::insertChars(LineOffset lineOffset, ColumnCount columnsToInsert)
{
    auto const sanitizedN =
        std::min(*columnsToInsert, *margin().horizontal.to - *logicalCursorPosition().column + 1);

    auto& line = _grid.lineAt(lineOffset);
    // Insert into a blank line with matching fill attrs is a no-op: shifting "all default cells"
    // by N still leaves all default cells, and the cleared range was already default.
    if (line.isBlankWithFillAttrs(_cursor.graphicsRendition))
        return;
    auto& storage = line.materializedStorage();
    auto const cursorCol = static_cast<size_t>(*realCursorPosition().column);
    auto const marginEnd = static_cast<size_t>(*margin().horizontal.to + 1);
    auto const moveCount = marginEnd - cursorCol - static_cast<size_t>(sanitizedN);

    if (moveCount > 0)
        moveColumns(storage, cursorCol, cursorCol + static_cast<size_t>(sanitizedN), moveCount);

    clearRange(storage, cursorCol, static_cast<size_t>(sanitizedN), _cursor.graphicsRendition);
}

void Screen::insertLines(LineCount n)
{
    if (isCursorInsideMargins())
    {
        scrollDown(n,
                   Margin { .vertical = Margin::Vertical { .from = _cursor.position.line,
                                                           .to = margin().vertical.to },
                            .horizontal = margin().horizontal });
        updateCursorIterator();
    }
}

void Screen::insertColumns(ColumnCount n)
{
    if (isCursorInsideMargins())
        for (auto lineNo = margin().vertical.from; lineNo <= margin().vertical.to; ++lineNo)
            insertChars(lineNo, n);
}

void Screen::scrollLeft(ColumnCount n)
{
    // SL -- Scroll Left (ECMA-48)
    // For each line in the scroll margin, shift cells left by N within the horizontal margins,
    // filling vacated cells on the right with blanks.
    for (auto lineNo = margin().vertical.from; lineNo <= margin().vertical.to; ++lineNo)
    {
        auto& line = _grid.lineAt(lineNo);
        if (line.isBlankWithFillAttrs(_cursor.graphicsRendition))
            continue;
        auto& storage = line.materializedStorage();
        auto const leftMargin = static_cast<size_t>(*margin().horizontal.from);
        auto const rightMargin = static_cast<size_t>(*margin().horizontal.to);
        auto const width = rightMargin - leftMargin + 1;
        auto const count = std::min(static_cast<size_t>(*n), width);

        if (count > 0 && count < width)
            moveColumns(storage, leftMargin + count, leftMargin, width - count);

        clearRange(storage, leftMargin + width - count, count, _cursor.graphicsRendition);
    }
}

void Screen::scrollRight(ColumnCount n)
{
    // SR -- Scroll Right (ECMA-48)
    // For each line in the scroll margin, shift cells right by N within the horizontal margins,
    // filling vacated cells on the left with blanks.
    for (auto lineNo = margin().vertical.from; lineNo <= margin().vertical.to; ++lineNo)
    {
        auto& line = _grid.lineAt(lineNo);
        if (line.isBlankWithFillAttrs(_cursor.graphicsRendition))
            continue;
        auto& storage = line.materializedStorage();
        auto const leftMargin = static_cast<size_t>(*margin().horizontal.from);
        auto const rightMargin = static_cast<size_t>(*margin().horizontal.to);
        auto const width = rightMargin - leftMargin + 1;
        auto const count = std::min(static_cast<size_t>(*n), width);

        if (count > 0 && count < width)
            moveColumns(storage, leftMargin, leftMargin + count, width - count);

        clearRange(storage, leftMargin, count, _cursor.graphicsRendition);
    }
}

void Screen::copyArea(Rect sourceArea, int page, CellLocation targetTopLeft, int targetPage)
{
    if (*sourceArea.bottom < *sourceArea.top || *sourceArea.right < *sourceArea.left)
        return;

    // Resolve page indices: 0 means "current page", otherwise 1-based page number.
    auto const sourcePageIndex = PageIndex(page == 0 ? _terminal->cursorPageIndex().value : page - 1);
    auto const targetPageIndex =
        PageIndex(targetPage == 0 ? _terminal->cursorPageIndex().value : targetPage - 1);

    // Clamp to valid range.
    if (sourcePageIndex.value < 0 || sourcePageIndex.value >= MaxPageCount)
        return;
    if (targetPageIndex.value < 0 || targetPageIndex.value >= MaxPageCount)
        return;

    auto const samePage = (sourcePageIndex == targetPageIndex);
    if (samePage && *sourceArea.top == *targetTopLeft.line && *sourceArea.left == *targetTopLeft.column)
        return; // Copy to its own location on same page => no-op.

    auto& srcGrid = _terminal->pageAt(sourcePageIndex).grid();
    auto& dstGrid = _terminal->pageAt(targetPageIndex).grid();

    // The copy is truncated at the target page's edge: an area that would not fit copies only the part
    // that does. Copying every cell the source names would run the write past the end of a line.
    auto const targetPageSize = dstGrid.pageSize();
    auto const height =
        std::min(*sourceArea.bottom - *sourceArea.top + 1, unbox(targetPageSize.lines) - *targetTopLeft.line);
    auto const width = std::min(*sourceArea.right - *sourceArea.left + 1,
                                unbox(targetPageSize.columns) - *targetTopLeft.column);
    if (height <= 0 || width <= 0)
        return;

    auto const [x0, xInc, xEnd] = [&]() {
        if (samePage && *targetTopLeft.column > *sourceArea.left) // moving right on same page
            return std::tuple { width - 1, -1, -1 };
        else
            return std::tuple { 0, +1, width };
    }();

    auto const [y0, yInc, yEnd] = [&]() {
        if (samePage && *targetTopLeft.line > *sourceArea.top) // moving down on same page
            return std::tuple { height - 1, -1, -1 };
        else
            return std::tuple { 0, +1, height };
    }();

    for (auto y = y0; y != yEnd; y += yInc)
    {
        for (auto x = x0; x != xEnd; x += xInc)
        {
            auto const srcLine = LineOffset::cast_from(*sourceArea.top + y);
            auto const srcCol = ColumnOffset::cast_from(sourceArea.left + x);
            auto const dstLine = LineOffset::cast_from(targetTopLeft.line + y);
            auto const dstCol = ColumnOffset::cast_from(targetTopLeft.column + x);

            auto sourceCell = srcGrid.at(srcLine, srcCol);
            auto targetCell = dstGrid.at(dstLine, dstCol);
            auto const attrs = extractAttributes(sourceCell);
            if (sourceCell.codepointCount() > 0)
            {
                targetCell.write(attrs, sourceCell.codepoint(0), sourceCell.width(), sourceCell.hyperlink());
                for (size_t ci = 1; ci < sourceCell.codepointCount(); ++ci)
                    // A copy reproduces the source cell; it does not re-measure it. The source's
                    // width is whatever the policy in force when it was WRITTEN decided, so
                    // re-measuring here would rewrite it under a policy the application may have
                    // since reset -- claiming a neighbouring column that holds live text, with no
                    // continuation cell to mark it. FirstCodepoint keeps the codepoints and leaves
                    // the width alone.
                    (void) targetCell.appendCharacter(sourceCell.codepoint(ci),
                                                      ClusterWidthPolicy::FirstCodepoint);
            }
            else
            {
                targetCell.reset(attrs, sourceCell.hyperlink());
            }

            // Both write() and reset() clear the sizing, but `attrs` still carries
            // MulticellContinuation to the rows a scaled block reaches into. Copying the flags
            // without the scale leaves those rows orphaned: RenderBufferBuilder sees a block of
            // height 1 whose origin is above them and redraws the head's text on each, and
            // eraseMulticellBlockAt -- also reading the scale -- never reaches them to clean up.
            targetCell.setTextScale(sourceCell.textScale());
        }
    }
}

void Screen::nextPage(int count)
{
    // NP: Move forward by count pages, cursor goes to home position.
    // Clamp within DEC page range [0, MaxPageCount-2], never entering the alternate screen page.
    auto const target =
        PageIndex(std::clamp(_terminal->cursorPageIndex().value + count, 0, MaxPageCount - 2));
    _terminal->setPage(target, true);
}

void Screen::previousPage(int count)
{
    // PP: Move backward by count pages, cursor goes to home position.
    auto const target =
        PageIndex(std::clamp(_terminal->cursorPageIndex().value - count, 0, MaxPageCount - 2));
    _terminal->setPage(target, true);
}

void Screen::pagePositionAbsolute(int pageNumber)
{
    // PPA: Move to absolute 1-based page number, preserve cursor position.
    auto const target = PageIndex(std::clamp(pageNumber - 1, 0, MaxPageCount - 2));
    _terminal->setPage(target, false);
}

void Screen::pagePositionRelative(int count)
{
    // PPR: Move forward by count pages, preserve cursor position.
    auto const target =
        PageIndex(std::clamp(_terminal->cursorPageIndex().value + count, 0, MaxPageCount - 2));
    _terminal->setPage(target, false);
}

void Screen::pagePositionBackward(int count)
{
    // PPB: Move backward by count pages, preserve cursor position.
    auto const target =
        PageIndex(std::clamp(_terminal->cursorPageIndex().value - count, 0, MaxPageCount - 2));
    _terminal->setPage(target, false);
}

void Screen::requestDisplayedExtent()
{
    // DECRQDE: Reply with CSI Phe ; Pwe ; 1 ; 1 ; Pp " w
    // Phe = page height, Pwe = page width, Pp = displayed page number (1-based).
    auto const ps = pageSize();
    reply("\033[{};{};1;1;{}\"w", *ps.lines, *ps.columns, _terminal->displayedPageNumber());
}

void Screen::requestUserPreferredSupplementalSet()
{
    // DECRQUPSS: reply with a DECAUPSS-shaped DCS Ps ! u D...D ST.
    //
    // Ps is re-derived from the set's own size rather than echoed from whatever was last assigned --
    // it describes the set, so it cannot be anything else. Matches xterm, which encodes the reply's
    // Ps from the charset it is reporting.
    //
    // Written 7-bit; Terminal::reply folds the C1 controls to 8-bit under S8C1T.
    auto const& upss = _terminal->userPreferredSupplementalSet();
    auto designator = std::string {};
    if (upss.intermediate != '\0')
        designator += upss.intermediate;
    designator += upss.final;

    reply("\033P{}!u{}\033\\", upss.is96 ? '1' : '0', designator);
}

void Screen::eraseArea(int top, int left, int bottom, int right)
{
    assert(right <= unbox(pageSize().columns));
    assert(bottom <= unbox(pageSize().lines));

    if (top > bottom || left > right)
        return;

    for (int y = top; y <= bottom; ++y)
    {
        for (int x = left; x <= right; ++x)
        {
            auto cell = grid().lineAt(LineOffset::cast_from(y)).useCellAt(ColumnOffset::cast_from(x));
            cell.write(_cursor.graphicsRendition, L' ', 1);
        }
    }
}

void Screen::fillArea(char32_t ch, int top, int left, int bottom, int right)
{
    // "Pch can be any value from 32 to 126 or from 160 to 255."
    if (!(32 <= ch && ch <= 126) && !(160 <= ch && ch <= 255))
        return;

    auto const w = static_cast<uint8_t>(unicode::width(ch));
    for (int y = top; y <= bottom; ++y)
    {
        for (int x = left; x <= right; ++x)
        {
            auto cell = grid().lineAt(LineOffset::cast_from(y)).useCellAt(ColumnOffset::cast_from(x));
            cell.write(cursor().graphicsRendition, ch, w);
        }
    }
}

void Screen::deleteLines(LineCount n)
{
    if (isCursorInsideMargins())
    {
        scrollUp(n,
                 Margin { .vertical =
                              Margin::Vertical { .from = _cursor.position.line, .to = margin().vertical.to },
                          .horizontal = margin().horizontal });
    }
}

void Screen::deleteCharacters(ColumnCount n)
{
    // DCH works outside the top/bottom scrolling margin (xterm patch 316); it is confined only by the
    // left/right margins (DECLRMM). So gate on the horizontal margins, not isCursorInsideMargins()
    // (which would also require the cursor to be within the vertical margin).
    if (isCursorInsideHorizontalMargins() && *n != 0)
        deleteChars(realCursorPosition().line, realCursorPosition().column, n);
}

void Screen::deleteChars(LineOffset lineOffset, ColumnOffset column, ColumnCount columnsToDelete)
{
    auto& line = _grid.lineAt(lineOffset);

    // Deleting from a blank line with matching fill attrs is a no-op: shifting "all default cells" left
    // by N still leaves all default cells, and the cleared range was already default. This is not only
    // an optimization -- DECDC deletes a column from *every* line within the vertical margin, so
    // without it a single `CSI ' ~` on a fresh page would materialize every blank line on it.
    if (line.isBlankWithFillAttrs(_cursor.graphicsRendition))
        return;

    // A blank line's SoA arrays are empty; writing through them without materializing first reads and
    // writes past the end of every one of them. insertChars() has always done this; its sibling never
    // did.
    auto& storage = line.materializedStorage();
    auto const leftCol = column.as<size_t>();
    auto const rightCol = static_cast<size_t>(*margin().horizontal.to + 1);
    auto const n =
        static_cast<size_t>(std::min(columnsToDelete.as<long>(), static_cast<long>(rightCol - leftCol)));

    if (n > 0 && leftCol + n < rightCol)
        moveColumns(storage, leftCol + n, leftCol, rightCol - leftCol - n);

    clearRange(storage, rightCol - n, n, _cursor.graphicsRendition);
}

void Screen::deleteColumns(ColumnCount n)
{
    if (isCursorInsideMargins())
        for (auto lineNo = margin().vertical.from; lineNo <= margin().vertical.to; ++lineNo)
            deleteChars(lineNo, realCursorPosition().column, n);
}

void Screen::horizontalTabClear(HorizontalTabClear which)
{
    switch (which)
    {
        case HorizontalTabClear::AllTabs: clearAllTabs(); break;
        case HorizontalTabClear::UnderCursor: clearTabUnderCursor(); break;
    }
}

void Screen::horizontalTabSet()
{
    setTabUnderCursor();
}

void Screen::setCurrentWorkingDirectory(string const& url)
{
    _terminal->setCurrentWorkingDirectory(url);
}

void Screen::hyperlink(string id, string uri)
{
    if (uri.empty())
        _cursor.hyperlink = {};
    else
    {
        auto cacheId { std::move(id) };
        if (!cacheId.empty())
        {
            cacheId += uri;
            _cursor.hyperlink = _terminal->hyperlinks().hyperlinkIdByUserId(cacheId);
            if (_cursor.hyperlink != HyperlinkId {})
                return;
        }
        // We ignore the user id since we need to ensure it's unique. We generate our own.
        _cursor.hyperlink = _terminal->hyperlinks().nextHyperlinkId++;
        _terminal->hyperlinks().cache.emplace(_cursor.hyperlink,
                                              make_shared<HyperlinkInfo>(HyperlinkInfo {
                                                  .userId = std::move(cacheId), .uri = std::move(uri) }));
    }
    // TODO:
    // Care about eviction.
    // Move hyperlink store into ScreenBuffer, so it gets reset upon every switch into
    // alternate screen (not for main screen!)
}

std::shared_ptr<HyperlinkInfo const> Screen::hyperlinkAt(CellLocation pos) const noexcept
{
    return _terminal->hyperlinks().hyperlinkById(hyperlinkIdAt(pos));
}

void Screen::moveCursorUp(LineCount n)
{
    _cursor.wrapPending = false;
    _cursor.position.line = margin().vertical.contains(_cursor.position.line)
                                ? margin().vertical.clamp(_cursor.position.line - n.as<LineOffset>())
                                : clampedLine(_cursor.position.line - n.as<LineOffset>());
    updateCursorIterator();
}

void Screen::moveCursorDown(LineCount n)
{
    _cursor.wrapPending = false;
    _cursor.position.line = margin().vertical.contains(_cursor.position.line)
                                ? margin().vertical.clamp(_cursor.position.line + n.as<LineOffset>())
                                : clampedLine(_cursor.position.line + n.as<LineOffset>());
    updateCursorIterator();
}

void Screen::moveCursorForward(ColumnCount n)
{
    if (margin().horizontal.contains(_cursor.position.column))
        _cursor.position.column = margin().horizontal.clamp(_cursor.position.column + n.as<ColumnOffset>());
    else
        _cursor.position.column = clampedColumn(_cursor.position.column + boxed_cast<ColumnOffset>(n));
    _cursor.wrapPending = false;
}

void Screen::moveCursorBackward(ColumnCount n)
{
    // A port of xterm's CursorBack(), which is what both BS and CUB do.
    //
    // Three things make this more than a subtraction:
    //
    //   - The cursor stops at the *left margin*, not at the screen's edge -- unless it already sits
    //     left of the margin, in which case the margin is not holding it and the edge is its bound.
    //   - Reverse wraparound (DEC mode 45, and 1045 for the unlimited variant) carries the cursor to
    //     the right margin of the line above. Both are inert without DECAWM: a terminal that does not
    //     wrap forward has no wrap to reverse. The plain one only follows a line the text actually
    //     wrapped onto; the extended one follows any line, and comes round at the bottom of the
    //     scrolling region when it walks off the top.
    //   - A cursor in the wrap-pending position is not *past* the right margin -- it still sits on the
    //     last column -- so the first step merely clears that flag rather than moving anywhere.
    auto const autoWrap = _terminal->isModeEnabled(DECMode::AutoWrap);
    auto const reverseWrap = autoWrap && _terminal->isModeEnabled(DECMode::ReverseWraparound);
    auto const reverseWrapExtended = autoWrap && _terminal->isModeEnabled(DECMode::ReverseWraparoundExtended);

    auto const top = margin().vertical.from;
    auto const bottom = margin().vertical.to;
    auto const right = margin().horizontal.to;
    auto const left =
        _cursor.position.column < margin().horizontal.from ? ColumnOffset(0) : margin().horizontal.from;

    auto line = _cursor.position.line;
    auto column = _cursor.position.column;
    auto count = unbox(n);

    // A count of zero moves nowhere -- it only clears the wrap-pending flag.
    if (count > 0)
    {
        if ((reverseWrap || reverseWrapExtended) && _cursor.wrapPending)
            --count;
        else
            --column;
    }

    while (count > 0)
    {
        if (column < left)
        {
            if (reverseWrapExtended)
            {
                // Off the top of the scrolling region, the unlimited form comes round at the bottom.
                if (line == top)
                    line = bottom + LineOffset(1);
            }
            else if (!reverseWrap)
            {
                column = left;
                break;
            }
            else if (!_grid.lineAt(line).wrapped())
            {
                // The plain form only reverses a wrap that actually happened, and the flag for it lives
                // on the *continuation* line -- the one the cursor is leaving. (xterm marks the line
                // that wrapped; Contour marks the line that continues it. Same fact, one line apart.)
                column = left;
                break;
            }

            // There is no line above the first one to wrap onto.
            if (line == LineOffset(0))
            {
                column = left;
                break;
            }

            --line;
            column = right;
        }

        if (--count <= 0)
            break;

        --column;
    }

    _cursor.position.line = clampedLine(line);
    _cursor.position.column = clampedColumn(column);
    _cursor.wrapPending = false;
    updateCursorIterator();
}

void Screen::moveCursorToColumn(ColumnOffset column)
{
    setCurrentColumn(column);
}

void Screen::moveCursorToBeginOfLine()
{
    // A port of xterm's CarriageReturn(). The cursor snaps to the left margin, which is the left edge
    // of the page unless DECLRMM narrowed it. The one exception is a cursor already left of the margin:
    // that is only reachable outside origin mode (absolute addressing may place it there), and since the
    // margin is not holding it, it falls to the screen's left edge instead.
    auto const left = margin().horizontal.from;
    auto const target = (_cursor.originMode || _cursor.position.column >= left) ? left : ColumnOffset(0);
    _cursor.wrapPending = false;
    _cursor.position.column = target;
}

void Screen::moveCursorToLine(LineOffset n)
{
    moveCursorTo(n, _cursor.position.column);
}

void Screen::moveCursorToNextTab()
{
    // TODO: I guess something must remember when a \t was added, for proper move-back?
    // TODO: respect HTS/TBC

    // DECSET 41 (MoreFix, xterm's curses hack): when the line has just been filled to the right margin
    // (a wrap is pending) and a TAB arrives, honour the pending wrap first -- moving to the next line --
    // and then tab from its left margin. Without it (the default), the pending wrap waits for the next
    // printable character and the TAB is swallowed at the right margin.
    // @see esctest DECSETTests.test_DECSET_MoreFix.
    if (_terminal->isModeEnabled(DECMode::MoreFix))
        crlfIfWrapPending();

    static_assert(TabWidth > ColumnCount(0));

    auto columnsToAdvance = ColumnCount(0);

    if (!_terminal->tabs().empty())
    {
        // advance to the next tab
        size_t i = 0;
        while (i < _terminal->tabs().size() && realCursorPosition().column >= _terminal->tabs()[i])
            ++i;

        auto const currentCursorColumn = logicalCursorPosition().column;

        if (i < _terminal->tabs().size())
            columnsToAdvance = boxed_cast<ColumnCount>(_terminal->tabs()[i] - currentCursorColumn);
        else if (realCursorPosition().column < margin().horizontal.to)
            columnsToAdvance = boxed_cast<ColumnCount>(margin().horizontal.to - currentCursorColumn);
    }
    else
    {
        // default tab settings
        if (realCursorPosition().column < margin().horizontal.to)
        {
            columnsToAdvance =
                std::min((TabWidth - boxed_cast<ColumnCount>(_cursor.position.column) % TabWidth),
                         pageSize().columns - boxed_cast<ColumnCount>(logicalCursorPosition().column));
        }
    }

    if (columnsToAdvance > ColumnCount(0))
    {
        // HT must NOT overwrite existing cell content — it only moves the cursor.
        moveCursorForward(columnsToAdvance);
    }
}

void Screen::notify(string const& title, string const& content)
{
    _terminal->notify(title, content);
}

void Screen::captureBuffer(LineCount lineCount, bool logicalLines)
{
    // TODO: Unit test case! (for ensuring line numbering and limits are working as expected)

    auto capturedBuffer = std::string();

    // TODO: when capturing lineCount < screenSize.lines, start at the lowest non-empty line.
    auto const relativeStartLine =
        logicalLines ? _grid.computeLogicalLineNumberFromBottom(LineCount::cast_from(lineCount))
                     : unbox(pageSize().lines - lineCount);
    auto const startLine =
        LineOffset::cast_from(clamp(relativeStartLine, -unbox(historyLineCount()), unbox(pageSize().lines)));

    vtCaptureBufferLog()("Capture buffer: {} lines {}", lineCount, logicalLines ? "logical" : "actual");

    size_t constexpr MaxChunkSize = 4096;
    size_t currentChunkSize = 0;
    auto const pushContent = [&](auto const& data) -> void {
        if (data.empty())
            return;
        if (currentChunkSize == 0) // initiate chunk
            reply("\033^{};", CaptureBufferCode);
        else if (currentChunkSize + data.size() >= MaxChunkSize)
        {
            vtCaptureBufferLog()("Transferred chunk of {} bytes.", currentChunkSize);
            reply("\033\\"); // ST
            reply("\033^{};", CaptureBufferCode);
            currentChunkSize = 0;
        }
        reply(data);
        currentChunkSize += data.size();
    };
    LineOffset const bottomLine = boxed_cast<LineOffset>(pageSize().lines - 1);
    vtCaptureBufferLog()("Capturing buffer. top: {}, bottom: {}", relativeStartLine, bottomLine);

    for (LineOffset line = startLine; line <= bottomLine; ++line)
    {
        if (logicalLines && _grid.lineAt(line).wrapped() && !capturedBuffer.empty())
            capturedBuffer.pop_back();

        auto const& lineBuffer = _grid.lineAt(line);
        auto const trimmedText = lineBuffer.toUtf8Trimmed(false, true);
        if (trimmedText.empty())
        {
            vtCaptureBufferLog()("Skipping blank line {}", line);
            continue;
        }
        auto const tl = trimmedText.size();
        // Push content in chunks
        auto remaining = std::string_view(trimmedText);
        while (!remaining.empty())
        {
            auto const available = MaxChunkSize - currentChunkSize;
            auto const n = std::min(available, remaining.size());
            pushContent(remaining.substr(0, n));
            remaining.remove_prefix(n);
        }
        vtCaptureBufferLog()("NL ({} len)", tl);
        pushContent("\n"sv);
    }

    if (currentChunkSize != 0)
        reply("\033\\"); // ST

    vtCaptureBufferLog()("Capturing buffer finished.");
    reply("\033^{};\033\\", CaptureBufferCode); // mark the end
}

void Screen::cursorForwardTab(TabStopCount count)
{
    for (int i = 0; i < unbox(count); ++i)
        moveCursorToNextTab();
}

void Screen::cursorBackwardTab(TabStopCount count)
{
    if (!count)
        return;

    // Every target below is a real column -- a tab stop as HTS recorded it, or the page's first column --
    // so all of them are placed with setCurrentAbsoluteColumn(). Going through moveCursorToColumn() would
    // add the left margin to them again under origin mode, landing the cursor past the tab stop.
    if (!_terminal->tabs().empty())
    {
        for (unsigned k = 0; k < unbox<unsigned>(count); ++k)
        {
            // HTS records tab stops as real columns (@see Screen::horizontalTabSet), so the cursor is
            // compared as one too -- under origin mode its logical column would be measured from the left
            // margin and pick the wrong stop.
            auto const i = std::find_if(
                rbegin(_terminal->tabs()), rend(_terminal->tabs()), [&](ColumnOffset tabPos) -> bool {
                    return tabPos < realCursorPosition().column;
                });
            if (i != rend(_terminal->tabs()))
            {
                // prev tab found -> move to prev tab
                setCurrentAbsoluteColumn(*i);
            }
            else
            {
                // No earlier tab stop: CBT ignores the left/right margin (xterm), so fall back to the
                // first column, not the left margin.
                setCurrentAbsoluteColumn(ColumnOffset(0));
                break;
            }
        }
    }
    else if (TabWidth.value)
    {
        // Default tab settings. CBT ignores the left/right margin (xterm), so set the target column
        // directly -- moveCursorBackward() would stop at the left margin.
        if (*_cursor.position.column < *TabWidth)
            setCurrentAbsoluteColumn(ColumnOffset(0));
        else
        {
            auto const m = (*_cursor.position.column + 1) % *TabWidth;
            auto const n = m ? (*count - 1) * *TabWidth + m : *count * *TabWidth + m;
            setCurrentAbsoluteColumn(ColumnOffset(std::max(0, *_cursor.position.column - (n - 1))));
        }
    }
    else
    {
        // no tab stops configured
        moveCursorToBeginOfLine();
    }
}

void Screen::index()
{
    // Index scrolls only at the bottom margin *and* within the left/right margins; outside that band
    // moveCursorDown() clamps at the bottom margin, so the cursor neither scrolls nor walks past it.
    if (*realCursorPosition().line == *margin().vertical.to && isCursorInsideHorizontalMargins())
        scrollUp(LineCount(1));
    else
        moveCursorDown(LineCount(1));
}

void Screen::reverseIndex()
{
    // Reverse index mirrors index() at the top margin: it reverse-scrolls only within the left/right
    // margins, and outside that band moveCursorUp() clamps at the top margin.
    if (unbox(realCursorPosition().line) == unbox(margin().vertical.from)
        && isCursorInsideHorizontalMargins())
        scrollDown(LineCount(1));
    else
        moveCursorUp(LineCount(1));
}

void Screen::backIndex()
{
    // DECBI, a port of xterm's xtermColIndex(toLeft): sitting on the left margin it scrolls the margined
    // region right by one column -- but only while the cursor also lies within the vertical margins, as
    // xterm's xtermColScroll requires -- and anywhere else it simply moves the cursor back one column
    // (no autowrap). The previous body left the scroll a TODO and, worse, moved the cursor *forward*.
    if (realCursorPosition().column == margin().horizontal.from)
    {
        if (margin().vertical.contains(realCursorPosition().line))
            scrollRight(ColumnCount(1));
    }
    else
        moveCursorBackward(ColumnCount(1));
}

void Screen::forwardIndex()
{
    if (*realCursorPosition().column == *margin().horizontal.to)
        _grid.scrollLeft(GraphicsAttributes {}, margin());
    else
        moveCursorForward(ColumnCount(1));
}

void Screen::setForegroundColor(Color color)
{
    _cursor.graphicsRendition.foregroundColor = color;
}

void Screen::setBackgroundColor(Color color)
{
    _cursor.graphicsRendition.backgroundColor = color;
}

void Screen::setUnderlineColor(Color color)
{
    _cursor.graphicsRendition.underlineColor = color;
}

void Screen::setGraphicsRendition(GraphicsRendition rendition)
{
    if (rendition == GraphicsRendition::Reset)
        _cursor.graphicsRendition = {};
    else
        _cursor.graphicsRendition.flags = CellUtil::makeCellFlags(rendition, _cursor.graphicsRendition.flags);
}

enum class ModeResponse : uint8_t
{ // TODO: respect response 0, 3, 4.
    NotRecognized = 0,
    Set = 1,
    Reset = 2,
    PermanentlySet = 3,
    PermanentlyReset = 4
};

void Screen::requestAnsiMode(unsigned int mode)
{
    auto const modeResponse = [&]() -> ModeResponse {
        if (isValidAnsiMode(mode))
            return _terminal->isModeEnabled(static_cast<AnsiMode>(mode)) ? ModeResponse::Set
                                                                         : ModeResponse::Reset;

        // A mode the standard defines and this terminal has hard-wired off is *not* an unrecognized
        // one. @see PermanentlyResetAnsiModes.
        if (isPermanentlyResetAnsiMode(mode))
            return ModeResponse::PermanentlyReset;

        return ModeResponse::NotRecognized;
    }();

    reply("\033[{};{}$y", mode, static_cast<unsigned>(modeResponse));
}

void Screen::requestDECMode(unsigned int mode)
{
    auto const modeResponse = [this, mode]() -> ModeResponse {
        // A mode Contour recognises but hard-wires off answers 4 (PermanentlyReset), not 2 (Reset):
        // the distinction tells the host the mode can never be turned on here. @see
        // PermanentlyResetDECModes.
        if (isPermanentlyResetDECMode(mode))
            return ModeResponse::PermanentlyReset;
        auto const modeEnum = fromDECModeNum(mode);
        if (modeEnum.has_value())
        {
            // A mode above the terminal's operating level is not recognised here (DECNCSM only at
            // VT500 / level 5), matching how DECSCL gates level-specific features.
            if (conformanceLevelOf(_terminal->operatingLevel()) < minimumConformanceLevel(modeEnum.value()))
                return ModeResponse::NotRecognized;
            return _terminal->isModeEnabled(modeEnum.value()) ? ModeResponse::Set : ModeResponse::Reset;
        }
        return ModeResponse::NotRecognized;
    }();

    reply("\033[?{};{}$y", mode, static_cast<unsigned>(modeResponse));
}

void Screen::screenAlignmentPattern()
{
    // sets the margins to the extremes of the page
    margin().vertical.from = LineOffset(0);
    margin().vertical.to = boxed_cast<LineOffset>(pageSize().lines) - LineOffset(1);
    margin().horizontal.from = ColumnOffset(0);
    margin().horizontal.to = boxed_cast<ColumnOffset>(pageSize().columns) - ColumnOffset(1);

    // and moves the cursor to the home position
    moveCursorTo({}, {});

    // Fills the complete screen area with a test pattern.
    //
    // Indexed line by line rather than over a contiguous span: the grid's line storage is a ring, so
    // once lines have scrolled into history the page wraps its physical end and a span would run off
    // the buffer.
    for (auto const line: std::views::iota(0, *pageSize().lines))
        _grid.lineAt(LineOffset(line)).fill(_grid.defaultLineFlags(), GraphicsAttributes {}, U'E', 1);
}

void Screen::applicationKeypadMode(bool enable)
{
    _terminal->setApplicationkeypadMode(enable);
}

bool Screen::tryHandleSCS(Sequence const& seq)
{
    auto const& intermediates = seq.intermediateCharacters();
    if (intermediates.empty())
        return false;

    // First intermediate selects the G-set and its size:
    //   94-charset designators:  ( = G0, ) = G1, * = G2, + = G3
    //   96-charset designators:        - = G1, . = G2, / = G3   (G0 cannot hold a 96-charset)
    struct GSetDesignation
    {
        CharsetTable table;
        bool is96;
    };
    auto const gSet = [&]() -> std::optional<GSetDesignation> {
        switch (intermediates[0])
        {
            case '(': return GSetDesignation { CharsetTable::G0, false };
            case ')': return GSetDesignation { CharsetTable::G1, false };
            case '*': return GSetDesignation { CharsetTable::G2, false };
            case '+': return GSetDesignation { CharsetTable::G3, false };
            case '-': return GSetDesignation { CharsetTable::G1, true };
            case '.': return GSetDesignation { CharsetTable::G2, true };
            case '/': return GSetDesignation { CharsetTable::G3, true };
            default: return std::nullopt;
        }
    }();
    if (!gSet)
        return false;

    // Build the designator string from remaining intermediates + final character.
    // Single-byte designator: intermediates has 1 char, final is the designator.
    // Two-byte designator (DRCS): intermediates has 2 chars, second is the intermediate, final is the
    // designator.
    auto const designator = [&]() -> std::string {
        auto result = std::string {};
        for (size_t i = 1; i < intermediates.size(); ++i)
            result += intermediates[i];
        result += seq.finalChar();
        return result;
    }();

    // `<` designates the User-Preferred Supplemental Set (xterm's nrc_DEC_UPSS). It is unlike every
    // other designator in naming no fixed set: it resolves to whatever DECAUPSS last assigned.
    //
    // This is also the one sanctioned way for G0 to hold a 96-character set. DEC STD 070 tells
    // applications not to assume they may designate a 96-charset into G0, "but that it is possible to
    // do this using UPSS" (quoted in xterm's misc.c, above decode_upss) -- so the slot's own syntax
    // does not decide the size here; the resolved set's does.
    //
    // A VT320-era designator, so a terminal operating below that level does not recognise it and the
    // designation is ignored -- silently, exactly as an unknown designator is below.
    if (designator == "<")
    {
        if (conformanceLevelOf(_terminal->operatingLevel()) >= conformanceLevelOf(VTType::VT320))
            _cursor.charsets.selectUserPreferred(gSet->table, _terminal->userPreferredSupplementalSet());
        return true;
    }

    // Try to map the designator to a standard charset first
    auto const standardCharset = [&]() -> std::optional<CharsetId> {
        if (designator.size() != 1)
            return std::nullopt;
        if (gSet->is96)
        {
            // 96-character sets (invoked into GR). Only ISO Latin-1 supplemental ('A') is defined here.
            switch (designator[0])
            {
                case 'A': return CharsetId::ISOLatin1Supplemental;
                default: return std::nullopt;
            }
        }
        switch (designator[0])
        {
            case '0': return CharsetId::Special;
            case 'A': return CharsetId::British;
            case 'B': return CharsetId::USASCII;
            case 'C': return CharsetId::Finnish;
            case '4': return CharsetId::Dutch;
            case 'E': return CharsetId::NorwegianDanish;
            case 'R': return CharsetId::French;
            case 'Q': return CharsetId::FrenchCanadian;
            case 'K': return CharsetId::German;
            case 'Z': return CharsetId::Spanish;
            case 'H': return CharsetId::Swedish;
            case '=': return CharsetId::Swiss;
            case '>': return CharsetId::Technical;
            default: return std::nullopt;
        }
    }();

    if (standardCharset)
    {
        if (gSet->is96)
            _cursor.charsets.select96(gSet->table, *standardCharset);
        else
            designateCharset(gSet->table, *standardCharset);
        return true;
    }

    // For DRCS designators: look up the font number from the Terminal's designator map.
    if (auto const fontNumber = _terminal->drcsDesignatorToFont(designator); fontNumber.has_value())
    {
        _cursor.charsets.selectDRCS(gSet->table, *fontNumber);
        return true;
    }

    // Accept any other unrecognized designator silently (unknown DRCS or future extension).
    return true;
}

void Screen::designateCharset(CharsetTable table, CharsetId charset)
{
    // TODO: unit test SCS and see if they also behave well with reset/softreset
    // Also, is the cursor shared between the two buffers?
    _cursor.charsets.select(table, charset);
}

void Screen::singleShiftSelect(CharsetTable table)
{
    // TODO: unit test SS2, SS3
    _cursor.charsets.singleShift(table);
}

void Screen::sixelImage(ImageSize pixelSize, Image::Data&& rgbaData)
{
    auto const columnCount = ColumnCount::cast_from(
        ceil(pixelSize.width.as<double>() / _terminal->cellPixelSize().width.as<double>()));
    auto const lineCount = LineCount::cast_from(
        ceil(pixelSize.height.as<double>() / _terminal->cellPixelSize().height.as<double>()));
    auto const extent = GridSize { .lines = lineCount, .columns = columnCount };
    auto const autoScrollAtBottomMargin = !_terminal->isModeEnabled(DECMode::NoSixelScrolling);
    auto const topLeft = autoScrollAtBottomMargin ? logicalCursorPosition() : CellLocation {};

    auto const alignmentPolicy = ImageAlignment::TopStart;
    auto const resizePolicy = ImageResize::ResizeToFit;

    auto const imageOffset = PixelCoordinate {};
    auto const imageSize = pixelSize;

    shared_ptr<Image const> const imageRef = uploadImage(ImageFormat::RGBA, pixelSize, std::move(rgbaData));
    renderImage(imageRef,
                topLeft,
                extent,
                imageOffset,
                imageSize,
                alignmentPolicy,
                resizePolicy,
                autoScrollAtBottomMargin);

    if (!_terminal->isModeEnabled(DECMode::SixelCursorNextToGraphic))
        linefeed(topLeft.column);
}

shared_ptr<Image const> Screen::uploadImage(ImageFormat format, ImageSize imageSize, Image::Data&& pixmap)
{
    return _terminal->imagePool().create(format, imageSize, std::move(pixmap));
}

void Screen::renderImage(shared_ptr<Image const> image,
                         CellLocation topLeft,
                         GridSize gridSize,
                         PixelCoordinate imageOffset,
                         ImageSize imageSize,
                         ImageAlignment alignmentPolicy,
                         ImageResize resizePolicy,
                         bool autoScroll,
                         bool updateCursor,
                         ImageLayer layer)
{
    auto const linesAvailable = pageSize().lines - topLeft.line.as<LineCount>();
    auto const linesToBeRendered = std::min(gridSize.lines, linesAvailable);
    auto const columnsAvailable = pageSize().columns - topLeft.column;
    auto const columnsToBeRendered = ColumnCount(std::min(columnsAvailable, gridSize.columns));
    auto const gapColor = RGBAColor { vtbackend::apply(_terminal->colorPalette(),
                                                       _cursor.graphicsRendition.backgroundColor,
                                                       ColorTarget::Background,
                                                       ColorMode::Normal) };

    auto const rasterizedImage = make_shared<RasterizedImage>(std::move(image),
                                                              alignmentPolicy,
                                                              resizePolicy,
                                                              gapColor,
                                                              gridSize,
                                                              _terminal->cellPixelSize(),
                                                              layer,
                                                              imageOffset,
                                                              imageSize);

    // Compute the cursor line offset after rendering.
    // For Sixel images (identified by non-zero pixel imageSize), use VT340-compatible sixel band math.
    // For GIP images, simply place the cursor at the last row of the rendered area.
    auto const isSixelImage = imageSize.width.value > 0 && imageSize.height.value > 0;
    auto const cursorLineOffset = [&]() -> LineOffset {
        if (isSixelImage)
        {
            auto const lastSixelBand = unbox(imageSize.height) % 6;
            auto lineOffset =
                LineOffset::cast_from(std::ceil((imageSize.height - lastSixelBand).as<double>()
                                                / _terminal->cellPixelSize().height.as<double>()))
                - 1 * (lastSixelBand == 0);
            auto const h = unbox(imageSize.height) - 1;
            // VT340 quirk: for some heights the text cursor is placed one row too high.
            // See: https://github.com/hackerb9/vt340test/blob/main/glitches.md
            if (h % 6 > h % unbox(_terminal->cellPixelSize().height))
                lineOffset = lineOffset - 1;
            return lineOffset;
        }
        // GIP: cursor goes to the line immediately below the last rendered image row.
        return boxed_cast<LineOffset>(linesToBeRendered);
    }();

    if (unbox(linesToBeRendered))
    {
        for (GridSize::Offset const gridOffset:
             GridSize { .lines = linesToBeRendered, .columns = columnsToBeRendered })
        {
            auto cell = at(topLeft + gridOffset);
            cell.setImageFragment(rasterizedImage,
                                  CellLocation { .line = gridOffset.line, .column = gridOffset.column });
            cell.setHyperlink(_cursor.hyperlink);
        };
        if (updateCursor)
            moveCursorTo(topLeft.line + cursorLineOffset, topLeft.column);
    }

    // If there are lines remaining (image didn't fit on screen) and autoScroll is enabled,
    // scroll content up and render the remaining lines.
    if (linesToBeRendered != gridSize.lines && autoScroll)
    {
        auto const remainingLineCount = gridSize.lines - linesToBeRendered;
        for (auto const lineOffset: crispy::times(*remainingLineCount))
        {
            linefeed(topLeft.column);
            for (auto const columnOffset: crispy::views::iota_as<ColumnOffset>(*columnsToBeRendered))
            {
                auto const fragOffset =
                    CellLocation { .line = boxed_cast<LineOffset>(linesToBeRendered) + lineOffset,
                                   .column = columnOffset };
                auto cell = at(boxed_cast<LineOffset>(pageSize().lines) - 1, topLeft.column + columnOffset);
                cell.setImageFragment(rasterizedImage, fragOffset);
                cell.setHyperlink(_cursor.hyperlink);
            };
        }
        // After auto-scroll, cursor is ON the last image row. Move it below.
        // Only for GIP — Sixel handles its own cursor positioning at the call site.
        if (updateCursor && !isSixelImage)
            linefeed(topLeft.column);
    }

    // Move ANSI text cursor to the correct column after image placement.
    if (updateCursor)
        moveCursorToColumn(topLeft.column);
}

namespace
{
    /// Resolves @p color to the concrete color a query must be answered with.
    ///
    /// A dynamic color need not name a color of its own: it may be configured to follow the cell's own
    /// foreground or background instead, which is how a selection inverts rather than tints. A query
    /// still has to be answered with one concrete color, so resolve those to the page's default
    /// foreground and background -- the colors the overwhelming majority of cells carry.
    [[nodiscard]] RGBColor resolveCellColor(CellRGBColor const& color, ColorPalette const& palette) noexcept
    {
        if (holds_alternative<CellForegroundColor>(color))
            return palette.defaultForeground;
        if (holds_alternative<CellBackgroundColor>(color))
            return palette.defaultBackground;
        return get<RGBColor>(color);
    }
} // namespace

void Screen::requestDynamicColor(DynamicColorName name)
{
    auto const& palette = _terminal->colorPalette();

    auto const color = [&]() -> RGBColor {
        switch (name)
        {
            case DynamicColorName::DefaultForegroundColor: return palette.defaultForeground;
            case DynamicColorName::DefaultBackgroundColor: return palette.defaultBackground;
            case DynamicColorName::TextCursorColor: return resolveCellColor(palette.cursor.color, palette);
            case DynamicColorName::MouseForegroundColor: return palette.mouseForeground;
            case DynamicColorName::MouseBackgroundColor: return palette.mouseBackground;
            case DynamicColorName::HighlightForegroundColor:
                return resolveCellColor(palette.selection.foreground, palette);
            case DynamicColorName::HighlightBackgroundColor:
                return resolveCellColor(palette.selection.background, palette);
        }
        crispy::unreachable();
    }();

    // Every query is answered. Falling silent -- as the highlight colors used to, whenever they were
    // following the cell's own color rather than naming one -- leaves the application reading some
    // later sequence's reply in place of the one it is waiting for.
    reply("\033]{};{}\033\\", setDynamicColorCommand(name), colorSpecification(color));
}

void Screen::requestPixelSize(RequestPixelSize area)
{
    switch (area)
    {
        case RequestPixelSize::WindowArea: [[fallthrough]]; // Contour draws no chrome of its own.
        case RequestPixelSize::TextArea:
            reply("\033[4;{};{}t", _terminal->pixelSize().height, _terminal->pixelSize().width);
            break;
        case RequestPixelSize::ScreenArea:
            reply("\033[5;{};{}t", _terminal->screenPixelSize().height, _terminal->screenPixelSize().width);
            break;
        case RequestPixelSize::CellArea:
            reply("\033[6;{};{}t", _terminal->cellPixelSize().height, _terminal->cellPixelSize().width);
            break;
    }
}

void Screen::requestCharacterSize(RequestPixelSize area)
{
    switch (area)
    {
        case RequestPixelSize::WindowArea: [[fallthrough]]; // Contour draws no chrome of its own.
        case RequestPixelSize::TextArea: reply("\033[8;{};{}t", pageSize().lines, pageSize().columns); break;
        case RequestPixelSize::ScreenArea: {
            // `CSI 19 t` asks how large the *screen* is, in characters -- how big the window could grow,
            // not how big it currently is. Answering with the page size, as this used to, tells every
            // application that the window is already maximized.
            auto const screen = _terminal->screenPageSize();
            reply("\033[9;{};{}t", screen.lines, screen.columns);
            break;
        }
        case RequestPixelSize::CellArea:
            Guarantee(false
                      && "Screen.requestCharacterSize: Doesn't make sense, and cannot be called, therefore, "
                         "fortytwo.");
            break;
    }
}

void Screen::requestStatusString(RequestStatusString value)
{
    // xterm responds with DCS 1 $ r Pt ST for valid requests
    // or DCS 0 $ r Pt ST for invalid requests.
    auto const response = [&](RequestStatusString value) -> optional<string> {
        switch (value)
        {
            case RequestStatusString::DECSCL: {
                auto level = 61;
                switch (_terminal->operatingLevel())
                {
                    case VTType::VT525:
                    case VTType::VT520:
                    case VTType::VT510: level = 65; break;
                    case VTType::VT420: level = 64; break;
                    case VTType::VT340:
                    case VTType::VT330:
                    case VTType::VT320: level = 63; break;
                    case VTType::VT240:
                    case VTType::VT220: level = 62; break;
                    case VTType::VT100: level = 61; break;
                }

                auto const c1t = _terminal->c1TransmissionMode() == ControlTransmissionMode::S7C1T ? 1 : 0;

                return std::format("{};{}\"p", level, c1t);
            }
            case RequestStatusString::DECSCUSR: // Set cursor style (DECSCUSR), VT520
            {
                int const blinkingOrSteady = _terminal->cursorDisplay() == CursorDisplay::Steady ? 1 : 0;
                int const shape = [&]() {
                    switch (_terminal->cursorShape())
                    {
                        case CursorShape::Block: return 1;
                        case CursorShape::Underscore: return 3;
                        case CursorShape::Bar: return 5;
                        case CursorShape::Rectangle: return 7;
                    }
                    return 1;
                }();
                return std::format("{} q", shape + blinkingOrSteady);
            }
            case RequestStatusString::DECSLPP:
                // Ps >= 2 4  -> Resize to Ps lines (DECSLPP), VT340 and VT420.
                // xterm adapts this by resizing its window.
                if (*pageSize().lines >= 24)
                    return std::format("{}t", pageSize().lines);
                errorLog()("Requesting device status for {} not with line count < 24 is undefined.");
                return nullopt;
            case RequestStatusString::DECSTBM:
                // Both bounds are stored 0-based and inclusive, so both convert back to 1-based with +1.
                // The `to` used to be reported raw, one short of the value DECSTBM was given.
                return std::format("{};{}r", 1 + *margin().vertical.from, 1 + *margin().vertical.to);
            case RequestStatusString::DECSLRM:
                return std::format("{};{}s", 1 + *margin().horizontal.from, 1 + *margin().horizontal.to);
            case RequestStatusString::DECSCPP:
                // EXTENSION: Usually DECSCPP only knows about 80 and 132, but we take any.
                return std::format("{}|$", pageSize().columns);
            case RequestStatusString::DECSNLS: return std::format("{}*|", pageSize().lines);
            case RequestStatusString::SGR:
                return std::format("0;{}m", vtSequenceParameterString(_cursor.graphicsRendition));
            case RequestStatusString::DECSCA: {
                auto const isProtected = _cursor.graphicsRendition.flags & CellFlag::CharacterProtected;
                return std::format("{}\"q", isProtected ? 1 : 2);
            }
            case RequestStatusString::DECSACE:
                // Ps=2 is rectangle mode; anything else (0 or 1) is stream. xterm reports the raw value,
                // but the 0-vs-1 distinction has no effect, so reporting stream as 0 is faithful enough.
                return std::format("{}*x", _rectangularAttributeMode ? 2 : 0);
            case RequestStatusString::DECELF: return std::format("{}+q", _enableLocalFunctions);
            case RequestStatusString::DECLFKC: return std::format("{}*}}", _localFunctionKeyControl);
            case RequestStatusString::DECSMKR: return std::format("{}+r", _modifierKeyReporting);
            case RequestStatusString::DECSASD:
                switch (_terminal->activeStatusDisplay())
                {
                    case ActiveStatusDisplay::Main: return "0$}";
                    case ActiveStatusDisplay::StatusLine: return "1$}";
                    case ActiveStatusDisplay::IndicatorStatusLine: return "2$}"; // XXX This is not standard
                }
                break;
            case RequestStatusString::DECSSDT:
                switch (_terminal->statusDisplayType())
                {
                    case StatusDisplayType::None: return "0$~";
                    case StatusDisplayType::Indicator: return "1$~";
                    case StatusDisplayType::HostWritable: return "2$~";
                }
                break;
        }
        return nullopt;
    }(value);

    reply("\033P{}$r{}\033\\", response.has_value() ? 1 : 0, response.value_or(""), "\"p");
}

void Screen::requestTabStops()
{
    // Response: `DCS 2 $ u Pt ST`
    ostringstream dcs;
    dcs << "\033P2$u"sv; // DCS
    if (!_terminal->tabs().empty())
    {
        for (size_t const i: times(_terminal->tabs().size()))
        {
            if (i)
                dcs << '/';
            dcs << *_terminal->tabs()[i] + 1;
        }
    }
    else if (*TabWidth != 0)
    {
        dcs << 1;
        for (auto column = *TabWidth + 1; column <= *pageSize().columns; column += *TabWidth)
            dcs << '/' << column;
    }
    dcs << "\033\\"sv; // ST

    reply(dcs.str());
}

namespace
{
    std::string asHex(std::string_view value)
    {
        std::string output;
        for (char const ch: value)
            output += std::format("{:02X}", unsigned(ch));
        return output;
    }
} // namespace

void Screen::requestCapability(std::string_view name)
{
    if (booleanCapability(name))
        reply("\033P1+r{}\033\\", toHexString(name));
    else if (auto const value = numericCapability(name); value != Database::Npos)
        reply("\033P1+r{}={}\033\\", toHexString(name), asHex(std::to_string(value)));
    else if (auto const value = stringCapability(name); !value.empty())
        reply("\033P1+r{}={}\033\\", toHexString(name), asHex(value));
    else
        reply("\033P0+r\033\\");
}

void Screen::requestCapability(capabilities::Code code)
{
    if (booleanCapability(code))
        reply("\033P1+r{}\033\\", code.hex());
    else if (auto const value = numericCapability(code); value != Database::Npos)
        reply("\033P1+r{}={}\033\\", code.hex(), asHex(std::to_string(value)));
    else if (auto const value = stringCapability(code); !value.empty())
        reply("\033P1+r{}={}\033\\", code.hex(), asHex(value));
    else
        reply("\033P0+r\033\\");
}

void Screen::resetDynamicColor(DynamicColorName name)
{
    switch (name)
    {
        case DynamicColorName::DefaultForegroundColor:
            _terminal->colorPalette().defaultForeground = _terminal->defaultColorPalette().defaultForeground;
            break;
        case DynamicColorName::DefaultBackgroundColor:
            _terminal->colorPalette().defaultBackground = _terminal->defaultColorPalette().defaultBackground;
            break;
        case DynamicColorName::TextCursorColor:
            _terminal->colorPalette().cursor = _terminal->defaultColorPalette().cursor;
            break;
        case DynamicColorName::MouseForegroundColor:
            _terminal->colorPalette().mouseForeground = _terminal->defaultColorPalette().mouseForeground;
            break;
        case DynamicColorName::MouseBackgroundColor:
            _terminal->colorPalette().mouseBackground = _terminal->defaultColorPalette().mouseBackground;
            break;
        case DynamicColorName::HighlightForegroundColor:
            _terminal->colorPalette().selection.foreground =
                _terminal->defaultColorPalette().selection.foreground;
            break;
        case DynamicColorName::HighlightBackgroundColor:
            _terminal->colorPalette().selection.background =
                _terminal->defaultColorPalette().selection.background;
            break;
    }
}

void Screen::setDynamicColor(DynamicColorName name, RGBColor color)
{
    switch (name)
    {
        case DynamicColorName::DefaultForegroundColor:
            _terminal->colorPalette().defaultForeground = color;
            break;
        case DynamicColorName::DefaultBackgroundColor:
            _terminal->colorPalette().defaultBackground = color;
            break;
        case DynamicColorName::TextCursorColor: _terminal->colorPalette().cursor.color = color; break;
        case DynamicColorName::MouseForegroundColor: _terminal->colorPalette().mouseForeground = color; break;
        case DynamicColorName::MouseBackgroundColor: _terminal->colorPalette().mouseBackground = color; break;
        case DynamicColorName::HighlightForegroundColor:
            _terminal->colorPalette().selection.foreground = color;
            break;
        case DynamicColorName::HighlightBackgroundColor:
            _terminal->colorPalette().selection.background = color;
            break;
    }
}

void Screen::inspect()
{
    _terminal->inspect();
}

void Screen::inspect(std::string const& message, std::ostream& os) const
{
    auto const hline = [&]() {
        for_each(crispy::times(*pageSize().columns), [&](auto) { os << '='; });
        os << '\n';
    };

    auto const gridInfoLine = [&](Grid const& grid) {
        return std::format("main page lines: scrollback cur {} max {}, main page lines {}, used lines "
                           "{}, zero index {}\n",
                           grid.historyLineCount(),
                           grid.maxHistoryLineCount(),
                           grid.pageSize().lines,
                           grid.linesUsed(),
                           grid.zero_index());
    };

    if (!message.empty())
    {
        hline();
        os << "\033[1;37;41m" << message << "\033[m" << '\n';
        hline();
    }

    os << std::format("Rendered screen at the time of failure\n");
    os << std::format("main page size       : {}\n", _settings->pageSize);
    os << std::format("history line count   : {} (max {})\n",
                      _terminal->primaryScreen().historyLineCount(),
                      _terminal->maxHistoryLineCount());
    os << std::format("cursor position      : {}\n", _cursor.position);
    os << std::format("vertical margins     : {}\n", margin().vertical);
    os << std::format("horizontal margins   : {}\n", margin().horizontal);
    os << gridInfoLine(grid());

    hline();
    os << screenshot([this](LineOffset lineNo) -> string {
        // auto const absoluteLine = _grid.toAbsoluteLine(lineNo);
        return std::format("{} {:>4}: {}", ":", lineNo.value, _grid.lineAt(lineNo).flags());
    });
    hline();
    _terminal->imagePool().inspect(os);
    hline();

    // TODO: print more useful debug information
    // - screen size
    // - left/right margin
    // - top/down margin
    // - cursor position
    // - autoWrap
    // - ... other output related modes
}

void Screen::smGraphics(XtSmGraphics::Item item, XtSmGraphics::Action action, XtSmGraphics::Value value)
{
    using Item = XtSmGraphics::Item;
    using Action = XtSmGraphics::Action;

    constexpr auto NumberOfColorRegistersItem = 1;
    constexpr auto SixelItem = 2;
    constexpr auto ReGISItem = 3;

    constexpr auto Success = 0;
    constexpr auto Failure = 3;

    switch (item)
    {
        case Item::NumberOfColorRegisters:
            switch (action)
            {
                case Action::Read: {
                    auto const value = _terminal->sixelColorPalette()->size();
                    reply("\033[?{};{};{}S", NumberOfColorRegistersItem, Success, value);
                    break;
                }
                case Action::ReadLimit: {
                    auto const value = _terminal->sixelColorPalette()->maxSize();
                    reply("\033[?{};{};{}S", NumberOfColorRegistersItem, Success, value);
                    break;
                }
                case Action::ResetToDefault: {
                    auto const value = _terminal->maxSixelColorRegisters();
                    _terminal->sixelColorPalette()->setSize(value);
                    reply("\033[?{};{};{}S", NumberOfColorRegistersItem, Success, value);
                    break;
                }
                case Action::SetToValue:
                    visit(overloaded {
                              [&](int number) {
                                  _terminal->sixelColorPalette()->setSize(static_cast<unsigned>(number));
                                  reply("\033[?{};{};{}S", NumberOfColorRegistersItem, Success, number);
                              },
                              [&](ImageSize) {
                                  reply("\033[?{};{};{}S", NumberOfColorRegistersItem, Failure, 0);
                              },
                              [&](monostate) {
                                  reply("\033[?{};{};{}S", NumberOfColorRegistersItem, Failure, 0);
                              },
                          },
                          value);
                    break;
            }
            break;

        case Item::SixelGraphicsGeometry:
            switch (action)
            {
                case Action::Read: {
                    // Report the area images are actually painted into. Screen::pageSize() is the
                    // main page: Terminal::pixelSize() would span totalPageSize(), which includes
                    // the indicator status line, so an application honouring this reply would
                    // overshoot the grid by one row.
                    auto const paintableSize = _terminal->cellPixelSize() * pageSize();
                    auto const size = vtpty::min(paintableSize, _terminal->maxImageSize());
                    reply("\033[?{};{};{};{}S", SixelItem, Success, size.width, size.height);
                }
                break;
                case Action::ReadLimit: {
                    auto const ceiling = _terminal->imageCanvasCeiling();
                    reply("\033[?{};{};{};{}S", SixelItem, Success, ceiling.width, ceiling.height);
                }
                break;
                case Action::ResetToDefault: {
                    // Following the ceiling IS the default, so this drops the negotiated size rather
                    // than pinning today's ceiling as one -- the ceiling moves with the display, and
                    // a reset must not freeze the canvas at whatever monitor happened to be current.
                    // Reply, as NumberOfColorRegisters does: an application that issues this and
                    // waits would otherwise hang.
                    auto const size = _terminal->resetEffectiveImageCanvasSize();
                    reply("\033[?{};{};{};{}S", SixelItem, Success, size.width, size.height);
                }
                break;
                case Action::SetToValue:
                    if (holds_alternative<ImageSize>(value))
                    {
                        auto const size = _terminal->setEffectiveImageCanvasSize(get<ImageSize>(value));
                        reply("\033[?{};{};{};{}S", SixelItem, Success, size.width, size.height);
                    }
                    else
                        reply("\033[?{};{};{}S", SixelItem, Failure, 0);
                    break;
            }
            break;

        case Item::ReGISGraphicsGeometry: {
            // ReGIS draws into the VT340 addressing space (default 800x480 pixels), which Contour
            // rasterizes and scales into the grid. This is the logical geometry an application reasons
            // in -- the internal supersampled buffer is not reported. Unlike the Sixel canvas the
            // geometry is fixed rather than negotiable: read/limit/reset report the fixed size, while a
            // request to set it to a different value is rejected rather than falsely acknowledged.
            auto constexpr RegisCanvasSize =
                ImageSize { Width(regis::DefaultAddressWidth), Height(regis::DefaultAddressHeight) };
            switch (action)
            {
                case Action::Read:
                case Action::ReadLimit:
                case Action::ResetToDefault:
                    reply("\033[?{};{};{};{}S",
                          ReGISItem,
                          Success,
                          RegisCanvasSize.width,
                          RegisCanvasSize.height);
                    break;
                case Action::SetToValue: reply("\033[?{};{};{}S", ReGISItem, Failure, 0); break;
            }
            break;
        }
    }
}
// }}}

// {{{ impl namespace (some command generator helpers)
namespace impl
{
    namespace
    {
        /// Reads the rectangular area a DEC rectangular-area sequence names: Pt, Pl, Pb and Pr, the four
        /// parameters starting at @p firstParameter.
        ///
        /// Six sequences name an area this way -- DECCARA, DECRARA, DECCRA, DECERA, DECFRA and DECSERA
        /// -- and each carried its own copy of this arithmetic, no two of them agreeing. One clamped
        /// the bottom-right corner but not the top-left, one clamped neither, one guarded the top-left
        /// against going negative and the rest did not. The one that did not is where esctest aborted
        /// the engine: `CSI ; ; 2 ; 2 ; ; 5 ; 5 ; 1 $ v` names no source corner, and no source corner
        /// minus one is column -1.
        ///
        /// The coordinates are one-based, and each takes its default when omitted, empty or zero: the
        /// origin for the top-left corner, the page's edge for the bottom-right. They are relative to
        /// the origin, which origin mode (DECOM) moves to the scrolling region's top-left corner. A
        /// coordinate naming a cell beyond the page names the page's edge instead, as the VT520 manual
        /// requires.
        ///
        /// @param seq            The sequence to read from.
        /// @param firstParameter Index of Pt; Pl, Pb and Pr follow it.
        /// @param origin         Top-left corner coordinates are relative to. @see Screen::origin().
        /// @param page           Size of the page the area lives on.
        /// @return The area, as zero-based offsets into @p page.
        [[nodiscard]] Rect readRectangularArea(Sequence const& seq,
                                               size_t firstParameter,
                                               CellLocation origin,
                                               PageSize page) noexcept
        {
            auto const lines = static_cast<unsigned>(unbox(page.lines));
            auto const columns = static_cast<unsigned>(unbox(page.columns));

            // One-based, relative to the origin, and never past the page's edge.
            auto const resolve = [&](size_t index, unsigned fallback, unsigned limit) {
                return std::min(seq.param_positive_or(index, fallback), limit);
            };

            auto const top = resolve(firstParameter + 0, 1, lines);
            auto const left = resolve(firstParameter + 1, 1, columns);
            auto const bottom = resolve(firstParameter + 2, lines, lines);
            auto const right = resolve(firstParameter + 3, columns, columns);

            // Zero-based, and translated onto the page.
            auto const line = static_cast<unsigned>(unbox(origin.line));
            auto const column = static_cast<unsigned>(unbox(origin.column));

            return Rect { .top = Top::cast_from(std::min(top - 1 + line, lines - 1)),
                          .left = Left::cast_from(std::min(left - 1 + column, columns - 1)),
                          .bottom = Bottom::cast_from(std::min(bottom - 1 + line, lines - 1)),
                          .right = Right::cast_from(std::min(right - 1 + column, columns - 1)) };
        }

        /// The ANSI modes the engine actually acts on, and therefore accepts.
        ///
        /// SRM (12) is deliberately absent. Its bit would be stored and faithfully reported back by
        /// DECRQM, but nothing acts on it -- the terminal never echoes locally -- so accepting
        /// `CSI 12 l` would advertise a capability Contour does not have. Leaving it Unsupported keeps
        /// the gap visible to the conformance harness instead of quietly papering over it.
        ///
        /// Adding a mode is adding a row.
        constexpr auto SupportedAnsiModes = std::array {
            AnsiMode::KeyboardAction,   // KAM -- gates input, see Terminal::allowInput()
            AnsiMode::Insert,           // IRM
            AnsiMode::SendReceive,      // SRM -- local echo, see Terminal::flushInput()
            AnsiMode::AutomaticNewLine, // LNM
        };

        ApplyResult setAnsiMode(Sequence const& seq, size_t modeIndex, bool enable, Terminal& term)
        {
            auto const modeNumber = seq.param(modeIndex);
            if (!isValidAnsiMode(modeNumber))
                return ApplyResult::Unsupported;

            auto const mode = static_cast<AnsiMode>(modeNumber);
            if (std::ranges::find(SupportedAnsiModes, mode) == SupportedAnsiModes.end())
                return ApplyResult::Unsupported;

            term.setMode(mode, enable);
            return ApplyResult::Ok;
        }

        ApplyResult setModeDEC(Sequence const& seq, size_t modeIndex, bool enable, Terminal& term)
        {
            if (auto const modeOpt = fromDECModeNum(seq.param(modeIndex)); modeOpt.has_value())
            {
                // Ignore a mode above the terminal's operating level (DECNCSM only at VT500 / level 5),
                // so a lower-level terminal neither enables nor reports it.
                if (conformanceLevelOf(term.operatingLevel()) < minimumConformanceLevel(modeOpt.value()))
                    return ApplyResult::Ok;
                term.setMode(modeOpt.value(), enable);
                return ApplyResult::Ok;
            }
            return ApplyResult::Invalid;
        }

        Color parseColor(Sequence const& seq, size_t* pi)
        {
            // We are at parameter index `i`.
            //
            // It may now follow:
            // - ":2::r:g:b"        RGB color
            // - ":3:F:C:M:Y"       CMY color  (F is scaling factor, what is max? 100 or 255?)
            // - ":4:F:C:M:Y:K"     CMYK color (F is scaling factor, what is max? 100 or 255?)
            // - ":5:P"
            // Sub-parameters can also be delimited with ';' and thus are no sub-parameters per-se.
            size_t i = *pi;
            auto const len = seq.subParameterCount(i);
            if (seq.subParameterCount(i) >= 1)
            {
                switch (seq.param(i + 1))
                {
                    case 2: // ":2::R:G:B" and ":2:R:G:B"
                    {
                        if (len == 4 || len == 5)
                        {
                            // NB: subparam(i, 1) may be ignored
                            auto const r = seq.subparam(i, len - 2);
                            auto const g = seq.subparam(i, len - 1);
                            auto const b = seq.subparam(i, len - 0);
                            if (r <= 255 && g <= 255 && b <= 255)
                            {
                                *pi += len;
                                return Color { RGBColor { static_cast<uint8_t>(r),
                                                          static_cast<uint8_t>(g),
                                                          static_cast<uint8_t>(b) } };
                            }
                        }
                        break;
                    }
                    case 3: // ":3:F:C:M:Y" (TODO)
                    case 4: // ":4:F:C:M:Y:K" (TODO)
                        *pi += len;
                        break;
                    case 5: // ":5:P"
                        if (auto const p = seq.subparam(i, 2); p <= 255)
                        {
                            *pi += len;
                            return static_cast<IndexedColor>(p);
                        }
                        break;
                    default:
                        // XXX invalid sub parameter
                        break;
                }
            }

            // Compatibility mode, colors using ';' instead of ':'.
            if (i + 1 < seq.parameterCount())
            {
                ++i;
                auto const mode = seq.param(i);
                if (mode == 5)
                {
                    if (i + 1 < seq.parameterCount())
                    {
                        ++i;
                        auto const value = seq.param(i);
                        if (i <= 255)
                        {
                            *pi = i;
                            return static_cast<IndexedColor>(value);
                        }
                        else
                        {
                        } // TODO: seq.logInvalidCSI("Invalid color indexing.");
                    }
                    else
                    {
                    } // TODO: seq.logInvalidCSI("Missing color index.");
                }
                else if (mode == 2)
                {
                    if (i + 3 < seq.parameterCount())
                    {
                        auto const r = seq.param(i + 1);
                        auto const g = seq.param(i + 2);
                        auto const b = seq.param(i + 3);
                        i += 3;
                        if (r <= 255 && g <= 255 && b <= 255)
                        {
                            *pi = i;
                            return RGBColor { static_cast<uint8_t>(r),
                                              static_cast<uint8_t>(g),
                                              static_cast<uint8_t>(b) };
                        }
                        else
                        {
                        } // TODO: seq.logInvalidCSI("RGB color out of range.");
                    }
                    else
                    {
                    } // TODO: seq.logInvalidCSI("Invalid color mode.");
                }
                else
                {
                } // TODO: seq.logInvalidCSI("Invalid color mode.");
            }
            else
            {
            } // TODO: seq.logInvalidCSI("Invalid color indexing.");

            // failure case, skip this argument
            *pi = i + 1;
            return Color {};
        }

        ApplyResult applySGR(auto& target, Sequence const& seq, size_t parameterStart, size_t parameterEnd)
        {
            if (parameterStart == parameterEnd)
            {
                target.setGraphicsRendition(GraphicsRendition::Reset);
                return ApplyResult::Ok;
            }

            for (size_t i = parameterStart; i < parameterEnd; ++i)
            {
                switch (seq.param(i))
                {
                    case 0: target.setGraphicsRendition(GraphicsRendition::Reset); break;
                    case 1: target.setGraphicsRendition(GraphicsRendition::Bold); break;
                    case 2: target.setGraphicsRendition(GraphicsRendition::Faint); break;
                    case 3: target.setGraphicsRendition(GraphicsRendition::Italic); break;
                    case 4:
                        if (seq.subParameterCount(i) == 1)
                        {
                            switch (seq.subparam(i, 1))
                            {
                                case 0: target.setGraphicsRendition(GraphicsRendition::NoUnderline); break;
                                case 1: target.setGraphicsRendition(GraphicsRendition::Underline); break;
                                case 2:
                                    target.setGraphicsRendition(GraphicsRendition::DoublyUnderlined);
                                    break;
                                case 3:
                                    target.setGraphicsRendition(GraphicsRendition::CurlyUnderlined);
                                    break;
                                case 4:
                                    target.setGraphicsRendition(GraphicsRendition::DottedUnderline);
                                    break;
                                case 5:
                                    target.setGraphicsRendition(GraphicsRendition::DashedUnderline);
                                    break;
                                default: target.setGraphicsRendition(GraphicsRendition::Underline); break;
                            }
                            ++i;
                        }
                        else
                            target.setGraphicsRendition(GraphicsRendition::Underline);
                        break;
                    case 5: target.setGraphicsRendition(GraphicsRendition::Blinking); break;
                    case 6: target.setGraphicsRendition(GraphicsRendition::RapidBlinking); break;
                    case 7: target.setGraphicsRendition(GraphicsRendition::Inverse); break;
                    case 8: target.setGraphicsRendition(GraphicsRendition::Hidden); break;
                    case 9: target.setGraphicsRendition(GraphicsRendition::CrossedOut); break;
                    case 21: target.setGraphicsRendition(GraphicsRendition::DoublyUnderlined); break;
                    case 22: target.setGraphicsRendition(GraphicsRendition::Normal); break;
                    case 23: target.setGraphicsRendition(GraphicsRendition::NoItalic); break;
                    case 24: target.setGraphicsRendition(GraphicsRendition::NoUnderline); break;
                    case 25: target.setGraphicsRendition(GraphicsRendition::NoBlinking); break;
                    case 27: target.setGraphicsRendition(GraphicsRendition::NoInverse); break;
                    case 28: target.setGraphicsRendition(GraphicsRendition::NoHidden); break;
                    case 29: target.setGraphicsRendition(GraphicsRendition::NoCrossedOut); break;
                    case 30: target.setForegroundColor(IndexedColor::Black); break;
                    case 31: target.setForegroundColor(IndexedColor::Red); break;
                    case 32: target.setForegroundColor(IndexedColor::Green); break;
                    case 33: target.setForegroundColor(IndexedColor::Yellow); break;
                    case 34: target.setForegroundColor(IndexedColor::Blue); break;
                    case 35: target.setForegroundColor(IndexedColor::Magenta); break;
                    case 36: target.setForegroundColor(IndexedColor::Cyan); break;
                    case 37: target.setForegroundColor(IndexedColor::White); break;
                    case 38: target.setForegroundColor(parseColor(seq, &i)); break;
                    case 39: target.setForegroundColor(DefaultColor()); break;
                    case 40: target.setBackgroundColor(IndexedColor::Black); break;
                    case 41: target.setBackgroundColor(IndexedColor::Red); break;
                    case 42: target.setBackgroundColor(IndexedColor::Green); break;
                    case 43: target.setBackgroundColor(IndexedColor::Yellow); break;
                    case 44: target.setBackgroundColor(IndexedColor::Blue); break;
                    case 45: target.setBackgroundColor(IndexedColor::Magenta); break;
                    case 46: target.setBackgroundColor(IndexedColor::Cyan); break;
                    case 47: target.setBackgroundColor(IndexedColor::White); break;
                    case 48: target.setBackgroundColor(parseColor(seq, &i)); break;
                    case 49: target.setBackgroundColor(DefaultColor()); break;
                    case 51: target.setGraphicsRendition(GraphicsRendition::Framed); break;
                    case 53: target.setGraphicsRendition(GraphicsRendition::Overline); break;
                    case 54: target.setGraphicsRendition(GraphicsRendition::NoFramed); break;
                    case 55: target.setGraphicsRendition(GraphicsRendition::NoOverline); break;
                    // 58 is reserved, but used for setting underline/decoration colors by some other VTEs
                    // (such as mintty, kitty, libvte)
                    case 58: target.setUnderlineColor(parseColor(seq, &i)); break;
                    case 90: target.setForegroundColor(BrightColor::Black); break;
                    case 91: target.setForegroundColor(BrightColor::Red); break;
                    case 92: target.setForegroundColor(BrightColor::Green); break;
                    case 93: target.setForegroundColor(BrightColor::Yellow); break;
                    case 94: target.setForegroundColor(BrightColor::Blue); break;
                    case 95: target.setForegroundColor(BrightColor::Magenta); break;
                    case 96: target.setForegroundColor(BrightColor::Cyan); break;
                    case 97: target.setForegroundColor(BrightColor::White); break;
                    case 100: target.setBackgroundColor(BrightColor::Black); break;
                    case 101: target.setBackgroundColor(BrightColor::Red); break;
                    case 102: target.setBackgroundColor(BrightColor::Green); break;
                    case 103: target.setBackgroundColor(BrightColor::Yellow); break;
                    case 104: target.setBackgroundColor(BrightColor::Blue); break;
                    case 105: target.setBackgroundColor(BrightColor::Magenta); break;
                    case 106: target.setBackgroundColor(BrightColor::Cyan); break;
                    case 107: target.setBackgroundColor(BrightColor::White); break;
                    default: break; // TODO: logInvalidCSI("Invalid SGR number: {}", seq.param(i));
                }
            }
            return ApplyResult::Ok;
        }

        /// Reads the foreground/background palette indices a DEC color-assignment sequence carries in
        /// parameters 1 and 2, and resolves them against @p palette. Shared by DECAC and DECATC, which
        /// declare the same pair of indices (0..255, following Windows Terminal's extended range).
        ///
        /// Both indices are validated even when the caller consumes only one of them: DECAC's
        /// window-frame item carries a foreground of its own in the DEC model (Windows Terminal keeps a
        /// FrameForeground alias for it) though Contour's tab strip derives its label color by contrast.
        /// An out-of-range index makes the whole sequence malformed, not a parameter to ignore.
        ///
        /// @param seq The sequence being applied.
        /// @param palette The palette to resolve the indices against.
        /// @return The resolved colors, or nullopt if either index is out of range.
        [[nodiscard]] std::optional<RGBColorPair> readAssignedColorPair(Sequence const& seq,
                                                                        ColorPalette const& palette) noexcept
        {
            auto const fg = seq.param<unsigned>(1);
            auto const bg = seq.param<unsigned>(2);
            if (fg > 255 || bg > 255)
                return std::nullopt;
            return RGBColorPair { .foreground = palette.indexedColor(fg),
                                  .background = palette.indexedColor(bg) };
        }

        ApplyResult ANSIDSR(Sequence const& seq, Screen& screen)
        {
            switch (seq.param(0))
            {
                case 5: screen.deviceStatusReport(); return ApplyResult::Ok;
                case 6: screen.reportCursorPosition(); return ApplyResult::Ok;
                default: return ApplyResult::Unsupported;
            }
        }

        /// One DECDSR (`CSI ? Ps n`) request whose answer never changes.
        ///
        /// A terminal with no printer, no user-defined keys, no macro memory and no session multiplexer
        /// still has to *say so*, in the words the standard gives it. Most of DECDSR is exactly that: a
        /// fixed answer to a fixed question. The two that are not -- the cursor's position, and the
        /// checksum of a macro memory that does not exist -- are handled apart from this table.
        ///
        /// Adding a report is adding a row.
        struct DeviceStatusReport
        {
            unsigned request; ///< The Ps that asks for it.
            std::string_view reply;
            std::string_view comment;
        };

        constexpr auto DeviceStatusReports = std::array {
            DeviceStatusReport { .request = 15, .reply = "\033[?13n", .comment = "Printer port: no printer" },
            DeviceStatusReport {
                .request = 25, .reply = "\033[?20n", .comment = "User-defined keys: unlocked" },
            DeviceStatusReport { .request = 26,
                                 .reply = "\033[?27;0;0;5n",
                                 .comment = "Keyboard: language unknown, ready, PCXAL" },
            DeviceStatusReport {
                .request = 53, .reply = "\033[?50n", .comment = "Locator: none, until DECELR is honoured" },
            DeviceStatusReport { .request = 55,
                                 .reply = "\033[?50n",
                                 .comment = "Locator, xterm's spelling of the same question" },
            DeviceStatusReport { .request = 56, .reply = "\033[?57;0n", .comment = "Locator type: unknown" },
            DeviceStatusReport { .request = 62,
                                 .reply = "\033[0*{",
                                 .comment = "DECMSR: no macro space, because there are no macros" },
            DeviceStatusReport { .request = 75,
                                 .reply = "\033[?70n",
                                 .comment = "Data integrity: no errors. There is no link to have any." },
            DeviceStatusReport {
                .request = 85, .reply = "\033[?83n", .comment = "Sessions: not configured for multiple" },
        };

        /// DECDSR -- `CSI ? Ps n`, the device status reports.
        ApplyResult DSR(Sequence const& seq, Screen& screen)
        {
            auto const request = seq.param_or(0, 0u);

            switch (request)
            {
                case 6:
                    // DECXCPR: the cursor's position, and the page it is on.
                    screen.reportExtendedCursorPosition();
                    return ApplyResult::Ok;
                case 63:
                    // DECCKSR: the checksum of the macro memory. There are no macros, so it is zero --
                    // but the reply still carries back the id the request was tagged with, so that an
                    // application issuing several can tell the answers apart.
                    screen.reportMacroSpaceChecksum(seq.param_or(1, 0u));
                    return ApplyResult::Ok;
                case ColorPaletteUpdateDsrRequestId:
                    screen.reportColorPaletteUpdate();
                    return ApplyResult::Ok;
                default: break;
            }

            // `auto const`, not `auto const*`: libstdc++'s array iterator is a raw pointer but MSVC's
            // is a class type. @see setDynamicColorCommand in primitives.h.
            auto const report = std::ranges::find(DeviceStatusReports, request, &DeviceStatusReport::request);
            if (report == DeviceStatusReports.end())
                return ApplyResult::Unsupported;

            screen.reply(report->reply);
            return ApplyResult::Ok;
        }

        ApplyResult DECRQPSR(Sequence const& seq, Screen& screen)
        {
            if (seq.parameterCount() != 1)
                return ApplyResult::Invalid; // -> error
            else if (seq.param(0) == 1)
            {
                screen.reportCursorInformation();
                return ApplyResult::Ok;
            }
            else if (seq.param(0) == 2)
            {
                screen.requestTabStops();
                return ApplyResult::Ok;
            }
            else
                return ApplyResult::Invalid;
        }

        ApplyResult DECSCUSR(Sequence const& seq, Terminal& terminal)
        {
            if (seq.parameterCount() <= 1)
            {
                switch (seq.param_or(0, Sequence::Parameter { 0 }))
                {
                    case 0:
                        // NB: This deviates from DECSCUSR, which is documented to reset to blinking block.
                        terminal.setCursorStyle(terminal.factorySettings().cursorDisplay,
                                                terminal.factorySettings().cursorShape);
                        break;
                    case 1: terminal.setCursorStyle(CursorDisplay::Blink, CursorShape::Block); break;
                    case 2: terminal.setCursorStyle(CursorDisplay::Steady, CursorShape::Block); break;
                    case 3: terminal.setCursorStyle(CursorDisplay::Blink, CursorShape::Underscore); break;
                    case 4: terminal.setCursorStyle(CursorDisplay::Steady, CursorShape::Underscore); break;
                    case 5: terminal.setCursorStyle(CursorDisplay::Blink, CursorShape::Bar); break;
                    case 6: terminal.setCursorStyle(CursorDisplay::Steady, CursorShape::Bar); break;
                    default: return ApplyResult::Invalid;
                }
                return ApplyResult::Ok;
            }
            else
                return ApplyResult::Invalid;
        }

        ApplyResult EL(Sequence const& seq, Screen& screen)
        {
            switch (seq.param_or(0, Sequence::Parameter { 0 }))
            {
                case 0: screen.clearToEndOfLine(); break;
                case 1: screen.clearToBeginOfLine(); break;
                case 2: screen.clearLine(); break;
                default: return ApplyResult::Invalid;
            }
            return ApplyResult::Ok;
        }

        ApplyResult TBC(Sequence const& seq, Screen& screen)
        {
            if (seq.parameterCount() != 1)
            {
                screen.horizontalTabClear(HorizontalTabClear::UnderCursor);
                return ApplyResult::Ok;
            }

            switch (seq.param(0))
            {
                case 0: screen.horizontalTabClear(HorizontalTabClear::UnderCursor); break;
                case 3: screen.horizontalTabClear(HorizontalTabClear::AllTabs); break;
                default: return ApplyResult::Invalid;
            }
            return ApplyResult::Ok;
        }

        inline std::unordered_map<std::string_view, std::string_view> parseSubParamKeyValuePairs(
            std::string_view const& s)
        {
            return crispy::splitKeyValuePairs(s, ':');
        }

        /// OSC 10..19 -- sets or queries the dynamic colors, starting at the one the sequence names.
        ///
        /// One sequence carries one specification per color, walking upward from @p firstName: a plain
        /// `OSC 10 ; fg ; bg ST` sets the foreground *and* the background. An empty specification skips
        /// its color, "?" queries it, and any other is an X11 color specification to set it to.
        ///
        /// A query is answered with the OSC command of the color it reports rather than the one the
        /// sequence began at, which is what makes `OSC 10 ; ? ; ? ST` answer with an OSC 10 *and* an
        /// OSC 11. Contour used to read the entire payload as one specification, so that sequence
        /// parsed as the color "?;?", failed, and was answered with nothing at all -- leaving the
        /// application to read some later sequence's reply in place of the two it was waiting for.
        ///
        /// @see xterm's ChangeColorsRequest() in misc.c.
        ApplyResult setOrRequestDynamicColor(Sequence const& seq, Screen& screen, DynamicColorName firstName)
        {
            auto command = setDynamicColorCommand(firstName);
            auto result = ApplyResult::Ok;

            crispy::split(
                std::string_view { seq.intermediateCharacters() }, ';', [&](std::string_view specification) {
                    auto const name = getChangeDynamicColorCommand(command);
                    if (command > LastDynamicColorCommand)
                        return false; // Ran past OSC 19; there is no color left to address.
                    ++command;

                    // An empty specification skips its color, as does one naming a color we do not model.
                    if (specification.empty() || !name.has_value())
                        return true;

                    if (specification == "?"sv)
                        screen.requestDynamicColor(*name);
                    else if (auto const color = vtbackend::parseColor(specification); color.has_value())
                        screen.setDynamicColor(*name, color.value());
                    else
                    {
                        // As in xterm, the first specification we cannot parse ends the sequence.
                        result = ApplyResult::Invalid;
                        return false;
                    }

                    return true;
                });

            return result;
        }

        /// How a color-palette sequence names a color, and how it reports one back.
        ///
        /// `OSC 4` and `OSC 5` are the same sequence twice over -- pairs of index and specification,
        /// answered in kind -- differing only in which colors an index reaches. So they share an
        /// implementation, and this is all that separates them.
        struct ColorPaletteSelector
        {
            /// The OSC command, which is also what a report is tagged with: 4 or 5.
            unsigned command;

            /// Maps the index an application names to the palette slot the color lives in.
            std::optional<size_t> (*slotOf)(unsigned index) noexcept;

            /// How many indices this selector reaches, i.e. what a reset with no index at all covers.
            unsigned indexCount;
        };

        constexpr auto IndexedColorSelector =
            ColorPaletteSelector { .command = 4,
                                   .slotOf = &paletteSlotOfColorIndex,
                                   .indexCount =
                                       static_cast<unsigned>(IndexedColorCount + SpecialColorCount) };
        constexpr auto SpecialColorSelector =
            ColorPaletteSelector { .command = 5,
                                   .slotOf = &paletteSlotOfSpecialColor,
                                   .indexCount = static_cast<unsigned>(SpecialColorCount) };

        /// `OSC 4` / `OSC 5` -- sets or queries palette colors, as index/specification pairs.
        ///
        /// As in xterm, the first pair that cannot be read ends the sequence: a bad index or an
        /// unparseable specification stops the walk rather than skipping to the next pair.
        ///
        /// @see xterm's ChangeAnsiColorRequest() in misc.c.
        ApplyResult setOrRequestColorPalette(Sequence const& seq,
                                             Terminal& terminal,
                                             ColorPaletteSelector const& selector)
        {
            // The index the application named, kept as it gave it: a report echoes that index, not the
            // slot we happen to keep the color in.
            auto pending = std::optional<std::pair<unsigned, size_t>> {};

            auto const ok =
                crispy::split(std::string_view { seq.intermediateCharacters() }, ';', [&](string_view value) {
                    if (!pending.has_value())
                    {
                        auto const index = crispy::to_integer<10, unsigned>(value);
                        if (!index.has_value())
                            return false;

                        auto const slot = selector.slotOf(*index);
                        if (!slot.has_value())
                            return false;

                        pending = std::pair { *index, *slot };
                        return true;
                    }

                    auto const [index, slot] = *pending;

                    if (value == "?"sv)
                        terminal.reply("\033]{};{};{}\033\\",
                                       selector.command,
                                       index,
                                       colorSpecification(terminal.colorPalette().palette.at(slot)));
                    else if (auto const color = vtbackend::parseColor(value); color.has_value())
                        terminal.colorPalette().palette.at(slot) = color.value();
                    else
                        return false;

                    pending = std::nullopt;
                    return true;
                });

            return ok ? ApplyResult::Ok : ApplyResult::Invalid;
        }

        /// `OSC 104` / `OSC 105` -- resets palette colors to the ones the terminal was configured with.
        ///
        /// With no index at all, every index the selector reaches is reset -- and only those. What the
        /// sequence can *address* is what it can reset: `OSC 104` covers the indexed colors plus the
        /// special ones (xterm walks its whole `Acolors` the same way), `OSC 105` only the special ones.
        ///
        /// Notably that is not the same as the whole ColorPalette. The dynamic colors -- default
        /// foreground/background, cursor, mouse, selection -- share the struct but are addressed by
        /// `OSC 10`..`19` and reset by `OSC 110`..`119` (xterm keeps them apart as `Tcolors`). Resetting
        /// them here would withdraw a background an application set with `OSC 11` that nothing asked to
        /// reset. Otherwise the indices are a ';'-separated list.
        ApplyResult resetColorPalette(Sequence const& seq,
                                      Terminal& terminal,
                                      ColorPaletteSelector const& selector)
        {
            if (seq.intermediateCharacters().empty())
            {
                for (auto const index: std::views::iota(0u, selector.indexCount))
                    if (auto const slot = selector.slotOf(index); slot.has_value())
                        terminal.colorPalette().palette.at(*slot) =
                            terminal.defaultColorPalette().palette.at(*slot);
                return ApplyResult::Ok;
            }

            auto const ok =
                crispy::split(std::string_view { seq.intermediateCharacters() }, ';', [&](string_view value) {
                    auto const index = crispy::to_integer<10, unsigned>(value);
                    if (!index.has_value())
                        return false;

                    auto const slot = selector.slotOf(*index);
                    if (!slot.has_value())
                        return false;

                    terminal.colorPalette().palette.at(*slot) =
                        terminal.defaultColorPalette().palette.at(*slot);
                    return true;
                });

            return ok ? ApplyResult::Ok : ApplyResult::Invalid;
        }

        int toInt(string_view value)
        {
            int out = 0;
            for (auto const ch: value)
            {
                if (!(ch >= '0' && ch <= '9'))
                    return 0;

                out = (out * 10) + (ch - '0');
            }
            return out;
        }

        ApplyResult setAllFont(Sequence const& seq, Terminal& terminal)
        {
            // [read]  OSC 60 ST
            // [write] OSC 60 ; size ; regular ; bold ; italic ; bold italic ST
            auto const& params = seq.intermediateCharacters();
            auto const splits = crispy::split(params, ';');
            auto const param = [&](unsigned index) -> string_view {
                if (index < splits.size())
                    return splits.at(index);
                else
                    return {};
            };
            auto const emptyParams = [&]() -> bool {
                for (auto const& x: splits)
                    if (!x.empty())
                        return false;
                return true;
            }();
            if (emptyParams)
            {
                auto const fonts = terminal.getFontDef();
                terminal.reply("\033]60;{};{};{};{};{};{}\033\\",
                               int(fonts.size * 100), // precission-shift
                               fonts.regular,
                               fonts.bold,
                               fonts.italic,
                               fonts.boldItalic,
                               fonts.emoji);
            }
            else
            {
                auto const size = double(toInt(param(0))) / 100.0;
                auto const regular = string(param(1));
                auto const bold = string(param(2));
                auto const italic = string(param(3));
                auto const boldItalic = string(param(4));
                auto const emoji = string(param(5));
                terminal.setFontDef({ .size = size,
                                      .regular = regular,
                                      .bold = bold,
                                      .italic = italic,
                                      .boldItalic = boldItalic,
                                      .emoji = emoji });
            }
            return ApplyResult::Ok;
        }

        ApplyResult setFont(Sequence const& seq, Terminal& terminal)
        {
            auto const& params = seq.intermediateCharacters();
            auto const splits = crispy::split(params, ';');

            if (splits.size() != 1)
                return ApplyResult::Invalid;

            if (splits[0] != "?"sv)
            {
                auto fontDef = FontDef {};
                fontDef.regular = splits[0];
                terminal.setFontDef(fontDef);
            }
            else
            {
                auto const fonts = terminal.getFontDef();
                terminal.reply("\033]50;{}\033\\", fonts.regular);
            }

            return ApplyResult::Ok;
        }

        ApplyResult clipboard(Sequence const& seq, Terminal& terminal)
        {
            // OSC 52: `OSC 52 ; Pc ; Pd ST`. Pd is base64 data to store, or "?" to read the clipboard
            // back. Contour models the clipboard ('c') and the default/empty selection.
            auto const& params = seq.intermediateCharacters();
            auto const splits = crispy::split(params, ';');
            if (splits.size() != 2 || !(splits[0] == "c" || splits[0].empty()))
                return ApplyResult::Invalid;

            if (splits[1] == "?")
                terminal.requestClipboardRead(splits[0]); // read (gated by Settings::allowClipboardRead)
            else
                terminal.copyToClipboard(crispy::base64::decode(splits[1]));
            return ApplyResult::Ok;
        }

        ApplyResult NOTIFY(Sequence const& seq, Screen& screen)
        {
            auto const& value = seq.intermediateCharacters();
            if (auto const splits = crispy::split(value, ';'); splits.size() == 3 && splits[0] == "notify")
            {
                screen.notify(string(splits[1]), string(splits[2]));
                return ApplyResult::Ok;
            }
            else
                return ApplyResult::Unsupported;
        }

        /// OSC 9 — ConEmu-style notification and progress indicator.
        ///
        /// Simple notification: OSC 9 ; <message> ST
        /// Progress indicator:  OSC 9 ; 4 ; <state> ; <progress> ST

        ApplyResult CONEMU(Sequence const& seq, Screen& screen)
        {
            auto const& value = seq.intermediateCharacters();
            if (value.empty())
                return ApplyResult::Invalid;

            // Check for progress indicator: "4;state;progress"
            if (value.size() >= 2 && value[0] == '4' && value[1] == ';')
            {
                // Progress indicator — currently, we silently accept it.
                // TODO: expose progress state to the GUI layer
                return ApplyResult::Ok;
            }

            // Simple notification: the payload is the notification message
            screen.notify(string("ConEmu"), string(value));
            return ApplyResult::Ok;
        }

        /// OSC 99 — Kitty Desktop Notification protocol.
        ApplyResult DESKTOPNOTIFY(Sequence const& seq, Terminal& terminal)
        {
            auto const& value = seq.intermediateCharacters();
            terminal.desktopNotificationManager().handleOSC99(value, terminal);
            return ApplyResult::Ok;
        }

        ApplyResult SETCWD(Sequence const& seq, Screen& screen)
        {
            string const& url = seq.intermediateCharacters();
            screen.setCurrentWorkingDirectory(url);
            return ApplyResult::Ok;
        }

        ApplyResult CAPTURE(Sequence const& seq, Terminal& terminal)
        {
            // CSI Mode ; [; Count] t
            //
            // Mode: 0 = physical lines
            //       1 = logical lines (unwrapped)
            //
            // Count: number of lines to capture from main page aera's bottom upwards
            //        If omitted or 0, the main page area's line count will be used.

            auto const logicalLines = seq.param_or(0, 0);
            if (logicalLines != 0 && logicalLines != 1)
                return ApplyResult::Invalid;

            auto const lineCount = LineCount(seq.param_or(1, *terminal.pageSize().lines));

            terminal.requestCaptureBuffer(lineCount, logicalLines);

            return ApplyResult::Ok;
        }

        /// The confidence tests DECTST can be asked to run, and what each means here.
        ///
        /// Adding a test is adding a row. @see documentation::DECTST.
        struct ConfidenceTest
        {
            unsigned id;

            /// Whether running it resets the terminal.
            ///
            /// True only for the power-up self test: it is a *power-up*, and that is the one effect of
            /// any of these tests a program can observe. The rest loop data back through an RS-232
            /// port, a printer port, a modem's control lines or a parallel port -- hardware a software
            /// terminal does not have, so there is nothing to drive and nothing that can fail.
            bool resetsTerminal;
        };

        constexpr auto ConfidenceTests = std::array {
            ConfidenceTest { .id = 0, .resetsTerminal = true },  // all tests -- includes the power-up one
            ConfidenceTest { .id = 1, .resetsTerminal = true },  // power-up self test
            ConfidenceTest { .id = 2, .resetsTerminal = false }, // RS-232 data loopback
            ConfidenceTest { .id = 3, .resetsTerminal = false }, // printer port loopback
            ConfidenceTest { .id = 4, .resetsTerminal = false }, // speed select and speed indicator
            ConfidenceTest { .id = 5, .resetsTerminal = false }, // reserved -- no action
            ConfidenceTest { .id = 6, .resetsTerminal = false }, // RS-232 modem control line loopback
            ConfidenceTest { .id = 7, .resetsTerminal = false }, // EIA-423 port loopback
            ConfidenceTest { .id = 8, .resetsTerminal = false }, // parallel port loopback
            ConfidenceTest { .id = 9, .resetsTerminal = false }, // repeat (loop on) the other tests
        };

        /// The Ps1 values that mean "invoke the tests named by the rest of the parameters".
        ///
        /// Two, because the sequence changed shape between generations: a VT100 invokes with 2, a VT510
        /// and later with 4. Both are honoured -- a terminal reporting VT525 is still driven by
        /// VT100-era software, and vttest sends the VT100 form to every terminal it meets.
        constexpr auto ConfidenceTestInvokeOpcodes = std::array { 2U, 4U };

        /// DECTST: runs the built-in confidence tests.
        ///
        /// Every test passes -- there is no hardware here to fail one -- and a failure would be
        /// reported by drawing a diagnostic code rather than by replying, so a terminal that passes
        /// writes nothing. @see documentation::DECTST.
        ApplyResult invokeConfidenceTest(Sequence const& seq, Terminal& terminal)
        {
            auto const invoke = seq.param_or(0, 0U);
            if (std::ranges::find(ConfidenceTestInvokeOpcodes, invoke) == ConfidenceTestInvokeOpcodes.end())
                return ApplyResult::Invalid;

            // No test named means no test run: DECTST invokes what it is asked for, and `CSI 2 y` asks
            // for nothing. (Ps2 defaults to 0 -- "all tests" -- only when it is present and empty.)
            auto reset = false;
            for (auto const i: std::views::iota(size_t { 1 }, seq.parameterCount()))
            {
                auto const requested = seq.param_or(i, 0U);
                // `auto const`, not `auto const*`: libstdc++'s array iterator is a raw pointer but
                // MSVC's is a class type. @see setDynamicColorCommand in primitives.h.
                auto const test = std::ranges::find(
                    ConfidenceTests, requested, [](ConfidenceTest const& t) { return t.id; });
                if (test == ConfidenceTests.end())
                    return ApplyResult::Invalid;
                reset = reset || test->resetsTerminal;
            }

            // Deferred to after the whole string is validated, so that an invalid test later in the
            // string cannot leave the terminal half-reset by an earlier valid one.
            if (reset)
                terminal.hardReset();

            return ApplyResult::Ok;
        }

        ApplyResult setTitleModes(Sequence const& seq, Terminal& terminal, bool enable)
        {
            // XTSMTITLE (`CSI > Ps t`, enable) and XTRMTITLE (`CSI > Ps T`, disable): each parameter names
            // a title-mode feature (0..3); an out-of-range parameter is ignored, matching xterm's
            // ValidTitleMode() guard. With no parameters at all, xterm resets every title mode to its
            // default (all disabled) -- for XTSMTITLE and XTRMTITLE alike.
            if (seq.parameterCount() == 0)
            {
                terminal.resetTitleModes();
                return ApplyResult::Ok;
            }

            for (auto const i: std::views::iota(size_t { 0 }, seq.parameterCount()))
            {
                auto const value = seq.param<unsigned>(i);
                if (value < TitleModeFeatureCount)
                    terminal.setTitleModeFeature(static_cast<TitleModeFeature>(value), enable);
            }

            return ApplyResult::Ok;
        }

        ApplyResult HYPERLINK(Sequence const& seq, Screen& screen)
        {
            auto const& value = seq.intermediateCharacters();
            // hyperlink_OSC ::= OSC '8' ';' params ';' URI
            // params := pair (':' pair)*
            // pair := TEXT '=' TEXT
            if (auto const pos = value.find(';'); pos != Sequence::Intermediaries::npos)
            {
                auto const paramsStr = value.substr(0, pos);
                auto const params = parseSubParamKeyValuePairs(paramsStr);

                auto id = string {};
                if (auto const p = params.find("id"); p != params.end())
                    id = p->second;

                if (pos + 1 != value.size())
                    screen.hyperlink(std::move(id), value.substr(pos + 1));
                else
                    screen.hyperlink(std::move(id), string {});

                return ApplyResult::Ok;
            }
            else
                screen.hyperlink(string {}, string {});

            return ApplyResult::Ok;
        }

        ApplyResult saveDECModes(Sequence const& seq, Terminal& terminal)
        {
            vector<DECMode> modes;
            for (size_t i = 0; i < seq.parameterCount(); ++i)
                if (optional<DECMode> mode = fromDECModeNum(seq.param(i)); mode.has_value())
                    modes.push_back(mode.value());
            terminal.saveModes(modes);
            return ApplyResult::Ok;
        }

        ApplyResult restoreDECModes(Sequence const& seq, Terminal& terminal)
        {
            vector<DECMode> modes;
            for (size_t i = 0; i < seq.parameterCount(); ++i)
                if (optional<DECMode> mode = fromDECModeNum(seq.param(i)); mode.has_value())
                    modes.push_back(mode.value());
            terminal.restoreModes(modes);
            return ApplyResult::Ok;
        }

        /// XTWINOPS (`CSI Ps ; Ps ; Ps t`) -- the window manipulation and window report operations.
        ///
        /// The operation is named by the *first* parameter, and each operation reads the parameters that
        /// follow it. Dispatching on the parameter *count* first -- as this used to -- reads
        /// `CSI 4 ; 100 t` (resize to a height of 100 pixels) as "resize to the display's size", because
        /// that sequence happens to carry two parameters rather than three.
        ///
        /// @see xterm's ctlseqs, "Window manipulation".
        ApplyResult WINDOWMANIP(Sequence const& seq, Terminal& terminal)
        {
            // "Omitted parameters reuse the current height or width. Zero parameters use the display's
            // height or width." -- xterm's ctlseqs. So a dimension has three readings, not two, and the
            // display's size reaches the terminal only through the frontend. @see Terminal::windowState().
            auto const dimension = [&](size_t index, unsigned current, unsigned display) {
                auto const value = seq.param_opt<unsigned>(index);
                if (!value.has_value())
                    return current;
                return *value != 0 ? *value : display;
            };

            switch (seq.param_or(0, 0))
            {
                case 1: // De-iconify.
                    terminal.requestWindowIconify(false);
                    return ApplyResult::Ok;
                case 2: // Iconify.
                    terminal.requestWindowIconify(true);
                    return ApplyResult::Ok;
                case 3: // Move the window's top-left corner to [x, y].
                    terminal.requestWindowMove(
                        WindowPosition { .x = seq.param_or<int>(1, 0), .y = seq.param_or<int>(2, 0) });
                    return ApplyResult::Ok;
                case 4: { // Resize the text area, in pixels.
                    auto const current = terminal.pixelSize();
                    auto const display = terminal.screenPixelSize();
                    terminal.requestWindowResize(ImageSize {
                        Width::cast_from(dimension(2, unbox(current.width), unbox(display.width))),
                        Height::cast_from(dimension(1, unbox(current.height), unbox(display.height))),
                    });
                    return ApplyResult::Ok;
                }
                case 8: { // Resize the text area, in characters.
                    auto const current = terminal.pageSize();
                    auto const display = terminal.screenPageSize();
                    terminal.requestWindowResize(PageSize {
                        .lines =
                            LineCount::cast_from(dimension(1, unbox(current.lines), unbox(display.lines))),
                        .columns = ColumnCount::cast_from(
                            dimension(2, unbox(current.columns), unbox(display.columns))),
                    });
                    return ApplyResult::Ok;
                }
                case 9: // Maximize the window, or restore it.
                    if (auto const how = windowMaximizeOf(seq.param_or(1, 0)); how.has_value())
                    {
                        terminal.requestWindowMaximize(*how);
                        return ApplyResult::Ok;
                    }
                    return ApplyResult::Invalid;
                case 10: // Full screen, or out of it.
                    if (auto const how = windowFullScreenOf(seq.param_or(1, 0)); how.has_value())
                    {
                        terminal.requestWindowFullScreen(*how);
                        return ApplyResult::Ok;
                    }
                    return ApplyResult::Invalid;
                case 11: // Report whether the window is iconified.
                    terminal.reply("\033[{}t", terminal.windowState().iconified ? 2 : 1);
                    return ApplyResult::Ok;
                case 13: // Report the window's position.
                    terminal.reply("\033[3;{};{}t",
                                   terminal.windowState().position.x,
                                   terminal.windowState().position.y);
                    return ApplyResult::Ok;
                case 14: // Report the text area's size in pixels; `CSI 14 ; 2 t` the window's.
                    terminal.primaryScreen().requestPixelSize(
                        seq.param_or(1, 0) == 2 ? RequestPixelSize::WindowArea : RequestPixelSize::TextArea);
                    return ApplyResult::Ok;
                case 15: // Report the screen's size in pixels.
                    terminal.primaryScreen().requestPixelSize(RequestPixelSize::ScreenArea);
                    return ApplyResult::Ok;
                case 16: // Report the cell's size in pixels.
                    terminal.primaryScreen().requestPixelSize(RequestPixelSize::CellArea);
                    return ApplyResult::Ok;
                case 18: // Report the text area's size in characters.
                    terminal.primaryScreen().requestCharacterSize(RequestPixelSize::TextArea);
                    return ApplyResult::Ok;
                case 19: // Report the screen's size in characters.
                    terminal.primaryScreen().requestCharacterSize(RequestPixelSize::ScreenArea);
                    return ApplyResult::Ok;
                case 20: // Report the icon's title, as OSC L <title> ST.
                    // The title is hex-encoded when the QueryHex title mode is active (XTSMTITLE).
                    terminal.reply("\033]L{}\033\\", terminal.encodeTitleForReport(terminal.iconTitle()));
                    return ApplyResult::Ok;
                case 21: // Report the window's title, as OSC l <title> ST.
                    terminal.reply("\033]l{}\033\\", terminal.encodeTitleForReport(terminal.windowTitle()));
                    return ApplyResult::Ok;
                case 22: // XTPUSHTITLE
                    if (auto const kinds = titleKindsOf(seq.param_or(1, 0)); kinds.has_value())
                    {
                        terminal.saveTitles(*kinds);
                        return ApplyResult::Ok;
                    }
                    return ApplyResult::Invalid;
                case 23: // XTPOPTITLE
                    if (auto const kinds = titleKindsOf(seq.param_or(1, 0)); kinds.has_value())
                    {
                        terminal.restoreTitles(*kinds);
                        return ApplyResult::Ok;
                    }
                    return ApplyResult::Invalid;
                default:
                    // DECSLPP: an operation of 24 or more sets the page's length to that many lines. It
                    // shares its final byte with XTWINOPS, and xterm resolves the collision exactly here
                    // -- by the value of the first parameter.
                    if (auto const lines = seq.param_or(0, 0); lines >= 24)
                    {
                        terminal.requestWindowResize(
                            PageSize { .lines = LineCount::cast_from(lines),
                                       .columns = terminal.totalPageSize().columns });
                        return ApplyResult::Ok;
                    }
                    return ApplyResult::Unsupported;
            }
        }

        ApplyResult XTSMGRAPHICS(Sequence const& seq, Screen& screen)
        {
            auto const pi = seq.param<unsigned>(0);
            auto const pa = seq.param<unsigned>(1);
            auto const pv = seq.param_or<unsigned>(2, 0);
            auto const pu = seq.param_or<unsigned>(3, 0);

            auto const item = [&]() -> optional<XtSmGraphics::Item> {
                switch (pi)
                {
                    case 1: return XtSmGraphics::Item::NumberOfColorRegisters;
                    case 2: return XtSmGraphics::Item::SixelGraphicsGeometry;
                    case 3: return XtSmGraphics::Item::ReGISGraphicsGeometry;
                    default: return nullopt;
                }
            }();
            if (!item.has_value())
                return ApplyResult::Invalid;

            auto const action = [&]() -> optional<XtSmGraphics::Action> {
                switch (pa)
                {
                    case 1: return XtSmGraphics::Action::Read;
                    case 2: return XtSmGraphics::Action::ResetToDefault;
                    case 3: return XtSmGraphics::Action::SetToValue;
                    case 4: return XtSmGraphics::Action::ReadLimit;
                    default: return nullopt;
                }
            }();
            if (!action.has_value())
                return ApplyResult::Invalid;

            if (*item != XtSmGraphics::Item::NumberOfColorRegisters
                && *action == XtSmGraphics::Action::SetToValue && (!pv || !pu))
                return ApplyResult::Invalid;

            auto const value = [&]() -> XtSmGraphics::Value {
                using Action = XtSmGraphics::Action;
                switch (*action)
                {
                    case Action::Read:
                    case Action::ResetToDefault:
                    case Action::ReadLimit: return std::monostate {};
                    case Action::SetToValue:
                        return *item == XtSmGraphics::Item::NumberOfColorRegisters
                                   ? XtSmGraphics::Value { pv }
                                   : XtSmGraphics::Value { ImageSize { Width(pv), Height(pu) } };
                }
                return std::monostate {};
            }();

            screen.smGraphics(*item, *action, value);

            return ApplyResult::Ok;
        }
    } // namespace
} // namespace impl
// }}}

void Screen::executeControlCode(char controlCode)
{
#if defined(LIBTERMINAL_LOG_TRACE)
    // Flush any pending text trace before processing the control code.
    // Without this, when the parser's bulk text optimization processes
    // text → C0 → text inline (without leaving Ground state), writeTextEnd()
    // is never called, causing gaps in the trace log.
    writeTextEnd();
    if (vtTraceSequenceLog)
        vtTraceSequenceLog()(
            "control U+{:02X} ({})", controlCode, to_string(static_cast<ControlCode::C0>(controlCode)));
#endif

    _terminal->incrementInstructionCounter();
    switch (controlCode)
    {
        case 0x00: // NUL
            break;
        case BEL.finalSymbol: _terminal->bell(); break;
        case BS.finalSymbol: backspace(); break;
        case TAB.finalSymbol: moveCursorToNextTab(); break;
        case LF.finalSymbol: linefeed(); break;
        case VT.finalSymbol:
            // VT (Vertical Tab) and FF (Form Feed) are treated exactly like LF: xterm routes all three
            // through its index-with-optional-newline path, so they index and -- in linefeed mode (LNM)
            // -- also return the carriage. linefeed() is that shared path; a bare index() would skip
            // LNM's carriage return (as well as linefeed()'s BCE scroll and below-region page clamp).
            [[fallthrough]];
        case FF.finalSymbol: linefeed(); break;
        case LS1.finalSymbol: // (SO)
            // Invokes G1 character set into GL. G1 is designated by a select-character-set (SCS) sequence.
            _cursor.charsets.lockingShift(CharsetTable::G1);
            break;
        case LS0.finalSymbol: // (SI)
            // Invoke G0 character set into GL. G0 is designated by a select-character-set sequence (SCS).
            _cursor.charsets.lockingShift(CharsetTable::G0);
            break;
        case CR.finalSymbol: moveCursorToBeginOfLine(); break;
        case 0x37: saveCursor(); break;
        case 0x38: restoreCursor(); break;
        default:
            // if (VTParserLog)
            //     VTParserLog()("Unsupported C0 sequence: {}", crispy::escape((uint8_t) controlCode));
            break;
    }

    // A control code moves the cursor and scrolls the page just as a sequence does -- LF below a
    // scrolling region once walked the cursor clean off the page -- so it is held to the same
    // invariants. Compiled out unless CONTOUR_VERIFY_STATE.
    _terminal->verifyState();
}

void Screen::saveCursor()
{
    // https://vt100.net/docs/vt510-rm/DECSC.html
    _savedCursor = _cursor;
    _terminal->saveCursorPage();
}

void Screen::restoreCursor()
{
    // https://vt100.net/docs/vt510-rm/DECRC.html
    _terminal->restoreCursorPage();
    restoreCursor(_savedCursor);
}

void Screen::restoreCursor(Cursor const& savedCursor)
{
    // DECRC restores cursor state -- position, SGR, charsets, origin mode (DECOM), and the last-column
    // (wrap-pending) flag -- but NOT the autowrap mode (DECAWM): that is a terminal mode, not cursor
    // state (DEC STD 070), so a DECRC after a DECRESET DECAWM leaves autowrap off. Matches xterm.
    _cursor = savedCursor;
    _cursor.position = clampCoordinate(_cursor.position);
    _terminal->setMode(DECMode::Origin, savedCursor.originMode);
    updateCursorIterator();
    verifyState();
}

void Screen::reply(std::string_view text)
{
    _terminal->reply(text);
}

void Screen::processITerm2(std::string_view payload)
{
    // OSC 1337 carries several unrelated iTerm2 extensions, told apart by the text before the first
    // '=' (or the whole payload, for the ones that take no argument).
    if (payload == "Capabilities")
    {
        reportITerm2Capabilities();
        return;
    }

    if (payload.starts_with("File="))
        renderITerm2InlineImage(payload.substr(5));
}

void Screen::reportITerm2Capabilities()
{
    // Only what Contour genuinely does. Advertising a capability we do not have would make an
    // application choose a path that then fails, which is worse than it choosing a lesser path.
    //
    // Codes and their value semantics are iTerm2's: T is a bitmask (2 = full 24-bit SGR sequences),
    // Sc a bitmask of the DECSCUSR shapes understood (1 = modes 1-4, 2 = modes 5-6, 4 = reset),
    // Uw the Unicode version the width tables come from, and Ts a bitmask (1 = title stacks,
    // 2 = title setting).
    auto capabilities = std::string {};
    capabilities += "T2";   // 24-bit colour, full sequences
    capabilities += "Cw";   // clipboard writable (OSC 52)
    capabilities += "Lr";   // DECSLRM
    capabilities += "M";    // mouse
    capabilities += "Sc7";  // DECSCUSR modes 1-6 plus reset
    capabilities += "U";    // basic Unicode
    capabilities += "Uw17"; // width tables from Unicode 17
    capabilities += "Ts3";  // title stacks and title setting
    capabilities += "B";    // bracketed paste
    capabilities += "F";    // focus reporting
    capabilities += "Gs";   // strikethrough
    capabilities += "Go";   // overline
    capabilities += "Sy";   // synchronized output (mode 2026)
    capabilities += "H";    // hyperlinks (OSC 8)
    capabilities += "No";   // notifications (OSC 99)
    capabilities += "Sx";   // sixel

    reply("\033]1337;Capabilities={}\a", capabilities);
}

void Screen::renderITerm2InlineImage(std::string_view arguments)
{
    // `File=key=value;...:<base64 data>`. The colon separates the arguments from the payload.
    auto const colon = arguments.find(':');
    if (colon == std::string_view::npos)
        return;

    auto const keyValues = arguments.substr(0, colon);
    auto const encoded = arguments.substr(colon + 1);

    auto widthCells = 0u;
    auto heightCells = 0u;
    auto inlineImage = false;

    for (size_t offset = 0; offset < keyValues.size();)
    {
        auto end = keyValues.find(';', offset);
        if (end == std::string_view::npos)
            end = keyValues.size();
        auto const pair = keyValues.substr(offset, end - offset);
        offset = end + 1;

        auto const equals = pair.find('=');
        if (equals == std::string_view::npos)
            continue;
        auto const key = pair.substr(0, equals);
        auto const value = pair.substr(equals + 1);

        // Only the cell-valued forms of width/height are honoured; the "10px" and "50%" forms would
        // each need their own unit handling and are simply left at "derive from the image".
        auto const cells = [](std::string_view text) -> unsigned {
            auto result = 0u;
            auto const* const last = text.data() + text.size();
            auto const [ptr, ec] = std::from_chars(text.data(), last, result);
            return (ec == std::errc {} && ptr == last) ? result : 0u;
        };

        if (key == "width")
            widthCells = cells(value);
        else if (key == "height")
            heightCells = cells(value);
        else if (key == "inline")
            inlineImage = value == "1";
    }

    // Without inline=1 the payload is a file download, not something to draw. Contour does not save
    // files an application sends, so there is nothing to do.
    if (!inlineImage)
        return;

    auto const decoded = crispy::base64::decode(encoded);
    if (decoded.empty())
        return;

    // iTerm2 sends whole image files rather than raw pixels, so the format is whatever the file is;
    // PNG is the one Contour can decode.
    auto pixmap = Image::Data(decoded.begin(), decoded.end());
    (void) renderImage(ImageFormat::PNG,
                       ImageSize {},
                       std::move(pixmap),
                       GridSize { .lines = LineCount::cast_from(clamp(heightCells, pageSize().lines)),
                                  .columns = ColumnCount::cast_from(clamp(widthCells, pageSize().columns)) },
                       ImageAlignment::TopStart,
                       ImageResize::ResizeToFit,
                       /*autoScroll*/ true,
                       /*updateCursor*/ true,
                       ImageLayer::Above);
}

ApplyResult Screen::processPointerShape(std::string_view payload)
{
    using namespace pointer_shape;

    if (payload.empty())
        return ApplyResult::Invalid;

    auto const operation = static_cast<Operation>(payload.front());
    auto const names = payload.substr(1);

    switch (operation)
    {
        case Operation::Query: {
            // Answer one value per queried name: `1` for a shape we can display, `0` for one we
            // cannot, and the actual CSS name for the three introspection pseudo-names. Answering
            // `1` for a shape we would not actually show would be a lie an application acts on.
            auto answers = std::string {};
            for (auto const& name: crispy::split(names, ','))
            {
                if (!answers.empty())
                    answers += ',';
                if (isSupportedName(name))
                    answers += '1';
                else if (name == "__current__")
                    answers += _terminal->pointerShape();
                else if (name == "__default__" || name == "__grabbed__")
                    answers += DefaultName;
                else
                    answers += '0';
            }
            reply("\033]22;{}\033\\", answers);
            return ApplyResult::Ok;
        }

        case Operation::Set:
        case Operation::Push: {
            // A list is pushed left to right, so the last name given ends up current.
            for (auto const& name: crispy::split(names, ','))
            {
                if (!isSupportedName(name))
                    continue;
                if (operation == Operation::Push)
                    _terminal->pushPointerShape(std::string(name));
                else
                    _terminal->setPointerShape(std::string(name));
            }
            return ApplyResult::Ok;
        }

        case Operation::Pop: _terminal->popPointerShape(); return ApplyResult::Ok;
    }

    return ApplyResult::Invalid;
}

ApplyResult Screen::processKittyClipboard(std::string_view payload)
{
    using namespace kitty_clipboard;

    auto const parsed = parsePacket(payload);
    if (!parsed)
        return ApplyResult::Invalid;
    auto const& packet = *parsed;

    auto const respond = [&](std::string_view status) {
        reply("\033]5522;type={}:id={}\033\\", status, packet.id);
    };

    switch (packet.type)
    {
        case PacketType::Read: {
            // Reading the clipboard lets an application exfiltrate whatever the user last copied,
            // so it is gated by the same setting OSC 52 reads are.
            if (!_terminal->settings().allowClipboardRead)
            {
                respond("EPERM");
                return ApplyResult::Ok;
            }

            // The payload lists the MIME types the application will accept. If none of them is one
            // this terminal can produce, say so rather than sending text under a type it did not ask
            // for.
            auto const requested = crispy::base64::decode(packet.payload);
            auto accepted = false;
            for (auto const& mimeType: crispy::split(requested, ' '))
                if (isSupportedMimeType(mimeType))
                    accepted = true;
            if (!requested.empty() && !accepted)
            {
                respond("ENOSYS");
                return ApplyResult::Ok;
            }

            _terminal->requestClipboardRead("c");
            return ApplyResult::Ok;
        }

        case PacketType::Write:
            // A write opens a transmission; the data arrives in the wdata packets that follow.
            _kittyClipboardWrite.clear();
            _kittyClipboardWriteOpen = true;
            respond("OK");
            return ApplyResult::Ok;

        case PacketType::WriteAlias:
            // Aliases map other MIME names onto one this terminal understands. Contour's clipboard
            // is text, so there is nothing to map them onto and nothing to record.
            return ApplyResult::Ok;

        case PacketType::WriteData: {
            if (!_kittyClipboardWriteOpen)
                return ApplyResult::Invalid;

            if (!isSupportedMimeType(packet.mimeType))
            {
                // Refuse rather than accept and drop: an application told its data was stored, which
                // then reads back something else, is worse off than one told no.
                _kittyClipboardWriteOpen = false;
                _kittyClipboardWrite.clear();
                respond("ENOSYS");
                return ApplyResult::Ok;
            }

            // An empty chunk is the end-of-transmission marker.
            if (packet.payload.empty())
            {
                _terminal->copyToClipboard(_kittyClipboardWrite);
                _kittyClipboardWrite.clear();
                _kittyClipboardWriteOpen = false;
                respond("DONE");
                return ApplyResult::Ok;
            }

            // The stream is attacker-controlled and is only flushed by the empty end-of-transmission
            // chunk, so one that never sends it would grow this buffer until the process is killed.
            // Abandon the whole transmission rather than truncate it: a partial clipboard is not the
            // data the application asked to store. @see kitty_graphics::MaxChunkedPayloadSize, which
            // bounds the other chunked protocol the same way.
            auto const decoded = crispy::base64::decode(packet.payload);
            if (_kittyClipboardWrite.size() + decoded.size() > MaxClipboardWriteSize)
            {
                _kittyClipboardWriteOpen = false;
                _kittyClipboardWrite.clear();
                _kittyClipboardWrite.shrink_to_fit();
                respond("EIO");
                return ApplyResult::Ok;
            }

            _kittyClipboardWrite += decoded;
            return ApplyResult::Ok;
        }
    }

    return ApplyResult::Unsupported;
}

namespace
{
    /// The per-cell sizing a parsed `OSC 66` request asks for.
    ///
    /// The block's extent in cells comes from `s` alone; `n`/`d`/`v`/`h` change only how the glyph is
    /// drawn inside it, which is why they travel together but are stored apart.
    [[nodiscard]] CellScale cellScaleOf(text_sizing::Request const& request) noexcept
    {
        return CellScale { .scale = request.scale,
                           .numerator = request.numerator,
                           .denominator = request.denominator,
                           .verticalAlignment = request.verticalAlignment,
                           .horizontalAlignment = request.horizontalAlignment };
    }
} // namespace

ApplyResult Screen::processTextSizing(std::string_view payload)
{
    auto const parsed = text_sizing::parseRequest(payload);
    if (!parsed)
        return ApplyResult::Invalid;
    auto const& request = *parsed;

    if (request.text.empty())
        return ApplyResult::Ok;

    // An explicit `w` states the size the application wants regardless of what the text measures, so
    // the whole run becomes ONE cell block of that width. Without it, each cluster the text would
    // normally occupy becomes a `scale`-wide block of its own.
    if (request.width != 0)
    {
        auto const columns = static_cast<uint8_t>(
            std::min<unsigned>(request.columnsFor(0), std::numeric_limits<uint8_t>::max()));
        writeSizedText(unicode::convert_to<char32_t>(request.text), columns, cellScaleOf(request));
        return ApplyResult::Ok;
    }

    // No explicit width: measure each grapheme cluster and scale it.
    auto segmenter = unicode::utf8_grapheme_segmenter(request.text);
    for (auto const& cluster: segmenter)
    {
        auto const natural = unicode::grapheme_cluster_width(cluster);
        auto const columns = static_cast<uint8_t>(
            std::min<unsigned>(request.columnsFor(natural), std::numeric_limits<uint8_t>::max()));
        writeSizedText(cluster, columns, cellScaleOf(request));
    }
    return ApplyResult::Ok;
}

std::optional<MulticellBlock> Screen::multicellBlockAt(CellLocation position) const noexcept
{
    // A line carrying a block is never trivial -- its cells hold continuation flags and a scale --
    // so a trivial line answers "no block here" without touching per-cell storage it does not have.
    auto const continuesInto = [this](CellLocation loc, CellFlag flag) noexcept {
        auto const& line = grid().lineAt(loc.line);
        return !line.isTrivialBuffer()
               && ConstCellProxy(line.storage(), unbox<size_t>(loc.column)).isFlagEnabled(flag);
    };

    // Walk to the block's head: up while this cell continues a block above, then left while it
    // continues one to its left. The axes are independent -- every column of a scaled block's
    // second row carries the vertical flag.
    // The walk stops at the first line the grid HAS, which is the top of the scrollback -- not at
    // line 0. Grid lines run negative into history, so bounding at 0 made the walk a no-op for every
    // block that had scrolled off the page: its continuation rows then reported themselves as heads,
    // which is empty, and the block stopped being drawn or erased as a whole.
    auto const topLine = -boxed_cast<LineOffset>(grid().historyLineCount());
    auto origin = position;
    while (origin.line > topLine && continuesInto(origin, CellFlag::MulticellContinuation))
        --origin.line;
    while (origin.column > ColumnOffset(0) && continuesInto(origin, CellFlag::WideCharContinuation))
        --origin.column;

    auto const& headLine = grid().lineAt(origin.line);
    if (headLine.isTrivialBuffer())
        return std::nullopt;

    auto const head = ConstCellProxy(headLine.storage(), unbox<size_t>(origin.column));
    auto const columns = std::max(1, static_cast<int>(head.width()));
    auto const rows = std::max(1, static_cast<int>(head.scale()));
    if (columns == 1 && rows == 1)
        return std::nullopt;

    return MulticellBlock { .origin = origin, .columns = columns, .rows = rows };
}

void Screen::eraseMulticellBlockAt(CellLocation position)
{
    // Half a glyph is not a thing that can be drawn, so touching any part of a block destroys all of
    // it. kitty's nuke_multicell_char_at() does the same.
    auto const block = multicellBlockAt(position);
    if (!block)
        return;

    for (auto const row: std::views::iota(0, block->rows))
    {
        auto const lineOffset = block->origin.line + LineOffset::cast_from(row);
        if (lineOffset >= boxed_cast<LineOffset>(pageSize().lines))
            break;
        auto& target = grid().lineAt(lineOffset);
        for (auto const i: std::views::iota(0, block->columns))
        {
            auto const column = block->origin.column + ColumnOffset::cast_from(i);
            if (column > lastWritableColumn())
                break;
            target.useCellAt(column).reset(_cursor.graphicsRendition);
        }
    }
}

void Screen::writeSizedText(std::u32string_view codepoints, uint8_t columns, CellScale const& cellScale)
{
    if (columns == 0)
        return;

    // This is a second entry point into writing text, parallel to writeTextInternal, so it owes the
    // same prologue: a wrap deferred by the PREVIOUS character is still outstanding and has to be
    // taken first. Skipping it let a one-column block overwrite the character sitting in the last
    // column -- the relocation test below compares the block's own extent against the margin and a
    // one-column block always fits -- and left wrapPending set, so every following block clobbered
    // that same cell. Wider blocks did move, but via linefeed(), which does not mark the line
    // Wrappable|Wrapped the way a real deferred wrap does, so reflow and selection then treated it
    // as a hard newline.
    crlfIfWrapPending();

    // A block wider than the line can never be placed, so it is dropped rather than clipped -- a
    // clipped block is a different size from the one the application asked for. kitty drops it too.
    auto const lineWidth = lastWritableColumn() - margin().horizontal.from + 1;
    if (ColumnOffset::cast_from(columns) > lineWidth)
        return;

    // A block that merely does not fit *here* moves rather than splits. With autowrap it goes to the
    // next line; without it, it is placed flush against the right edge, which is what kitty's
    // move_cursor_past_multicell() does when DECAWM is off.
    if (_cursor.position.column + ColumnOffset::cast_from(columns - 1) > lastWritableColumn())
    {
        if (_terminal->isModeEnabled(DECMode::AutoWrap))
            linefeed(margin().horizontal.from);
        else
            _cursor.position.column = lastWritableColumn() - ColumnOffset::cast_from(columns - 1);
    }

    if (codepoints.empty())
        return;

    // A block `scale` cells tall needs that many rows BELOW the cursor. When they are not there the
    // page SCROLLS to make room, exactly as it would for text running off the bottom -- the block is
    // not clipped.
    //
    // This is the overwhelmingly common case, not an edge case: a terminal that has been printing
    // sits on its last line, so every block written by a program that has produced any output at all
    // arrives with no room beneath it. Clipping it left only the head row, and the head row is band
    // 0 -- the TOP slice of the glyph -- so all that reached the screen was a sliver of the glyph's
    // top edge, on one row, while the rest of the block did not exist to be drawn.
    //
    // kitty does the same in handle_fixed_width_multicell_command() (screen.c): it scrolls by the
    // shortfall and walks the cursor back up by as much.
    auto const height = LineCount::cast_from(std::max<uint8_t>(cellScale.scale, 1));
    if (height > LineCount(1))
    {
        // Taller than the scroll region can ever be: there is nowhere to put it, so it is dropped
        // whole rather than clipped -- a clipped block is a different size from the one asked for.
        if (height > margin().vertical.length())
            return;

        auto const available =
            LineCount::cast_from(margin().vertical.to - _cursor.position.line) + LineCount(1);
        if (height > available)
        {
            auto const shortfall = height - available;
            scrollUp(shortfall);
            // The cursor follows its content up. `currentLine()` reads a CACHED pointer, so moving
            // the cursor without refreshing it would write the block into the line the cursor used
            // to be on.
            _cursor.position.line -= LineOffset::cast_from(shortfall);
            updateCursorIterator();
        }
    }

    // The cells this block is about to claim may already belong to blocks on screen -- a run that
    // wrapped lands on the very row the previous run's blocks reach down into. Each of those is
    // destroyed WHOLE before this one takes their space; overwriting only the cells that overlap
    // would leave the old block's head behind, still describing a body that is gone.
    //
    // This is the same rule the ordinary write path applies to the single cell it touches. @see
    // writeText. A block covers up to MaxWidth * MaxScale columns and MaxScale rows, so the walk is
    // bounded by the protocol, and it is reached only on an `OSC 66` write.
    for (auto const row: std::views::iota(0, static_cast<int>(std::max<uint8_t>(cellScale.scale, 1))))
    {
        auto const lineOffset = _cursor.position.line + LineOffset::cast_from(row);
        if (lineOffset >= boxed_cast<LineOffset>(pageSize().lines))
            break;
        for (auto const i: std::views::iota(0, static_cast<int>(columns)))
        {
            auto const column = _cursor.position.column + ColumnOffset::cast_from(i);
            if (column > lastWritableColumn())
                break;
            eraseMulticellBlockAt(CellLocation { .line = lineOffset, .column = column });
        }
    }

    auto& line = currentLine();
    auto cell = line.useCellAt(_cursor.position.column);
    cell.write(_cursor.graphicsRendition, codepoints[0], columns, _cursor.hyperlink);
    for (size_t i = 1; i < codepoints.size(); ++i)
        // The application stated this block's width explicitly; re-measuring the cluster would
        // silently overrule it. FirstCodepoint takes the codepoints and leaves the width write()
        // already set alone.
        (void) cell.appendCharacter(codepoints[i], ClusterWidthPolicy::FirstCodepoint);

    cell.setTextScale(cellScale);

    auto const sgr = _cursor.graphicsRendition.with(CellFlag::WideCharContinuation);
    for (uint8_t i = 1; i < columns; ++i)
    {
        auto continuation = line.useCellAt(_cursor.position.column + ColumnOffset::cast_from(i));
        continuation.reset(sgr, _cursor.hyperlink);
        continuation.setTextScale(cellScale);
    }

    // A scaled block is `scale` cells TALL as well as `columns` wide -- the first thing in this grid
    // that occupies more than one line. The rows beneath are claimed so that nothing else can be
    // written into them and so that erasing any part of the block finds the whole of it.
    for (uint8_t row = 1; row < cellScale.scale; ++row)
    {
        auto const lineOffset = _cursor.position.line + LineOffset::cast_from(row);
        if (lineOffset >= boxed_cast<LineOffset>(pageSize().lines))
            break;
        auto& below = grid().lineAt(lineOffset);
        for (uint8_t i = 0; i < columns; ++i)
        {
            auto continuation = below.useCellAt(_cursor.position.column + ColumnOffset::cast_from(i));
            continuation.reset(sgr.with(CellFlag::MulticellContinuation), _cursor.hyperlink);
            continuation.setTextScale(cellScale);
        }
    }

    _lastCursorPosition = _cursor.position;
    _terminal->markCellDirty(_cursor.position);

    // The cursor may not step past the last column -- verifyState() requires it to stay addressable.
    // A block ending exactly at the edge therefore leaves the cursor on the edge with the wrap
    // deferred, exactly as an ordinary write does. @see clearAndAdvance.
    auto const landing = _cursor.position.column + ColumnOffset::cast_from(columns);
    if (landing <= lastWritableColumn())
        _cursor.position.column = landing;
    else
    {
        _cursor.position.column = lastWritableColumn();
        if (_terminal->isModeEnabled(DECMode::AutoWrap))
            _cursor.wrapPending = true;
    }
}

void Screen::processAPC(std::string_view body)
{
    // APC carries application-defined protocols that share no grammar, so each is recognised by its
    // own introducer. 'G' is the kitty graphics protocol; anything else is not ours to interpret.
    if (body.empty() || body.front() != 'G')
        return;

    processKittyGraphics(body.substr(1));
}

void Screen::replyKittyGraphics(kitty_graphics::Command const& command, std::string_view status)
{
    auto const isError = !status.starts_with("OK");
    if (command.quietAlways || (command.quietOnSuccess && !isError))
        return;

    // An unsolicited response would desynchronise an application that never identified its image,
    // so a command carrying no identity is answered only when it failed.
    if (command.imageId == 0 && command.imageNumber == 0 && !isError)
        return;

    if (command.imageNumber != 0)
        reply("\033_Gi={},I={};{}\033\\", command.imageId, command.imageNumber, status);
    else
        reply("\033_Gi={};{}\033\\", command.imageId, status);
}

std::optional<std::string_view> Screen::validateKittyTransmission(
    kitty_graphics::Command const& command) noexcept
{
    using namespace kitty_graphics;

    // A raw pixel transmission has no header to carry its dimensions, so the control data must. PNG
    // is exempt: the file states its own size. Checked here rather than in the parser because a
    // continuation chunk legitimately carries no dimensions -- only the reassembled command has them.
    if (command.format != Format::Png && (command.pixelWidth == 0 || command.pixelHeight == 0))
        return "EINVAL:missing image dimensions";

    if (command.medium != Medium::Direct)
        // Reading a path or a shared-memory object the application names would let it point the
        // terminal at any file the user can read. Not implemented deliberately.
        return "ENOTSUP:only direct transmission is supported";

    if (command.compression != Compression::None)
        return "ENOTSUP:compressed payloads are not supported";

    return std::nullopt;
}

void Screen::removeKittyPlacements(std::shared_ptr<Image const> const& image)
{
    // Placements are not tracked separately: a placement IS the image fragments sitting in the cells
    // it covers. Dropping one therefore means clearing those fragments, and only those -- the text
    // sharing the cells is not the placement's to erase.
    for (auto const line: std::views::iota(0, *pageSize().lines))
    {
        for (auto const column: std::views::iota(0, *pageSize().columns))
        {
            auto cell = at(LineOffset(line), ColumnOffset(column));
            auto const fragment = cell.imageFragment();
            if (!fragment)
                continue;
            if (image && fragment->rasterizedImage().imagePointer() != image)
                continue;
            cell.clearImageFragment();
            _terminal->markCellDirty(
                CellLocation { .line = LineOffset(line), .column = ColumnOffset(column) });
        }
    }
}

void Screen::deleteKittyGraphics(kitty_graphics::Command const& command)
{
    // The CASE of `d=` decides how far the delete reaches: lower case removes placements and leaves
    // the transmitted data resident, upper case additionally frees it. Ignoring the distinction broke
    // the protocol's standard redraw idiom -- `a=d,d=a` to clear placements, then `a=p,i=1` to place
    // the image already transmitted -- because the data the re-placement needs had been destroyed.
    auto const freesData = static_cast<bool>(std::isupper(static_cast<unsigned char>(command.deleteTarget)));
    auto const target = static_cast<char>(std::tolower(static_cast<unsigned char>(command.deleteTarget)));

    // 'i' names one image, by id; 'a' (and everything Contour does not implement, such as the
    // positional targets) clears every placement.
    auto const image = [&]() -> std::shared_ptr<Image const> {
        if (target != 'i' || command.imageId == 0)
            return {};
        auto const it = _kittyImages.find(command.imageId);
        return it != _kittyImages.end() ? it->second : nullptr;
    }();

    if (target == 'i' && command.imageId != 0 && !image)
        return; // Nothing transmitted under that id; nothing placed from it either.

    removeKittyPlacements(image);

    if (!freesData)
        return;

    if (target == 'i' && command.imageId != 0)
        _kittyImages.erase(command.imageId);
    else
        _kittyImages.clear();
}

void Screen::resetKittyState() noexcept
{
    // RIS must not leave a half-open transmission behind: the next application's first graphics
    // command would be swallowed as a continuation chunk of the dead one, taking that command's
    // format and dimensions instead of its own. The same applies to a clipboard write left open, and
    // to images Terminal::hardReset() has already dropped from the image pool.
    _kittyChunkedPayload.clear();
    _kittyChunkedCommand.reset();
    _kittyImages.clear();
    _kittyClipboardWrite.clear();
    _kittyClipboardWriteOpen = false;
}

void Screen::processKittyGraphics(std::string_view body)
{
    using namespace kitty_graphics;

    auto parsed = parseCommand(body);
    if (!parsed)
    {
        auto const failed = Command {};
        replyKittyGraphics(failed, std::format("{}:bad control data", errorCode(parsed.error())));
        return;
    }
    auto command = std::move(parsed.value());

    // A chunked transmission carries its control data only in the FIRST chunk; the rest is payload
    // that has to be stitched onto it before anything can be decided.
    if (_kittyChunkedCommand)
    {
        // A chunk stream that never terminates is otherwise an unbounded allocation driven straight
        // from the wire. Abandon the whole transmission rather than truncate it: a partial image
        // decoded against its declared dimensions is not an image.
        if (_kittyChunkedPayload.size() + command.payload.size() > MaxChunkedPayloadSize)
        {
            auto const abandoned = *_kittyChunkedCommand;
            _kittyChunkedCommand.reset();
            _kittyChunkedPayload.clear();
            _kittyChunkedPayload.shrink_to_fit();
            replyKittyGraphics(abandoned, "EINVAL:image transmission too large");
            return;
        }

        _kittyChunkedPayload += command.payload;
        if (command.moreChunksFollow)
            return;
        auto continued = *_kittyChunkedCommand;
        continued.payload = std::move(_kittyChunkedPayload);
        _kittyChunkedCommand.reset();
        _kittyChunkedPayload.clear();
        command = std::move(continued);
    }
    else if (command.moreChunksFollow)
    {
        _kittyChunkedPayload = command.payload;
        auto opener = command;
        opener.payload.clear();
        _kittyChunkedCommand = std::move(opener);
        return;
    }

    switch (command.action)
    {
        case Action::Query:
            // A query must be validated exactly as a transmission would be -- answering OK to a
            // command we could not actually honour is worse than answering the error. An application
            // probes with `a=q` precisely so that it can fall back; telling it yes and then failing
            // the real transmission leaves it with nothing to fall back to.
            replyKittyGraphics(command, validateKittyTransmission(command).value_or("OK"));
            return;
        case Action::Delete:
            deleteKittyGraphics(command);
            replyKittyGraphics(command, "OK");
            return;
        case Action::Put: {
            auto const it = _kittyImages.find(command.imageId);
            if (it == _kittyImages.end())
            {
                replyKittyGraphics(command, "ENOENT:no such image");
                return;
            }
            renderKittyImage(command, it->second);
            replyKittyGraphics(command, "OK");
            return;
        }
        case Action::Transmit:
        case Action::TransmitAndDisplay: break;
        case Action::Frame:
        case Action::Animate:
        case Action::Compose:
            // Animation is not implemented. Say so rather than silently accepting frames that will
            // never be shown.
            replyKittyGraphics(command, "ENOTSUP:animation not supported");
            return;
    }

    if (auto const rejection = validateKittyTransmission(command))
    {
        replyKittyGraphics(command, *rejection);
        return;
    }

    auto const decoded = crispy::base64::decode(command.payload);
    auto pixmap = Image::Data(decoded.begin(), decoded.end());

    auto const format = [&] {
        switch (command.format)
        {
            case Format::Png: return ImageFormat::PNG;
            case Format::Rgb: return ImageFormat::RGB;
            case Format::Rgba: break;
        }
        return ImageFormat::RGBA;
    }();
    auto const pixelSize =
        ImageSize { Width::cast_from(command.pixelWidth), Height::cast_from(command.pixelHeight) };

    if (format != ImageFormat::PNG)
    {
        // The renderer reads width*height*bytesPerPixel from this buffer, so a payload that does not
        // match the declared size is a read past the end waiting to happen.
        auto const expected =
            static_cast<size_t>(command.pixelWidth) * command.pixelHeight * bytesPerPixel(command.format);
        if (pixmap.size() != expected)
        {
            replyKittyGraphics(command, "EINVAL:payload size does not match dimensions");
            return;
        }
    }

    auto image = _terminal->imagePool().create(format, pixelSize, std::move(pixmap));
    if (!image)
    {
        replyKittyGraphics(command, "EINVAL:could not decode image");
        return;
    }

    if (command.imageId != 0)
    {
        // Ids are 32-bit, so without a quota an application can park billions of decoded images in
        // the terminal. Refusing is preferable to evicting: the whole point of storing an image is
        // that a later `a=p` can place it, and silently dropping one turns that into ENOENT.
        auto const stored = std::accumulate(
            _kittyImages.begin(), _kittyImages.end(), size_t { 0 }, [&](size_t sum, auto const& entry) {
                return entry.first == command.imageId ? sum : sum + entry.second->data().size();
            });
        if (stored + image->data().size() > MaxStoredImageBytes)
        {
            replyKittyGraphics(command, "ENOSPC:image storage quota exceeded");
            return;
        }
        _kittyImages[command.imageId] = image;
    }

    if (command.action == Action::TransmitAndDisplay)
        renderKittyImage(command, image);

    replyKittyGraphics(command, "OK");
}

void Screen::renderKittyImage(kitty_graphics::Command const& command,
                              std::shared_ptr<Image const> const& image)
{
    auto const cellSize = _terminal->cellPixelSize();
    auto const pixels = image->size();

    // `c=`/`r=` state the display size in cells; without them it follows from the pixel size, rounded
    // up so that a partial cell is still drawn rather than clipped away.
    auto const cellWidth = std::max(unbox<int>(cellSize.width), 1);
    auto const cellHeight = std::max(unbox<int>(cellSize.height), 1);
    auto const columns = command.columns != 0
                             ? ColumnCount::cast_from(command.columns)
                             : ColumnCount::cast_from((unbox<int>(pixels.width) + cellWidth - 1) / cellWidth);
    auto const lines = command.rows != 0
                           ? LineCount::cast_from(command.rows)
                           : LineCount::cast_from((unbox<int>(pixels.height) + cellHeight - 1) / cellHeight);

    renderImage(image,
                _cursor.position,
                GridSize { .lines = lines, .columns = columns },
                PixelCoordinate {},
                ImageSize {},
                ImageAlignment::TopStart,
                ImageResize::ResizeToFit,
                /*autoScroll*/ false,
                /*updateCursor*/ !command.doNotMoveCursor,
                command.zIndex < 0 ? ImageLayer::Below : ImageLayer::Above);
}

void Screen::processSequence(Sequence const& seq)
{
#if defined(LIBTERMINAL_LOG_TRACE)
    if (vtTraceSequenceLog)
    {
        if (auto const* fd = seq.functionDefinition(_terminal->activeSequences()))
        {
            vtTraceSequenceLog()("[{}] Processing {:<14} {}", _name, fd->documentation.mnemonic, seq.text());
        }
        else
            vtTraceSequenceLog()("[{}] Processing unknown sequence: {}", _name, seq.text());
    }
#endif

    // std::cerr << std::format("\t{} \t; {}\n", seq,
    //         seq.functionDefinition() ? seq.functionDefinition()->comment : ""sv);

    _terminal->incrementInstructionCounter();
    if (Function const* funcSpec = seq.functionDefinition(_terminal->activeSequences()); funcSpec != nullptr)
        applyAndLog(*funcSpec, seq);
    else if (seq.category() == FunctionCategory::ESC && tryHandleSCS(seq))
        ; // Handled as SCS designation (e.g., DRCS two-byte designators)
    else if (auto const sel = seq.selector();
             std::ranges::any_of(_terminal->allSequences(),
                                 [&sel](Function const& fn) noexcept { return compare(sel, fn) == 0; }))
        ; // Recognised sequence, but gated out at the current operating level (set by DECSCL): a
          // terminal operating below a sequence's conformance level silently ignores it -- exactly as a
          // real VT220 ignores a VT300 query. It is NOT unknown, so it must not be logged as such; doing
          // so would report correct level-gating as an "ignored sequence" diagnostic. A linear scan is
          // required here because the full table is only partially sorted (activeSequences() is sorted
          // for select()'s binary search; the gated remainder is not), and this path is cold anyway.
    else if (vtParserLog)
        vtParserLog()("Unknown VT sequence: {}", seq);
}

namespace
{
    // DCS response status codes for SBQUERY.
    constexpr auto SBQueryResponseDisabled = std::string_view("\033P>0b\033\\");
    constexpr auto SBQueryResponseSuccess = std::string_view("\033P>1b");
    constexpr auto SBQueryResponseAuthRequired = std::string_view("\033P>2b\033\\");
    constexpr auto SBQueryResponseAuthFailed = std::string_view("\033P>3b\033\\");
    constexpr auto DcsTerminator = std::string_view("\033\\");

    /// JSON-encodes a string value, escaping special characters.
    std::string jsonEscape(std::string_view input)
    {
        auto result = std::string {};
        result.reserve(input.size() + 2);
        result += '"';
        for (auto const ch: input)
        {
            switch (ch)
            {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\b': result += "\\b"; break;
                case '\f': result += "\\f"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(ch) < 0x20)
                        result +=
                            std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(ch)));
                    else
                        result += ch;
                    break;
            }
        }
        result += '"';
        return result;
    }

    /// Formats a single CommandBlockInfo as a JSON object.
    std::string formatBlockJson(CommandBlockInfo const& block,
                                std::string_view prompt,
                                std::string_view output,
                                int outputLineCount)
    {
        return std::format(
            R"({{"command":{},"prompt":{},"output":{},"exitCode":{},"finished":{},"outputLineCount":{}}})",
            block.commandLine ? jsonEscape(*block.commandLine) : "null",
            jsonEscape(prompt),
            jsonEscape(output),
            block.exitCode,
            block.finished ? "true" : "false",
            outputLineCount);
    }

    /// Feeds a Screen's grid to the command-block scanner, walking backwards from a starting line.
    ///
    /// The adapter half of the CommandBlockLineSource seam: the state machine itself lives in
    /// CommandBlocks.cpp and knows nothing about a Grid, which is what lets it be tested against a plain
    /// vector of lines.
    ///
    /// It presents the grid's physical lines as the LOGICAL lines the shell actually wrote: a head plus
    /// the continuations a wrap chopped it into are one line here, which is the only unit the OSC 133
    /// marks are stamped on and therefore the only unit a resize cannot disturb.
    ///
    /// The walk is LAZY, and that is the whole point of hasLineAt() being a predicate rather than a count:
    /// this runs on the GUI thread, under the terminal lock, on every right-click. Counting the logical
    /// lines up front would mean climbing the entire scrollback — 100'000 lines at the configurable limit —
    /// before the scan had looked at a single flag, when the scan wants the handful of lines above the
    /// cursor that hold the last command. So it climbs exactly as far as it is asked to, and keeps nothing
    /// but the line it is standing on: the scan is a single forward pass, so nothing older is ever needed.
    // Implements BOTH line sources: the two scans walk the same lazy, reflow-safe climb over logical
    // lines, differing only in what they ask about each one. One hasLineAt() override satisfies both.
    class GridCommandBlockLines final: public CommandBlockLineSource, public PromptRegionLineSource
    {
      public:
        /// @param grid The grid to walk. Must outlive this source.
        /// @param fromLine The line to start at; the walk proceeds upwards from there into the history.
        GridCommandBlockLines(Grid const& grid, LineOffset fromLine):
            _grid { grid },
            _fromLine { fromLine },
            _historyTop { -boxed_cast<LineOffset>(grid.historyLineCount()) },
            _head { grid.logicalLineHead(fromLine) }
        {
        }

        [[nodiscard]] bool hasLineAt(size_t index) const override { return seekTo(index); }

        [[nodiscard]] LineFlags flagsAt(size_t index) const override { return headAt(index).flags(); }

        [[nodiscard]] LogicalLineMarks marksAt(size_t index) const override
        {
            auto const& head = headAt(index);
            return { .flags = head.flags(),
                     .promptEndOffset = head.promptEndOffset(),
                     .commandEndOffset = head.commandEndOffset() };
        }

        /// The physical lines the logical line @p index is chopped into.
        ///
        /// Public so a caller that scanned by INDEX can translate the result back into grid coordinates;
        /// the scan itself never deals in physical lines.
        [[nodiscard]] std::pair<LineOffset, LineOffset> physicalExtentOf(size_t index) const
        {
            return extentOf(index);
        }

        [[nodiscard]] std::string textAt(size_t index) const override
        {
            return textOfLogicalLine(index, ColumnOffset(0), std::nullopt);
        }

        [[nodiscard]] std::string textBeforeCommandEndAt(size_t index) const override
        {
            return textOfLogicalLine(index, ColumnOffset(0), headAt(index).commandEndOffset());
        }

        [[nodiscard]] std::string textFromCommandEndAt(size_t index) const override
        {
            return textOfLogicalLine(index, headAt(index).commandEndOffset(), std::nullopt);
        }

      private:
        /// Climbs to the logical line @p index, one line at a time, and stands there.
        /// @return false once the walk has run past the top of the history — there is no such line.
        [[nodiscard]] bool seekTo(size_t index) const
        {
            Require(index >= _index); // the scan only ever walks forwards

            while (_index < index)
            {
                if (_head <= _historyTop)
                    return false;
                _previousHead = _head;
                _head = _grid.logicalLineHead(_head - LineOffset(1));
                ++_index;
            }
            return true;
        }

        [[nodiscard]] Line const& headAt(size_t index) const
        {
            Require(index == _index); // always preceded by the hasLineAt() that walked us here
            return _grid.lineAt(_head);
        }

        /// The physical lines the logical line @p index is chopped into: its head, and everything below it
        /// up to the head of the logical line after it.
        [[nodiscard]] std::pair<LineOffset, LineOffset> extentOf(size_t index) const
        {
            Require(index == _index);
            auto const last = _index == 0 ? _fromLine : _previousHead - LineOffset(1);
            return { _head, std::max(_head, last) };
        }

        /// The text of the logical line @p index over its columns [@p begin, @p end), trailing blanks
        /// dropped. Columns are counted across the whole logical line, so they run straight through the
        /// wraps — which is exactly what makes a command-end offset survive a resize.
        [[nodiscard]] std::string textOfLogicalLine(size_t index,
                                                    ColumnOffset begin,
                                                    std::optional<ColumnOffset> end) const
        {
            auto const [first, last] = extentOf(index);
            auto const width = _grid.pageSize().columns;

            auto text = std::string {};
            for (auto line = first; line <= last; ++line)
            {
                auto const lineBegin = ColumnOffset::cast_from((*line - *first) * *width);
                auto const lineEnd = lineBegin + boxed_cast<ColumnOffset>(width);
                if (end.has_value() && lineBegin >= *end)
                    break;
                auto const from = std::max(begin, lineBegin) - lineBegin;
                auto const to = (end.has_value() ? std::min(*end, lineEnd) : lineEnd) - lineBegin;
                if (from < to)
                    text += _grid.lineAt(line).toUtf8(from, to);
            }

            text.resize(crispy::trimRight(text).size());
            return text;
        }

        Grid const& _grid;
        LineOffset _fromLine;
        LineOffset _historyTop;

        // Where the lazy walk currently stands. Mutable because the source is handed to the scan as a
        // const&: climbing is how it ANSWERS, not a change to what it says.
        mutable size_t _index = 0;        ///< The logical line _head refers to.
        mutable LineOffset _head;         ///< Its first physical line.
        mutable LineOffset _previousHead; ///< The head of the logical line below it (unused at index 0).
    };
} // namespace

void Screen::handleSemanticBlockQuery(Sequence const& seq)
{
    // If mode 2034 is disabled, reply with error DCS.
    if (!_terminal->isModeEnabled(DECMode::SemanticBlockProtocol))
    {
        reply(SBQueryResponseDisabled);
        return;
    }

    // Validate authentication token (params 2..5).
    auto const& tracker = _terminal->semanticBlockTracker();
    if (seq.parameterCount() < 6)
    {
        reply(SBQueryResponseAuthRequired);
        return;
    }

    auto const candidateToken = SemanticBlockTracker::Token {
        static_cast<uint16_t>(seq.param(2)),
        static_cast<uint16_t>(seq.param(3)),
        static_cast<uint16_t>(seq.param(4)),
        static_cast<uint16_t>(seq.param(5)),
    };
    if (!tracker.validateToken(candidateToken))
    {
        reply(SBQueryResponseAuthFailed);
        return;
    }

    auto const queryType = seq.param_or(0, SBQueryType::LastCommand);
    auto const count = seq.param_or(1, 1); // Pn: count (default 1)

    auto const& completedBlocks = tracker.completedBlocks();

    // Handle in-progress query.
    if (queryType == SBQueryType::InProgress)
    {
        handleInProgressQuery(tracker);
        return;
    }

    // Handle last completed command(s).
    handleCompletedBlocksQuery(tracker, completedBlocks, queryType, count);
}

void Screen::handleInProgressQuery(SemanticBlockTracker const& tracker)
{
    auto const& currentBlock = tracker.currentBlock();
    if (!currentBlock || currentBlock->finished)
    {
        reply(SBQueryResponseDisabled);
        return;
    }

    auto const cursorLine = cursor().position.line;

    // Find the most recent OutputStart line by scanning backward from cursor.
    auto const [foundOutputStart, outputStartLine] = [&]() -> std::pair<bool, LineOffset> {
        for (auto line = cursorLine; line >= LineOffset(0); --line)
            if (_grid.lineAt(line).isFlagEnabled(LineFlag::OutputStart))
                return { true, line };
        return { false, cursorLine };
    }();

    // Collect output text from OutputStart to cursor.
    auto outputText = std::string {};
    auto outputLineCount = 0;
    if (foundOutputStart)
    {
        for (auto line: std::views::iota(*outputStartLine, *cursorLine + 1))
        {
            auto const lineOffset = LineOffset::cast_from(line);
            if (lineOffset != outputStartLine)
                outputText += '\n';
            outputText += _grid.lineAt(lineOffset).toUtf8Trimmed(false, true);
            ++outputLineCount;
        }
    }

    // Collect prompt text by scanning backward from OutputStart to find the Marked line.
    auto promptText = std::string {};
    if (foundOutputStart)
    {
        for (auto line = outputStartLine - 1; line >= LineOffset(0); --line)
        {
            if (_grid.lineAt(line).isFlagEnabled(LineFlag::Marked))
            {
                for (auto pl: std::views::iota(*line, *outputStartLine))
                {
                    auto const plOffset = LineOffset::cast_from(pl);
                    if (plOffset != line)
                        promptText += '\n';
                    promptText += _grid.lineAt(plOffset).toUtf8Trimmed(false, true);
                }
                break;
            }
        }
    }

    auto const json = formatBlockJson(*currentBlock, promptText, outputText, outputLineCount);
    reply("{}{{\"version\":1,\"blocks\":[{}]}}{}", SBQueryResponseSuccess, json, DcsTerminator);
}

void Screen::handleCompletedBlocksQuery(SemanticBlockTracker const& tracker,
                                        std::deque<CommandBlockInfo> const& completedBlocks,
                                        unsigned queryType,
                                        int count)
{
    auto const requestedCount = (queryType == SBQueryType::LastCommand) ? 1 : std::max(count, 1);

    if (completedBlocks.empty())
    {
        // Check if there's a finished current block not yet pushed.
        if (!tracker.currentBlock() || !tracker.currentBlock()->finished)
        {
            reply(SBQueryResponseDisabled);
            return;
        }
    }

    // Collect blocks to return (most recent first, then reverse for JSON output).
    auto blocks = std::vector<CommandBlockInfo const*> {};

    // Include the current block if finished.
    if (tracker.currentBlock() && tracker.currentBlock()->finished)
        blocks.push_back(&*tracker.currentBlock());

    // Add from completed blocks (back = most recent).
    for (auto it = completedBlocks.rbegin();
         it != completedBlocks.rend() && static_cast<int>(blocks.size()) < requestedCount;
         ++it)
        blocks.push_back(&*it);

    if (blocks.empty())
    {
        reply(SBQueryResponseDisabled);
        return;
    }

    // Reconstruct each block's text from the OSC 133 marks the shell left in the grid. The tracker knows
    // HOW MANY blocks there are and what their metadata is; the grid is what still holds their text.
    auto const blockTexts =
        scanCommandBlocksBackward(GridCommandBlockLines { _grid, cursor().position.line }, blocks.size());

    // Build JSON response with blocks in chronological order (oldest first).
    auto json = std::string {};
    json += R"({"version":1,"blocks":[)";
    auto const actualCount = std::min(blockTexts.size(), blocks.size());
    for (auto i = actualCount; i-- > 0;)
    {
        if (i != actualCount - 1)
            json += ',';
        json += formatBlockJson(
            *blocks[i], blockTexts[i].prompt, blockTexts[i].output, blockTexts[i].outputLineCount);
    }
    json += "]}";
    reply("{}{}{}", SBQueryResponseSuccess, json, DcsTerminator);
}

std::optional<CommandBlockText> Screen::lastCommandBlock() const
{
    auto blocks = scanCommandBlocksBackward(GridCommandBlockLines { _grid, cursor().position.line }, 1);
    if (blocks.empty())
        return std::nullopt;
    return std::move(blocks.front());
}

std::expected<LivePromptSpan, PromptRegionError> Screen::livePromptSpan() const
{
    auto const cursorLine = cursor().position.line;

    return findLivePromptRegion(GridCommandBlockLines { _grid, cursorLine }, MaxPromptScanLines)
        .transform([&](PromptRegion const& region) {
            // Back from logical-line INDICES into grid coordinates, over a FRESH walk: the source climbs
            // lazily and only forwards, so it cannot be asked about an index it has already passed.
            auto translator = GridCommandBlockLines { _grid, cursorLine };

            // Ascending order, for that same reason. The prompt ends on the line ;B landed on — the
            // cursor's own line when the shell emitted no ;B at all.
            auto const inputIndex = region.inputBegin.has_value() ? region.inputBeginIndex : 0;
            [[maybe_unused]] auto const inputReachable = translator.hasLineAt(inputIndex);
            auto const lastLine = translator.physicalExtentOf(inputIndex).second;

            [[maybe_unused]] auto const startReachable = translator.hasLineAt(region.startIndex);
            auto const firstLine = translator.physicalExtentOf(region.startIndex).first;

            return LivePromptSpan {
                .firstLine = firstLine,
                .lastLine = lastLine,
                .inputBegin = region.inputBegin,
            };
        });
}

void Screen::setMark()
{
    markLogicalLineAtCursor(LineFlag::Marked);
}

void Screen::processShellIntegration(Sequence const& seq)
{
    auto const& cmd = seq.intermediateCharacters();
    if (cmd.empty())
        return;

    auto const forEachKeyValue = []<typename Callback>(std::string_view text, Callback&& callback) {
        crispy::for_each_key_value(
            crispy::for_each_key_value_params {
                .text = text,
                .entryDelimiter = ';',
                .assignmentDelimiter = '=',
            },
            std::forward<Callback>(callback));
    };

    switch (cmd[0])
    {
        case 'A': {
            setMark();
            bool clickEvents = false;
            auto const params = seq.intermediateCharacters().substr(1);
            forEachKeyValue(params, [&](std::string_view key, std::string_view value) {
                if (key == "click_events" && value == "1")
                    clickEvents = true;
            });
            _terminal->shellIntegration().promptStart(clickEvents);
            _terminal->semanticBlockTracker().promptStart();
            break;
        }
        case 'B': {
            // Recorded unconditionally, for the reason given at ;C below.
            //
            // Along with WHERE the prompt stopped. The shell emits ;B the moment it has finished painting
            // and hands the line over, so the cursor stands exactly on the border between what the shell
            // wrote and what the user is about to type. That border is the only thing that can tell an
            // accessibility client — or anything else asking about the LIVE prompt — where the prompt
            // area ends, and it cannot be recovered afterwards: once the user types, the two are the same
            // run of cells.
            auto const cursorPosition = cursor().position;
            auto& head = _grid.lineAt(_grid.logicalLineHead(cursorPosition.line));
            head.setFlag(LineFlag::PromptEnd, true);
            head.setPromptEndOffset(_grid.logicalColumnOf(cursorPosition));

            _terminal->shellIntegration().promptEnd();
            break;
        }
        case 'C': {
            // The line flags are terminal MEMORY, and are recorded unconditionally — exactly as OSC 133;A's
            // Marked flag always has been. The shell said where this command's output begins, so the
            // terminal remembers it. DEC mode 2034 gates the semantic-block READER PROTOCOL (the block
            // deque, the command metadata, the session token, the DCS query), not what the terminal knows
            // about its own scrollback. Gating the flags too made every feature built on them — "copy last
            // command output", precise mark navigation — impossible for a shell that speaks plain OSC 133.
            markLogicalLineAtCursor(LineFlag::OutputStart);
            std::optional<std::string> commandLine;
            auto const params = seq.intermediateCharacters().substr(1);
            forEachKeyValue(params, [&](std::string_view key, std::string_view value) {
                if (key == "cmdline_url")
                    commandLine = crispy::unescapeURL(value);
            });
            _terminal->shellIntegration().commandOutputStart(commandLine);
            _terminal->semanticBlockTracker().commandOutputStart(commandLine);
            break;
        }
        case 'D': {
            // Recorded unconditionally, for the reason given at ;C above.
            //
            // Along with WHERE on that line the command's output actually stopped. The shell emits ;D from
            // its precmd hook, so the cursor is still standing wherever the output left it — and when that
            // output did not end in a newline, the prompt about to be printed lands on the very same line.
            // Remembering the column is what later tells the two apart; without it a copy of the command's
            // output either swallows the next prompt or loses its own last line.
            auto const cursorPosition = cursor().position;
            auto& head = _grid.lineAt(_grid.logicalLineHead(cursorPosition.line));
            head.setFlag(LineFlag::CommandEnd, true);
            head.setCommandEndOffset(_grid.logicalColumnOf(cursorPosition));

            auto const exitCode = (cmd.size() > 2 && cmd[1] == ';')
                                      ? crispy::to_integer<10, int>(cmd.substr(2)).value_or(0)
                                      : 0;
            _terminal->shellIntegration().commandFinished(exitCode);
            _terminal->semanticBlockTracker().commandFinished(exitCode);
            break;
        }
        default: break;
    }
}

void Screen::applyAndLog(Function const& function, Sequence const& seq)
{
    auto const result = apply(function, seq);
    switch (result)
    {
        case ApplyResult::Invalid: vtParserLog()("Invalid VT sequence: {}", seq); break;
        case ApplyResult::Unsupported: vtParserLog()("Unsupported VT sequence: {}", seq); break;
        case ApplyResult::Ok: break;
    }

    // Verify after *every* sequence, not only after one that reported Ok.
    //
    // A handler validates its parameters as it goes, so a sequence can mutate the screen and only then
    // decide it is Unsupported or Invalid -- and such a sequence used to escape the check entirely. The
    // damage it did then surfaced later, on whichever innocent sequence happened to come next, which is
    // exactly how a DECSNLS that resized the wrong axis got blamed on a DSR that cannot move a cursor.
    //
    // Compiled out unless CONTOUR_VERIFY_STATE, so it costs a release build nothing.
    _terminal->verifyState();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
ApplyResult Screen::apply(Function const& function, Sequence const& seq)
{
    // This function assumed that the incoming instruction has been already resolved to a given
    // FunctionDefinition
    switch (function)
    {
        // C0
        case BEL: _terminal->bell(); break;
        case BS: backspace(); break;
        case TAB: moveCursorToNextTab(); break;
        case LF: linefeed(); break;
        // VT and FF index like LF, and in linefeed mode (LNM) they too perform a carriage return.
        // Route them through linefeed() so they honour LNM (and smooth scroll) exactly as LF does.
        case VT: [[fallthrough]];
        case FF: linefeed(); break;
        case CR: moveCursorToBeginOfLine(); break;

        // ESC
        // SCS — G0
        case SCS_G0_SPECIAL: designateCharset(CharsetTable::G0, CharsetId::Special); break;
        case SCS_G0_BRITISH: designateCharset(CharsetTable::G0, CharsetId::British); break;
        case SCS_G0_USASCII: designateCharset(CharsetTable::G0, CharsetId::USASCII); break;
        case SCS_G0_FINNISH: designateCharset(CharsetTable::G0, CharsetId::Finnish); break;
        case SCS_G0_DUTCH: designateCharset(CharsetTable::G0, CharsetId::Dutch); break;
        case SCS_G0_NORWEGIAN: designateCharset(CharsetTable::G0, CharsetId::NorwegianDanish); break;
        case SCS_G0_FRENCH: designateCharset(CharsetTable::G0, CharsetId::French); break;
        case SCS_G0_FRENCHCANADIAN: designateCharset(CharsetTable::G0, CharsetId::FrenchCanadian); break;
        case SCS_G0_GERMAN: designateCharset(CharsetTable::G0, CharsetId::German); break;
        case SCS_G0_SPANISH: designateCharset(CharsetTable::G0, CharsetId::Spanish); break;
        case SCS_G0_SWEDISH: designateCharset(CharsetTable::G0, CharsetId::Swedish); break;
        case SCS_G0_SWISS: designateCharset(CharsetTable::G0, CharsetId::Swiss); break;
        case SCS_G0_TECHNICAL: designateCharset(CharsetTable::G0, CharsetId::Technical); break;
        // SCS — G1
        case SCS_G1_SPECIAL: designateCharset(CharsetTable::G1, CharsetId::Special); break;
        case SCS_G1_BRITISH: designateCharset(CharsetTable::G1, CharsetId::British); break;
        case SCS_G1_USASCII: designateCharset(CharsetTable::G1, CharsetId::USASCII); break;
        case SCS_G1_FINNISH: designateCharset(CharsetTable::G1, CharsetId::Finnish); break;
        case SCS_G1_DUTCH: designateCharset(CharsetTable::G1, CharsetId::Dutch); break;
        case SCS_G1_NORWEGIAN: designateCharset(CharsetTable::G1, CharsetId::NorwegianDanish); break;
        case SCS_G1_FRENCH: designateCharset(CharsetTable::G1, CharsetId::French); break;
        case SCS_G1_FRENCHCANADIAN: designateCharset(CharsetTable::G1, CharsetId::FrenchCanadian); break;
        case SCS_G1_GERMAN: designateCharset(CharsetTable::G1, CharsetId::German); break;
        case SCS_G1_SPANISH: designateCharset(CharsetTable::G1, CharsetId::Spanish); break;
        case SCS_G1_SWEDISH: designateCharset(CharsetTable::G1, CharsetId::Swedish); break;
        case SCS_G1_SWISS: designateCharset(CharsetTable::G1, CharsetId::Swiss); break;
        case SCS_G1_TECHNICAL: designateCharset(CharsetTable::G1, CharsetId::Technical); break;
        // SCS — G2/G3
        case SCS_G2_SPECIAL: designateCharset(CharsetTable::G2, CharsetId::Special); break;
        case SCS_G2_USASCII: designateCharset(CharsetTable::G2, CharsetId::USASCII); break;
        case SCS_G2_BRITISH: designateCharset(CharsetTable::G2, CharsetId::British); break;
        case SCS_G3_SPECIAL: designateCharset(CharsetTable::G3, CharsetId::Special); break;
        case SCS_G3_USASCII: designateCharset(CharsetTable::G3, CharsetId::USASCII); break;
        case SCS_G3_BRITISH: designateCharset(CharsetTable::G3, CharsetId::British); break;
        case DECALN: screenAlignmentPattern(); break;
        // S7C1T/S8C1T select 7- or 8-bit C1 transmission, but only from VT200 upward: at VT100 a real
        // VT500 ignores them -- xterm's `vtXX_level >= 2` guard.
        case S7C1T:
            if (_terminal->operatingLevel() != VTType::VT100)
                _terminal->setC1TransmissionMode(ControlTransmissionMode::S7C1T);
            break;
        case S8C1T:
            if (_terminal->operatingLevel() != VTType::VT100)
                _terminal->setC1TransmissionMode(ControlTransmissionMode::S8C1T);
            break;
        case DOCS_DEFAULT:
        case DOCS_UTF8:
            // Designate Other Coding System (ESC % @ selects ISO 8859-1, ESC % G selects UTF-8).
            // Contour's parser is always UTF-8, so both are accepted as no-ops for decoding: UTF-8 is
            // already the mode, and honouring the ISO 8859-1 default would need a Latin-1 decode path
            // Contour deliberately omits (remapping decoded codepoints would corrupt them -- see the
            // charset/UTF-8 constraint). Accepting them keeps applications that set their encoding at
            // startup (vttest) out of the unknown-sequence log.
            break;
        case DECID: sendDeviceAttributes(); break; // ESC Z: identify, answered like DA1
        case DECBI: backIndex(); break;
        case DECDHL_Top:
            configureCurrentLineSize({ LineFlag::DoubleWidth, LineFlag::DoubleHeightTop });
            break;
        case DECDHL_Bottom:
            configureCurrentLineSize({ LineFlag::DoubleWidth, LineFlag::DoubleHeightBottom });
            break;
        case DECDWL: //.
            configureCurrentLineSize({ LineFlag::DoubleWidth });
            break;
        case DECSWL: //.
            configureCurrentLineSize({});
            break;
        case DECFI: forwardIndex(); break;
        case DECKPAM: applicationKeypadMode(true); break;
        case DECKPNM: applicationKeypadMode(false); break;
        case DECRS: restoreCursor(); break;
        case DECSC: saveCursor(); break;
        case HTS: horizontalTabSet(); break;
        case IND: index(); break;
        case NEL:
            // Next Line is an index followed by a carriage return (xterm inlines the same pair). Both
            // now respect the left/right margins, so NEL scrolls only within the band -- and never when
            // the cursor sits outside it -- and returns the cursor to the left margin.
            index();
            moveCursorToBeginOfLine();
            break;
        case RI: reverseIndex(); break;
        case RIS: _terminal->hardReset(); break;
        case SS2: singleShiftSelect(CharsetTable::G2); break;
        case SS3: singleShiftSelect(CharsetTable::G3); break;
        case LS2: _cursor.charsets.lockingShift(CharsetTable::G2); break;
        case LS3: _cursor.charsets.lockingShift(CharsetTable::G3); break;
        case LS1R: _cursor.charsets.lockingShiftGR(CharsetTable::G1); break;
        case LS2R: _cursor.charsets.lockingShiftGR(CharsetTable::G2); break;
        case LS3R: _cursor.charsets.lockingShiftGR(CharsetTable::G3); break;
        case SPA:
            // Start of Protected Area (ISO 6429): guard cells written from here on with the ISO flag,
            // and arm ISO protection so the regular ED/EL/ECH spare those cells.
            _isoProtectionActive = true;
            _cursor.graphicsRendition.flags.enable(CellFlag::CharacterProtectedISO);
            break;
        case EPA:
            // End of Protected Area: stop guarding newly written cells. ISO protection itself stays
            // armed until a reset (matching xterm), so already-guarded cells keep surviving erases.
            _cursor.graphicsRendition.flags.disable(CellFlag::CharacterProtectedISO);
            break;

        // VT52 -- the legacy single-character escape grammar, dispatched only while the parser is in
        // VT52 mode (DECANM reset). Cursor moves and erases reuse the ANSI primitives.
        case VT52_CUU: moveCursorUp(LineCount(1)); break;
        case VT52_CUD: moveCursorDown(LineCount(1)); break;
        case VT52_CUF: moveCursorForward(ColumnCount(1)); break;
        case VT52_CUB: moveCursorBackward(ColumnCount(1)); break;
        case VT52_HOME: moveCursorTo(LineOffset(0), ColumnOffset(0)); break;
        case VT52_RI: reverseIndex(); break;
        case VT52_ED: clearToEndOfScreen(); break;
        case VT52_EL: clearToEndOfLine(); break;
        case VT52_CUP: {
            // ESC Y row col -- 1-based coordinates decoded by the parser; clamp to the page.
            auto const line = std::min(seq.param_or(0, 1) - 1, unbox<int>(pageSize().lines) - 1);
            auto const column = std::min(seq.param_or(1, 1) - 1, unbox<int>(pageSize().columns) - 1);
            moveCursorTo(LineOffset(std::max(0, line)), ColumnOffset(std::max(0, column)));
            break;
        }
        case VT52_GRAPHICS_ON:
            // Enter graphics mode: show the DEC special-graphics glyphs (xterm's SO). Designate G1 to
            // the special set and shift GL to it; VT52 exit / reset restores ASCII.
            designateCharset(CharsetTable::G1, CharsetId::Special);
            _cursor.charsets.lockingShift(CharsetTable::G1);
            break;
        case VT52_GRAPHICS_OFF:
            // Exit graphics mode: shift GL back to G0 (xterm's SI).
            _cursor.charsets.lockingShift(CharsetTable::G0);
            break;
        case VT52_DECID: reply("\033/Z"); break; // VT52 identify response.
        case VT52_DECKPAM: applicationKeypadMode(true); break;
        case VT52_DECKPNM: applicationKeypadMode(false); break;
        case VT52_ANSI:
            _terminal->setVT52Mode(false);
            break; // ESC < leaves VT52 for ANSI.

        // CSI
        case ANSISYSSC: restoreCursor(); break;
        case CBT:
            cursorBackwardTab(TabStopCount::cast_from(seq.param_positive_or(0, Sequence::Parameter { 1 })));
            break;
        case CHA: moveCursorToColumn(seq.param_positive_or<ColumnOffset>(0, ColumnOffset { 1 }) - 1); break;
        case CHT:
            cursorForwardTab(TabStopCount::cast_from(seq.param_positive_or(0, Sequence::Parameter { 1 })));
            break;
        case CNL:
            moveCursorToNextLine(LineCount::cast_from(seq.param_positive_or(0, Sequence::Parameter { 1 })));
            break;
        case CPL:
            moveCursorToPrevLine(LineCount::cast_from(seq.param_positive_or(0, Sequence::Parameter { 1 })));
            break;
        case ANSIDSR: return impl::ANSIDSR(seq, *this);
        case DSR: return impl::DSR(seq, *this);
        case CUB: moveCursorBackward(seq.param_positive_or<ColumnCount>(0, ColumnCount { 1 })); break;
        case CUD: moveCursorDown(seq.param_positive_or<LineCount>(0, LineCount { 1 })); break;
        case CUF: moveCursorForward(seq.param_positive_or<ColumnCount>(0, ColumnCount { 1 })); break;
        case CUP:
            moveCursorTo(LineOffset::cast_from(seq.param_positive_or<int>(0, 1) - 1),
                         ColumnOffset::cast_from(seq.param_positive_or<int>(1, 1) - 1));
            break;
        case CUU: moveCursorUp(seq.param_positive_or<LineCount>(0, LineCount { 1 })); break;
        case DA1: sendDeviceAttributes(); break;
        case DA2: sendTerminalId(); break;
        case DA3:
            // terminal identification, 4 hex codes
            reply("\033P!|C0000000\033\\");
            break;
        case DECREQTPARM: {
            // DECREPTPARM: CSI Psol ; Ppar ; Pnbits ; Pxspeed ; Prspeed ; Pclkmul ; Pflags x
            //
            // Psol echoes the request's Ps back, offset by 2. The rest describe a serial line that
            // has not existed for decades, so -- like every other terminal -- we report the same
            // fiction: no parity, eight bits, 38400 baud each way.
            auto constexpr NoParity = 1;
            auto constexpr EightBits = 1;
            auto constexpr Baud38400 = 128;
            auto constexpr ClockMultiplier = 1;
            auto constexpr StpFlags = 0;

            auto const ps = seq.param_or(0, 0);
            if (ps != 0 && ps != 1)
                return ApplyResult::Invalid;

            reply("\033[{};{};{};{};{};{};{}x",
                  ps + 2,
                  NoParity,
                  EightBits,
                  Baud38400,
                  Baud38400,
                  ClockMultiplier,
                  StpFlags);
            break;
        }
        case DCH: deleteCharacters(seq.param_positive_or<ColumnCount>(0, ColumnCount { 1 })); break;
        case DECCARA: {
            auto const area = impl::readRectangularArea(seq, 0, origin(), pageSize());
            auto const top = LineOffset::cast_from(area.top);
            auto const left = ColumnOffset::cast_from(area.left);
            auto const bottom = LineOffset::cast_from(area.bottom);
            auto const right = ColumnOffset::cast_from(area.right);
            if (_rectangularAttributeMode)
            {
                for (auto row = top; row <= bottom; ++row)
                    for (auto column = left; column <= right; ++column)
                    {
                        auto cell = at(row, column);
                        impl::applySGR(cell, seq, 4, seq.parameterCount());
                    }
            }
            else
            {
                // Stream mode: iterate left-to-right, top-to-bottom
                for (auto row = top; row <= bottom; ++row)
                {
                    auto const colStart = (row == top) ? left : ColumnOffset(0);
                    auto const colEnd = (row == bottom) ? right : ColumnOffset(*pageSize().columns - 1);
                    for (auto column = colStart; column <= colEnd; ++column)
                    {
                        auto cell = at(row, column);
                        impl::applySGR(cell, seq, 4, seq.parameterCount());
                    }
                }
            }
        }
        break;
        case DECRARA: {
            auto const area = impl::readRectangularArea(seq, 0, origin(), pageSize());
            auto const top = LineOffset::cast_from(area.top);
            auto const left = ColumnOffset::cast_from(area.left);
            auto const bottom = LineOffset::cast_from(area.bottom);
            auto const right = ColumnOffset::cast_from(area.right);
            // Build a bitmask of CellFlags to toggle from the SGR params at position 4+
            auto flagsToToggle = CellFlags {};
            for (size_t i = 4; i < seq.parameterCount(); ++i)
            {
                switch (seq.param(i))
                {
                    case 0: break; // reset has no toggle equivalent
                    case 1: flagsToToggle.enable(CellFlag::Bold); break;
                    case 2: flagsToToggle.enable(CellFlag::Faint); break;
                    case 3: flagsToToggle.enable(CellFlag::Italic); break;
                    case 4: flagsToToggle.enable(CellFlag::Underline); break;
                    case 5: flagsToToggle.enable(CellFlag::Blinking); break;
                    case 7: flagsToToggle.enable(CellFlag::Inverse); break;
                    case 8: flagsToToggle.enable(CellFlag::Hidden); break;
                    case 9: flagsToToggle.enable(CellFlag::CrossedOut); break;
                    default: break;
                }
            }
            auto const toggleCell = [&](LineOffset row, ColumnOffset column) {
                auto cell = at(row, column);
                auto const oldFlags = cell.flags();
                auto const toggled = CellFlags::from_value(oldFlags.value() ^ flagsToToggle.value());
                cell.resetFlags(toggled);
            };
            if (_rectangularAttributeMode)
            {
                for (auto row = top; row <= bottom; ++row)
                    for (auto column = left; column <= right; ++column)
                        toggleCell(row, column);
            }
            else
            {
                for (auto row = top; row <= bottom; ++row)
                {
                    auto const colStart = (row == top) ? left : ColumnOffset(0);
                    auto const colEnd = (row == bottom) ? right : ColumnOffset(*pageSize().columns - 1);
                    for (auto column = colStart; column <= colEnd; ++column)
                        toggleCell(row, column);
                }
            }
        }
        break;
        case DECCRA: {
            // The coordinates are affected by origin mode (DECOM), but not by the page margins.
            auto const sourceArea = impl::readRectangularArea(seq, 0, origin(), pageSize());
            auto const sourcePage = seq.param_or(4, 0);

            // The destination is named by its top-left corner alone; its extent is the source's.
            auto const target = impl::readRectangularArea(seq, 5, origin(), pageSize());
            auto const targetTopLeft = CellLocation { .line = LineOffset::cast_from(target.top),
                                                      .column = ColumnOffset::cast_from(target.left) };
            auto const targetPage = seq.param_or(7, 0);

            copyArea(sourceArea, sourcePage, targetTopLeft, targetPage);
        }
        break;
        case DECERA: {
            auto const area = impl::readRectangularArea(seq, 0, origin(), pageSize());
            eraseArea(unbox(area.top), unbox(area.left), unbox(area.bottom), unbox(area.right));
        }
        break;
        case DECFRA: {
            auto const ch = seq.param_or(0, Sequence::Parameter { 0 });
            auto const area = impl::readRectangularArea(seq, 1, origin(), pageSize());
            fillArea(ch, unbox(area.top), unbox(area.left), unbox(area.bottom), unbox(area.right));
        }
        break;
        case DECDC: deleteColumns(seq.param_positive_or(0, ColumnCount(1))); break;
        case DECELR: _terminal->setLocatorMode(seq.param_or(0, 0), seq.param_or(1, 0)); break;
        case DECSLE: {
            auto params = std::vector<int> {};
            for (size_t i = 0; i < seq.parameterCount(); ++i)
                params.push_back(seq.param_or(i, 0));
            _terminal->selectLocatorEvents(params);
            break;
        }
        case DECRQLP: _terminal->requestLocatorPosition(); break;
        case DECIC: insertColumns(seq.param_positive_or(0, ColumnCount(1))); break;
        case DECINVM: _terminal->invokeMacro(seq.param_or(0, 0)); break;
        case DECSACE:
            // Ps=0 or 1 → stream mode, Ps=2 → rectangle mode
            _rectangularAttributeMode = (seq.param_or(0, 1) == 2);
            break;
        // VT525 keyboard settings Contour remembers and reports through DECRQSS, but does not act on.
        case DECELF: _enableLocalFunctions = seq.param_or(0, 0); break;
        case DECLFKC: _localFunctionKeyControl = seq.param_or(0, 0); break;
        case DECSMKR: _modifierKeyReporting = seq.param_or(0, 0); break;
        case DECSCA: {
            auto const pc = seq.param_or(0, 0);
            // DECSCA (DEC) protection is per-cell via CellFlag::CharacterProtected; only the selective
            // erases (DECSED/DECSEL/DECSERA) spare it. It is independent of the ISO SPA/EPA guard.
            switch (pc)
            {
                case 1:
                    _cursor.graphicsRendition.flags.enable(CellFlag::CharacterProtected);
                    return ApplyResult::Ok;
                case 0:
                case 2:
                    _cursor.graphicsRendition.flags.disable(CellFlag::CharacterProtected);
                    return ApplyResult::Ok;
                default: return ApplyResult::Invalid;
            }
        }
        case DECSED: {
            switch (seq.param_or(0, Sequence::Parameter { 0 }))
            {
                case 0: selectiveEraseToEndOfScreen(); break;
                case 1: selectiveEraseToBeginOfScreen(); break;
                case 2: selectiveEraseScreen(); break;
                default: return ApplyResult::Unsupported;
            }
            return ApplyResult::Ok;
        }
        case DECSERA:
            selectiveEraseArea(impl::readRectangularArea(seq, 0, origin(), pageSize()));
            return ApplyResult::Ok;
        case DECSEL: {
            switch (seq.param_or(0, Sequence::Parameter { 0 }))
            {
                case 0: selectiveEraseToEndOfLine(); break;
                case 1: selectiveEraseToBeginOfLine(); break;
                case 2: selectiveEraseLine(_cursor.position.line); break;
                default: return ApplyResult::Invalid;
            }
            return ApplyResult::Ok;
        }
        case DECRM: {
            ApplyResult r = ApplyResult::Ok;
            crispy::for_each(crispy::times(seq.parameterCount()), [&](size_t i) {
                auto const t = impl::setModeDEC(seq, i, false, *_terminal);
                r = std::max(r, t);
            });
            return r;
        }
        break;
        case DECRQM:
            if (seq.parameterCount() != 1)
                return ApplyResult::Invalid;
            requestDECMode(seq.param(0));
            return ApplyResult::Ok;
        case DECRQM_ANSI:
            if (seq.parameterCount() != 1)
                return ApplyResult::Invalid;
            requestAnsiMode(seq.param(0));
            return ApplyResult::Ok;
        case DECRQCRA: {
            // CSI Pid ; Pp ; Pt ; Pl ; Pb ; Pr * y
            auto const requestId = seq.param_or(0, 0);
            // Pp is the page number; Contour has a single page, so it is echoed and ignored. The
            // rectangle (Pt;Pl;Pb;Pr, at parameter index 2) is read like every other rectangular-area
            // sequence: one-based, relative to the origin -- so in origin mode (DECOM) it is measured
            // from the scroll region's top-left, not the page's -- and clamped to the page edge.
            auto const area = impl::readRectangularArea(seq, 2, origin(), pageSize());
            auto const top = LineOffset::cast_from(area.top);
            auto const left = ColumnOffset::cast_from(area.left);
            auto const bottom = LineOffset::cast_from(area.bottom);
            auto const right = ColumnOffset::cast_from(area.right);

            auto checksum = RectangularAreaChecksum { _terminal->checksumExtension() };
            // A cell holds at most one base codepoint plus its combining marks; sized generously so
            // no realistic grapheme cluster is truncated, and without allocating per cell.
            auto codepoints = std::array<char32_t, 16> {};
            for (auto const row: std::views::iota(*top, *bottom + 1))
            {
                for (auto const column: std::views::iota(*left, *right + 1))
                {
                    auto const cell = at(LineOffset(row), ColumnOffset(column));
                    auto const count = std::min(cell.codepointCount(), codepoints.size());
                    for (auto const i: std::views::iota(size_t { 0 }, count))
                        codepoints[i] = cell.codepoint(i);
                    checksum.addCell(ChecksumCell {
                        .codepoints = std::span { codepoints.data(), count },
                        .flags = cell.flags(),
                    });
                }
                checksum.endOfLine();
            }

            reply("\033P{}!~{:04X}\033\\", requestId, checksum.result());
            return ApplyResult::Ok;
        }
        case DECRQPSR: return impl::DECRQPSR(seq, *this);
        case DECSCL: {
            // DECSCL — Set Conformance Level
            // CSI Ps1 ; Ps2 " p
            auto const level = seq.param_or(0, 65);
            auto const c1Mode = seq.param_or(1, 0);
            auto const vtType = [&]() -> std::optional<VTType> {
                switch (level)
                {
                    case 61: return VTType::VT100;
                    case 62: return VTType::VT220;
                    case 63: return VTType::VT320;
                    case 64: return VTType::VT420;
                    case 65: return VTType::VT525;
                    default: return std::nullopt;
                }
            }();
            if (!vtType)
                return ApplyResult::Invalid;

            selectConformanceLevel(*vtType);

            // DECSCL resets the terminal. The DEC manuals disagree on how much: the VT300/VT420/VT520
            // programmer manuals call it a hard reset (RIS), while the VT220 manual and DEC STD 070 (which
            // document levels 1-4 in detail) call it a soft reset. Contour takes the middle reading that
            // matches both the observable RIS effects a conformance suite checks -- the screen is erased,
            // the saved cursor returns to the origin, and insert mode (IRM) is cleared -- and xterm's
            // caution: it is a soft reset (so hardware-capability modes such as DECSET(?40) allow-80-to-132
            // are left alone, which a mere conformance-level change has no business resetting) *plus* a
            // screen erase. @see esctest DECSCLTests.test_DECSCL_RISOnChange.
            _terminal->softReset();
            clearScreen();

            // Set C1 transmission mode: Ps2=1 → 7-bit, Ps2=0 or 2 → 8-bit
            // (For level 61/VT100, C1 mode is always 7-bit)
            if (level == 61 || c1Mode == 1)
                _terminal->setC1TransmissionMode(ControlTransmissionMode::S7C1T);
            else
                _terminal->setC1TransmissionMode(ControlTransmissionMode::S8C1T);

            return ApplyResult::Ok;
        }
        case DECSCUSR: return impl::DECSCUSR(seq, *_terminal);
        case DECSCPP:
            if (auto const columnCount = seq.param_or(0, 80); columnCount == 80 || columnCount == 132)
            {
                // If the cursor is beyond the width of the new page,
                // then the cursor moves to the right column of the new page.
                if (*cursor().position.column >= columnCount)
                    cursor().position.column = ColumnOffset::cast_from(columnCount) - 1;

                // The usable (main-page) line count, not the total: the frontend adds the status-line
                // height back itself (see DECCOLM / XTWINOPS `CSI 8 t`). Passing the total double-counts
                // the status line.
                _terminal->requestWindowResize(PageSize {
                    _terminal->pageSize().lines, ColumnCount::cast_from(columnCount ? columnCount : 80) });
                return ApplyResult::Ok;
            }
            else
                return ApplyResult::Invalid;
        case DECSNLS: {
            // DECSNLS selects the number of LINES per screen; the column count is left alone.
            //
            // It used to take its parameter as a column count and resize the columns instead — so
            // `CSI 24 * |` silently narrowed an 80-column page to 24. And it resized the screen
            // directly rather than asking the frontend, which is the terminal reaching around the
            // window that owns its size. DECSCPP and DECCOLM both go through requestWindowResize();
            // so does xterm, which answers DECSNLS with `RequestResize(xw, value, -1, True)` — rows
            // to `value`, columns untouched.
            auto const lines = seq.param_or(0, 0);
            if (lines == 0)
                return ApplyResult::Ok; // Omitted or zero: no change, as in xterm.
            if (lines < 1 || lines > 255)
                return ApplyResult::Invalid;

            _terminal->requestWindowResize(
                PageSize { LineCount::cast_from(lines), _terminal->totalPageSize().columns });
            return ApplyResult::Ok;
        }
        case DECSLRM: {
            if (!_terminal->isModeEnabled(DECMode::LeftRightMargin))
                return ApplyResult::Invalid;
            auto l = decr(seq.param_opt<ColumnOffset>(0));
            auto r = decr(seq.param_opt<ColumnOffset>(1));
            _terminal->setLeftRightMargin(l, r);
            moveCursorTo({}, {});
        }
        break;
        case DECSSCLS: setScrollSpeed(seq.param_or(0, 2)); break;
        case DECSM: {
            ApplyResult r = ApplyResult::Ok;
            crispy::for_each(crispy::times(seq.parameterCount()), [&](size_t i) {
                auto const t = impl::setModeDEC(seq, i, true, *_terminal);
                r = std::max(r, t);
            });
            return r;
        }
        case DECSTBM:
            _terminal->setTopBottomMargin(decr(seq.param_opt<LineOffset>(0)),
                                          decr(seq.param_opt<LineOffset>(1)));
            moveCursorTo({}, {});
            break;
        case DECSTR:
            // For VTType VT100 and VT52 ignore this sequence
            if (_terminal->operatingLevel() == VTType::VT100)
                return ApplyResult::Invalid;
            _terminal->softReset();
            break;
        case DECTST: return impl::invokeConfidenceTest(seq, *_terminal);
        case NP: nextPage(seq.param_or(0, 1)); break;
        case PP: previousPage(seq.param_or(0, 1)); break;
        case PPA: pagePositionAbsolute(seq.param_or(0, 1)); break;
        case PPR: pagePositionRelative(seq.param_or(0, 1)); break;
        case PPB: pagePositionBackward(seq.param_or(0, 1)); break;
        case DECRQDE: requestDisplayedExtent(); break;
        case DECRQUPSS: requestUserPreferredSupplementalSet(); break;
        case DL: deleteLines(seq.param_positive_or(0, LineCount(1))); break;
        case ECH: eraseCharacters(seq.param_positive_or(0, ColumnCount(1))); break;
        case ED:
            if (seq.parameterCount() == 0)
                clearToEndOfScreen();
            else
            {
                for (size_t i = 0; i < seq.parameterCount(); ++i)
                {
                    switch (seq.param(i))
                    {
                        case 0: clearToEndOfScreen(); break;
                        case 1: clearToBeginOfScreen(); break;
                        case 2: clearScreen(); break;
                        case 3:
                            _grid.clearHistory();
                            _terminal->scrollbackBufferCleared();
                            break;
                        default: return ApplyResult::Invalid;
                    }
                }
            }
            break;
        case EL: return impl::EL(seq, *this);
        case HPA: moveCursorToColumn(seq.param_positive_or<ColumnOffset>(0, ColumnOffset { 1 }) - 1); break;
        case HPR: moveCursorForward(seq.param_positive_or<ColumnCount>(0, ColumnCount { 1 })); break;
        case HVP:
            moveCursorTo(seq.param_positive_or(0, LineOffset(1)) - 1,
                         seq.param_positive_or(1, ColumnOffset(1)) - 1);
            break; // YES, it's like a CUP!
        case ICH: insertCharacters(seq.param_positive_or(0, ColumnCount { 1 })); break;
        case IL: insertLines(seq.param_positive_or(0, LineCount { 1 })); break;
        case REP:
            if (auto const precedingChar = _terminal->parser().precedingGraphicCharacter())
            {
                // REP repeats the last graphic character through the normal text path, so autowrap, the
                // left/right margins and scrolling at the bottom margin all apply. Clamping the count to
                // the current line -- as this once did -- defeated every one of them: past the right
                // margin the repeats must wrap to the left margin of the next line, not stop dead.
                // REP's parameter is a one-based count, so `CSI b` and `CSI 0 b` both repeat once --
                // xterm folds both with one_if_default(). param_or() would take an explicit zero
                // literally and repeat nothing.
                auto const count = seq.param_positive_or<size_t>(0, 1);
                crispy::for_each(crispy::times(count), [&](auto) { writeText(precedingChar); });
            }
            break;
        case RM: {
            ApplyResult r = ApplyResult::Ok;
            crispy::for_each(crispy::times(seq.parameterCount()), [&](size_t i) {
                auto const t = impl::setAnsiMode(seq, i, false, *_terminal);
                r = std::max(r, t);
            });
            return r;
        }
        case SCOSC: saveCursor(); break;
        case SD: scrollDown(seq.param_positive_or<LineCount>(0, LineCount { 1 })); break;
        case UNSCROLL: unscroll(seq.param_or<LineCount>(0, LineCount(1))); break;
        case SBQUERY: handleSemanticBlockQuery(seq); break;
        case SETMARK:
            // TODO: deprecated. Remove in some future version.
            // Users should migrate to OSC 133.
            errorLog()("CSI > M is deprecated. Use OSC 133 instead.");
            setMark();
            break;
        case SGR: return impl::applySGR(*this, seq, 0, seq.parameterCount());
        case SGRRESTORE: restoreGraphicsRendition(); return ApplyResult::Ok;
        case SGRSAVE: saveGraphicsRendition(); return ApplyResult::Ok;
        case SM: {
            ApplyResult r = ApplyResult::Ok;
            crispy::for_each(crispy::times(seq.parameterCount()), [&](size_t i) {
                auto const t = impl::setAnsiMode(seq, i, true, *_terminal);
                r = std::max(r, t);
            });
            return r;
        }
        case SL: scrollLeft(seq.param_or<ColumnCount>(0, ColumnCount(1))); break;
        case SR: scrollRight(seq.param_or<ColumnCount>(0, ColumnCount(1))); break;
        case SU: scrollUp(seq.param_positive_or<LineCount>(0, LineCount(1))); break;
        case TBC: return impl::TBC(seq, *this);
        case VPA: moveCursorToLine(seq.param_positive_or<LineOffset>(0, LineOffset { 1 }) - 1); break;
        case VPR: moveCursorDown(seq.param_positive_or<LineCount>(0, LineCount { 1 })); break;
        case WINMANIP: return impl::WINDOWMANIP(seq, *_terminal);
        case XTRESTORE: return impl::restoreDECModes(seq, *_terminal);
        case XTSAVE: return impl::saveDECModes(seq, *_terminal);
        case DECAC: {
            // Assign Color (VT525): CSI item ; fg ; bg , |
            // item 1 = normal text (default fg/bg), item 2 = window frame (GUI tab color).
            // fg/bg are palette indices (0..255). Three forms: the item alone (1 param) resets the
            // item to its host default; item + both colors (3 params) assigns them; any other arity
            // (e.g. item + one color) is malformed and rejected rather than silently reset.
            auto const item = seq.param_or<unsigned>(0, 0);
            if (item != 1 && item != 2)
                return ApplyResult::Invalid;
            auto const paramCount = seq.parameterCount();
            if (paramCount != 1 && paramCount != 3)
                return ApplyResult::Invalid;
            auto colors = std::optional<RGBColorPair> {};
            if (paramCount == 3)
            {
                colors = impl::readAssignedColorPair(seq, _terminal->colorPalette());
                if (!colors.has_value())
                    return ApplyResult::Invalid;
            }
            switch (item)
            {
                case 1:
                    if (colors.has_value())
                    {
                        setDynamicColor(DynamicColorName::DefaultForegroundColor, colors->foreground);
                        setDynamicColor(DynamicColorName::DefaultBackgroundColor, colors->background);
                    }
                    else
                    {
                        resetDynamicColor(DynamicColorName::DefaultForegroundColor);
                        resetDynamicColor(DynamicColorName::DefaultBackgroundColor);
                    }
                    break;
                case 2:
                    // The DEC frame carries fg+bg; Contour's tab strip is a single background swatch,
                    // so the background index is used as the tab color.
                    if (colors.has_value())
                        _terminal->setWindowFrameColor(colors->background);
                    else
                        _terminal->resetWindowFrameColor();
                    break;
                default: break; // unreachable: item is validated to 1 or 2 above.
            }
            return ApplyResult::Ok;
        }
        case DECATC: {
            // Alternate Text Color (VT525): CSI attribute ; fg ; bg , }
            // attribute is the DEC-enumerated combination index from the manual's table (0=Normal,
            // 1=Bold, 2=Reverse, 3=Underline, 4=Blink, 5=Bold reverse, ...), NOT a bitmask — see
            // alternateTextColorIndexFromMask, which owns that ordering. fg/bg are palette indices
            // (0..255). Like DECAC: the attribute alone resets that entry, attribute + both colors
            // (3 params) assigns them, and any other arity is malformed and rejected rather than
            // silently reset.
            //
            // Resetting entry 0 is why DECATC accepts a zero-parameter form: a lone `0` parameter
            // collapses to "no parameter" under the VT convention (see SequenceParameterBuilder::count),
            // so `CSI 0 , }` — the only way to spell "reset the normal-text entry" — arrives here with
            // no parameters at all. Defaulting the attribute to 0 makes that form, and the equivalent
            // `CSI , }`, reset entry 0 rather than be rejected as malformed.
            auto const attribute = seq.param_or<unsigned>(0, 0);
            if (attribute >= ColorPalette::AlternateTextColorCount)
                return ApplyResult::Invalid;
            auto const paramCount = seq.parameterCount();
            if (paramCount > 1 && paramCount != 3)
                return ApplyResult::Invalid;
            auto& palette = _terminal->colorPalette();
            if (paramCount == 3)
            {
                auto const colors = impl::readAssignedColorPair(seq, palette);
                if (!colors.has_value())
                    return ApplyResult::Invalid;
                palette.setAlternateTextColor(attribute, *colors);
            }
            else
            {
                palette.resetAlternateTextColor(attribute);
            }
            return ApplyResult::Ok;
        }
        case DECSTGLT: {
            // Select Color Look-Up Table (VT525): CSI Ps ) {
            //   1/2 = alternate color (DECATC attribute colors; SGR colors ignored),
            //   3 = ANSI SGR color (power-up default, and what an omitted parameter selects).
            // The manual's monochrome table (Ps 0) is not supported: a lone 0 collapses to "no
            // parameter" in the parser (VT convention), making it indistinguishable from the omitted
            // form, so the table could not be selected even if there were one to select.
            switch (seq.param_or<unsigned>(0, 3))
            {
                case 1:
                case 2: _terminal->colorPalette().colorLookupTable = ColorLookupTable::Alternate; break;
                case 3: _terminal->colorPalette().colorLookupTable = ColorLookupTable::AnsiSgr; break;
                default: return ApplyResult::Invalid;
            }
            return ApplyResult::Ok;
        }
        case XTPOPCOLORS:
            if (!seq.parameterCount())
                _terminal->popColorPalette(0);
            else
                for (size_t i = 0; i < seq.parameterCount(); ++i)
                    _terminal->popColorPalette(seq.param<size_t>(i));
            return ApplyResult::Ok;
        case XTPUSHCOLORS:
            if (!seq.parameterCount())
                _terminal->pushColorPalette(0);
            else
                for (size_t i = 0; i < seq.parameterCount(); ++i)
                    _terminal->pushColorPalette(seq.param<size_t>(i));
            return ApplyResult::Ok;
        case XTREPORTCOLORS: _terminal->reportColorPaletteStack(); return ApplyResult::Ok;
        case XTCHECKSUM:
            _terminal->setChecksumExtension(
                ChecksumFlags::from_value(static_cast<uint8_t>(seq.param_or(0, 0))));
            break;
        case XTSMGRAPHICS: return impl::XTSMGRAPHICS(seq, *this);
        case XTVERSION:
            reply("\033P>|{} {}\033\\", LIBTERMINAL_NAME, LIBTERMINAL_VERSION_STRING);
            return ApplyResult::Ok;
        case DECSSDT: {
            // Changes the status line display type.
            switch (seq.param_or(0, 0))
            {
                case 0: _terminal->setStatusDisplay(StatusDisplayType::None); break;
                case 1: _terminal->setStatusDisplay(StatusDisplayType::Indicator); break;
                case 2:
                    if (_terminal->statusDisplayType() != StatusDisplayType::HostWritable)
                        _terminal->requestShowHostWritableStatusLine();
                    break;
                default: return ApplyResult::Invalid;
            }
            break;
        }
        case DECSASD:
            // Selects whether the terminal sends data to the main display or the status line.
            switch (seq.param_or(0, 0))
            {
                case 0:
                    if (_terminal->activeStatusDisplay() == ActiveStatusDisplay::StatusLine
                        && _terminal->syncWindowTitleWithHostWritableStatusDisplay())
                    {
                        _terminal->setWindowTitle(crispy::trimRight(
                            _terminal->hostWritableStatusLineDisplay().grid().lineText(LineOffset(0))));
                        _terminal->setSyncWindowTitleWithHostWritableStatusDisplay(false);
                    }
                    _terminal->setActiveStatusDisplay(ActiveStatusDisplay::Main);
                    break;

                case 1: _terminal->setActiveStatusDisplay(ActiveStatusDisplay::StatusLine); break;
                default: return ApplyResult::Invalid;
            }
            break;

        case DECPS: _terminal->playSound(seq.parameters()); break;
        case MODIFYOTHERKEYS: {
            auto const mode = seq.param_or(0, 0);
            _terminal->setModifyOtherKeys(mode);
            return ApplyResult::Ok;
        }
        case CSIUENTER: {
            auto const flags = KeyboardEventFlags::from_value(seq.param_or(0, 1));
            _terminal->keyboardProtocol().enter(flags);
            return ApplyResult::Ok;
        }
        case CSIUQUERY: {
            reply("\033[?{}u", _terminal->keyboardProtocol().flags().value());
            return ApplyResult::Ok;
        }
        case CSIUENHCE: {
            // Defaulting flags to 0. (Seems not to be documented by the spec, but Fish shell is doing that!)
            auto const flags = KeyboardEventFlags::from_value(seq.param_or(0, 0));
            auto const mode = seq.param_or(1, 1);
            switch (mode)
            {
                case 1: _terminal->keyboardProtocol().flags() = flags; return ApplyResult::Ok;
                case 2: _terminal->keyboardProtocol().flags().enable(flags); return ApplyResult::Ok;
                case 3: _terminal->keyboardProtocol().flags().disable(flags); return ApplyResult::Ok;
                default: break;
            }
            return ApplyResult::Invalid;
        }
        case CSIULEAVE: {
            auto const count = seq.param_or<size_t>(0, 1);
            _terminal->keyboardProtocol().leave(count);
            return ApplyResult::Ok;
        }
        // OSC
        case SETTITLE: {
            // OSC 0 sets *both* titles; OSC 1 the icon's alone, OSC 2 the window's alone. The argument is
            // hex-decoded first when the SetHex title mode is active (XTSMTITLE). @see Terminal::decodeTitle.
            auto const title = _terminal->decodeTitle(seq.intermediateCharacters());
            _terminal->setIconTitle(title);
            _terminal->setWindowTitle(title);
            return ApplyResult::Ok;
        }
        case SETICON: _terminal->setIconTitle(_terminal->decodeTitle(seq.intermediateCharacters())); break;
        case SETWINTITLE:
            _terminal->setWindowTitle(_terminal->decodeTitle(seq.intermediateCharacters()));
            break;
        case SETTABNAME: _terminal->setTabName(seq.intermediateCharacters()); break;
        case SETXPROP: return ApplyResult::Unsupported;
        case SETCOLPAL: return impl::setOrRequestColorPalette(seq, *_terminal, impl::IndexedColorSelector);
        case SETSPECIALCOLPAL:
            return impl::setOrRequestColorPalette(seq, *_terminal, impl::SpecialColorSelector);
        case RCOLPAL: return impl::resetColorPalette(seq, *_terminal, impl::IndexedColorSelector);
        case RCOLSPECIALPAL: return impl::resetColorPalette(seq, *_terminal, impl::SpecialColorSelector);
        case SETCWD: return impl::SETCWD(seq, *this);
        case HYPERLINK: return impl::HYPERLINK(seq, *this);
        case XTCAPTURE: return impl::CAPTURE(seq, *_terminal);
        case XTSMTITLE: return impl::setTitleModes(seq, *_terminal, true);
        case XTRMTITLE: return impl::setTitleModes(seq, *_terminal, false);
        case COLORFG:
            return impl::setOrRequestDynamicColor(seq, *this, DynamicColorName::DefaultForegroundColor);
        case COLORBG:
            return impl::setOrRequestDynamicColor(seq, *this, DynamicColorName::DefaultBackgroundColor);
        case COLORCURSOR:
            return impl::setOrRequestDynamicColor(seq, *this, DynamicColorName::TextCursorColor);
        case COLORMOUSEFG:
            return impl::setOrRequestDynamicColor(seq, *this, DynamicColorName::MouseForegroundColor);
        case COLORMOUSEBG:
            return impl::setOrRequestDynamicColor(seq, *this, DynamicColorName::MouseBackgroundColor);
        case COLORHIGHLIGHTFG:
            return impl::setOrRequestDynamicColor(seq, *this, DynamicColorName::HighlightForegroundColor);
        case COLORHIGHLIGHTBG:
            return impl::setOrRequestDynamicColor(seq, *this, DynamicColorName::HighlightBackgroundColor);
        case SETFONT: return impl::setFont(seq, *_terminal);
        case SETFONTALL: return impl::setAllFont(seq, *_terminal);
        case CLIPBOARD: return impl::clipboard(seq, *_terminal);
        // TODO: case COLORSPECIAL: return impl::setOrRequestDynamicColor(seq, _output,
        // DynamicColorName::HighlightForegroundColor);
        case RCOLORFG: resetDynamicColor(DynamicColorName::DefaultForegroundColor); break;
        case RCOLORBG: resetDynamicColor(DynamicColorName::DefaultBackgroundColor); break;
        case RCOLORCURSOR: resetDynamicColor(DynamicColorName::TextCursorColor); break;
        case RCOLORMOUSEFG: resetDynamicColor(DynamicColorName::MouseForegroundColor); break;
        case RCOLORMOUSEBG: resetDynamicColor(DynamicColorName::MouseBackgroundColor); break;
        case RCOLORHIGHLIGHTFG: resetDynamicColor(DynamicColorName::HighlightForegroundColor); break;
        case RCOLORHIGHLIGHTBG: resetDynamicColor(DynamicColorName::HighlightBackgroundColor); break;
        case CONEMU: return impl::CONEMU(seq, *this);
        case NOTIFY: return impl::NOTIFY(seq, *this);
        case ITERM2: processITerm2(seq.intermediateCharacters()); return ApplyResult::Ok;
        case TEXTSIZING: return processTextSizing(seq.intermediateCharacters());
        case KITTYCLIPBOARD: return processKittyClipboard(seq.intermediateCharacters());
        case POINTERSHAPE: return processPointerShape(seq.intermediateCharacters());
        case DESKTOPNOTIFY: return impl::DESKTOPNOTIFY(seq, *_terminal);
        case DUMPSTATE: inspect(); break;
        case SEMA: processShellIntegration(seq); break;

        // hooks
        case DECAUPSS:
            // Ps names the set's size and has only two readings. A third value names nothing, so the
            // sequence is rejected outright and deliberately left un-hooked: SequenceBuilder::put
            // discards a DCS payload when no hook is installed, so the designator cannot reach the
            // screen as text.
            if (auto const ps = seq.param_or(0, 0); ps != 0 && ps != 1)
                return ApplyResult::Invalid;
            _terminal->hookParser(hookDECAUPSS(seq));
            break;
        case DECDLD: _terminal->hookParser(hookDECDLD(seq)); break;
        case DECDMAC: _terminal->hookParser(hookDECDMAC(seq)); break;
        case DECSIXEL: _terminal->hookParser(hookSixel(seq)); break;
        case REGIS: _terminal->hookParser(hookReGIS(seq)); break;
        case STP: _terminal->hookParser(hookSTP(seq)); break;
        case DECRQSS: _terminal->hookParser(hookDECRQSS(seq)); break;
        case DECUDK: _terminal->hookParser(hookDECUDK(seq)); break;
        case XTGETTCAP: _terminal->hookParser(hookXTGETTCAP(seq)); break;
        case GIP: _terminal->hookParser(hookGoodImageProtocol(seq)); break;

        default: return ApplyResult::Unsupported;
    }
    return ApplyResult::Ok;
}

// @brief Configures the current line's size attributes, used for DECDHL and related sequences.

void Screen::configureCurrentLineSize(LineFlags enabled)
{
    auto constexpr LineSizeAttributes = LineFlags {
        LineFlag::DoubleHeightTop,
        LineFlag::DoubleHeightBottom,
        LineFlag::DoubleWidth,
    };

    Require(enabled.without(LineSizeAttributes).none());

    auto& line = currentLine();

    line.flags().disable(LineSizeAttributes);
    line.flags().enable(LineSizeAttributes.intersect(enabled));

    // See:
    //     https://vt100.net/docs/vt510-rm/DECDHL.html
    //     https://vt100.net/docs/vt510-rm/DECDWL.html
    // The spec says:
    // """
    // If the line was single width and single height,
    // then all characters to the right of the screen's center are lost.
    // """
    // However, xterm seems to retain them, so we do nothing here.
    //
    // if (enabled.intersect(LineSizeAttributes).any())
    //     clearToEndOfLine();

    (void) enabled;
}

unique_ptr<ParserExtension> Screen::hookSixel(Sequence const& seq)
{
    auto const pa = seq.param_or(0, 1);
    auto const pb = seq.param_or(1, 2);

    auto const aspectVertical = sixelAspectVertical(pa);
    auto const aspectHorizontal = 1;
    auto const transparentBackground = pb == 1;

    _sixelImageBuilder = make_unique<SixelImageBuilder>(
        _terminal->maxImageSize(),
        aspectVertical,
        aspectHorizontal,
        transparentBackground ? RGBAColor { 0, 0, 0, 0 } : _terminal->colorPalette().defaultBackground,
        _terminal->usePrivateColorRegisters()
            ? make_shared<SixelColorPalette>(_terminal->maxSixelColorRegisters(),
                                             std::clamp(_terminal->maxSixelColorRegisters(), 0u, 16384u))
            : _terminal->sixelColorPalette());

    return make_unique<SixelParser>(*_sixelImageBuilder, [this]() {
        {
            sixelImage(_sixelImageBuilder->size(), std::move(_sixelImageBuilder->data()));
        }
    });
}

unique_ptr<ParserExtension> Screen::hookReGIS(Sequence const& seq)
{
    // ReGIS state persists across DCS strings, so the interpreter context and canvas live on the
    // Screen and are created once, lazily. A fresh, lightweight parser references them each hook.
    if (!_regisContext)
    {
        // Render the canvas at a supersampled resolution so ReGIS text and graphics stay crisp when
        // the fixed 800x480 logical addressing space is scaled up to the display. The context carries
        // the factor and the matching (supersampled) canvasSize -- both preserved across resets -- so
        // userToPixel maps logical coordinates onto the larger buffer automatically.
        auto const canvasSize = ImageSize { Width(regis::DefaultAddressWidth * regis::RegisSupersample),
                                            Height(regis::DefaultAddressHeight * regis::RegisSupersample) };
        _regisContext = make_unique<regis::ReGISContext>();
        _regisContext->supersample = regis::RegisSupersample;
        _regisContext->canvasSize = canvasSize;
        _regisCanvas = make_unique<regis::ReGISRasterizer>(canvasSize);
        _regisEvents = make_unique<regis::CallbackReGISEvents>(
            [this](string_view data) { reply(data); },
            [this]() -> std::optional<std::pair<int, int>> {
                // Report the current mouse position for the ReGIS interactive locator R(P(I)):
                // grid cell -> canvas pixel -> ReGIS user coordinates.
                auto const gridPosition = _terminal->currentMouseGridPosition();
                if (!gridPosition || !_regisContext || !_regisCanvas)
                    return std::nullopt;
                auto const columns = unbox<double>(pageSize().columns);
                auto const lines = unbox<double>(pageSize().lines);
                if (columns <= 0 || lines <= 0)
                    return std::nullopt;
                auto const canvasWidth = unbox<double>(_regisCanvas->size().width);
                auto const canvasHeight = unbox<double>(_regisCanvas->size().height);
                auto const pixel = crispy::point {
                    .x = static_cast<int>((unbox<double>(gridPosition->column) / columns) * canvasWidth),
                    .y = static_cast<int>((unbox<double>(gridPosition->line) / lines) * canvasHeight),
                };
                auto const [userX, userY] = _regisContext->pixelToUser(pixel);
                return std::pair<int, int> { static_cast<int>(std::lround(userX)),
                                             static_cast<int>(std::lround(userY)) };
            });
    }
    // Use the embedded font unless the display injected a text_shaper-backed rasterizer.
    if (!_regisTextRasterizer)
        _regisTextRasterizer = make_shared<regis::EmbeddedReGISTextRasterizer>();

    // Pass the rasterizer as a shared_ptr (not a raw reference): the parser persists across PTY read
    // chunks, and a session rebind may reassign _regisTextRasterizer between them; shared ownership
    // keeps the in-flight referent alive.
    auto parser = make_unique<regis::ReGISParser>(
        *_regisContext, *_regisCanvas, _regisTextRasterizer, *_regisEvents, [this]() { commitReGIS(); });

    // Pmode 1 or 3 resets the graphics state and clears the canvas; 0 and 2 resume the persistent
    // context (position, colours, write controls and addressing window carry over).
    auto const pMode = seq.param_or(0, 0);
    if (pMode == 1 || pMode == 3)
    {
        _regisContext->reset();
        _regisCanvas->eraseTo(RGBAColor { 0 }); // transparent
        // Ensure the cleared canvas is published even if this DCS string draws nothing, so a
        // reset-only reset erases the previously committed graphics from the grid.
        parser->notifyCanvasCleared();
    }

    return parser;
}

void Screen::commitReGIS()
{
    if (!_regisCanvas)
        return;
    // The canvas persists across DCS strings, so publish a copy and leave the original intact for
    // subsequent drawing to build upon. One copy is unavoidable here: uploadImage() takes ownership
    // of the pixels while the canvas must retain them. The uploaded images are not leaked -- the
    // ImagePool is an LRU cache that evicts superseded ReGIS frames -- so the per-commit cost is the
    // single buffer copy, not unbounded growth.
    auto pixels = _regisCanvas->data();
    regisImage(_regisCanvas->size(), std::move(pixels));
}

void Screen::regisImage(ImageSize pixelSize, Image::Data&& rgbaData)
{
    // ReGIS graphics are a full-screen plane: scale the canvas across the whole page and place it
    // above the text, so drawn pixels overlay the cells and transparent areas reveal them.
    auto const imageRef = uploadImage(ImageFormat::RGBA, pixelSize, std::move(rgbaData));
    renderImage(imageRef,
                CellLocation {},
                GridSize { .lines = pageSize().lines, .columns = pageSize().columns },
                PixelCoordinate {},
                pixelSize,
                ImageAlignment::TopStart,
                ImageResize::ResizeToFit,
                /*autoScroll*/ false,
                /*updateCursor*/ false,
                ImageLayer::Above);
}

unique_ptr<ParserExtension> Screen::hookSTP(Sequence const& /*seq*/)
{
    return make_unique<SimpleStringCollector>(
        [this](string_view const& data) { _terminal->setTerminalProfile(unicode::convert_to<char>(data)); });
}

unique_ptr<ParserExtension> Screen::hookXTGETTCAP(Sequence const& /*seq*/)
{
    // DCS + q Pt ST
    //           Request Termcap/Terminfo String (XTGETTCAP), xterm.  The
    //           string following the "q" is a list of names encoded in
    //           hexadecimal (2 digits per character) separated by ; which
    //           correspond to termcap or terminfo key names.
    //           A few special features are also recognized, which are not key
    //           names:
    //
    //           o   Co for termcap colors (or colors for terminfo colors), and
    //
    //           o   TN for termcap name (or name for terminfo name).
    //
    //           o   RGB for the ncurses direct-color extension.
    //               Only a terminfo name is provided, since termcap
    //               applications cannot use this information.
    //
    //           xterm responds with
    //           DCS 1 + r Pt ST for valid requests, adding to Pt an = , and
    //           the value of the corresponding string that xterm would send,
    //           or
    //           DCS 0 + r Pt ST for invalid requests.
    //           The strings are encoded in hexadecimal (2 digits per
    //           character).

    return make_unique<SimpleStringCollector>([this](string_view const& data) {
        auto const capsInHex = crispy::split(data, ';');
        for (auto hexCap: capsInHex)
        {
            auto const hexCap8 = unicode::convert_to<char>(hexCap);
            if (auto const capOpt = crispy::fromHexString(string_view(hexCap8.data(), hexCap8.size())))
                requestCapability(capOpt.value());
        }
    });
}

unique_ptr<ParserExtension> Screen::hookDECRQSS(Sequence const& /*seq*/)
{
    return make_unique<SimpleStringCollector>([this](string_view data) {
        auto const s = [](string_view dataString) -> optional<RequestStatusString> {
            auto const mappings = array<pair<string_view, RequestStatusString>, 15> {
                pair { "m", RequestStatusString::SGR },       pair { "\"p", RequestStatusString::DECSCL },
                pair { " q", RequestStatusString::DECSCUSR }, pair { "\"q", RequestStatusString::DECSCA },
                pair { "*x", RequestStatusString::DECSACE },  pair { "+q", RequestStatusString::DECELF },
                pair { "*}", RequestStatusString::DECLFKC },  pair { "+r", RequestStatusString::DECSMKR },
                pair { "r", RequestStatusString::DECSTBM },   pair { "s", RequestStatusString::DECSLRM },
                pair { "t", RequestStatusString::DECSLPP },   pair { "$|", RequestStatusString::DECSCPP },
                pair { "$}", RequestStatusString::DECSASD },  pair { "$~", RequestStatusString::DECSSDT },
                pair { "*|", RequestStatusString::DECSNLS }
            };
            for (auto const& mapping: mappings)
                if (dataString == mapping.first)
                    return mapping.second;
            return nullopt;
        }(data);

        if (s.has_value())
            requestStatusString(s.value());

        // TODO: handle batching
    });
}

unique_ptr<ParserExtension> Screen::hookDECAUPSS(Sequence const& seq)
{
    // DECAUPSS — Assign User-Preferred Supplemental Set
    // DCS Ps ! u D...D ST
    //
    // Ps is the set's *size* (0 = 94-character, 1 = 96-character), not a free parameter: it must agree
    // with the designator that follows, so `DCS 0 ! u A ST` is US ASCII while `DCS 1 ! u A ST` is ISO
    // Latin-1 -- the same designator naming two different sets. A disagreeing Ps names no set at all.
    //
    // param_or (not param_positive_or): zero is a legitimate value here, and is also the default.
    auto const is96 = seq.param_or(0, 0) == 1;

    return make_unique<SimpleStringCollector>([this, is96](string_view data) {
        // The designator is either a lone final byte ("A") or an intermediate plus a final ("%5").
        auto const upss = [&]() -> std::optional<UserPreferredSupplementalSet> {
            switch (data.size())
            {
                case 1: return findUserPreferredSupplementalSet('\0', data[0], is96);
                case 2: return findUserPreferredSupplementalSet(data[0], data[1], is96);
                default: return std::nullopt;
            }
        }();

        // A set DEC introduced above the terminal's operating level is not assignable here -- the
        // DEC/ISO Greek, Hebrew, Turkish and Cyrillic sets are VT500-era, finer-grained than the VT320
        // gate DECAUPSS itself carries. Gate on conformanceLevelOf(), never on VTType ordering:
        // VTType's values are the DA2 encoding and are not level-ordered (VT330 = 18, VT340 = 19, but
        // VT320 = 24).
        if (!upss.has_value()
            || conformanceLevelOf(_terminal->operatingLevel())
                   < conformanceLevelOf(upss->minimumConformanceLevel))
            return; // Names no set we can honour: leave UPSS as it was.

        _terminal->setUserPreferredSupplementalSet(*upss);
    });
}

unique_ptr<ParserExtension> Screen::hookDECDLD(Sequence const& seq)
{
    // DECDLD — Down-Line-Load Character Set (DRCS)
    // DCS Pfn;Pcn;Pe;Pcmw;Pw;Pt;Pcmh;Pcss { Dscs Sxbp1;Sxbp2;...ST
    auto const fontNumber = seq.param_or(0, 0);
    auto const startingChar = seq.param_or(1, 0);
    auto const eraseControl = seq.param_or(2, 0);
    auto const charMatrixWidth = seq.param_or(3, 0);
    auto const fontWidth = seq.param_or(4, 0);
    auto const textOrFullCell = seq.param_or(5, 0);
    auto const charMatrixHeight = seq.param_or(6, 0);
    auto const charsetSize = seq.param_or(7, 0);

    return make_unique<SimpleStringCollector>([this,
                                               fontNumber,
                                               startingChar,
                                               eraseControl,
                                               charMatrixWidth,
                                               fontWidth,
                                               textOrFullCell,
                                               charMatrixHeight,
                                               charsetSize](string_view data) {
        // Data starts with Dscs (charset designator), followed by sixel glyph data.
        // Dscs is either 1 byte (final only) or 2 bytes (intermediate 0x20-0x2F + final).
        auto designator = string_view {};
        auto glyphData = data;
        if (data.size() >= 2 && data[0] >= 0x20 && data[0] <= 0x2F)
        {
            // Two-byte designator: intermediate + final
            designator = data.substr(0, 2);
            glyphData = data.substr(2);
        }
        else if (!data.empty())
        {
            designator = data.substr(0, 1);
            glyphData = data.substr(1);
        }
        _terminal->defineDRCS(fontNumber,
                              startingChar,
                              eraseControl,
                              charMatrixWidth,
                              fontWidth,
                              textOrFullCell,
                              charMatrixHeight,
                              charsetSize,
                              designator,
                              glyphData);
    });
}

unique_ptr<ParserExtension> Screen::hookDECDMAC(Sequence const& seq)
{
    // DECDMAC — Define Macro
    // DCS Pid ; Pdt ; Pen ! z D...D ST
    auto const macroId = seq.param_or(0, 0);
    auto const deleteAll = seq.param_or(1, 0) == 1;
    auto const hexEncoded = seq.param_or(2, 0) == 1;

    return make_unique<SimpleStringCollector>([this, macroId, deleteAll, hexEncoded](string_view data) {
        auto body = std::string {};
        if (hexEncoded)
        {
            // Decode hex-encoded data: pairs of hex digits → bytes
            body.reserve(data.size() / 2);
            for (size_t i = 0; i + 1 < data.size(); i += 2)
            {
                auto const hi = data[i];
                auto const lo = data[i + 1];
                auto const hexToByte = [](char ch) -> uint8_t {
                    if (ch >= '0' && ch <= '9')
                        return static_cast<uint8_t>(ch - '0');
                    if (ch >= 'A' && ch <= 'F')
                        return static_cast<uint8_t>(ch - 'A' + 10);
                    if (ch >= 'a' && ch <= 'f')
                        return static_cast<uint8_t>(ch - 'a' + 10);
                    return 0;
                };
                body.push_back(static_cast<char>((hexToByte(hi) << 4) | hexToByte(lo)));
            }
        }
        else
        {
            body = std::string(data);
        }

        _terminal->defineMacro(macroId, deleteAll, std::move(body));
    });
}

unique_ptr<ParserExtension> Screen::hookDECUDK(Sequence const& seq)
{
    // DECUDK — User-Defined Keys
    // DCS Pc ; Pl | Ky1/St1 ; Ky2/St2 ; ... ST
    auto const clearAll = seq.param_or(0, 0) == 0;
    auto const locked = seq.param_or(1, 0) == 0;

    return make_unique<SimpleStringCollector>(
        [this, clearAll, locked](string_view data) { _terminal->programUDK(clearAll, locked, data); });
}

optional<CellLocation> Screen::search(std::u32string_view searchText, CellLocation startPosition)
{
    // TODO use LogicalLines to spawn logical lines for improving the search on wrapped lines.

    auto const isCaseSensitive =
        std::any_of(searchText.begin(), searchText.end(), [](auto ch) { return std::isupper(ch); });

    if (searchText.empty())
        return nullopt;

    // First try match at start location.
    if (_grid.lineAt(startPosition.line)
            .matchTextAtWithSensetivityMode(searchText, startPosition.column, isCaseSensitive))
        return startPosition;

    // Search reverse until found or exhausted.
    auto const lines = _grid.logicalLinesFrom(startPosition.line);
    for (auto const& line: lines)
    {
        auto const result = line.search(searchText, startPosition.column, isCaseSensitive);
        if (result.has_value())
            return result; // new match found
        startPosition.column = ColumnOffset(0);
    }
    return nullopt;
}

optional<CellLocation> Screen::searchReverse(std::u32string_view searchText, CellLocation startPosition)
{
    // TODO use LogicalLinesReverse to spawn logical lines for improving the search on wrapped lines.
    auto const isCaseSensitive =
        std::any_of(searchText.begin(), searchText.end(), [](auto ch) { return std::isupper(ch); });

    if (searchText.empty())
        return nullopt;

    // First try match at start location.
    if (_grid.lineAt(startPosition.line)
            .matchTextAtWithSensetivityMode(searchText, startPosition.column, isCaseSensitive))
        return startPosition;

    // Search reverse until found or exhausted.
    auto const lines = _grid.logicalLinesReverseFrom(startPosition.line);
    for (auto const& line: lines)
    {
        auto const result = line.searchReverse(searchText, startPosition.column, isCaseSensitive);
        if (result.has_value())
            return result; // new match found
        startPosition.column = boxed_cast<ColumnOffset>(pageSize().columns) - 1;
    }
    return nullopt;
}

bool Screen::isCursorInsideHorizontalMargins() const noexcept
{
    return !_terminal->isModeEnabled(DECMode::LeftRightMargin)
           || margin().horizontal.contains(_cursor.position.column);
}

bool Screen::isCursorInsideMargins() const noexcept
{
    bool const insideVerticalMargin = margin().vertical.contains(_cursor.position.line);
    return insideVerticalMargin && isCursorInsideHorizontalMargins();
}

unique_ptr<ParserExtension> Screen::hookGoodImageProtocol(Sequence const&)
{
    if (!_terminal->settings().goodImageProtocol)
        return nullptr;
    return make_unique<MessageParser>([this](Message message) {
        auto const* const operation = message.header("o");
        if (!operation || operation->empty())
            return; // Missing operation header — silently ignore.

        auto const op = (*operation)[0];
        switch (op)
        {
            case 'u': handleGipUpload(std::move(message)); break;
            case 'r': handleGipRender(message); break;
            case 'd': handleGipRelease(message); break;
            case 's': handleGipOneshot(std::move(message)); break;
            case 'q': handleGipQuery(); break;
            default: break; // Unrecognized operation — silently ignore.
        }
    });
}

void Screen::handleGipUpload(Message message)
{
    auto const* const name = message.header("n");
    auto const imageFormat = toImageFormat(message.header("f"));
    auto const width = Width::cast_from(toNumber(message.header("w")).value_or(0));
    auto const height = Height::cast_from(toNumber(message.header("h")).value_or(0));
    auto const size = ImageSize { width, height };
    auto const requestStatus = message.header("s") != nullptr;

    // Validate name: ASCII alphanumeric + underscore, 1-512 chars.
    auto const validName = [&]() -> bool {
        if (!name || name->empty() || name->size() > 512)
            return false;
        return std::ranges::all_of(*name, [](char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
                   || ch == '_';
        });
    }();

    // Resolve Auto format from the data before validation.
    auto const resolvedFormat = [&]() -> std::optional<ImageFormat> {
        if (imageFormat.has_value() && *imageFormat == ImageFormat::Auto)
            return resolveAutoFormat(std::span<uint8_t const>(message.body().data(), message.body().size()),
                                     size);
        return imageFormat;
    }();

    // PNG width/height are optional (extracted from PNG header).
    bool const validImage = resolvedFormat.has_value()
                            && (*resolvedFormat == ImageFormat::PNG || (*size.width > 0 && *size.height > 0));

    // Validate RGB/RGBA data size: body must equal width * height * bytes-per-pixel.
    auto const validDataSize = validImage && resolvedFormat.has_value()
                               && isConsistentPixmap(*resolvedFormat, size, message.body().size());

    if (validName && validImage && validDataSize)
    {
        uploadImage(*name, resolvedFormat.value(), size, message.takeBody());
        if (requestStatus)
            replyGipStatus(0);
    }
    else if (requestStatus)
    {
        replyGipStatus(2); // invalid image data
    }
}

void Screen::handleGipRender(Message const& message)
{
    auto const* const name = message.header("n");
    auto const params = parseGipRenderParams(message);
    auto const x = PixelCoordinate::X { toNumber(message.header("x")).value_or(0) };
    auto const y = PixelCoordinate::Y { toNumber(message.header("y")).value_or(0) };

    renderImageByName(name ? *name : "",
                      GridSize { .lines = params.screenRows, .columns = params.screenCols },
                      PixelCoordinate { .x = x, .y = y },
                      ImageSize { params.imageWidth, params.imageHeight },
                      params.alignmentPolicy,
                      params.resizePolicy,
                      params.autoScroll,
                      params.requestStatus,
                      params.updateCursor,
                      params.layer);
}

void Screen::handleGipRelease(Message const& message)
{
    if (auto const* const name = message.header("n"); name)
        releaseImage(*name);
}

void Screen::handleGipQuery()
{
    auto constexpr MaxImages = 100; // matches ImagePool LRU capacity
    auto const maxSize = _terminal->maxImageSize();
    auto const maxBytes =
        static_cast<unsigned long>(*maxSize.width) * *maxSize.height * 4; // RGBA = 4 bytes per pixel
    _terminal->reply(
        "\033P!gs=8,m={},b={},w={},h={}\033\\", MaxImages, maxBytes, *maxSize.width, *maxSize.height);
    _terminal->flushInput();
}

void Screen::handleGipOneshot(Message message)
{
    auto const params = parseGipRenderParams(message);
    auto imageFormat = toImageFormat(message.header("f"));

    // Resolve Auto format from the data before rendering.
    if (imageFormat.has_value() && *imageFormat == ImageFormat::Auto)
    {
        imageFormat =
            resolveAutoFormat(std::span<uint8_t const>(message.body().data(), message.body().size()),
                              ImageSize { params.imageWidth, params.imageHeight });
        if (!imageFormat.has_value())
        {
            if (params.requestStatus)
                replyGipStatus(2);
            return;
        }
    }

    auto const success = renderImage(imageFormat.value_or(ImageFormat::RGB),
                                     ImageSize { params.imageWidth, params.imageHeight },
                                     message.takeBody(),
                                     GridSize { .lines = params.screenRows, .columns = params.screenCols },
                                     params.alignmentPolicy,
                                     params.resizePolicy,
                                     params.autoScroll,
                                     params.updateCursor,
                                     params.layer);

    if (params.requestStatus)
        replyGipStatus(success ? 0 : 2);
}

void Screen::replyGipStatus(int statusCode)
{
    _terminal->reply("\033P!gs={}\033\\", statusCode);
    _terminal->flushInput();
}

std::optional<Image::Data> Screen::decodePng(std::span<uint8_t const> data, ImageSize& size) const
{
    if (!_terminal->imageDecoder())
        return std::nullopt;
    auto decoded = _terminal->imageDecoder()(ImageFormat::PNG, data, size);

    // The decoder is an injected dependency, so its output is as untrusted as wire data: the renderer
    // uploads `size` to the GPU verbatim and would read past a buffer that does not match it.
    if (decoded.has_value() && !isConsistentPixmap(ImageFormat::RGBA, size, decoded->size()))
    {
        errorLog()(
            "Rejecting decoded PNG: {} bytes do not match the decoded size {}.", decoded->size(), size);
        return std::nullopt;
    }
    return decoded;
}

void Screen::uploadImage(string name, ImageFormat format, ImageSize imageSize, Image::Data&& pixmap)
{
    assert(format != ImageFormat::Auto && "Auto must be resolved before upload");
    if (!isConsistentPixmap(format, imageSize, pixmap.size()))
    {
        errorLog()("Rejecting image {}: {} pixmap of {} bytes does not match declared size {}.",
                   name,
                   format,
                   pixmap.size(),
                   imageSize);
        return;
    }
    if (format == ImageFormat::PNG)
    {
        auto decodedSize = imageSize;
        if (auto decodedData = decodePng(pixmap, decodedSize))
            _terminal->imagePool().link(std::move(name),
                                        uploadImage(ImageFormat::RGBA, decodedSize, std::move(*decodedData)));
        else
            errorLog()("Failed to decode PNG image for upload.");
        return;
    }

    _terminal->imagePool().link(std::move(name), uploadImage(format, imageSize, std::move(pixmap)));
}

void Screen::renderImageByName(std::string const& name,
                               GridSize gridSize,
                               PixelCoordinate imageOffset,
                               ImageSize imageSize,
                               ImageAlignment alignmentPolicy,
                               ImageResize resizePolicy,
                               bool autoScroll,
                               bool requestStatus,
                               bool updateCursor,
                               ImageLayer layer)
{
    auto const imageRef = _terminal->imagePool().findImageByName(name);
    auto const topLeft = _cursor.position;

    if (imageRef)
        renderImage(imageRef,
                    topLeft,
                    gridSize,
                    imageOffset,
                    imageSize,
                    alignmentPolicy,
                    resizePolicy,
                    autoScroll,
                    updateCursor,
                    layer);

    if (requestStatus)
        replyGipStatus(imageRef ? 0 : 1);
}

bool Screen::renderImage(ImageFormat format,
                         ImageSize imageSize,
                         Image::Data&& pixmap,
                         GridSize gridSize,
                         ImageAlignment alignmentPolicy,
                         ImageResize resizePolicy,
                         bool autoScroll,
                         bool updateCursor,
                         ImageLayer layer)
{
    assert(format != ImageFormat::Auto && "Auto must be resolved before render");
    auto constexpr PixelOffset = PixelCoordinate {};
    auto constexpr PixelSize = ImageSize {};

    // The renderer uploads the declared geometry to the GPU verbatim, reading width * height * 4 bytes
    // from this buffer -- a pixmap that does not match its geometry would be read out of bounds.
    if (!isConsistentPixmap(format, imageSize, pixmap.size()))
    {
        errorLog()("Rejecting image: {} pixmap of {} bytes does not match declared size {}.",
                   format,
                   pixmap.size(),
                   imageSize);
        return false;
    }

    auto const topLeft = _cursor.position;

    auto const computeGridSize = [&](ImageSize decodedPixelSize) -> GridSize {
        if (*gridSize.lines && *gridSize.columns)
            return gridSize;
        auto const cellSize = _terminal->cellPixelSize();
        auto const columns = ColumnCount::cast_from(
            std::ceil(decodedPixelSize.width.as<double>() / cellSize.width.as<double>()));
        auto const lines = LineCount::cast_from(
            std::ceil(decodedPixelSize.height.as<double>() / cellSize.height.as<double>()));
        return GridSize { .lines = *gridSize.lines ? gridSize.lines : lines,
                          .columns = *gridSize.columns ? gridSize.columns : columns };
    };

    if (format == ImageFormat::PNG)
    {
        auto decodedSize = imageSize;
        if (auto decodedData = decodePng(pixmap, decodedSize))
        {
            auto const effectiveGridSize = computeGridSize(decodedSize);
            auto const imageRef = uploadImage(ImageFormat::RGBA, decodedSize, std::move(*decodedData));
            renderImage(imageRef,
                        topLeft,
                        effectiveGridSize,
                        PixelOffset,
                        PixelSize,
                        alignmentPolicy,
                        resizePolicy,
                        autoScroll,
                        updateCursor,
                        layer);
            return true;
        }
        errorLog()("Failed to decode PNG image for oneshot render.");
        return false;
    }

    auto const effectiveGridSize = computeGridSize(imageSize);
    auto const imageRef = uploadImage(format, imageSize, std::move(pixmap));

    renderImage(imageRef,
                topLeft,
                effectiveGridSize,
                PixelOffset,
                PixelSize,
                alignmentPolicy,
                resizePolicy,
                autoScroll,
                updateCursor,
                layer);
    return true;
}

void Screen::releaseImage(std::string const& name)
{
    _terminal->imagePool().unlink(name);
}

} // namespace vtbackend
