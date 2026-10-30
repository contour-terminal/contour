// SPDX-License-Identifier: Apache-2.0
#pragma once

// A global hotkey that records into a struct instead of reaching the OS, so no test registers a chord
// with the real desktop.

#include <contour/config/HotkeyChord.hpp>
#include <contour/platform/GlobalHotkey.hpp>

#include <string>
#include <utility>
#include <vector>

namespace contour::test
{

/// A global hotkey that records what it is asked.
class RecordingGlobalHotkey final: public platform::IGlobalHotkey
{
  public:
    /// Every chord registered successfully, in order.
    std::vector<config::HotkeyChord> registered {};
    /// How often unregisterHotkey() was called.
    int unregisters = 0;
    /// What registerHotkey() answers; success by default. An error registers nothing.
    platform::HotkeyResult result {};

    /// Records @p chord unless result is an error, then settles @p onResult with result.
    /// @return result.
    [[nodiscard]] platform::HotkeyResult registerHotkey(config::HotkeyChord const& chord,
                                                        platform::HotkeyPressed onPressed,
                                                        platform::HotkeyResultCallback onResult) override
    {
        if (result)
        {
            registered.push_back(chord);
            _onPressed = std::move(onPressed);
        }
        if (onResult)
            onResult(result);
        return result;
    }

    /// Counts the call and forgets the pressed callback.
    void unregisterHotkey() override
    {
        ++unregisters;
        _onPressed = nullptr;
    }

    /// Presses the registered chord, as the OS would: calls the callback of the last registration.
    /// Does nothing when nothing is registered.
    /// @param activationToken The focus-stealing token to deliver; empty by default.
    void press(std::string activationToken = {}) const
    {
        if (_onPressed)
            _onPressed(std::move(activationToken));
    }

  private:
    platform::HotkeyPressed _onPressed {};
};

} // namespace contour::test
