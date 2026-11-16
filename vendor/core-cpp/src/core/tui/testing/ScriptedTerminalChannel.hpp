// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file ScriptedTerminalChannel.hpp
/// @brief A terminal, scripted, for tests of anything that talks to one through a TerminalChannel.
///
/// @c ScriptedTerminal is the terminal: how it answers the OSC 5522 probe, whether it confirms a
/// write, and every byte it was sent. Each @c ScriptedTerminal::factory() call opens a new
/// @c ScriptedTerminalChannel onto it, the way a writer opens a channel per copy. Time is a
/// @c core::platform::ManualClock that a poll with nothing to deliver advances by its timeout, so a
/// case that waits for a timeout costs no real time.

#include <core/Base64.hpp>
#include <core/platform/Clock.hpp>
#include <core/tui/ClipboardProtocol.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/TerminalChannel.hpp>
#include <core/tui/TerminalProtocols.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace core::tui::testing
{

/// @brief Whether a scripted terminal is there to be opened.
enum class TerminalPresence : std::uint8_t
{
    Present, ///< Opening it succeeds.
    Absent,  ///< Opening it fails with @c TerminalChannelError::NoTerminal.
};

/// @brief A scripted terminal: its answers, and a record of what it was sent.
struct ScriptedTerminal
{
    TerminalPresence presence = TerminalPresence::Present;
    ChannelAccess access = ChannelAccess::ReadWrite;
    std::optional<int> decModeStatus;          ///< Answer to `CSI ? 5522 $ p`; nullopt: no answer.
    bool answersDeviceAttributes = true;       ///< Whether `CSI c` (DA1) is answered.
    std::optional<std::string> writeStatus;    ///< OSC 5522 status (`DONE`, `EPERM`, ...); nullopt: none.
    std::vector<InputEvent> beforeWriteStatus; ///< Replies sent ahead of the write status.
    std::vector<InputEvent> queuedAtOpen;      ///< Input already waiting when a channel opens.
    std::vector<std::string> written;          ///< Every write, in order.
    int opens = 0;                             ///< How many channels were opened.
    int discards = 0;                          ///< How often queued input was discarded.

    /// @brief A factory that opens a channel onto this terminal, timed by @p clock.
    /// @param clock The clock a poll with nothing to deliver advances.
    /// @return The factory; this terminal and @p clock must outlive every channel it opens.
    [[nodiscard]] auto factory(core::platform::ManualClock& clock) -> std::function<TerminalChannelResult()>;

    /// @brief The bytes the last copy carried, decoded from OSC 52 or OSC 5522.
    [[nodiscard]] auto decodedPayload() const -> std::string;

    /// @brief The MIME type of the last copy: empty for OSC 52, which has none.
    [[nodiscard]] auto mimeType() const -> std::string;

    /// @brief The selection the last copy replaced, or nullopt when nothing was copied.
    [[nodiscard]] auto target() const -> std::optional<ClipboardTarget>;

    /// @brief How many writes contain @p needle.
    [[nodiscard]] auto writesContaining(std::string_view needle) const -> std::size_t
    {
        return static_cast<std::size_t>(
            std::ranges::count_if(written, [needle](std::string const& w) { return w.contains(needle); }));
    }

    /// The prefix of every OSC 5522 data packet, up to its base64 MIME type.
    static constexpr auto DataPacket = std::string_view { "\033]5522;type=wdata:mime=" };

    /// The OSC 52 introducer.
    static constexpr auto Osc52Introducer = std::string_view { "\033]52;" };

  private:
    /// @brief The last write that carried a copy, or nullptr.
    [[nodiscard]] auto lastCopy() const -> std::string const*
    {
        auto const isCopy = [](std::string const& w) {
            return w.contains(Osc52Introducer) || w.contains(DataPacket);
        };
        auto const found = std::ranges::find_if(written | std::views::reverse, isCopy);
        return found == std::ranges::end(written | std::views::reverse) ? nullptr : &*found;
    }

    /// @brief The base64 between @p start and the next @p terminator in @p copy, decoded.
    [[nodiscard]] static auto decodeUntil(std::string_view copy,
                                          std::size_t start,
                                          std::string_view terminator) -> std::string
    {
        return core::base64::decode(copy.substr(start, copy.find(terminator, start) - start));
    }
};

/// @brief A channel onto a @c ScriptedTerminal.
///
/// A write is recorded and answered: the OSC 5522 probe with a DECRQM reply and a DA1 reply, as
/// the terminal is scripted to; the end packet of an OSC 5522 write with @c beforeWriteStatus and
/// the write status. A poll delivers one queued reply, so a reply that arrives over several reads
/// is what a caller always sees; with nothing queued it advances the clock by its timeout.
class ScriptedTerminalChannel final: public TerminalChannel
{
  public:
    /// @param terminal The terminal this channel is onto.
    /// @param clock The clock an empty poll advances.
    ScriptedTerminalChannel(ScriptedTerminal& terminal, core::platform::ManualClock& clock):
        _terminal(terminal),
        _clock(clock),
        _replies(terminal.queuedAtOpen.begin(), terminal.queuedAtOpen.end())
    {
    }

    [[nodiscard]] auto access() const noexcept -> ChannelAccess override { return _terminal.access; }

    [[nodiscard]] auto write(std::string_view bytes) -> std::expected<void, TerminalChannelError> override
    {
        _terminal.written.emplace_back(bytes);
        if (_terminal.access == ChannelAccess::WriteOnly)
            return {};
        if (bytes.contains(Osc5522ModeQuery) && _terminal.decModeStatus)
            _replies.emplace_back(DecModeReport { .mode = Osc5522Mode, .status = *_terminal.decModeStatus });
        if (bytes.contains(protocols::QueryPrimaryDeviceAttributes) && _terminal.answersDeviceAttributes)
            _replies.emplace_back(DeviceAttributesReport { .attributes = { 62, 4, 22 } });
        if (bytes.ends_with("\033]5522;type=wdata\033\\"))
        {
            _replies.insert(
                _replies.end(), _terminal.beforeWriteStatus.begin(), _terminal.beforeWriteStatus.end());
            if (_terminal.writeStatus)
                _replies.emplace_back(
                    OscResponse { .payload = "5522;type=write:status=" + *_terminal.writeStatus });
        }
        return {};
    }

    [[nodiscard]] auto poll(int timeoutMs)
        -> std::expected<std::vector<InputEvent>, TerminalChannelError> override
    {
        if (_replies.empty())
        {
            _clock.advance(std::chrono::milliseconds(timeoutMs));
            return std::vector<InputEvent> {};
        }
        auto events = std::vector<InputEvent> {};
        events.push_back(std::move(_replies.front()));
        _replies.pop_front();
        return events;
    }

    void discardPendingInput() override
    {
        ++_terminal.discards;
        _replies.clear();
    }

  private:
    ScriptedTerminal& _terminal;
    core::platform::ManualClock& _clock;
    std::deque<InputEvent> _replies;
};

inline auto ScriptedTerminal::factory(core::platform::ManualClock& clock)
    -> std::function<TerminalChannelResult()>
{
    return [this, &clock]() -> TerminalChannelResult {
        if (presence == TerminalPresence::Absent)
            return std::unexpected { TerminalChannelError::NoTerminal };
        ++opens;
        return std::make_unique<ScriptedTerminalChannel>(*this, clock);
    };
}

inline auto ScriptedTerminal::decodedPayload() const -> std::string
{
    auto const* copy = lastCopy();
    if (copy == nullptr)
        return {};

    // OSC 52: ESC ] 52 ; <sel> ; <base64> ST
    if (auto const osc52 = copy->find(Osc52Introducer); osc52 != std::string::npos)
        return decodeUntil(
            *copy, copy->find(';', osc52 + Osc52Introducer.size()) + 1, protocols::StringTerminator);

    // OSC 5522: every data packet's payload, decoded and concatenated.
    auto payload = std::string {};
    auto pos = copy->find(DataPacket);
    while (pos != std::string::npos)
    {
        payload +=
            decodeUntil(*copy, copy->find(';', pos + DataPacket.size()) + 1, protocols::StringTerminator);
        pos = copy->find(DataPacket, pos + 1);
    }
    return payload;
}

inline auto ScriptedTerminal::mimeType() const -> std::string
{
    auto const* copy = lastCopy();
    if (copy == nullptr)
        return {};
    auto const pos = copy->find(DataPacket);
    if (pos == std::string::npos)
        return {};
    return decodeUntil(*copy, pos + DataPacket.size(), ";");
}

inline auto ScriptedTerminal::target() const -> std::optional<ClipboardTarget>
{
    auto const* copy = lastCopy();
    if (copy == nullptr)
        return std::nullopt;
    auto const primary = copy->contains("\033]52;p;") || copy->contains("type=write:loc=primary");
    return primary ? ClipboardTarget::Primary : ClipboardTarget::Clipboard;
}

} // namespace core::tui::testing
