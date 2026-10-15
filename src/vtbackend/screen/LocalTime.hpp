// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstddef>

namespace vtbackend
{

/// The production conversion: the process's own time zone, as the C library reports it.
/// @param when The instant to convert.
/// @return @p when in local civil time, to the second -- or read as UTC if the zone lookup fails.
[[nodiscard]] std::chrono::local_seconds systemLocalTime(std::chrono::system_clock::time_point when) noexcept;

/// Converts a wall-clock instant to the civil time a person reads where they are.
///
/// The time zone is ambient process state, so it is reached through this seam rather than called: the
/// production code passes @ref systemLocalTime, a test passes a converter with a fixed offset and gets
/// the same text on every machine.
///
/// A value rather than a bare function pointer so that it cannot be missing: it has no default and
/// refuses a null pointer at compile time, as the clocks beside it refuse a temporary -- a conversion
/// left out would otherwise be found only when the first timestamp is drawn.
/// @see TerminalClocks::localTime
class LocalTimeConverter
{
  public:
    /// The shape of a conversion: an instant in, local civil time to the second out.
    using Function = std::chrono::local_seconds (*)(std::chrono::system_clock::time_point) noexcept;

    /// Implicit, so a call site names the function it injects and nothing else.
    /// @param convert The conversion, e.g. `&systemLocalTime`.
    LocalTimeConverter(Function convert) noexcept: _convert { convert } {}

    /// Refuses a null conversion, which would only fail once a timestamp is drawn.
    LocalTimeConverter(std::nullptr_t) = delete;

    /// @param when The instant to convert.
    /// @return @p when in local civil time, to the second.
    [[nodiscard]] std::chrono::local_seconds operator()(
        std::chrono::system_clock::time_point when) const noexcept
    {
        return _convert(when);
    }

  private:
    Function _convert;
};

} // namespace vtbackend
