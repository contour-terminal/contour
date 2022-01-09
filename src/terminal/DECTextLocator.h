/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <terminal/primitives.h>

#include <crispy/assert.h>
#include <crispy/logstore.h>

#include <array>
#include <string>
#include <string_view>

#pragma once

namespace terminal
{

enum class DECLocatorEvent
{
    // Only report to explicit requests (DECRQLP).
    Explicit = 0x00,

    // Report button down events.
    ButtonDown = 0x01,

    // Report button up events.
    ButtonUp = 0x02,
};

// DECEFR properties
struct DECLocatorRectangle: public Rect
{
};

enum class DECLocatorReportingMode
{
    Disabled,          // DECELR 0
    Enabled,           // DECELR 1
    EnabledOnce,       // DECELR 2
    FilterRectangular, // DECEFR
};

auto const inline VTLocatorLog = logstore::Category("vt.locator",
                                                    "Reports DEC text locator events.",
                                                    logstore::Category::State::Disabled,
                                                    logstore::Category::Visibility::Public);

/**
 * Implements the DEC Text Locator extension,
 *
 * Documented in DEC STD 070 manuall, section 13 (Text Locator Extension).
 */
class DECTextLocator
{
  public:
    void reset();

    // DECSLE
    void selectLocatorEvents(DECLocatorEvent event, bool enabled) noexcept;
    [[nodiscard]] bool reportButtonUpEvents() const noexcept;
    [[nodiscard]] bool reportButtonDownEvents() const noexcept;
    [[nodiscard]] bool reportEventExplicitOnly() const noexcept;

    // DECELR (cancels prior DECEFR)
    void disableLocatorReporting() noexcept;
    void enableLocatorReporting(CoordinateUnits units) noexcept;
    void enableLocatorReportingOnce(CoordinateUnits units) noexcept;

    // DECEFR (cancelled by DECELR)
    void enableFilterRectangle(DECLocatorRectangle rect) noexcept;
    void disableFilterRectangle() noexcept;
    [[nodiscard]] bool filterRectangleEnabled() const noexcept;

    /// DECRQLP
    ///
    /// requests the locator position, possibly appending it the the reply buffer
    /// that has to be consumed.
    ///
    /// @sse fetchReplyAndClear()
    void requestLocatorPosition() noexcept;

    /// DECLRP
    /// peeks into the local pending reply buffer without consuming it.
    [[nodiscard]] std::string_view peekLocatorReply() const noexcept;

    /// DECLRP
    /// fetches any pending reply data and clears the internal buffer.
    [[nodiscard]] std::string_view fetchReplyAndClear() noexcept;

    /// Updates the current mouse state.
    ///
    /// @param button        Determines what button has been pressed or released.
    ///                      If this value is nullopt, than it's a simple move event.
    /// @param buttonPressed if true, the given button is pressed, otherwise released
    /// @param cellPosition  Defines the cursor position in cell coordinates.
    /// @param pixelPosition Defines the cursor position in pixel coordinates.
    ///
    /// This function upcates the local state and the appends any new
    /// text locator events to the internal reply buffer
    ///
    /// This function should be always called upon mouse move and
    /// button press/release events.
    void update(MouseButton button,
                bool buttonPressed,
                CellLocation cellPosition,
                PixelCoordinate pixelPosition);

    void updateMousePress(MouseButton button, bool buttonPressed);
    void updateMouseMove(CellLocation cellPosition, PixelCoordinate pixelPosition);

  private:
    template <typename Format, typename... Args>
    void reply(Format&& format, Args&&... args)
    {
        _replyBuffer[_replyBackBufferIndex] += fmt::format(format, std::forward<Args>(args)...);
    }

    [[nodiscard]] std::tuple<LineOffset, ColumnOffset> currentMousePosition() const noexcept
    {
        switch (_units)
        {
            case CoordinateUnits::Cells:
                // cell coordinates
                return std::tuple { _cellPosition.line, _cellPosition.column };
            case CoordinateUnits::Pixels:
                // pixel coordinates
                return std::tuple { LineOffset(_pixelPosition.y.value),
                                    ColumnOffset(_pixelPosition.x.value) };
        }
        crispy::unreachable();
    }

    // configuration
    DECLocatorReportingMode _reportingMode = DECLocatorReportingMode::Disabled;
    DECLocatorRectangle _filterRectangle;
    CoordinateUnits _units = CoordinateUnits::Cells;
    uint32_t _selectedLocatorEvents = static_cast<uint32_t>(DECLocatorEvent::Explicit);

    // current state
    CellLocation _cellPosition;
    PixelCoordinate _pixelPosition;
    MouseButton _currentlyPressedMouseButtons = MouseButton::None;

    // TODO(pr) button states

    // pending output sequences
    // e.g. filled by requestLocatorPosition()
    size_t _replyBackBufferIndex = 0;
    std::array<std::string, 2> _replyBuffer;
};

} // namespace terminal

// {{{ fmt
namespace fmt
{
template <>
struct formatter<terminal::DECLocatorReportingMode>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::DECLocatorReportingMode _value, FormatContext& ctx)
    {
        switch (_value)
        {
            // clang-format off
            case terminal::DECLocatorReportingMode::Disabled: return fmt::format_to(ctx.out(), "Disabled");
            case terminal::DECLocatorReportingMode::Enabled: return fmt::format_to(ctx.out(), "Enabled");
            case terminal::DECLocatorReportingMode::EnabledOnce: return fmt::format_to(ctx.out(), "EnabledOnce");
            case terminal::DECLocatorReportingMode::FilterRectangular: return fmt::format_to(ctx.out(), "FilterRectangular");
            // clang-format off
        }
        crispy::unreachable();
    }
};

template <>
struct formatter<terminal::DECLocatorEvent>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::DECLocatorEvent _value, FormatContext& ctx)
    {
        switch (_value)
        {
            case terminal::DECLocatorEvent::ButtonUp: return fmt::format_to(ctx.out(), "ButtonUp");
            case terminal::DECLocatorEvent::ButtonDown: return fmt::format_to(ctx.out(), "ButtonDown");
            case terminal::DECLocatorEvent::Explicit: return fmt::format_to(ctx.out(), "Explicit");
        }
        crispy::unreachable();
    }
};
} // namespace fmt
// }}}
