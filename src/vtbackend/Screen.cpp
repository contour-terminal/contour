// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/ControlCode.h>
#include <vtbackend/DesktopNotification.h>
#include <vtbackend/InputGenerator.h>
#include <vtbackend/MessageParser.h>
#include <vtbackend/Screen.h>
#include <vtbackend/SixelParser.h>
#include <vtbackend/SoAClusterWriter.h>
#include <vtbackend/Terminal.h>
#include <vtbackend/VTType.h>
#include <vtbackend/VTWriter.h>
#include <vtbackend/logging.h>

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
#include <libunicode/word_segmenter.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <iterator>
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
        else if (isDefaultColor(sgr.foregroundColor))
            sgrAdd(39);
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
        else if (isDefaultColor(sgr.backgroundColor))
            sgrAdd(49);
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
        && _cursor.charsets.isSelected(CharsetId::USASCII))
    {
        crlfIfWrapPending();

        auto const columnsAvailable =
            static_cast<size_t>(pageSize().columns.value - _cursor.position.column.value);

        auto& line = currentLine();
        auto& soa = line.storage();
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
            else if (_cursor.autoWrap)
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
            if (prevCol < unbox<size_t>(prevLine.size()))
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
                auto const extendedWidth = usePreviousCell().appendCharacter(cp);
                clearAndAdvance(0, extendedWidth);
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
    if (_pendingCharTraceLog.empty())
        return;

    if (vtTraceSequenceLog)
        vtTraceSequenceLog()("[{}] text: \"{}\"", _name, _pendingCharTraceLog);

    _pendingCharTraceLog.clear();
#endif
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
    if (_cursor.wrapPending && _cursor.autoWrap) // && !_terminal->isModeEnabled(DECMode::TextReflow))
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
        if (prevCol < unbox<size_t>(prevLine.size()))
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
        auto const extendedWidth = usePreviousCell().appendCharacter(codepoint);
        clearAndAdvance(0, extendedWidth);
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

    if (cell.isFlagEnabled(CellFlag::WideCharContinuation) && _cursor.position.column > ColumnOffset(0))
    {
        // Erase the left half of the wide char.
        auto prevCell = line.useCellAt(_cursor.position.column - 1);
        prevCell.reset(_cursor.graphicsRendition);
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

void Screen::clearAndAdvance(int oldWidth, int newWidth) noexcept
{
    bool const cursorInsideMargin =
        _terminal->isModeEnabled(DECMode::LeftRightMargin) && isCursorInsideMargins();
    auto const cellsAvailable = cursorInsideMargin ? *(margin().horizontal.to - _cursor.position.column) - 1
                                                   : *pageSize().columns - *_cursor.position.column - 1;

    auto const sgr = newWidth > 1 ? _cursor.graphicsRendition.with(CellFlag::WideCharContinuation)
                                  : _cursor.graphicsRendition;
    auto& line = currentLine();
    for (int i = 1; i < std::min(std::max(oldWidth, newWidth), cellsAvailable); ++i)
        line.useCellAt(_cursor.position.column + i).reset(sgr, _cursor.hyperlink);

    if (newWidth == std::min(newWidth, cellsAvailable))
        _cursor.position.column += ColumnOffset::cast_from(newWidth);
    else if (_cursor.autoWrap)
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
    _cursor.wrapPending = false;
    _cursor.position.column = newColumn;
    if (unbox(historyLineCount()) > 0)
        _terminal->addLineOffsetToJumpHistory(LineOffset { 1 });
    if (*realCursorPosition().line == *margin().vertical.to)
    {
        // TODO(perf) if we know that we text is following this LF
        // (i.e. parser state will be ground state),
        // then invoke scrollUpUninitialized instead
        // and make sure the subsequent text write will
        // possibly also reset remaining grid cells in that line
        // if the incoming text did not write to the full line
        scrollUp(LineCount(1), _cursor.graphicsRendition, margin());
    }
    else
    {
        // using moveCursorTo() would embrace code reusage,
        // but due to the fact that it's fully recalculating iterators,
        // it may be faster to just incrementally update them.
        // moveCursorTo({logicalCursorPosition().line + 1, margin().horizontal.from});
        _cursor.position.line++;
        updateCursorIterator();
    }
}

void Screen::scrollUp(LineCount n, GraphicsAttributes sgr, Margin margin)
{
    auto const scrollCount = _grid.scrollUp(n, sgr, margin);
    updateCursorIterator();
    // TODO only call onBufferScrolled if full page margin
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
    auto const clampedCol = std::min(col, boxed_cast<ColumnOffset>(pageSize().columns) - 1);
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
    if (_cursor.position.column.value)
        _cursor.position.column--;
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

    // Pgr: GR charset table index (GR not tracked; default to G2 per VT standard)
    auto const pgr = 2;

    // Scss: character set size for each G-set (0x40 base)
    // Bit N = G(N-1) size: 0=94-char, 1=96-char. All supported charsets are 94-char.
    auto const scss = static_cast<char>(0x40);

    // Sdesig: SCS designation final characters for G0 through G3
    auto const sdesig = std::string { charsetDesignation(charsets.charsetIdOf(CharsetTable::G0)),
                                      charsetDesignation(charsets.charsetIdOf(CharsetTable::G1)),
                                      charsetDesignation(charsets.charsetIdOf(CharsetTable::G2)),
                                      charsetDesignation(charsets.charsetIdOf(CharsetTable::G3)) };

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

    auto da = DeviceAttributes::AnsiColor |
              // TODO: DeviceAttributes::AnsiTextLocator |
              DeviceAttributes::CaptureScreenBuffer | DeviceAttributes::Columns132 |
              DeviceAttributes::HorizontalScrolling |
              // TODO: DeviceAttributes::NationalReplacementCharacterSets |
              DeviceAttributes::RectangularEditing | DeviceAttributes::SelectiveErase |
              DeviceAttributes::SixelGraphics | DeviceAttributes::StatusDisplay |
              // TODO: DeviceAttributes::TechnicalCharacters |
              DeviceAttributes::TextMacros |
              // TODO: DeviceAttributes::UserDefinedKeys |
              DeviceAttributes::Windowing | DeviceAttributes::ClipboardExtension;
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
    clearToEndOfLine();

    for (auto const lineOffset: iota(unbox(_cursor.position.line) + 1, unbox(pageSize().lines)))
    {
        Line& line = _grid.lineAt(LineOffset::cast_from(lineOffset));
        line.reset(_grid.defaultLineFlags(), _cursor.graphicsRendition);
    }
}

void Screen::clearToBeginOfScreen()
{
    clearToBeginOfLine();

    for (auto const lineOffset: iota(0, *_cursor.position.line))
    {
        Line& line = _grid.lineAt(LineOffset::cast_from(lineOffset));
        line.reset(_grid.defaultLineFlags(), _cursor.graphicsRendition);
    }
}

void Screen::clearScreen()
{
    // Instead of *just* clearing the screen, and thus, losing potential important content,
    // we scroll up by RowCount number of lines, so move it all into history, so the user can scroll
    // up in case the content is still needed.
    scrollUp(_grid.pageSize().lines);
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

    auto& line = currentLine();
    for (int i = 0; i < clampedN; ++i)
        line.useCellAt(_cursor.position.column + i).reset(_cursor.graphicsRendition);
}

// {{{ DECSEL

void Screen::selectiveEraseToEndOfLine()
{
    if (isFullHorizontalMargins() && _cursor.position.column.value == 0)
        selectiveEraseLine(_cursor.position.line);
    else
        selectiveErase(
            _cursor.position.line, _cursor.position.column, ColumnOffset::cast_from(pageSize().columns));
}

void Screen::selectiveEraseToBeginOfLine()
{
    if (isFullHorizontalMargins() && _cursor.position.column.value == pageSize().columns.value)
        selectiveEraseLine(_cursor.position.line);
    else
        selectiveErase(_cursor.position.line, ColumnOffset(0), _cursor.position.column + 1);
}

void Screen::selectiveEraseLine(LineOffset line)
{
    if (containsProtectedCharacters(line, ColumnOffset(0), ColumnOffset::cast_from(pageSize().columns)))
    {
        selectiveErase(line, ColumnOffset(0), ColumnOffset::cast_from(pageSize().columns));
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

void Screen::selectiveErase(LineOffset line, ColumnOffset begin, ColumnOffset end)
{
    for (auto col = begin; col < end; ++col)
    {
        auto cell = at(line, col);
        if (!cell.isFlagEnabled(CellFlag::CharacterProtected))
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

bool Screen::containsProtectedCharacters(LineOffset line, ColumnOffset begin, ColumnOffset end) const
{
    for (auto col = begin; col < end; ++col)
    {
        auto cell = at(line, col);
        if (cell.isFlagEnabled(CellFlag::CharacterProtected))
            return true;
    }
    return false;
}
// }}}
// {{{ DECSED

void Screen::selectiveEraseToEndOfScreen()
{
    selectiveEraseToEndOfLine();

    auto const lineStart = unbox(_cursor.position.line) + 1;
    auto const lineEnd = unbox(pageSize().lines);

    for (auto const lineOffset: iota(lineStart, lineEnd))
        selectiveEraseLine(LineOffset::cast_from(lineOffset));
}

void Screen::selectiveEraseToBeginOfScreen()
{
    selectiveEraseToBeginOfLine();

    for (auto const lineOffset: iota(0, *_cursor.position.line))
        selectiveEraseLine(LineOffset::cast_from(lineOffset));
}

void Screen::selectiveEraseScreen()
{
    for (auto const lineOffset: iota(0, *pageSize().lines))
        selectiveEraseLine(LineOffset::cast_from(lineOffset));
}
// }}}
// {{{ DECSERA

void Screen::selectiveEraseArea(Rect area)
{
    auto const [top, left, bottom, right] = applyOriginMode(area).clampTo(_settings->pageSize);
    assert(unbox(right) <= unbox(pageSize().columns));
    assert(unbox(bottom) <= unbox(pageSize().lines));

    if (top.value > bottom.value || left.value > right.value)
        return;

    for (int y = top.value; y <= bottom.value; ++y)
    {
        for (int x = left.value; x <= right.value; ++x)
        {
            auto cell = grid().lineAt(LineOffset::cast_from(y)).useCellAt(ColumnOffset::cast_from(x));
            if (!cell.isFlagEnabled(CellFlag::CharacterProtected))
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
    if (isFullHorizontalMargins() && _cursor.position.column.value == 0)
    {
        currentLine().reset(currentLine().flags(), _cursor.graphicsRendition);
        return;
    }

    auto& storage = currentLine().storage();
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
    auto& storage = _grid.lineAt(_cursor.position.line).storage();
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
    moveCursorTo(logicalCursorPosition().line + n.as<LineOffset>(), ColumnOffset(0));
}

void Screen::moveCursorToPrevLine(LineCount n)
{
    auto const sanitizedN = std::min(n.as<LineOffset>(), logicalCursorPosition().line);
    moveCursorTo(logicalCursorPosition().line - sanitizedN, ColumnOffset(0));
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
    auto& storage = line.storage();
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
        auto& storage = line.storage();
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
        auto& storage = line.storage();
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

    auto const [x0, xInc, xEnd] = [&]() {
        if (samePage && *targetTopLeft.column > *sourceArea.left) // moving right on same page
            return std::tuple { *sourceArea.right - *sourceArea.left, -1, -1 };
        else
            return std::tuple { 0, +1, *sourceArea.right - *sourceArea.left + 1 };
    }();

    auto const [y0, yInc, yEnd] = [&]() {
        if (samePage && *targetTopLeft.line > *sourceArea.top) // moving down on same page
            return std::tuple { *sourceArea.bottom - *sourceArea.top, -1, -1 };
        else
            return std::tuple { 0, +1, *sourceArea.bottom - *sourceArea.top + 1 };
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
                    (void) targetCell.appendCharacter(sourceCell.codepoint(ci));
            }
            else
            {
                targetCell.reset(attrs, sourceCell.hyperlink());
            }
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
    if (isCursorInsideMargins() && *n != 0)
        deleteChars(realCursorPosition().line, realCursorPosition().column, n);
}

void Screen::deleteChars(LineOffset lineOffset, ColumnOffset column, ColumnCount columnsToDelete)
{
    auto& line = _grid.lineAt(lineOffset);
    auto& storage = line.storage();
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
    // even if you move to 80th of 80 columns, it'll first write a char and THEN flag wrap pending
    if (margin().horizontal.contains(_cursor.position.column))
        _cursor.position.column = margin().horizontal.clamp(_cursor.position.column - n.as<ColumnOffset>());
    else
        _cursor.position.column = clampedColumn(_cursor.position.column + boxed_cast<ColumnOffset>(n));
    _cursor.wrapPending = false;
}

void Screen::moveCursorToColumn(ColumnOffset column)
{
    setCurrentColumn(column);
}

void Screen::moveCursorToBeginOfLine()
{
    setCurrentColumn(ColumnOffset(0));
}

void Screen::moveCursorToLine(LineOffset n)
{
    moveCursorTo(n, _cursor.position.column);
}

void Screen::moveCursorToNextTab()
{
    // TODO: I guess something must remember when a \t was added, for proper move-back?
    // TODO: respect HTS/TBC

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

    if (!_terminal->tabs().empty())
    {
        for (unsigned k = 0; k < unbox<unsigned>(count); ++k)
        {
            auto const i = std::find_if(
                rbegin(_terminal->tabs()), rend(_terminal->tabs()), [&](ColumnOffset tabPos) -> bool {
                    return tabPos < logicalCursorPosition().column;
                });
            if (i != rend(_terminal->tabs()))
            {
                // prev tab found -> move to prev tab
                moveCursorToColumn(*i);
            }
            else
            {
                moveCursorToColumn(margin().horizontal.from);
                break;
            }
        }
    }
    else if (TabWidth.value)
    {
        // default tab settings
        if (*_cursor.position.column < *TabWidth)
            moveCursorToBeginOfLine();
        else
        {
            auto const m = (*_cursor.position.column + 1) % *TabWidth;
            auto const n = m ? (*count - 1) * *TabWidth + m : *count * *TabWidth + m;
            moveCursorBackward(ColumnCount(n - 1));
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
    if (*realCursorPosition().line == *margin().vertical.to)
        scrollUp(LineCount(1));
    else
        moveCursorDown(LineCount(1));
}

void Screen::reverseIndex()
{
    if (unbox(realCursorPosition().line) == unbox(margin().vertical.from))
        scrollDown(LineCount(1));
    else
        moveCursorUp(LineCount(1));
}

void Screen::backIndex()
{
    if (realCursorPosition().column == margin().horizontal.from)
        ; // TODO: scrollRight(1);
    else
        moveCursorForward(ColumnCount(1));
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
    auto const modeResponse = [&](auto mode) -> ModeResponse {
        if (isValidAnsiMode(mode))
        {
            if (_terminal->isModeEnabled(static_cast<AnsiMode>(mode)))
                return ModeResponse::Set;
            else
                return ModeResponse::Reset;
        }
        return ModeResponse::NotRecognized;
    }(mode);

    reply("\033[{};{}$y", mode, static_cast<unsigned>(modeResponse));
}

void Screen::requestDECMode(unsigned int mode)
{
    auto const modeResponse = [this, mode]() -> ModeResponse {
        auto const modeEnum = fromDECModeNum(mode);
        if (modeEnum.has_value())
        {
            auto const modeEnum = fromDECModeNum(mode);
            if (_terminal->isModeEnabled(modeEnum.value()))
                return ModeResponse::Set;
            else
                return ModeResponse::Reset;
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

    // fills the complete screen area with a test pattern
    for (auto& line: _grid.mainPage())
    {
        line.fill(_grid.defaultLineFlags(), GraphicsAttributes {}, U'E', 1);
    }
}

void Screen::applicationKeypadMode(bool enable)
{
    _terminal->setApplicationkeypadMode(enable);
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

void Screen::requestDynamicColor(DynamicColorName name)
{
    auto const color = [&]() -> optional<RGBColor> {
        switch (name)
        {
            case DynamicColorName::DefaultForegroundColor: return _terminal->colorPalette().defaultForeground;
            case DynamicColorName::DefaultBackgroundColor: return _terminal->colorPalette().defaultBackground;
            case DynamicColorName::TextCursorColor:
                if (holds_alternative<CellForegroundColor>(_terminal->colorPalette().cursor.color))
                    return _terminal->colorPalette().defaultForeground;
                else if (holds_alternative<CellBackgroundColor>(_terminal->colorPalette().cursor.color))
                    return _terminal->colorPalette().defaultBackground;
                else
                    return get<RGBColor>(_terminal->colorPalette().cursor.color);
            case DynamicColorName::MouseForegroundColor: return _terminal->colorPalette().mouseForeground;
            case DynamicColorName::MouseBackgroundColor: return _terminal->colorPalette().mouseBackground;
            case DynamicColorName::HighlightForegroundColor:
                if (holds_alternative<RGBColor>(_terminal->colorPalette().selection.foreground))
                    return get<RGBColor>(_terminal->colorPalette().selection.foreground);
                else
                    return nullopt;
            case DynamicColorName::HighlightBackgroundColor:
                if (holds_alternative<RGBColor>(_terminal->colorPalette().selection.background))
                    return get<RGBColor>(_terminal->colorPalette().selection.background);
                else
                    return nullopt;
        }
        return nullopt; // should never happen
    }();

    if (color.has_value())
    {
        reply("\033]{};{}\033\\", setDynamicColorCommand(name), setDynamicColorValue(color.value()));
    }
}

void Screen::requestPixelSize(RequestPixelSize area)
{
    switch (area)
    {
        case RequestPixelSize::WindowArea: [[fallthrough]]; // TODO
        case RequestPixelSize::TextArea: {
            // Result is CSI  4 ;  height ;  width t
            reply("\033[4;{};{}t", _terminal->pixelSize().height, _terminal->pixelSize().width);
            break;
        }
        case RequestPixelSize::CellArea:
            // Result is CSI  6 ;  height ;  width t
            reply("\033[6;{};{}t", _terminal->cellPixelSize().height, _terminal->cellPixelSize().width);
            break;
    }
}

void Screen::requestCharacterSize(RequestPixelSize area)
{
    switch (area)
    {
        case RequestPixelSize::TextArea: reply("\033[8;{};{}t", pageSize().lines, pageSize().columns); break;
        case RequestPixelSize::WindowArea:
            reply("\033[9;{};{}t", pageSize().lines, pageSize().columns);
            break;
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
                return std::format("{};{}r", 1 + *margin().vertical.from, *margin().vertical.to);
            case RequestStatusString::DECSLRM:
                return std::format("{};{}s", 1 + *margin().horizontal.from, *margin().horizontal.to);
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
    {
        auto hexValue = std::format("{:X}", value);
        if (hexValue.size() % 2)
            hexValue.insert(hexValue.begin(), '0');
        reply("\033P1+r{}={}\033\\", toHexString(name), hexValue);
    }
    else if (auto const value = stringCapability(name); !value.empty())
        reply("\033P1+r{}={}\033\\", toHexString(name), asHex(value));
    else
        reply("\033P0+r\033\\");
}

void Screen::requestCapability(capabilities::Code code)
{
    if (booleanCapability(code))
        reply("\033P1+r{}\033\\", code.hex());
    else if (auto const value = numericCapability(code); value >= 0)
    {
        auto hexValue = std::format("{:X}", value);
        if (hexValue.size() % 2)
            hexValue.insert(hexValue.begin(), '0');
        reply("\033P1+r{}={}\033\\", code.hex(), hexValue);
    }
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
                    auto const viewportSize = _terminal->pixelSize();
                    reply("\033[?{};{};{};{}S",
                          SixelItem,
                          Success,
                          std::min(viewportSize.width, _terminal->maxImageSize().width),
                          std::min(viewportSize.height, _terminal->maxImageSize().height));
                }
                break;
                case Action::ReadLimit:
                    reply("\033[?{};{};{};{}S",
                          SixelItem,
                          Success,
                          _settings->maxImageSize.width,
                          _settings->maxImageSize.height);
                    break;
                case Action::ResetToDefault:
                    // The limit is the default at the same time.
                    _terminal->setMaxImageSize(_settings->maxImageSize);
                    break;
                case Action::SetToValue:
                    if (holds_alternative<ImageSize>(value))
                    {
                        auto const size = std::min(get<ImageSize>(value), _settings->maxImageSize);
                        _terminal->setMaxImageSize(size);
                        reply("\033[?{};{};{};{}S", SixelItem, Success, size.width, size.height);
                    }
                    else
                        reply("\033[?{};{};{}S", SixelItem, Failure, 0);
                    break;
            }
            break;

        case Item::ReGISGraphicsGeometry: // Surely, we don't do ReGIS just yet. :-)
            break;
    }
}
// }}}

// {{{ impl namespace (some command generator helpers)
namespace impl
{
    namespace
    {
        ApplyResult setAnsiMode(Sequence const& seq, size_t modeIndex, bool enable, Terminal& term)
        {
            switch (seq.param(modeIndex))
            {
                case 2: // (AM) Keyboard Action Mode
                    return ApplyResult::Unsupported;
                case 4: // (IRM) Insert Mode
                    term.setMode(AnsiMode::Insert, enable);
                    return ApplyResult::Ok;
                case 12: // (SRM) Send/Receive Mode
                case 20: // (LNM) Automatic Newline
                default: return ApplyResult::Unsupported;
            }
        }

        ApplyResult setModeDEC(Sequence const& seq, size_t modeIndex, bool enable, Terminal& term)
        {
            if (auto const modeOpt = fromDECModeNum(seq.param(modeIndex)); modeOpt.has_value())
            {
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

        ApplyResult ANSIDSR(Sequence const& seq, Screen& screen)
        {
            switch (seq.param(0))
            {
                case 5: screen.deviceStatusReport(); return ApplyResult::Ok;
                case 6: screen.reportCursorPosition(); return ApplyResult::Ok;
                default: return ApplyResult::Unsupported;
            }
        }

        ApplyResult DSR(Sequence const& seq, Screen& screen)
        {
            switch (seq.param(0))
            {
                case ColorPaletteUpdateDsrRequestId:
                    screen.reportColorPaletteUpdate();
                    return ApplyResult::Ok;
                default: return ApplyResult::Unsupported;
            }
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

        ApplyResult setOrRequestDynamicColor(Sequence const& seq, Screen& screen, DynamicColorName name)
        {
            auto const& value = seq.intermediateCharacters();
            if (value == "?")
                screen.requestDynamicColor(name);
            else if (auto color = vtbackend::parseColor(value); color.has_value())
                screen.setDynamicColor(name, color.value());
            else
                return ApplyResult::Invalid;

            return ApplyResult::Ok;
        }

        bool queryOrSetColorPalette(string_view text,
                                    std::function<void(uint8_t)> queryColor,
                                    std::function<void(uint8_t, RGBColor)> setColor)
        {
            // Sequence := [Param (';' Param)*]
            // Param    := Index ';' Query | Set
            // Index    := DIGIT+
            // Query    := ?'
            // Set      := 'rgb:' Hex8 '/' Hex8 '/' Hex8
            // Hex8     := [0-9A-Za-z] [0-9A-Za-z]
            // DIGIT    := [0-9]
            int index = -1;
            return crispy::split(text, ';', [&](string_view value) {
                if (index < 0)
                {
                    index = crispy::to_integer<10, int>(value).value_or(-1);
                    if (!(0 <= index && index <= 0xFF))
                        return false;
                }
                else if (value == "?"sv)
                {
                    queryColor((uint8_t) index);
                    index = -1;
                }
                else if (auto const color = vtbackend::parseColor(value))
                {
                    setColor((uint8_t) index, color.value());
                    index = -1;
                }
                else
                    return false;

                return true;
            });
        }

        ApplyResult RCOLPAL(Sequence const& seq, Terminal& terminal)
        {
            if (seq.intermediateCharacters().empty())
            {
                terminal.colorPalette() = terminal.defaultColorPalette();
                return ApplyResult::Ok;
            }

            auto const index = crispy::to_integer<10, uint8_t>(seq.intermediateCharacters());
            if (!index.has_value())
                return ApplyResult::Invalid;

            terminal.colorPalette().palette[*index] = terminal.defaultColorPalette().palette[*index];

            return ApplyResult::Ok;
        }

        ApplyResult SETCOLPAL(Sequence const& seq, Terminal& terminal)
        {
            bool const ok = queryOrSetColorPalette(
                seq.intermediateCharacters(),
                [&](uint8_t index) {
                    auto const color = terminal.colorPalette().palette.at(index);
                    terminal.reply("\033]4;{};rgb:{:04x}/{:04x}/{:04x}\033\\",
                                   index,
                                   static_cast<uint16_t>(color.red) << 8 | color.red,
                                   static_cast<uint16_t>(color.green) << 8 | color.green,
                                   static_cast<uint16_t>(color.blue) << 8 | color.blue);
                },
                [&](uint8_t index, RGBColor color) { terminal.colorPalette().palette.at(index) = color; });

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
            // Only setting clipboard contents is supported, not reading.
            auto const& params = seq.intermediateCharacters();
            if (auto const splits = crispy::split(params, ';');
                splits.size() == 2 && (splits[0] == "c" || splits[0].empty()))
            {
                terminal.copyToClipboard(crispy::base64::decode(splits[1]));
                return ApplyResult::Ok;
            }
            else
                return ApplyResult::Invalid;
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

        ApplyResult WINDOWMANIP(Sequence const& seq, Terminal& terminal)
        {
            if (seq.parameterCount() == 3)
            {
                switch (seq.param(0))
                {
                    case 4: // resize in pixel units
                        terminal.requestWindowResize(ImageSize { Width(seq.param(2)), Height(seq.param(1)) });
                        break;
                    case 8: // resize in cell units
                        terminal.requestWindowResize(PageSize { LineCount::cast_from(seq.param(1)),
                                                                ColumnCount::cast_from(seq.param(2)) });
                        break;
                    case 22: terminal.saveWindowTitle(); break;
                    case 23: terminal.restoreWindowTitle(); break;
                    default: return ApplyResult::Unsupported;
                }
                return ApplyResult::Ok;
            }
            else if (seq.parameterCount() == 2 || seq.parameterCount() == 1)
            {
                switch (seq.param(0))
                {
                    case 4:
                    case 8:
                        // this means, resize to full display size
                        // TODO: just create a dedicated callback for fulscreen resize!
                        terminal.requestWindowResize(ImageSize {});
                        return ApplyResult::Ok;
                    case 14:
                        if (seq.parameterCount() == 2 && seq.param(1) == 2)
                            terminal.primaryScreen().requestPixelSize(
                                RequestPixelSize::WindowArea); // CSI 14 ; 2 t
                        else
                            terminal.primaryScreen().requestPixelSize(RequestPixelSize::TextArea); // CSI 14 t
                        return ApplyResult::Ok;
                    case 16:
                        terminal.primaryScreen().requestPixelSize(RequestPixelSize::CellArea);
                        return ApplyResult::Ok;
                    case 18:
                        terminal.primaryScreen().requestCharacterSize(RequestPixelSize::TextArea);
                        return ApplyResult::Ok;
                    case 19:
                        terminal.primaryScreen().requestCharacterSize(RequestPixelSize::WindowArea);
                        return ApplyResult::Ok;
                    case 22: {
                        switch (seq.param_or(1, 0))
                        {
                            case 0:
                                // CSI 22 ; 0 t | save icon & window title
                                terminal.saveWindowTitle();
                                return ApplyResult::Ok;
                            case 1:
                                // CSI 22 ; 1 t | save icon title
                                return ApplyResult::Unsupported;
                            case 2:
                                // CSI 22 ; 2 t | save window title
                                terminal.saveWindowTitle();
                                return ApplyResult::Ok;
                            default: return ApplyResult::Unsupported;
                        }
                    }
                    case 23: {
                        switch (seq.param_or(1, 0))
                        {
                            case 0:
                                terminal.restoreWindowTitle();
                                break; // CSI 22 ; 0 t | save icon & window title
                            case 1: return ApplyResult::Unsupported;      // CSI 22 ; 1 t | save icon title
                            case 2: terminal.restoreWindowTitle(); break; // CSI 22 ; 2 t | save window title
                            default: return ApplyResult::Unsupported;
                        }
                        return ApplyResult::Ok;
                    }
                    default: return ApplyResult::Invalid;
                }
            }
            else
                return ApplyResult::Unsupported;
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
            // Even though VT means Vertical Tab, it seems that xterm is doing an IND instead.
            [[fallthrough]];
        case FF.finalSymbol:
            // Even though FF means Form Feed, it seems that xterm is doing an IND instead.
            index();
            break;
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
    _cursor = savedCursor;
    _cursor.position = clampCoordinate(_cursor.position);
    _terminal->setMode(DECMode::AutoWrap, savedCursor.autoWrap);
    _terminal->setMode(DECMode::Origin, savedCursor.originMode);
    updateCursorIterator();
    verifyState();
}

void Screen::reply(std::string_view text)
{
    _terminal->reply(text);
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

    /// Prepends a line of text to an accumulator, inserting a newline separator.
    void prependLine(std::string& accumulator, std::string_view lineText)
    {
        if (!accumulator.empty())
            accumulator = std::string(lineText).append(1, '\n').append(accumulator);
        else
            accumulator = lineText;
    }
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

    // Scan grid backward to find text regions for each block.
    struct BlockTextInfo
    {
        std::string prompt;
        std::string output;
        int outputLineCount = 0;
    };
    auto blockTexts = std::vector<BlockTextInfo>(blocks.size());

    auto const cursorLine = cursor().position.line;
    auto const historyTop = -boxed_cast<LineOffset>(_grid.historyLineCount());

    enum class ScanState : uint8_t
    {
        Searching,
        InOutput,
        InPrompt
    };

    auto state = ScanState::Searching;
    auto blockIndex = 0;
    auto currentOutput = std::string {};
    auto currentPrompt = std::string {};
    auto currentOutputLineCount = 0;

    /// Finalizes the current block's text and advances to the next block.
    auto const finalizeBlock = [&]() {
        blockTexts[blockIndex].output = std::move(currentOutput);
        blockTexts[blockIndex].outputLineCount = currentOutputLineCount;
        blockTexts[blockIndex].prompt = std::move(currentPrompt);
        currentOutput.clear();
        currentPrompt.clear();
        currentOutputLineCount = 0;
        ++blockIndex;
    };

    /// Handles a grid line in the Searching state.
    auto const handleSearching = [&](auto const& gridLine, auto flags) {
        if (flags.contains(LineFlag::CommandEnd))
        {
            state = ScanState::InOutput;
            currentOutput = gridLine.toUtf8Trimmed(false, true);
            currentOutputLineCount = 1;
        }
    };

    /// Handles a grid line in the InOutput state.
    auto const handleInOutput = [&](auto const& gridLine, auto flags) {
        auto const lineText = gridLine.toUtf8Trimmed(false, true);
        if (flags.contains(LineFlag::OutputStart))
        {
            prependLine(currentOutput, lineText);
            ++currentOutputLineCount;
            state = ScanState::InPrompt;
        }
        else if (flags.contains(LineFlag::Marked))
        {
            currentPrompt = lineText;
            finalizeBlock();
            state = (flags.contains(LineFlag::CommandEnd) && blockIndex < static_cast<int>(blocks.size()))
                        ? ScanState::InOutput
                        : ScanState::Searching;
        }
        else
        {
            prependLine(currentOutput, lineText);
            ++currentOutputLineCount;
        }
    };

    /// Handles a grid line in the InPrompt state.
    auto const handleInPrompt = [&](auto const& gridLine, auto flags) {
        auto const lineText = gridLine.toUtf8Trimmed(false, true);
        if (flags.contains(LineFlag::Marked))
        {
            prependLine(currentPrompt, lineText);
            finalizeBlock();
            state = (flags.contains(LineFlag::CommandEnd) && blockIndex < static_cast<int>(blocks.size()))
                        ? ScanState::InOutput
                        : ScanState::Searching;
        }
        else
        {
            prependLine(currentPrompt, lineText);
        }
    };

    // Scan backward from cursor through main page and history.
    for (auto line = cursorLine; line >= historyTop && blockIndex < static_cast<int>(blocks.size()); --line)
    {
        auto const& gridLine = _grid.lineAt(line);
        auto const flags = gridLine.flags();

        switch (state)
        {
            case ScanState::Searching: handleSearching(gridLine, flags); break;
            case ScanState::InOutput: handleInOutput(gridLine, flags); break;
            case ScanState::InPrompt: handleInPrompt(gridLine, flags); break;
        }
    }

    // If we were still collecting when we ran out of lines, finalize.
    if (blockIndex < static_cast<int>(blocks.size())
        && (state == ScanState::InOutput || state == ScanState::InPrompt))
    {
        finalizeBlock();
    }

    // Build JSON response with blocks in chronological order (oldest first).
    auto json = std::string {};
    json += "{\"version\":1,\"blocks\":[";
    auto const actualCount = std::min(blockIndex, static_cast<int>(blocks.size()));
    for (auto i = actualCount - 1; i >= 0; --i)
    {
        if (i != actualCount - 1)
            json += ',';
        json += formatBlockJson(
            *blocks[i], blockTexts[i].prompt, blockTexts[i].output, blockTexts[i].outputLineCount);
    }
    json += "]}";
    reply("{}{}{}", SBQueryResponseSuccess, json, DcsTerminator);
}

void Screen::setMark()
{
    currentLine().setMarked(true);
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
            _terminal->shellIntegration().promptEnd();
            break;
        }
        case 'C': {
            if (_terminal->isModeEnabled(DECMode::SemanticBlockProtocol))
                enableLineFlags(cursor().position.line, LineFlag::OutputStart, true);
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
            if (_terminal->isModeEnabled(DECMode::SemanticBlockProtocol))
                enableLineFlags(cursor().position.line, LineFlag::CommandEnd, true);
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
        case ApplyResult::Invalid: {
            vtParserLog()("Invalid VT sequence: {}", seq);
            break;
        }
        case ApplyResult::Unsupported: {
            vtParserLog()("Unsupported VT sequence: {}", seq);
            break;
        }
        case ApplyResult::Ok: {
            _terminal->verifyState();
            break;
        }
    }
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
        case VT: [[fallthrough]];
        case FF: index(); break;
        case CR: moveCursorToBeginOfLine(); break;

        // ESC
        case SCS_G0_SPECIAL: designateCharset(CharsetTable::G0, CharsetId::Special); break;
        case SCS_G0_USASCII: designateCharset(CharsetTable::G0, CharsetId::USASCII); break;
        case SCS_G1_SPECIAL: designateCharset(CharsetTable::G1, CharsetId::Special); break;
        case SCS_G1_USASCII: designateCharset(CharsetTable::G1, CharsetId::USASCII); break;
        case DECALN: screenAlignmentPattern(); break;
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
        case NEL: moveCursorToNextLine(LineCount(1)); break;
        case RI: reverseIndex(); break;
        case RIS: _terminal->hardReset(); break;
        case SS2: singleShiftSelect(CharsetTable::G2); break;
        case SS3: singleShiftSelect(CharsetTable::G3); break;

        // CSI
        case ANSISYSSC: restoreCursor(); break;
        case CBT:
            cursorBackwardTab(TabStopCount::cast_from(seq.param_or(0, Sequence::Parameter { 1 })));
            break;
        case CHA: moveCursorToColumn(seq.param_or<ColumnOffset>(0, ColumnOffset { 1 }) - 1); break;
        case CHT:
            cursorForwardTab(TabStopCount::cast_from(seq.param_or(0, Sequence::Parameter { 1 })));
            break;
        case CNL:
            moveCursorToNextLine(LineCount::cast_from(seq.param_or(0, Sequence::Parameter { 1 })));
            break;
        case CPL:
            moveCursorToPrevLine(LineCount::cast_from(seq.param_or(0, Sequence::Parameter { 1 })));
            break;
        case ANSIDSR: return impl::ANSIDSR(seq, *this);
        case DSR: return impl::DSR(seq, *this);
        case CUB: moveCursorBackward(seq.param_or<ColumnCount>(0, ColumnCount { 1 })); break;
        case CUD: moveCursorDown(seq.param_or<LineCount>(0, LineCount { 1 })); break;
        case CUF: moveCursorForward(seq.param_or<ColumnCount>(0, ColumnCount { 1 })); break;
        case CUP:
            moveCursorTo(LineOffset::cast_from(seq.param_or<int>(0, 1) - 1),
                         ColumnOffset::cast_from(seq.param_or<int>(1, 1) - 1));
            break;
        case CUU: moveCursorUp(seq.param_or<LineCount>(0, LineCount { 1 })); break;
        case DA1: sendDeviceAttributes(); break;
        case DA2: sendTerminalId(); break;
        case DA3:
            // terminal identification, 4 hex codes
            reply("\033P!|C0000000\033\\");
            break;
        case DCH: deleteCharacters(seq.param_or<ColumnCount>(0, ColumnCount { 1 })); break;
        case DECCARA: {
            auto const origin = this->origin();
            auto const top = LineOffset(seq.param_or(0, *origin.line + 1) - 1);
            auto const left = ColumnOffset(seq.param_or(1, *origin.column + 1) - 1);
            auto const bottom = LineOffset(seq.param_or(2, *pageSize().lines) - 1);
            auto const right = ColumnOffset(seq.param_or(3, *pageSize().columns) - 1);
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
            auto const origin = this->origin();
            auto const top = LineOffset(seq.param_or(0, *origin.line + 1) - 1);
            auto const left = ColumnOffset(seq.param_or(1, *origin.column + 1) - 1);
            auto const bottom = LineOffset(seq.param_or(2, *pageSize().lines) - 1);
            auto const right = ColumnOffset(seq.param_or(3, *pageSize().columns) - 1);
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
            // The coordinates of the rectangular area are affected by the setting of origin mode (DECOM).
            // DECCRA is not affected by the page margins.
            auto const origin = this->origin();
            auto const top = Top(seq.param_or(0, *origin.line + 1) - 1);
            auto const left = Left(seq.param_or(1, *origin.column + 1) - 1);
            auto const bottom = Bottom(seq.param_or(2, *pageSize().lines) - 1);
            auto const right = Right(seq.param_or(3, *pageSize().columns) - 1);
            auto const page = seq.param_or(4, 0);

            auto const targetTop = LineOffset(seq.param_or(5, *origin.line + 1) - 1);
            auto const targetLeft = ColumnOffset(seq.param_or(6, *origin.column + 1) - 1);
            auto const targetTopLeft = CellLocation { .line = targetTop, .column = targetLeft };
            auto const targetPage = seq.param_or(7, 0);

            copyArea(Rect { .top = top, .left = left, .bottom = bottom, .right = right },
                     page,
                     targetTopLeft,
                     targetPage);
        }
        break;
        case DECERA: {
            // The coordinates of the rectangular area are affected by the setting of origin mode (DECOM).
            auto const origin = this->origin();
            auto const top = seq.param_or(0, *origin.line + 1) - 1;
            auto const left = seq.param_or(1, *origin.column + 1) - 1;

            // If the value of Pt, Pl, Pb, or Pr exceeds the width or height of the active page, then the
            // value is treated as the width or height of that page.
            auto const size = pageSize();
            auto const bottom = std::min(seq.param_or(2, unbox(size.lines)), unbox(size.lines)) - 1;
            auto const right = std::min(seq.param_or(3, unbox(size.columns)), unbox(size.columns)) - 1;

            eraseArea(top, left, bottom, right);
        }
        break;
        case DECFRA: {
            auto const ch = seq.param_or(0, Sequence::Parameter { 0 });
            // The coordinates of the rectangular area are affected by the setting of origin mode (DECOM).
            auto const origin = this->origin();
            auto const top = seq.param_or(1, origin.line);
            auto const left = seq.param_or(2, origin.column);

            // If the value of Pt, Pl, Pb, or Pr exceeds the width or height of the active page, then the
            // value is treated as the width or height of that page.
            auto const size = pageSize();
            auto const bottom = std::min(seq.param_or(3, unbox(size.lines)), unbox(size.lines));
            auto const right = std::min(seq.param_or(4, unbox(size.columns)), unbox(size.columns));

            // internal indices starts at 0, for DECFRA they start from 1
            // we need to adjust it and then make sure they are in bounds
            fillArea(ch, std::max(0, unbox(top) - 1), std::max(0, unbox(left) - 1), bottom - 1, right - 1);
        }
        break;
        case DECDC: deleteColumns(seq.param_or(0, ColumnCount(1))); break;
        case DECIC: insertColumns(seq.param_or(0, ColumnCount(1))); break;
        case DECINVM: _terminal->invokeMacro(seq.param_or(0, 0)); break;
        case DECSACE:
            // Ps=0 or 1 → stream mode, Ps=2 → rectangle mode
            _rectangularAttributeMode = (seq.param_or(0, 1) == 2);
            break;
        case DECSCA: {
            auto const pc = seq.param_or(0, 0);
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
        case DECSERA: {
            auto const top = seq.param_or(0, Top(1)) - 1;
            auto const left = seq.param_or(1, Left(1)) - 1;
            auto const bottom = seq.param_or(2, Bottom::cast_from(pageSize().lines)) - 1;
            auto const right = seq.param_or(3, Right::cast_from(pageSize().columns)) - 1;
            selectiveEraseArea(Rect { .top = top, .left = left, .bottom = bottom, .right = right });
            return ApplyResult::Ok;
        }
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
            // CSI Pid ; Pp ; Pt ; Pl ; Pb ; Pr $ y
            auto const requestId = seq.param_or(0, 0);
            // Pp is page number (ignored, we have one page)
            auto const top = LineOffset(std::max(seq.param_or(2, 1), 1) - 1);
            auto const left = ColumnOffset(std::max(seq.param_or(3, 1), 1) - 1);
            auto const bottom =
                LineOffset(std::min(seq.param_or(4, *pageSize().lines), *pageSize().lines) - 1);
            auto const right =
                ColumnOffset(std::min(seq.param_or(5, *pageSize().columns), *pageSize().columns) - 1);
            uint16_t checksum = 0;
            for (auto row = top; row <= bottom; ++row)
                for (auto column = left; column <= right; ++column)
                {
                    auto const& cell = at(row, column);
                    auto const text = cell.toUtf8();
                    for (auto const ch: text)
                        checksum += static_cast<uint16_t>(static_cast<uint8_t>(ch));
                }
            reply("\033P{}!~{:04X}\033\\", requestId, checksum);
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

            // DECSCL implies a soft reset (per DEC spec)
            _terminal->softReset();

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

                _terminal->requestWindowResize(
                    PageSize { _terminal->totalPageSize().lines,
                               ColumnCount::cast_from(columnCount ? columnCount : 80) });
                return ApplyResult::Ok;
            }
            else
                return ApplyResult::Invalid;
        case DECSNLS:
            _terminal->resizeScreen(PageSize { pageSize().lines, seq.param<ColumnCount>(0) });
            return ApplyResult::Ok;
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
        case DECXCPR: reportExtendedCursorPosition(); break;
        case NP: nextPage(seq.param_or(0, 1)); break;
        case PP: previousPage(seq.param_or(0, 1)); break;
        case PPA: pagePositionAbsolute(seq.param_or(0, 1)); break;
        case PPR: pagePositionRelative(seq.param_or(0, 1)); break;
        case PPB: pagePositionBackward(seq.param_or(0, 1)); break;
        case DECRQDE: requestDisplayedExtent(); break;
        case DL: deleteLines(seq.param_or(0, LineCount(1))); break;
        case ECH: eraseCharacters(seq.param_or(0, ColumnCount(1))); break;
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
        case HPA: moveCursorToColumn(seq.param<ColumnOffset>(0) - 1); break;
        case HPR: moveCursorForward(seq.param<ColumnCount>(0)); break;
        case HVP:
            moveCursorTo(seq.param_or(0, LineOffset(1)) - 1, seq.param_or(1, ColumnOffset(1)) - 1);
            break; // YES, it's like a CUP!
        case ICH: insertCharacters(seq.param_or(0, ColumnCount { 1 })); break;
        case IL: insertLines(seq.param_or(0, LineCount { 1 })); break;
        case REP:
            if (_terminal->parser().precedingGraphicCharacter())
            {
                auto const requestedCount = seq.param<size_t>(0);
                auto const availableColumns =
                    (margin().horizontal.to - cursor().position.column).template as<size_t>();
                auto const effectiveCount = std::min(requestedCount, availableColumns);
                for (size_t i = 0; i < effectiveCount; i++)
                    writeText(_terminal->parser().precedingGraphicCharacter());
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
        case SD: scrollDown(seq.param_or<LineCount>(0, LineCount { 1 })); break;
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
        case SU: scrollUp(seq.param_or<LineCount>(0, LineCount(1))); break;
        case TBC: return impl::TBC(seq, *this);
        case VPA: moveCursorToLine(seq.param_or<LineOffset>(0, LineOffset { 1 }) - 1); break;
        case WINMANIP: return impl::WINDOWMANIP(seq, *_terminal);
        case XTRESTORE: return impl::restoreDECModes(seq, *_terminal);
        case XTSAVE: return impl::saveDECModes(seq, *_terminal);
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
        case XTCHECKSUM: _checksumExtension = seq.param_or(0, 0); break;
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
        case SETTITLE:
            //(not supported) ChangeIconTitle(seq.intermediateCharacters());
            _terminal->setWindowTitle(seq.intermediateCharacters());
            return ApplyResult::Ok;
        case SETICON: return ApplyResult::Ok; // NB: Silently ignore!
        case SETWINTITLE: _terminal->setWindowTitle(seq.intermediateCharacters()); break;
        case SETTABNAME: _terminal->setTabName(seq.intermediateCharacters()); break;
        case SETXPROP: return ApplyResult::Unsupported;
        case SETCOLPAL: return impl::SETCOLPAL(seq, *_terminal);
        case RCOLPAL: return impl::RCOLPAL(seq, *_terminal);
        case SETCWD: return impl::SETCWD(seq, *this);
        case HYPERLINK: return impl::HYPERLINK(seq, *this);
        case XTCAPTURE: return impl::CAPTURE(seq, *_terminal);
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
        case DESKTOPNOTIFY: return impl::DESKTOPNOTIFY(seq, *_terminal);
        case DUMPSTATE: inspect(); break;
        case SEMA: processShellIntegration(seq); break;

        // hooks
        case DECDMAC: _terminal->hookParser(hookDECDMAC(seq)); break;
        case DECSIXEL: _terminal->hookParser(hookSixel(seq)); break;
        case STP: _terminal->hookParser(hookSTP(seq)); break;
        case DECRQSS: _terminal->hookParser(hookDECRQSS(seq)); break;
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

    auto const aspectVertical = [](int pA) {
        switch (pA)
        {
            case 9:
            case 8:
            case 7: return 1;
            case 6:
            case 5: return 2;
            case 4:
            case 3: return 3;
            case 2: return 5;
            case 1:
            case 0: return 2;
            default: return 1;
        }
    }(pa);

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
            auto const mappings = array<pair<string_view, RequestStatusString>, 11> {
                pair { "m", RequestStatusString::SGR },       pair { "\"p", RequestStatusString::DECSCL },
                pair { " q", RequestStatusString::DECSCUSR }, pair { "\"q", RequestStatusString::DECSCA },
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

bool Screen::isCursorInsideMargins() const noexcept
{
    bool const insideVerticalMargin = margin().vertical.contains(_cursor.position.line);
    bool const insideHorizontalMargin = !_terminal->isModeEnabled(DECMode::LeftRightMargin)
                                        || margin().horizontal.contains(_cursor.position.column);
    return insideVerticalMargin && insideHorizontalMargin;
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
    auto const validDataSize = [&]() -> bool {
        if (!validImage || !resolvedFormat.has_value())
            return false;
        if (*resolvedFormat == ImageFormat::PNG)
            return true; // PNG is self-describing
        auto const bpp = (*resolvedFormat == ImageFormat::RGBA) ? 4u : 3u;
        auto const expectedSize = static_cast<size_t>(*size.width) * *size.height * bpp;
        return message.body().size() == expectedSize;
    }();

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
    return _terminal->imageDecoder()(ImageFormat::PNG, data, size);
}

void Screen::uploadImage(string name, ImageFormat format, ImageSize imageSize, Image::Data&& pixmap)
{
    assert(format != ImageFormat::Auto && "Auto must be resolved before upload");
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
