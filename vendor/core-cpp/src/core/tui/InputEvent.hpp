// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace core::tui
{

/// @brief Keyboard input event.
struct KeyEvent
{
    KeyCode key {};                      ///< Key code (printable uses codepoint, special keys use enum).
    Modifier modifiers = Modifier::None; ///< Active modifier keys.
    char32_t codepoint = 0;              ///< Original Unicode codepoint (0 for non-printable keys).
};

/// @brief Mouse input event.
struct MouseEvent
{
    /// @brief Type of mouse action.
    enum class Type : std::uint8_t
    {
        Press,
        Release,
        Move,
        ScrollUp,
        ScrollDown,
    };

    Type type {};                        ///< Action type.
    int button = 0;                      ///< Button: 0=left, 1=middle, 2=right.
    int x = 0;                           ///< Column (1-based).
    int y = 0;                           ///< Row (1-based).
    Modifier modifiers = Modifier::None; ///< Active modifier keys.
    bool uiHandled = false;              ///< True if terminal UI consumed this event.
};

/// @brief Terminal resize event.
struct ResizeEvent
{
    int columns; ///< New terminal width in columns.
    int rows;    ///< New terminal height in rows.
};

/// @brief Bracketed paste event.
struct PasteEvent
{
    std::string text; ///< Pasted text content.
};

/// @brief Cursor position report event (response to DSR).
struct CursorPositionReport
{
    int row;    ///< Cursor row (1-based).
    int column; ///< Cursor column (1-based).
};

/// @brief Color scheme report from the terminal (CSI ? 997 ; N n).
///
/// This is an internal event consumed by Terminal::poll() and not propagated
/// to application code. It signals a dark/light mode change.
struct ColorSchemeReport
{
    int mode; ///< 1 = dark, 2 = light.
};

/// @brief Cell size report from the terminal (response to CSI 16 t).
///
/// Reports the cell dimensions in pixels. This is an internal event
/// consumed by Terminal during initialization and not propagated to
/// application code.
struct CellSizeReport
{
    int height; ///< Cell height in pixels.
    int width;  ///< Cell width in pixels.
};

/// @brief Terminal focus change event (DECSET 1004).
///
/// This is an internal event consumed by Terminal::poll() and not propagated
/// to application code. It signals when the terminal window gains or loses focus.
struct FocusEvent
{
    bool focused; ///< True when terminal gained focus, false when lost.
};

/// @brief DCS (Device Control String) response from the terminal.
///
/// Emitted when the parser receives a complete DCS sequence (ESC P ... ESC \).
/// The payload contains the full DCS content between the introducer and string terminator.
struct DcsResponse
{
    std::string payload; ///< DCS content (excludes ESC P prefix and ST terminator).
};

/// @brief OSC (Operating System Command) reply from the terminal: ESC ] ... ST (or BEL).
///
/// Only a @c VtParser constructed with @c VtParser::OscRecognition::Response emits it; by default
/// `ESC ]` reads as Alt+]. The payload is what lies between the introducer and the terminator,
/// e.g. `5522;type=write:status=DONE`.
struct OscResponse
{
    std::string payload; ///< OSC content (excludes ESC ] prefix and the ST/BEL terminator).
};

/// @brief DECRQM (DEC Request Mode) response: CSI ? mode ; status $ y.
///
/// Reports whether a specific DEC private mode is set, reset, or unsupported.
/// Status values: 0 = not recognized, 1 = set, 2 = reset, 3 = permanently set, 4 = permanently reset.
struct DecModeReport
{
    int mode;   ///< The DEC private mode number queried.
    int status; ///< Mode status (0=unknown, 1=set, 2=reset, 3=perm set, 4=perm reset).
};

/// @brief Primary Device Attributes (DA1) response: CSI ? p1 ; p2 ; ... c.
///
/// The terminal's feature list, one number per attribute. Attribute 4 advertises Sixel graphics.
struct DeviceAttributesReport
{
    std::vector<int> attributes; ///< The reported attributes, in the order the terminal sent them.
};

/// @brief Tells whether a DA1 response advertises Sixel graphics (attribute 4).
/// @param report The decoded DA1 response.
/// @return True when attribute 4 is present.
[[nodiscard]] inline auto advertisesSixel(DeviceAttributesReport const& report) noexcept -> bool
{
    return std::ranges::find(report.attributes, 4) != report.attributes.end();
}

/// @brief Discriminated union of all possible terminal input events.
using InputEvent = std::variant<KeyEvent,
                                MouseEvent,
                                ResizeEvent,
                                PasteEvent,
                                CursorPositionReport,
                                ColorSchemeReport,
                                CellSizeReport,
                                FocusEvent,
                                DcsResponse,
                                OscResponse,
                                DecModeReport,
                                DeviceAttributesReport>;

/// @brief Tells whether @p event is an internal protocol-response report rather
/// than an application input event.
///
/// Protocol reports are terminal responses to queries / mode changes
/// (color-scheme, cursor-position, cell-size, DEC-mode, device-attributes, focus, DCS, OSC). They are
/// consumed internally and never surfaced to application code. This is the
/// single source of truth for that classification, shared by
/// @c Terminal::consumeProtocolReports (which also dispatches the color-scheme
/// and focus reports to their handlers) and by the coroutine runtime's event
/// router, so the set lives in exactly one place.
/// @param event The decoded input event to classify.
/// @return True if @p event is an internal protocol report.
[[nodiscard]] inline bool isProtocolReport(InputEvent const& event) noexcept
{
    return std::visit(
        [](auto const& concrete) {
            using T = std::decay_t<decltype(concrete)>;
            return std::is_same_v<T, ColorSchemeReport> || std::is_same_v<T, CellSizeReport>
                   || std::is_same_v<T, CursorPositionReport> || std::is_same_v<T, DecModeReport>
                   || std::is_same_v<T, FocusEvent> || std::is_same_v<T, DcsResponse>
                   || std::is_same_v<T, OscResponse> || std::is_same_v<T, DeviceAttributesReport>;
        },
        event);
}

} // namespace core::tui
