// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the control API reads out of a pane: its text, bounded to the reply cap, and its command blocks as
/// API records. Pure functions over a terminal the caller has locked, so every rule here is table-testable.

#include <vtbackend/shell/CommandBlock.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include <vtcontrol/Api.hpp>
#include <vtcontrol/Uuid.hpp>

namespace vtbackend
{
class Terminal;
}

namespace vthost::control
{

/// A text cut to fit a reply.
struct BoundedText
{
    std::string text;                                              ///< What is left of it.
    vtcontrol::Truncation truncated = vtcontrol::Truncation::None; ///< `Head` when its start was cut.
};

/// Keeps the end of @p text: what a person reading a pane wants is what it printed last.
/// @param text The text, valid UTF-8.
/// @param maxBytes The most bytes to keep.
/// @return The last at most @p maxBytes bytes, starting at a UTF-8 sequence boundary, and whether the head
/// was cut.
[[nodiscard]] BoundedText keepTail(std::string text, std::size_t maxBytes);

/// Bounds a block's prompt and output together to @p maxBytes: the output is cut first, from its head, as it
/// is what grows; the prompt is cut only when it alone does not fit, and the output is then dropped.
/// @param prompt The prompt text, valid UTF-8.
/// @param output The output text, valid UTF-8.
/// @param maxBytes The most bytes the two may hold together.
/// @return The bounded pair, prompt first, and whether anything was cut.
[[nodiscard]] std::pair<BoundedText, BoundedText> boundBlockText(std::string prompt,
                                                                 std::string output,
                                                                 std::size_t maxBytes);

/// The text of the screen on display: its page rows and, above them, history rows. Each row's trailing blanks
/// are trimmed and trailing empty rows dropped; the bytes are as the cells hold them (not yet valid UTF-8).
/// The caller holds the terminal's lock.
/// @param terminal The terminal.
/// @param scrollback How many history rows above the page: 0 for none, -1 for all of them.
/// @return The rows, joined by `\n`, without a trailing newline.
[[nodiscard]] std::string captureText(vtbackend::Terminal const& terminal, int32_t scrollback);

/// @param instant A wall-clock instant.
/// @return It as RFC 3339 UTC with milliseconds, e.g. `2026-10-24T12:00:00.123Z`.
[[nodiscard]] std::string formatTimestamp(std::chrono::system_clock::time_point instant);

/// @param record A block's record.
/// @return Its API state: a finished record that a newer prompt closed is `Superseded`.
[[nodiscard]] vtcontrol::BlockState blockStateOf(vtbackend::CommandBlockRecord const& record) noexcept;

/// @param pane The UUID of the pane the block lives in.
/// @param record The block's record.
/// @param epoch The block store's id epoch.
/// @return The block as the API names and describes it.
[[nodiscard]] vtcontrol::BlockInfo blockInfoOf(vtcontrol::Uuid pane,
                                               vtbackend::CommandBlockRecord const& record,
                                               uint64_t epoch);

} // namespace vthost::control
