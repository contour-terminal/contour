// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/Clock.hpp>
#include <core/tui/ClipboardProtocol.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/TerminalChannel.hpp>

#include <chrono>
#include <expected>
#include <functional>
#include <optional>
#include <string_view>

namespace core::tui
{

/// @brief How long a @c ClipboardWriter waits for the terminal.
struct ClipboardWriterTimeouts
{
    /// The OSC 5522 probe. It ends at the DA1 reply every terminal sends, so this is only a
    /// backstop for one that answers nothing; generous enough for a slow SSH link.
    std::chrono::milliseconds probe { 500 };

    /// An OSC 5522 write's status. Long enough for a terminal that asks its user first.
    std::chrono::milliseconds status { 5000 };
};

/// @brief Copies to the clipboard through the terminal: OSC 5522 where the terminal speaks it,
/// OSC 52 where it does not.
///
/// The first copy over a readable channel asks the terminal whether it knows mode 5522 (DECRQM),
/// followed by a DA1 query that every terminal answers, so the answer is in as soon as the DA1
/// reply is, and the probe waits for its timeout only when nothing answers at all. The answer is
/// kept for this writer's lifetime. Each copy opens its own channel and closes it again, so nothing
/// holds the terminal between copies.
class ClipboardWriter
{
  public:
    /// @brief Opens the channel a copy is sent over.
    using ChannelFactory = std::function<TerminalChannelResult()>;

    /// @param openChannel Opens a channel per copy (@c openControllingTerminal in production).
    /// @param clock The clock the timeouts are measured on (not owned; must outlive this writer).
    /// @param timeouts How long to wait for the terminal.
    ClipboardWriter(ChannelFactory openChannel,
                    core::platform::IClock& clock,
                    ClipboardWriterTimeouts timeouts = {});

    /// @brief Copies @p data to the terminal's clipboard.
    /// @param data The bytes to copy.
    /// @param mime Their MIME type; anything but `text/plain` needs OSC 5522.
    /// @param target The selection to replace.
    /// @return The protocol that carried the copy, or why there was none.
    [[nodiscard]] auto write(std::string_view data, std::string_view mime, ClipboardTarget target)
        -> std::expected<ClipboardTransport, ClipboardWriteError>;

  private:
    /// @brief The protocol to use over @p channel, probing the terminal the first time.
    [[nodiscard]] auto transportFor(TerminalChannel& channel)
        -> std::expected<ClipboardTransport, ClipboardWriteError>;

    /// @brief Sends the copy over @p transport and, for OSC 5522, waits for its status.
    [[nodiscard]] auto send(TerminalChannel& channel,
                            ClipboardTransport transport,
                            std::string_view data,
                            std::string_view mime,
                            ClipboardTarget target) -> std::expected<ClipboardTransport, ClipboardWriteError>;

    ChannelFactory _openChannel;
    core::platform::IClock& _clock;
    ClipboardWriterTimeouts _timeouts;
    std::optional<ClipboardTransport> _probed; ///< The probe's answer, once there was one.
};

} // namespace core::tui
