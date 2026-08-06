// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

namespace vtbackend
{

/// What an application's progress indicator is currently saying.
///
/// The `<state>` parameter of the ConEmu-style `OSC 9 ; 4 ; <state> [; <progress>] ST` sequence.
/// Zero is the absent/negative case, so a zero-initialized @ref Progress means "nothing to show".
enum class ProgressState : uint8_t
{
    Inactive = 0,      ///< No progress indicator; any previous one is withdrawn.
    Normal = 1,        ///< Ordinary progress, at @ref Progress::percentage.
    Error = 2,         ///< The operation failed; the frontend paints an error indicator.
    Indeterminate = 3, ///< Busy with no known completion; the frontend pulses.
    Paused = 4,        ///< The operation is paused, or warns; the frontend paints it as such.
};

/// An application's progress indicator: what it is doing, and how far along it is.
struct Progress
{
    ProgressState state = ProgressState::Inactive;

    /// How far along, 0..100.
    ///
    /// Meaningful for @ref ProgressState::Normal, and for @ref ProgressState::Error and
    /// @ref ProgressState::Paused when the application supplied one. Carried across an
    /// @ref ProgressState::Indeterminate transition rather than reset, so an application that pauses
    /// and resumes does not lose the bar it had drawn.
    uint8_t percentage = 0;

    bool operator==(Progress const&) const = default;
};

/// The largest `<progress>` the protocol allows; larger values clamp to it.
inline constexpr uint8_t MaxProgressPercentage = 100;

/// Why an `OSC 9 ; 4` payload could not be decoded.
enum class ProgressError : uint8_t
{
    MalformedPayload,  ///< Not shaped like `4;<state>[;<progress>]`.
    UnknownState,      ///< `<state>` was not one of the five @ref ProgressState values.
    MalformedProgress, ///< `<progress>` was present but not a non-negative integer.
};

/// Applies one `OSC 9 ; 4` sequence to the progress state a session already had.
///
/// Which of @p current's fields survive depends on the state, and the protocol is deliberately not
/// uniform about it: `Normal` always takes the new percentage, `Indeterminate` never does, and
/// `Error`/`Paused` take one only if the application supplied it. Expressed as a pure function of
/// (previous state, payload) so the whole table is exercised without a Screen.
///
/// @param current  The progress state in effect before this sequence.
/// @param payload  The OSC 9 body INCLUDING its leading "4", i.e. `4;<state>[;<progress>]`.
/// @return The progress state the sequence establishes, or why the payload was rejected. On
///         rejection the caller keeps @p current -- a malformed sequence never clears an indicator.
[[nodiscard]] std::expected<Progress, ProgressError> applyProgressSequence(Progress current,
                                                                           std::string_view payload);

} // namespace vtbackend
