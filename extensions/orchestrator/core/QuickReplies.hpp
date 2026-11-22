// SPDX-License-Identifier: Apache-2.0
//
// The quick replies: what the digits answer an agent that asks for a permission with, as the `quick_replies`
// table configures them. The extension never reads the agent's tab, so a reply's label is the configuration's
// words, never the menu's.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orchestrator
{

/// A key a quick reply presses in the agent's tab: the host's `pane-key-name`, in its order.
enum class PaneKey : uint8_t
{
    Digit1 = 0, ///< `1`.
    Digit2,     ///< `2`.
    Digit3,     ///< `3`.
    Digit4,     ///< `4`.
    Digit5,     ///< `5`.
    Digit6,     ///< `6`.
    Digit7,     ///< `7`.
    Digit8,     ///< `8`.
    Digit9,     ///< `9`.
    Escape,     ///< The Escape key: Claude Code's menu then rejects the request.
};

/// What follows a quick reply on the dashboard.
enum class AfterReply : uint8_t
{
    Wait = 0, ///< Nothing: the asking box stays, dimmed, until the agent stops asking.
    Message,  ///< The message box opens for the run, so that the next keys say why.
};

/// One row of `quick_replies`.
struct QuickReply
{
    char key = '1';                     ///< The dashboard's digit, `1` to `9`.
    std::string label;                  ///< What its chip says, such as `yes`.
    PaneKey press = PaneKey::Digit1;    ///< What it presses in the agent's tab; by default its own digit.
    AfterReply then = AfterReply::Wait; ///< What follows it.

    [[nodiscard]] bool operator==(QuickReply const&) const = default;
};

/// The longest label a quick reply may have, in bytes: its chip shares a line with the others.
inline constexpr std::size_t MaxQuickReplyLabelBytes = 40;

/// @return The replies a configuration without `quick_replies` has, in Claude Code's own words, shortened:
///         `1 yes` and `2 yes, don't ask again` press their digit; `3 no, and say why` presses Escape, which
///         rejects the request whichever number the menu gave No, and opens the message box.
[[nodiscard]] std::vector<QuickReply> defaultQuickReplies();

/// @param text Such as `1` or `esc`.
/// @return The key it names; std::nullopt for any other text.
[[nodiscard]] std::optional<PaneKey> paneKeyNamed(std::string_view text) noexcept;

/// @param key A key.
/// @return How the configuration spells it: `1` to `9`, or `esc`.
[[nodiscard]] std::string_view paneKeyName(PaneKey key) noexcept;

/// @return Every spelling paneKeyNamed() accepts, in PaneKey's order.
[[nodiscard]] std::span<std::string_view const> paneKeyNames() noexcept;

/// @param text Such as `wait` or `message`.
/// @return What it names; std::nullopt for any other text.
[[nodiscard]] std::optional<AfterReply> afterReplyNamed(std::string_view text) noexcept;

/// @return Every spelling afterReplyNamed() accepts, in AfterReply's order.
[[nodiscard]] std::span<std::string_view const> afterReplyNames() noexcept;

/// @param replies The configured replies.
/// @param key A dashboard digit.
/// @return The reply on @p key; nullptr when none is.
[[nodiscard]] QuickReply const* findQuickReply(std::span<QuickReply const> replies, char key) noexcept;

} // namespace orchestrator
