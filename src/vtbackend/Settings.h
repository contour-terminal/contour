#pragma once

#include <vtbackend/ColorPalette.h>
#include <vtbackend/InputGenerator.h> // Modifier
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
    StatusDisplayType statusDisplayType = StatusDisplayType::None;
    StatusDisplayPosition statusDisplayPosition = StatusDisplayPosition::Bottom;
    bool syncWindowTitleWithHostWritableStatusDisplay = true;
    CursorDisplay cursorDisplay = CursorDisplay::Steady;
    CursorShape cursorShape = CursorShape::Block;

    bool usePrivateColorRegisters = false;

    std::chrono::milliseconds cursorBlinkInterval = std::chrono::milliseconds { 500 };
    RefreshRate refreshRate = { 30.0 };

    // Defines the time to wait before the terminal executes the line feed (LF) command.
    // This is used to implement the DECSCLM (slow scroll) mode.
    std::chrono::milliseconds smoothLineScrolling { 100 };

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

    // TODO: we could configure also the number of lines of the host writable statusline and indicator
    // statusline.
};

} // namespace vtbackend
