// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file TerminalProtocols.hpp
/// @brief Terminal protocol escape sequence constants shared between platform implementations.

#include <core/tui/MouseTracking.hpp>

#include <string>
#include <string_view>

namespace core::tui::protocols
{

using namespace std::string_view_literals;

// Kitty keyboard protocol flags (CSI > flags u):
//   1 = Disambiguate escape codes
//   2 = Report event types (press, repeat, release)
//   4 = Report alternate keys
//   8 = Report all keys as escape codes
//  16 = Report associated text
//
// We use flags 1|4|8=13 to ensure modifiers are reported for all keys including Enter.
// Flag 4 is needed to receive the shifted_key for non-letter shifted characters
// (e.g., Shift+3→'#' sends key='3':shifted_key='#').
// Flag 8 is needed because some terminals only report Shift+Enter with this flag.
constexpr auto EnableCsiU =
    "\033[>13u"sv; ///< Enable Kitty keyboard protocol (disambiguate + alternate + all keys).
constexpr auto DisableCsiU = "\033[<u"sv; ///< Pop Kitty keyboard protocol.

constexpr auto SgrReset = "\033[m"sv; ///< Reset every SGR attribute to the default (SGR 0).

constexpr auto EnableBracketedPaste = "\033[?2004h"sv;  ///< Enable bracketed paste mode.
constexpr auto DisableBracketedPaste = "\033[?2004l"sv; ///< Disable bracketed paste mode.

// Color scheme change notifications (DEC mode 2031)
// When enabled, the terminal sends CSI ? 997 ; N n when the palette changes
// N: 1 = dark, 2 = light
constexpr auto EnableColorSchemeNotify = "\033[?2031h"sv;  ///< Enable color scheme change notifications.
constexpr auto DisableColorSchemeNotify = "\033[?2031l"sv; ///< Disable color scheme change notifications.

// One-shot color scheme query: CSI ? 996 n
// Response: CSI ? 997 ; N n (same format as notifications)
constexpr auto QueryColorScheme = "\033[?996n"sv; ///< Query current color scheme.

// SGR extended mouse format (mode 1006) - allows coordinates > 223
// Uses CSI < button ; column ; row M/m format instead of legacy X10 encoding
constexpr auto EnableSGRMouse = "\033[?1006h"sv;  ///< Enable SGR extended mouse format.
constexpr auto DisableSGRMouse = "\033[?1006l"sv; ///< Disable SGR extended mouse format.

// Any-motion mouse tracking (mode 1003) - reports ALL mouse movements
// Required for hover tooltips - tracks mouse even without button held
// Without this, only button press/release events are reported
constexpr auto EnableAnyMotionTracking = "\033[?1003h"sv;  ///< Enable any-motion mouse tracking.
constexpr auto DisableAnyMotionTracking = "\033[?1003l"sv; ///< Disable any-motion mouse tracking.

// Passive mouse tracking (DEC mode 2029) - Contour terminal extension
// Adds an additional uiHandled parameter indicating whether the terminal UI
// consumed the event (e.g., for scrollback selection).
constexpr auto EnablePassiveMouseTracking = "\033[?2029h"sv;  ///< Enable passive mouse tracking.
constexpr auto DisablePassiveMouseTracking = "\033[?2029l"sv; ///< Disable passive mouse tracking.

// Button-event tracking (mode 1000): press and release only.
constexpr auto EnableButtonTracking = "\033[?1000h"sv;  ///< Enable press/release mouse tracking.
constexpr auto DisableButtonTracking = "\033[?1000l"sv; ///< Disable press/release mouse tracking.

// Button-motion tracking (mode 1002): press, release, and motion while a button is held.
constexpr auto EnableDragTracking = "\033[?1002h"sv;  ///< Enable drag mouse tracking.
constexpr auto DisableDragTracking = "\033[?1002l"sv; ///< Disable drag mouse tracking.

/// @brief Returns the DEC private mode set that asks for @p mode.
/// @param mode The tracking mode.
/// @return `CSI ? 1000 h`, `CSI ? 1002 h` or `CSI ? 1003 h`; empty for @c MouseTracking::Off.
[[nodiscard]] constexpr auto mouseTrackingSet(MouseTracking mode) noexcept -> std::string_view
{
    switch (mode)
    {
        case MouseTracking::Off: return {};
        case MouseTracking::Buttons: return EnableButtonTracking;
        case MouseTracking::Drag: return EnableDragTracking;
        case MouseTracking::AnyMotion: return EnableAnyMotionTracking;
    }
    return {};
}

/// @brief Returns the DEC private mode reset that ends @p mode.
/// @param mode The tracking mode.
/// @return `CSI ? 1000 l`, `CSI ? 1002 l` or `CSI ? 1003 l`; empty for @c MouseTracking::Off.
[[nodiscard]] constexpr auto mouseTrackingReset(MouseTracking mode) noexcept -> std::string_view
{
    switch (mode)
    {
        case MouseTracking::Off: return {};
        case MouseTracking::Buttons: return DisableButtonTracking;
        case MouseTracking::Drag: return DisableDragTracking;
        case MouseTracking::AnyMotion: return DisableAnyMotionTracking;
    }
    return {};
}

/// @brief Appends the sequences that move a terminal from mouse tracking @p from to @p to.
///
/// From @c MouseTracking::Off: the mode's set, then SGR encoding (1006). To @c MouseTracking::Off:
/// the reverse, 1006's reset and then the mode's. Between two modes: the old mode's reset and the new
/// one's set, leaving 1006 on. Nothing when the two are equal. Enabling a terminal's protocols is the
/// change from Off and disabling them the change to Off, so what is reset always mirrors what was set.
/// @param out Destination to append to.
/// @param from The mode the terminal is in.
/// @param to The mode it is to be in.
inline void appendMouseTrackingChange(std::string& out, MouseTracking from, MouseTracking to)
{
    if (from == to)
        return;
    if (from == MouseTracking::Off)
    {
        out.append(mouseTrackingSet(to));
        out.append(EnableSGRMouse);
        return;
    }
    if (to == MouseTracking::Off)
    {
        out.append(DisableSGRMouse);
        out.append(mouseTrackingReset(from));
        return;
    }
    out.append(mouseTrackingReset(from));
    out.append(mouseTrackingSet(to));
}

// Focus tracking (DEC mode 1004)
// When enabled, the terminal sends CSI I on focus-in and CSI O on focus-out.
constexpr auto EnableFocusTracking = "\033[?1004h"sv;  ///< Enable focus in/out notifications.
constexpr auto DisableFocusTracking = "\033[?1004l"sv; ///< Disable focus in/out notifications.

// Win32 input mode (DEC private mode 9001) — Windows Terminal extension.
// Sends raw Win32 key events as CSI Vk ; Sc ; Uc ; Kd ; Cs ; Rc _ sequences,
// preserving full modifier fidelity that is lost in standard VT translation.
constexpr auto EnableWin32InputMode = "\033[?9001h"sv;  ///< Enable win32-input-mode.
constexpr auto DisableWin32InputMode = "\033[?9001l"sv; ///< Disable win32-input-mode.

// Primary Device Attributes (DA1): CSI c
// Response: CSI ? 6x ; p1 ; p2 ; ... c  — parameter 4 advertises Sixel graphics.
constexpr auto QueryPrimaryDeviceAttributes = "\033[c"sv; ///< Request DA1 (feature list).

// OSC 8 hyperlinks: OSC 8 ; params ; URI ST ... OSC 8 ; ; ST
constexpr auto HyperlinkIntroducer = "\033]8;"sv;   ///< Opens an OSC 8 hyperlink's parameter list.
constexpr auto StringTerminator = "\033\\"sv;       ///< ST, terminating an OSC/DCS string.
constexpr auto HyperlinkClose = "\033]8;;\033\\"sv; ///< Closes the current OSC 8 hyperlink.

/// Everything an OSC 8 open needs before the URI, with no parameters: `ESC ] 8 ; ;`.
/// For callers that must splice the URI themselves — grep appends a `#line` fragment per output
/// line — so that the parameter-list structure is still spelled only here.
constexpr auto HyperlinkOpenPrefix = "\033]8;;"sv;

/// @brief Appends the OSC 8 sequence that opens a hyperlink to @p url.
///
/// Shared by the platform TerminalOutput implementations, HelpPrinter and the ls table so the
/// escape is spelled in exactly one place.
///
/// Passing an @p id makes every run carrying that same id part of one logical link, which
/// matters when a link is emitted as several runs — a cell-diffing renderer rewrites only
/// the cells that changed, so one visual link can reach the terminal as multiple runs.
/// Without an id, terminals group only immediately adjacent runs sharing a URI.
///
/// @param out Destination to append to.
/// @param url The link target.
/// @param id Optional OSC 8 `id=` value. Empty emits no parameters, keeping the sequence
///           byte-identical to the id-less form.
inline void appendHyperlinkOpen(std::string& out, std::string_view url, std::string_view id = {})
{
    out.append(HyperlinkIntroducer);
    if (!id.empty())
    {
        out.append("id="sv);
        out.append(id);
    }
    out.append(";"sv);
    out.append(url);
    out.append(StringTerminator);
}

/// @brief Returns the OSC 8 sequence that opens a hyperlink to @p url.
///
/// Prefer appendHyperlinkOpen() when the result goes straight into an output buffer; this
/// overload exists for callers that need the sequence as a value.
///
/// @param url The link target.
/// @param id Optional OSC 8 `id=` value.
/// @return The complete opening sequence: `ESC ] 8 ; [id=<id>] ; <url> ESC \`.
[[nodiscard]] inline auto buildHyperlinkOpen(std::string_view url, std::string_view id = {}) -> std::string
{
    auto result = std::string {};
    result.reserve(HyperlinkIntroducer.size() + id.size() + url.size() + StringTerminator.size() + 4);
    appendHyperlinkOpen(result, url, id);
    return result;
}

/// @brief Parses a DA1 response and reports whether it advertises Sixel graphics.
///
/// Accepts the canonical form `ESC [ ? p1 ; p2 ; ... c` (and the 0x9B single-byte
/// CSI variant), scanning the semicolon-separated parameters for the value 4.
/// Anything unparseable — empty, truncated, or garbage — reports false.
///
/// @param response The raw bytes read back from the terminal.
/// @return true when parameter 4 is present.
[[nodiscard]] inline auto parseSixelFromDeviceAttributes(std::string_view response) -> bool
{
    auto start = std::string_view::npos;
    if (auto const escForm = response.find("\033[?"); escForm != std::string_view::npos)
        start = escForm + 3;
    else if (auto const csiForm = response.find("\x9B?"); csiForm != std::string_view::npos)
        start = csiForm + 2;
    else
        return false;

    auto const end = response.find('c', start);
    if (end == std::string_view::npos)
        return false;

    auto params = response.substr(start, end - start);
    while (!params.empty())
    {
        auto const semi = params.find(';');
        auto const token = params.substr(0, semi);
        if (token == "4")
            return true;
        if (semi == std::string_view::npos)
            break;
        params.remove_prefix(semi + 1);
    }
    return false;
}

} // namespace core::tui::protocols
