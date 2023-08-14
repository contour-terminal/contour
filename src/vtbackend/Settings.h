#pragma once

#include <vtbackend/ColorPalette.h>
#include <vtbackend/InputGenerator.h> // Modifier
#include <vtbackend/VTType.h>
#include <vtbackend/primitives.h>

#include <chrono>

namespace terminal
{

struct refresh_rate
{
    double value;
};

struct refresh_interval
{
    std::chrono::milliseconds value;

    explicit refresh_interval(refresh_rate rate): value { static_cast<long long>(1000.0 / rate.value) } {}
};

/// Terminal settings, enabling hardware reset to be easier implemented.
struct settings
{
    vt_type terminalId = vt_type::VT525;
    color_palette colorPalette; // NB: The default color palette can be taken from the factory settings.

    // total page size available to this terminal.
    // This page size may differ from the main displays (primary/alternate screen) page size If
    // some other display is shown along with it (e.g. below the main display).
    PageSize pageSize = PageSize { LineCount(25), ColumnCount(80) };

    max_history_line_count maxHistoryLineCount;
    image_size maxImageSize { width(800), height(600) };
    unsigned maxImageRegisterCount = 256;
    status_display_type statusDisplayType = status_display_type::None;
    status_display_position statusDisplayPosition = status_display_position::Bottom;
    bool syncWindowTitleWithHostWritableStatusDisplay = true;
    cursor_display cursorDisplay = cursor_display::Steady;
    cursor_shape cursorShape = cursor_shape::Block;

    bool usePrivateColorRegisters = false;

    std::chrono::milliseconds cursorBlinkInterval = std::chrono::milliseconds { 500 };
    refresh_rate refreshRate = { 30.0 };

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
    modifier mouseProtocolBypassModifier = modifier::shift;
    modifier mouseBlockSelectionModifier = modifier::control;
    line_offset copyLastMarkRangeOffset = line_offset(0);
    bool visualizeSelectedWord = true;
    std::chrono::milliseconds highlightTimeout = std::chrono::milliseconds { 150 };
    bool highlightDoubleClickedWord = true;
    // TODO: ^^^ make also use of it. probably rename to how VScode has named it.

    struct primary_screen
    {
        bool allowReflowOnResize = true;
    };
    primary_screen primaryScreen;

    // TODO: we could configure also the number of lines of the host writable statusline and indicator
    // statusline.
};

} // namespace terminal
