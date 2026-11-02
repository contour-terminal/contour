// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/input/InputGenerator.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace contour::input
{

/// Where a key event came from.
enum class KeyEventOrigin : uint8_t
{
    Keyboard = 0,      //!< A physical key press, repeat or release.
    InputMethodCommit, //!< Text committed by an input method; no key was involved.
};

/// A key event as plain data: everything the platform reported, and nothing derived from it.
///
/// It has no Qt in it, so it can be recorded, compared, stored in a table and replayed.
struct NativeKeyEvent
{
    int qtKey = 0;                ///< Qt::Key value.
    std::u32string text = {};     ///< QKeyEvent::text() as UTF-32.
    uint32_t qtModifiers = 0;     ///< Qt::KeyboardModifiers bits.
    uint32_t nativeModifiers = 0; ///< After Win32 lock sampling (see captureKeyEvent).
    uint32_t nativeVirtualKey = 0;
    uint32_t nativeScanCode = 0;
    vtbackend::KeyboardEventType type = vtbackend::KeyboardEventType::Press;
    KeyEventOrigin origin = KeyEventOrigin::Keyboard;

    [[nodiscard]] bool operator==(NativeKeyEvent const&) const = default;
};

/// Formats @p event as one line, in the form parseRecorded() reads back:
/// `key=0x… mods=0x… native=0x… vk=0x… scan=0x… type=press|repeat|release origin=keyboard|ime
/// text=<escaped>`.
/// @param event The event to record.
/// @return The line, without a trailing newline.
[[nodiscard]] std::string formatRecorded(NativeKeyEvent const& event);

/// Parses a line written by formatRecorded().
/// @param line The recorded line.
/// @return The event, or nullopt when the line is malformed.
/// A raw space inside the text field is malformed, as formatRecorded() writes it escaped.
[[nodiscard]] std::optional<NativeKeyEvent> parseRecorded(std::string_view line);

/// Logs @p event through the `gui.input` log category in formatRecorded()'s form, so a tester can send back
/// exactly what the platform delivered. Nothing is formatted or allocated while the category is disabled.
/// @param event The captured event.
void logRecorded(NativeKeyEvent const& event);

} // namespace contour::input
