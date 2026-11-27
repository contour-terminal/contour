// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/Clock.hpp>
#include <core/tui/Error.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/MouseTracking.hpp>
#include <core/tui/ScreenSurface.hpp>
#include <core/tui/TerminalInput.hpp>
#include <core/tui/TerminalOutput.hpp>
#include <core/tui/TuiExport.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace core::tui
{

/// @brief Detected terminal color scheme (dark or light mode).
enum class ColorScheme : std::uint8_t
{
    Unknown, ///< Color scheme not yet detected.
    Dark,    ///< Dark background.
    Light,   ///< Light background.
};

/// @brief A terminal's answer to a DECRQM query for one DEC private mode, or why there is none.
///
/// Two of these are not answers, and they are different facts: a query nobody sent, and a terminal
/// that stayed silent. A caller turning a feature off because the terminal declined and one turning
/// it off because nobody asked are making different decisions, so they must not share a value.
enum class DecModeStatus : std::uint8_t
{
    NotAsked,         ///< No query was sent: this terminal has no input to read a reply from (mock output).
    NoReply,          ///< The query was sent and nothing answered before the timeout.
    NotRecognized,    ///< Answered, status 0: the terminal does not know the mode.
    Set,              ///< Answered, status 1.
    Reset,            ///< Answered, status 2.
    PermanentlySet,   ///< Answered, status 3.
    PermanentlyReset, ///< Answered, status 4.
};

/// @brief Maps a DECRPM status value to the answer it carries.
/// @param status The status field of a @c DecModeReport.
/// @return The answer; a value outside 0..4 is not a status the terminal protocol defines, and
///         reads as @c NotRecognized.
[[nodiscard]] constexpr auto decModeStatusFromReply(int status) noexcept -> DecModeStatus
{
    switch (status)
    {
        case 1: return DecModeStatus::Set;
        case 2: return DecModeStatus::Reset;
        case 3: return DecModeStatus::PermanentlySet;
        case 4: return DecModeStatus::PermanentlyReset;
        default: return DecModeStatus::NotRecognized;
    }
}

/// @brief The input a terminal query reads its reply through.
///
/// @c Terminal's own @c TerminalInput is the production implementation. Tests script one, so a
/// query can be driven to its reply, or to its timeout under a @c ManualClock, with no terminal
/// attached and no real time elapsing.
class TerminalQueryInput
{
  public:
    TerminalQueryInput() = default;
    virtual ~TerminalQueryInput() = default;
    TerminalQueryInput(TerminalQueryInput const&) = delete;
    auto operator=(TerminalQueryInput const&) -> TerminalQueryInput& = delete;
    TerminalQueryInput(TerminalQueryInput&&) = delete;
    auto operator=(TerminalQueryInput&&) -> TerminalQueryInput& = delete;

    /// @brief Waits for input.
    /// @param timeoutMs The longest to wait, in milliseconds.
    /// @return Decoded events; empty when nothing arrived.
    [[nodiscard]] virtual auto poll(int timeoutMs) -> std::vector<InputEvent> = 0;

    /// @brief Hands back events a query read that were not its reply.
    /// @param events Events in arrival order.
    virtual void unread(std::vector<InputEvent> events) = 0;
};

/// @brief Top-level terminal coordinator that owns both input and output subsystems.
///
/// Manages initialization order, cleanup, and SIGWINCH handler installation.
/// Provides convenience methods that delegate to TerminalInput and TerminalOutput.
class CORE_TUI_API Terminal: public ScreenSurface
{
  public:
    /// @brief Constructs a Terminal with the default real TerminalOutput.
    Terminal();

    /// @brief Constructs a Terminal with a custom TerminalOutput (for dependency injection in tests).
    /// @param output The output implementation to use (ownership transferred).
    /// @pre @p output is not null. A null output fails the precondition (`Require`), which reports
    ///      through the fail handler and ends the process; it is never dereferenced.
    explicit Terminal(std::unique_ptr<TerminalOutput> output);

    /// @brief Constructs a Terminal whose queries read replies through @p queryInput and time out
    /// by @p clock, whatever @p output is -- so a test can drive a query with a mock output.
    /// @param output The output implementation to use (ownership transferred).
    /// @pre @p output is not null, as for the constructor above.
    /// @param queryInput The input queries read from (not owned; must outlive this Terminal).
    /// @param clock The clock query timeouts are measured on (not owned; must outlive this Terminal).
    Terminal(std::unique_ptr<TerminalOutput> output,
             TerminalQueryInput& queryInput,
             core::platform::IClock& clock);

    ~Terminal() override;

    Terminal(Terminal const&) = delete;
    auto operator=(Terminal const&) -> Terminal& = delete;
    Terminal(Terminal&&) = delete;
    auto operator=(Terminal&&) -> Terminal& = delete;

    /// @brief Initializes both input and output subsystems and installs the SIGWINCH handler.
    /// @return Success or an error.
    [[nodiscard]] auto initialize() -> VoidResult;

    /// @brief Shuts down both subsystems and restores the original SIGWINCH handler.
    void shutdown();

    /// @brief Returns a reference to the input subsystem.
    [[nodiscard]] auto input() noexcept -> TerminalInput&;

    /// @brief Returns a reference to the output subsystem.
    [[nodiscard]] auto output() noexcept -> TerminalOutput& override;

    /// @brief Convenience: polls for input events with the given timeout.
    /// @param timeoutMs -1 = block, 0 = non-blocking, >0 = timeout in ms.
    /// @return Vector of parsed events.
    [[nodiscard]] auto poll(int timeoutMs = -1) -> std::vector<InputEvent>;

    /// @brief Returns terminal width in columns.
    [[nodiscard]] auto columns() const noexcept -> int override;

    /// @brief Returns terminal height in rows.
    [[nodiscard]] auto rows() const noexcept -> int override;

    /// @brief Suspends terminal protocols and raw mode for external command execution.
    ///
    /// Call this before executing external commands to restore the terminal to
    /// a normal state that programs expect. Call resume() after the command completes.
    void suspend();

    /// @brief Resumes terminal protocols and raw mode after external command execution.
    ///
    /// Call this after an external command completes to restore the shell's
    /// terminal configuration.
    void resume();

    /// @brief Returns whether the terminal is currently suspended.
    [[nodiscard]] auto isSuspended() const noexcept -> bool;

    /// @brief Sets how much mouse input the terminal is asked to report.
    ///
    /// Forwards to @c TerminalInput::setMouseTracking(): effective at @c initialize() when called
    /// before it, and at once on an initialized terminal. A terminal over a mock output never enables
    /// its protocols, so there the mode is only recorded.
    /// @param mode The mode the application asks for.
    void setMouseTracking(MouseTracking mode);

    /// @brief Queries the current cursor position from the terminal.
    ///
    /// On a mock output with no query input, the mock's own cursor is the answer.
    /// @return Pair of (row, column), both 1-based, or why there is none.
    [[nodiscard]] auto queryCursorPosition() -> std::expected<std::pair<int, int>, QueryUnanswered> override;

    /// @brief Queries the cell size in pixels from the terminal via CSI 16 t.
    /// @return Pair of (width, height) in pixels, or why there is none.
    [[nodiscard]] auto queryCellSize() -> std::expected<std::pair<int, int>, QueryUnanswered>;

    /// @brief Queries the terminal's Primary Device Attributes (DA1) and waits for the reply.
    ///
    /// The reply lists the terminal's features; @c advertisesSixel reads attribute 4 from it. A
    /// terminal that never answers reports @c NoReply at the query timeout rather than hanging,
    /// and input read while waiting is handed back as with every other query.
    /// @return The reply, or why there is none.
    [[nodiscard]] auto queryDeviceAttributes() -> std::expected<DeviceAttributesReport, QueryUnanswered>;

    /// @brief Queries a DEC private mode via DECRQM and waits for the reply.
    ///
    /// Input read while waiting that is not the reply is handed back to @c TerminalInput, so it is
    /// delivered to the application rather than lost.
    /// @param mode The DEC private mode number to query.
    /// @return The terminal's answer, or which of not asked and no reply applies.
    [[nodiscard]] auto queryDecMode(int mode) -> DecModeStatus;

    /// @brief Returns the cached cell pixel width (0 if unknown).
    [[nodiscard]] auto cellPixelWidth() const noexcept -> int;

    /// @brief Returns the cached cell pixel height (0 if unknown).
    [[nodiscard]] auto cellPixelHeight() const noexcept -> int;

    /// @brief Returns the cached color scheme (dark or light mode).
    [[nodiscard]] auto colorScheme() const noexcept -> ColorScheme;

    /// @brief Registers a callback for color scheme change notifications.
    ///
    /// The callback is invoked when the terminal reports a color scheme change
    /// via CSI ? 997 ; N n (triggered by DEC mode 2031 subscription).
    /// Multiple handlers can be registered.
    /// @param callback The callback to invoke on scheme change.
    void onColorSchemeChanged(std::function<void(ColorScheme)> callback);

    /// @brief Called internally by VtParser when a color scheme report is received.
    /// @param scheme The reported color scheme.
    void handleColorSchemeReport(ColorScheme scheme);

    /// @brief Consumes a focus-change report, updating state and notifying handlers.
    ///
    /// Public so the coroutine runtime's event source can route the report the
    /// same way poll() does (it is otherwise an internal protocol response).
    /// @param focused True if the terminal gained focus, false if it lost focus.
    /// @return True if the focus state actually changed (handlers were notified).
    bool handleFocusEvent(bool focused);

    /// @brief Removes protocol-response events from @p events, dispatching the
    /// color-scheme and focus reports to their handlers and dropping the rest.
    ///
    /// Shared by poll() and the coroutine runtime's event source so the
    /// "what is an internal report vs. application input" policy lives in one
    /// place (the @c core::tui::isProtocolReport predicate). @c CursorPositionReport /
    /// @c CellSizeReport are dropped (their values are consumed synchronously by
    /// the query methods).
    /// @param events The decoded events to filter in place.
    /// @return True if a focus-change report was dispatched (so a caller waiting
    ///         on activity can redraw); false otherwise.
    bool consumeProtocolReports(std::vector<InputEvent>& events);

    /// @brief Returns whether the terminal window currently has focus.
    [[nodiscard]] auto isFocused() const noexcept -> bool;

    /// @brief Returns whether the terminal supports HUD overlay mode (DEC mode 2035).
    ///
    /// When supported, the command palette can render on a transparent HUD layer
    /// above the primary screen content. When not supported, falls back to inline rendering.
    [[nodiscard]] auto hudSupported() const noexcept -> bool;

    /// @brief Registers a callback for focus change notifications (DECSET 1004).
    ///
    /// The callback is invoked when the terminal reports focus gained or lost.
    /// Multiple handlers can be registered.
    /// @param callback The callback to invoke on focus change (true = focused).
    void onFocusChanged(std::function<void(bool)> callback);

  private:
    TerminalInput _input;
    std::unique_ptr<TerminalOutput> _output;
    core::platform::IClock& _clock;            ///< The clock query timeouts are measured on.
    TerminalQueryInput* _queryInput = nullptr; ///< Where queries read replies, or nullptr for _input.
    bool _initialized = false;
    bool _mockMode = false; ///< Output is OutputKind::Simulation: no input, raw mode or signals.
    ColorScheme _colorScheme = ColorScheme::Unknown;
    std::vector<std::function<void(ColorScheme)>> _colorSchemeCallbacks;
    bool _focused = true;       ///< Whether the terminal window has focus (assume focused on startup).
    bool _hudSupported = false; ///< Whether terminal supports HUD overlay (DEC mode 2035).
    std::vector<std::function<void(bool)>> _focusCallbacks;
    int _cellPixelWidth = 0;  ///< Cached cell width in pixels (0 if unknown).
    int _cellPixelHeight = 0; ///< Cached cell height in pixels (0 if unknown).

    /// @brief Whether a query can read a reply: an injected query input, or a real (non-mock) terminal.
    [[nodiscard]] auto canQuery() const noexcept -> bool;

    /// @brief Waits for the first event @p isReply accepts, until the query timeout on the injected clock.
    ///
    /// On every exit path, answered or timed out, the other events read meanwhile are passed through
    /// @c consumeProtocolReports (so a color-scheme or focus report is dispatched, as poll() does)
    /// and what remains is handed back to the input, so no application input is lost to a probe.
    /// @param isReply Accepts the reply this query is waiting for.
    /// @return The reply, or std::nullopt when none arrived in time.
    [[nodiscard]] auto awaitReport(std::function<bool(InputEvent const&)> const& isReply)
        -> std::optional<InputEvent>;

    /// @brief Asks for the color scheme and waits for it, unless it is already known.
    void awaitColorScheme();
};

} // namespace core::tui
