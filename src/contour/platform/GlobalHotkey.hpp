// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The operating system's global hotkey: one chord that reaches this application while another has the
/// focus. Qt-free: every backend is behind IGlobalHotkey and the key table is tested in contour_test.

#include <contour/config/HotkeyChord.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>

namespace contour::platform
{

/// Why a global hotkey could not be registered.
enum class HotkeyError : uint8_t
{
    Unsupported = 0, ///< This platform or session has no global hotkeys.
    UnsupportedKey,  ///< The key has no row in HotkeyKeyRows.
    AlreadyTaken,    ///< Another application owns the chord.
    MenuOwned,       ///< The chord is one the macOS menu bar keeps; refused before the OS is asked.
    Declined,        ///< The user or the desktop refused it (the Linux portal).
    Failed,          ///< The OS call failed for another reason.
};

/// Called on the GUI thread when the registered chord is pressed.
/// The argument is the desktop's focus-stealing token where one is given (the Linux portal), else empty.
using HotkeyPressed = std::function<void(std::string activationToken)>;

/// How a registration ended: bound, or why not.
using HotkeyResult = std::expected<void, HotkeyError>;

/// Called once a registration is settled; the Linux portal answers later than the call returns.
using HotkeyResultCallback = std::function<void(HotkeyResult)>;

/// One application-wide hotkey. Registering replaces any chord registered before.
class IGlobalHotkey
{
  public:
    IGlobalHotkey() = default;
    IGlobalHotkey(IGlobalHotkey const&) = delete;
    IGlobalHotkey& operator=(IGlobalHotkey const&) = delete;
    IGlobalHotkey(IGlobalHotkey&&) = delete;
    IGlobalHotkey& operator=(IGlobalHotkey&&) = delete;
    virtual ~IGlobalHotkey() = default;

    /// Binds @p chord, replacing the chord bound before.
    /// @param chord     The chord to bind.
    /// @param onPressed Called on the GUI thread each time the chord is pressed.
    /// @param onResult  Called once the registration is settled. The Linux portal answers later, so
    ///                  registerHotkey() returns at once and the portal's refusal arrives here.
    /// @return An error when the chord cannot be bound; nothing is registered then.
    /// @pre GUI (main) thread.
    [[nodiscard]] virtual HotkeyResult registerHotkey(config::HotkeyChord const& chord,
                                                      HotkeyPressed onPressed,
                                                      HotkeyResultCallback onResult) = 0;

    /// Releases the chord, if one is bound.
    /// @pre GUI (main) thread. Idempotent.
    virtual void unregisterHotkey() = 0;
};

/// The hotkey of a platform that has none: every registration is refused as Unsupported.
class NullGlobalHotkey final: public IGlobalHotkey
{
  public:
    /// @return Unsupported, and settles @p onResult with it.
    [[nodiscard]] HotkeyResult registerHotkey(config::HotkeyChord const& /*chord*/,
                                              HotkeyPressed /*onPressed*/,
                                              HotkeyResultCallback onResult) override
    {
        auto const result = HotkeyResult { std::unexpected(HotkeyError::Unsupported) };
        if (onResult)
            onResult(result);
        return result;
    }

    /// Does nothing.
    void unregisterHotkey() override {}
};

/// Creates the operating system's global hotkey, or the null one where there is none. Registers nothing
/// until registerHotkey() is called.
/// @return The hotkey.
[[nodiscard]] std::unique_ptr<IGlobalHotkey> makeGlobalHotkey();

} // namespace contour::platform
