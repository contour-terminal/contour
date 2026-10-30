// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What Settings says when a chord cannot be the quick terminal's hotkey, one sentence per reason. Qt-free.

#include <contour/config/HotkeyChord.hpp>
#include <contour/platform/GlobalHotkey.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace contour::window
{

/// How each HotkeyChordError reads, in enumerator order.
inline constexpr auto HotkeyChordErrorSentences = std::array<std::string_view, 1> {
    "A hotkey needs Control, Alt or Super held with the key; without one it would take over typing.",
};

static_assert(HotkeyChordErrorSentences.size()
                  == std::to_underlying(config::HotkeyChordError::NoModifier) + 1U,
              "HotkeyChordErrorSentences must have a sentence for every chord error");

/// How each HotkeyError reads in Settings, in enumerator order.
inline constexpr auto HotkeyErrorSentences = std::array<std::string_view, 6> {
    "This desktop has no global hotkeys.",             // Unsupported
    "That key cannot be a global hotkey.",             // UnsupportedKey
    "Another application already uses that shortcut.", // AlreadyTaken
    "The menu bar keeps that shortcut.",               // MenuOwned
    "The desktop did not allow that shortcut.",        // Declined
    "The system could not register that shortcut.",    // Failed
};

static_assert(HotkeyErrorSentences.size() == std::to_underlying(platform::HotkeyError::Failed) + 1U,
              "HotkeyErrorSentences must have a sentence for every hotkey error");

/// @param error Why a chord is refused whatever the platform.
/// @return The sentence Settings shows.
[[nodiscard]] constexpr std::string_view hotkeyRefusalText(config::HotkeyChordError error) noexcept
{
    return HotkeyChordErrorSentences[std::to_underlying(error)];
}

/// @param error Why a chord could not be registered.
/// @return The sentence Settings shows.
[[nodiscard]] constexpr std::string_view hotkeyRefusalText(platform::HotkeyError error) noexcept
{
    return HotkeyErrorSentences[std::to_underlying(error)];
}

/// Keeps the desktop's refusal of the quick terminal's hotkey until something can show it.
///
/// A refusal can come before there is anything to show it on: the hotkey is registered while the app starts,
/// ahead of the first window and of Settings, which is built on first use. The relay holds the last refusal
/// for Settings to show beside the hotkey field whenever it is built, and holds whether the one-time notice
/// has reached a window yet. GUI thread only.
class HotkeyRefusalRelay
{
  public:
    /// Records a refusal, and that its notice is yet to be shown.
    /// @param error Why the hotkey could not be registered.
    void refused(platform::HotkeyError error) noexcept
    {
        _problem = error;
        _noticePending = NoticeState::Pending;
    }

    /// Records that the hotkey is registered: no problem stands any more.
    void bound() noexcept
    {
        _problem.reset();
        _noticePending = NoticeState::Shown;
    }

    /// @return The refusal that stands, or nullopt when there is none; what Settings shows beside the field.
    [[nodiscard]] std::optional<platform::HotkeyError> problem() const noexcept { return _problem; }

    /// Hands out the refusal whose notice has not been shown yet, once.
    /// @return The refusal to announce, or nullopt when it was announced already or there is none.
    [[nodiscard]] std::optional<platform::HotkeyError> takeUnannounced() noexcept
    {
        if (_noticePending == NoticeState::Shown)
            return std::nullopt;
        _noticePending = NoticeState::Shown;
        return _problem;
    }

  private:
    /// Whether the notice of the stored refusal has been shown.
    enum class NoticeState : uint8_t
    {
        Shown = 0, ///< Nothing is waiting to be announced.
        Pending,   ///< The stored refusal has not been announced.
    };

    std::optional<platform::HotkeyError> _problem {};
    NoticeState _noticePending = NoticeState::Shown;
};

} // namespace contour::window
