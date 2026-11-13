// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The wire between Contour's askpass mode and the GUI that started the ssh: what one prompt is, how it
/// is told over the local socket, and how it is classified. Qt-free, so the helper process that ssh
/// starts links no Qt and tests it in contour_test.

#include <contour/askpass/Secret.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace contour::askpass
{

/// The environment variable holding where the GUI listens: a Unix socket path, or a named pipe on Windows.
inline constexpr std::string_view AskpassEndpointVariable = "CONTOUR_ASKPASS_ENDPOINT";

/// The environment variable holding the session's secret nonce, which every request must repeat.
inline constexpr std::string_view AskpassNonceVariable = "CONTOUR_ASKPASS_NONCE";

/// The environment variable OpenSSH sets to tell a confirmation (`confirm`) or a notice (`none`) from a
/// secret prompt.
inline constexpr std::string_view SshAskpassPromptVariable = "SSH_ASKPASS_PROMPT";

/// What a prompt asks of the user.
enum class AskpassPromptKind : std::uint8_t
{
    Secret = 0, ///< A password, a passphrase, a one-time code: typed, never echoed.
    Confirm,    ///< A yes-or-no question answered by exit status (`SSH_ASKPASS_PROMPT=confirm`).
    Notice,     ///< A message to show, answered by nothing (`SSH_ASKPASS_PROMPT=none`), e.g. a FIDO touch.
    HostKey,    ///< The unknown-host question, answered with `yes` or `no` or a fingerprint.
    /// ssh warns about a host key it has other evidence about -- the IP address's key differs
    /// (CheckHostIP), or keys of another type are known -- and asks whether to go on, answered `yes` or `no`.
    HostKeyWarning,
    HostKeyUpdate, ///< UpdateHostKeys: whether to take the new keys the server offers, answered `yes` or
                   ///< `no`.
    Question, ///< Any other question ssh asks in the `(yes/no` form: shown as it is, answered `yes` or `no`.
    /// Whether to trust a daemon met for the first time over TLS and pin its fingerprint. Not ssh's: asked
    /// by the application itself, answered Answered (trust) or Cancelled.
    PinConfirm,
};

/// One row of the table classifyAskpassPrompt() reads: what in the request selects a kind.
struct AskpassPromptRule
{
    /// Which field of the request the rule reads.
    enum class Field : std::uint8_t
    {
        SshAskpassPrompt, ///< `SSH_ASKPASS_PROMPT`, compared whole.
        PromptText,       ///< The prompt argument, searched.
    };

    Field field;
    std::string_view needle;
    AskpassPromptKind kind;
};

/// The classification, first matching row wins; a request no row matches is a Secret.
inline constexpr auto AskpassPromptRules = std::array {
    AskpassPromptRule { AskpassPromptRule::Field::SshAskpassPrompt, "confirm", AskpassPromptKind::Confirm },
    AskpassPromptRule { AskpassPromptRule::Field::SshAskpassPrompt, "none", AskpassPromptKind::Notice },
    // The warnings first: their text holds the generic question's `(yes/no` too.
    AskpassPromptRule { AskpassPromptRule::Field::PromptText,
                        "differs from the key for the IP address",
                        AskpassPromptKind::HostKeyWarning },
    AskpassPromptRule { AskpassPromptRule::Field::PromptText,
                        "keys of different type are already known",
                        AskpassPromptKind::HostKeyWarning },
    AskpassPromptRule {
        AskpassPromptRule::Field::PromptText, "Accept updated hostkeys", AskpassPromptKind::HostKeyUpdate },
    AskpassPromptRule {
        AskpassPromptRule::Field::PromptText, "can't be established", AskpassPromptKind::HostKey },
    // What no row above names, but asks yes or no: neutral, and shown verbatim.
    AskpassPromptRule { AskpassPromptRule::Field::PromptText, "(yes/no", AskpassPromptKind::Question },
};

/// @param prompt The prompt text ssh passed as the helper's argument.
/// @param sshAskpassPrompt The value of `SSH_ASKPASS_PROMPT`, empty when unset.
/// @return What the prompt asks of the user.
[[nodiscard]] AskpassPromptKind classifyAskpassPrompt(std::string_view prompt,
                                                      std::string_view sshAskpassPrompt);

/// What the helper process tells the GUI.
struct AskpassRequest
{
    std::string nonce;            ///< `CONTOUR_ASKPASS_NONCE`: proves the sender was started by this session.
    std::string prompt;           ///< The prompt text.
    std::string sshAskpassPrompt; ///< `SSH_ASKPASS_PROMPT`, or empty.

    [[nodiscard]] bool operator==(AskpassRequest const&) const = default;
};

/// How a prompt ended.
enum class AskpassOutcome : std::uint8_t
{
    Answered = 0,
    Cancelled,
};

/// What the GUI tells the helper process back.
struct AskpassReply
{
    AskpassOutcome outcome = AskpassOutcome::Cancelled;
    Secret answer; ///< Meaningful only for Answered. A secret: wiped when it goes, never logged.

    [[nodiscard]] bool operator==(AskpassReply const&) const = default;
};

/// @param answer The text to answer with.
/// @return A reply that answers with @p answer.
[[nodiscard]] inline AskpassReply answeredReply(std::string_view answer)
{
    return AskpassReply { .outcome = AskpassOutcome::Answered, .answer = Secret { answer } };
}

/// The longest payload either direction carries.
inline constexpr std::size_t MaxAskpassPayload = std::size_t { 64 } * 1024;

/// The bytes of the length prefix every frame starts with.
inline constexpr std::size_t AskpassFramePrefixSize = 4;

/// @param request The request.
/// @return A frame: the payload length (u32, little endian), then the length-prefixed fields.
[[nodiscard]] std::string encode(AskpassRequest const& request);

/// @param reply The reply.
/// @return A frame, laid out like a request's, the outcome byte first. It carries the answer, so it is a
/// Secret.
[[nodiscard]] Secret encode(AskpassReply const& reply);

/// @param buffered The bytes received so far, from the first byte of a frame.
/// @return The frame's total size (prefix included) once the prefix is in; nullopt while it is not;
///         a reason when the frame declares more than MaxAskpassPayload.
[[nodiscard]] std::expected<std::optional<std::size_t>, std::string> askpassFrameSize(
    std::string_view buffered);

/// @param frame Exactly one whole frame.
/// @return The request, or why the frame is not one.
[[nodiscard]] std::expected<AskpassRequest, std::string> decodeAskpassRequest(std::string_view frame);

/// @param frame Exactly one whole frame.
/// @return The reply, or why the frame is not one.
[[nodiscard]] std::expected<AskpassReply, std::string> decodeAskpassReply(std::string_view frame);

} // namespace contour::askpass
