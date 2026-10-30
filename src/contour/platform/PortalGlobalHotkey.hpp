// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/HotkeyChord.hpp>
#include <contour/platform/GlobalHotkey.hpp>
#include <contour/platform/GlobalShortcutsChannel.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace contour::platform
{

/// The id of the one shortcut the application binds.
inline constexpr auto QuickTerminalShortcutId = std::string_view("quick-terminal");

/// The global hotkey of a desktop that has the GlobalShortcuts portal: one session per process, created
/// by the first registration, with one shortcut bound to it.
///
/// The portal answers later than registerHotkey() returns, and the user may refuse in the desktop's own
/// dialog, so registerHotkey() always returns success for a chord it can spell and reports the outcome
/// through its result callback. Compiled everywhere and tested against a fake channel; only the channel
/// is Linux-specific.
class PortalGlobalHotkey final: public IGlobalHotkey
{
  public:
    /// @param channel The portal, owned for the lifetime of this hotkey.
    explicit PortalGlobalHotkey(std::unique_ptr<IGlobalShortcutsChannel> channel);

    /// Closes the session, if one is open.
    ~PortalGlobalHotkey() override;

    /// Binds @p chord. Registering a chord equal to the bound one sends nothing to the portal: it only
    /// replaces the callbacks, and repeats the portal's answer if it gave one. Registering another chord,
    /// or one the portal refused, closes the open session and binds the chord on a new one, so the new
    /// preferred trigger is never left to the portal's idea of replacing an existing shortcut.
    /// @return UnsupportedKey when the chord's key has no portal spelling, else success.
    /// @pre GUI thread.
    [[nodiscard]] HotkeyResult registerHotkey(config::HotkeyChord const& chord,
                                              HotkeyPressed onPressed,
                                              HotkeyResultCallback onResult) override;

    /// Closes the session and forgets the chord.
    /// @pre GUI thread.
    void unregisterHotkey() override;

  private:
    /// Where the portal session stands.
    enum class SessionState : uint8_t
    {
        Closed = 0, ///< No session; the next registration creates one.
        Creating,   ///< CreateSession is out; the chord is bound when it answers.
        Open,       ///< The session handle is held.
    };

    void createSession();
    void closeSession();
    void bindChord();
    void settle(HotkeyResult result);
    void onActivated(std::string const& shortcutId, std::string const& activationToken) const;

    std::unique_ptr<IGlobalShortcutsChannel> _channel;
    SessionState _session = SessionState::Closed;
    std::string _sessionHandle {};
    std::optional<config::HotkeyChord> _chord {};
    std::optional<HotkeyResult> _settled {}; ///< The outcome of the bound chord, once the portal answered.
    HotkeyPressed _onPressed {};
    HotkeyResultCallback _onResult {};
    /// Counts unregistrations, so a late CreateSession answer for a session nobody wants is recognised.
    std::uint64_t _generation = 0;
    /// Counts bind requests and unregistrations, so a late BindShortcuts answer for a superseded chord is
    /// dropped.
    std::uint64_t _bindSeq = 0;
};

} // namespace contour::platform
