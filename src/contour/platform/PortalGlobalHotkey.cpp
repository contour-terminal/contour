// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/HotkeyKeys.hpp>
#include <contour/platform/PortalGlobalHotkey.hpp>

#include <utility>
#include <vector>

namespace contour::platform
{

namespace
{
    /// What the desktop's dialog says the shortcut is for.
    constexpr auto QuickTerminalDescription = std::string_view("Show or hide the quick terminal");

    /// @return How a refused or failed request is reported.
    [[nodiscard]] HotkeyResult resultOf(PortalResponse response)
    {
        switch (response)
        {
            case PortalResponse::Success: return {};
            case PortalResponse::Cancelled: return std::unexpected(HotkeyError::Declined);
            case PortalResponse::Other: break;
        }
        return std::unexpected(HotkeyError::Failed);
    }
} // namespace

PortalGlobalHotkey::PortalGlobalHotkey(std::unique_ptr<IGlobalShortcutsChannel> channel):
    _channel { std::move(channel) }
{
    _channel->onActivated([this](std::string const& shortcutId, std::string const& activationToken) {
        onActivated(shortcutId, activationToken);
    });
}

PortalGlobalHotkey::~PortalGlobalHotkey()
{
    unregisterHotkey();
}

HotkeyResult PortalGlobalHotkey::registerHotkey(config::HotkeyChord const& chord,
                                                HotkeyPressed onPressed,
                                                HotkeyResultCallback onResult)
{
    if (xdgTrigger(chord).empty())
    {
        auto const refusal = HotkeyResult { std::unexpected(HotkeyError::UnsupportedKey) };
        if (onResult)
            onResult(refusal);
        return refusal;
    }

    _onPressed = std::move(onPressed);
    _onResult = std::move(onResult);

    // An unchanged chord the portal accepted, or has yet to answer for, sends nothing. One it refused is
    // tried afresh: a refusal says no to that attempt, not to the chord for good (the user may have freed
    // it since), and replaying it would pin the stale answer for as long as the process lives.
    auto const refused = _settled && !*_settled;
    if (_chord == chord && !refused)
    {
        if (_settled && _onResult)
            _onResult(*_settled);
        return {};
    }

    _chord = chord;
    _settled.reset();
    ++_bindSeq;
    switch (_session)
    {
        case SessionState::Closed: createSession(); break;
        case SessionState::Creating: break; // bindChord() runs when the session answers, with this chord.
        case SessionState::Open:
            // A new session carrying the new preferred trigger, rather than a second BindShortcuts on
            // this one: a portal may keep the shortcut's earlier trigger (the user's choice, or the
            // first one bound) and ignore a changed preference.
            closeSession();
            createSession();
            break;
    }
    return {};
}

void PortalGlobalHotkey::closeSession()
{
    // A late CreateSession answer of the session being left is recognised by the generation and closed.
    ++_generation;
    ++_bindSeq;
    if (_session == SessionState::Open)
        _channel->closeSession(_sessionHandle);
    _session = SessionState::Closed;
    _sessionHandle.clear();
}

void PortalGlobalHotkey::unregisterHotkey()
{
    closeSession();
    _chord.reset();
    _settled.reset();
    _onPressed = nullptr;
    _onResult = nullptr;
}

void PortalGlobalHotkey::createSession()
{
    // Set before asking: the null channel answers before createSession() returns.
    _session = SessionState::Creating;
    _channel->createSession(
        [this, generation = _generation](PortalResponse response, std::string sessionHandle) {
            if (generation != _generation)
            {
                // Unregistered while the request was out: nothing wants this session.
                if (response == PortalResponse::Success)
                    _channel->closeSession(sessionHandle);
                return;
            }
            if (response != PortalResponse::Success)
            {
                _session = SessionState::Closed;
                settle(resultOf(response));
                return;
            }
            _session = SessionState::Open;
            _sessionHandle = std::move(sessionHandle);
            bindChord();
        });
}

void PortalGlobalHotkey::bindChord()
{
    if (!_chord)
        return;
    auto const binding = std::vector { ShortcutBinding { .id = std::string(QuickTerminalShortcutId),
                                                         .description = std::string(QuickTerminalDescription),
                                                         .preferredTrigger = xdgTrigger(*_chord) } };
    _channel->bindShortcuts(_sessionHandle, binding, [this, bindSeq = ++_bindSeq](PortalResponse response) {
        if (bindSeq == _bindSeq)
            settle(resultOf(response));
    });
}

void PortalGlobalHotkey::settle(HotkeyResult result)
{
    _settled = result;
    if (_onResult)
        _onResult(result);
}

void PortalGlobalHotkey::onActivated(std::string const& shortcutId, std::string const& activationToken) const
{
    if (shortcutId == QuickTerminalShortcutId && _onPressed)
        _onPressed(activationToken);
}

} // namespace contour::platform
