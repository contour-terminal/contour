// SPDX-License-Identifier: Apache-2.0
#include <core/tui/ClipboardWriter.hpp>

#include <core/tui/Terminal.hpp>

#include <chrono>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace core::tui
{

namespace
{
    /// @brief Whether a DECRQM answer says the terminal knows the mode.
    constexpr auto knowsMode(DecModeStatus status) noexcept -> bool
    {
        return status == DecModeStatus::Set || status == DecModeStatus::Reset
               || status == DecModeStatus::PermanentlySet;
    }

    /// @brief A channel's failure, as the copy's.
    constexpr auto toClipboardError(TerminalChannelError error) noexcept -> ClipboardWriteError
    {
        return error == TerminalChannelError::NoTerminal ? ClipboardWriteError::NoTerminal
                                                         : ClipboardWriteError::IoError;
    }

    /// @brief Polls @p channel until @p extract finds what it waits for in an event, or @p timeout
    /// elapses on @p clock.
    /// @param extract Maps an event to std::optional<T>: a value ends the wait.
    /// @return The value @p extract found, nullopt at the timeout, or the channel's error.
    template <typename Extract>
    auto awaitEvent(TerminalChannel& channel,
                    core::platform::IClock& clock,
                    std::chrono::milliseconds timeout,
                    Extract const& extract)
        -> std::expected<std::invoke_result_t<Extract const&, InputEvent const&>, ClipboardWriteError>
    {
        auto const deadline = clock.now() + timeout;
        // The deadline alone ends the wait: an empty poll may be a signal or half of a reply.
        auto now = clock.now();
        while (now < deadline)
        {
            // Rounded UP: a sub-millisecond remainder truncated to 0 makes the last wait a spin.
            auto const remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
            auto events = channel.poll(static_cast<int>(remaining));
            if (!events)
                return std::unexpected { toClipboardError(events.error()) };
            for (auto const& event: *events)
                if (auto found = extract(event))
                    return found;
            now = clock.now();
        }
        return std::nullopt;
    }
} // namespace

ClipboardWriter::ClipboardWriter(ChannelFactory openChannel,
                                 core::platform::IClock& clock,
                                 ClipboardWriterTimeouts timeouts):
    _openChannel(std::move(openChannel)), _clock(clock), _timeouts(timeouts)
{
}

auto ClipboardWriter::write(std::string_view data, std::string_view mime, ClipboardTarget target)
    -> std::expected<ClipboardTransport, ClipboardWriteError>
{
    return _openChannel()
        .transform_error(toClipboardError)
        .and_then([&](std::unique_ptr<TerminalChannel> const& channel) {
            return transportFor(*channel).and_then(
                [&](ClipboardTransport transport) { return send(*channel, transport, data, mime, target); });
        });
}

auto ClipboardWriter::transportFor(TerminalChannel& channel)
    -> std::expected<ClipboardTransport, ClipboardWriteError>
{
    // A background process may write but not read: no probe, and nothing learned to keep.
    if (channel.access() == ChannelAccess::WriteOnly)
        return ClipboardTransport::Osc52;
    if (_probed)
        return *_probed;

    // DECRQM's answer, if any, arrives before DA1's, and DA1's ends the probe.
    auto supported = false;
    auto const probeResult = [&supported](InputEvent const& event) -> std::optional<bool> {
        if (auto const* report = std::get_if<DecModeReport>(&event); report && report->mode == Osc5522Mode)
            supported = knowsMode(decModeStatusFromReply(report->status));
        if (std::holds_alternative<DeviceAttributesReport>(event))
            return supported;
        return std::nullopt;
    };

    channel.discardPendingInput();
    return channel.write(Osc5522Probe)
        .transform_error(toClipboardError)
        .and_then([&] { return awaitEvent(channel, _clock, _timeouts.probe, probeResult); })
        .transform([&](std::optional<bool> answered) {
            _probed = answered.value_or(false) ? ClipboardTransport::Osc5522 : ClipboardTransport::Osc52;
            return *_probed;
        });
}

auto ClipboardWriter::send(TerminalChannel& channel,
                           ClipboardTransport transport,
                           std::string_view data,
                           std::string_view mime,
                           ClipboardTarget target) -> std::expected<ClipboardTransport, ClipboardWriteError>
{
    if (transport == ClipboardTransport::Osc52)
    {
        if (!isPlainTextMimeType(mime))
            return std::unexpected { ClipboardWriteError::UnsupportedMimeType };
        return channel.write(encodeOsc52(data, target)).transform_error(toClipboardError).transform([] {
            return ClipboardTransport::Osc52;
        });
    }

    auto const writeStatus =
        [](InputEvent const& event) -> std::optional<std::expected<void, ClipboardWriteError>> {
        if (auto const* osc = std::get_if<OscResponse>(&event))
            return parseOsc5522WriteStatus(osc->payload);
        return std::nullopt;
    };

    // Only a reply that arrives after this write may confirm it.
    channel.discardPendingInput();
    return channel.write(encodeOsc5522Write(data, mime, target))
        .transform_error(toClipboardError)
        .and_then([&] { return awaitEvent(channel, _clock, _timeouts.status, writeStatus); })
        .and_then([](std::optional<std::expected<void, ClipboardWriteError>> status)
                      -> std::expected<ClipboardTransport, ClipboardWriteError> {
            if (!status)
                return std::unexpected { ClipboardWriteError::NoConfirmation };
            return status->transform([] { return ClipboardTransport::Osc5522; });
        });
}

} // namespace core::tui
