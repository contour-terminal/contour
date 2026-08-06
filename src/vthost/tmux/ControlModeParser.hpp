// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The tmux control-mode line classifier (the client half of the protocol):
/// one line in, one typed event out. Field-level parsing — guard triples,
/// %output octal unescaping, %extended-output's age before " : ", layout
/// fields — is our own code (wezterm's grammar treats payloads as opaque).
/// Classification is a table: adding a notification is adding a row.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace vthost::tmux
{

/// `%begin time number flags` (also carried by end/error).
struct GuardBegin
{
    int64_t time = 0;
    uint32_t number = 0;
    uint32_t flags = 0;
    bool operator==(GuardBegin const&) const = default;
};

/// `%end` / `%error` closing a guard block.
struct GuardEnd
{
    int64_t time = 0;
    uint32_t number = 0;
    uint32_t flags = 0;
    bool isError = false;
    bool operator==(GuardEnd const&) const = default;
};

/// `%output %N ...` / `%extended-output %N age ... : ...` (bytes unescaped).
struct OutputEvent
{
    uint64_t pane = 0;
    std::string bytes;
    std::optional<uint64_t> ageMs; ///< Set for %extended-output.
    bool operator==(OutputEvent const&) const = default;
};

/// `%layout-change @W layout visible-layout ...`.
struct LayoutChangeEvent
{
    uint64_t window = 0;
    std::string layout;
    bool operator==(LayoutChangeEvent const&) const = default;
};

/// `%window-add @W`.
struct WindowAddEvent
{
    uint64_t window = 0;
    bool operator==(WindowAddEvent const&) const = default;
};

/// `%window-close @W` (and `%unlinked-window-close`).
struct WindowCloseEvent
{
    uint64_t window = 0;
    bool operator==(WindowCloseEvent const&) const = default;
};

/// `%window-renamed @W name`.
struct WindowRenamedEvent
{
    uint64_t window = 0;
    std::string name;
    bool operator==(WindowRenamedEvent const&) const = default;
};

/// `%session-changed $S name`.
struct SessionChangedEvent
{
    uint64_t session = 0;
    std::string name;
    bool operator==(SessionChangedEvent const&) const = default;
};

/// `%pause %N` / `%continue %N`.
struct PauseEvent
{
    uint64_t pane = 0;
    bool paused = true;
    bool operator==(PauseEvent const&) const = default;
};

/// `%exit [reason]`.
struct ExitEvent
{
    std::string reason;
    bool operator==(ExitEvent const&) const = default;
};

/// A line inside a guard block (a command's response body).
struct BodyLine
{
    std::string text;
    bool operator==(BodyLine const&) const = default;
};

/// A `%`-notification this client does not interpret (kept, never fatal).
struct UnknownNotification
{
    std::string line;
    bool operator==(UnknownNotification const&) const = default;
};

using ControlEvent = std::variant<GuardBegin,
                                  GuardEnd,
                                  OutputEvent,
                                  LayoutChangeEvent,
                                  WindowAddEvent,
                                  WindowCloseEvent,
                                  WindowRenamedEvent,
                                  SessionChangedEvent,
                                  PauseEvent,
                                  ExitEvent,
                                  BodyLine,
                                  UnknownNotification>;

/// Reverses tmux's %output escaping: `\ooo` (exactly three octal digits) back
/// to its byte; everything else passes through.
[[nodiscard]] std::string unescapeOutput(std::string_view escaped);

/// Classifies one received line (LF/CR already stripped).
[[nodiscard]] ControlEvent classifyLine(std::string_view line);

} // namespace vthost::tmux
