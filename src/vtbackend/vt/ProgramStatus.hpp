// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace vtbackend
{

/// What a program says it is doing (OSC 7501 `state`, program-status rev 0.4).
enum class ProgramState : uint8_t
{
    Idle = 0, ///< At rest, awaiting input; also after the user interrupted or cancelled.
    Working,  ///< Running, optionally with a progress.
    Done,     ///< Finished, with results the user has not seen yet.
    Blocked,  ///< Waiting on the user, for the reason the kind names.
    Error,    ///< Failed and stopped.
};

/// @param state A program state.
/// @return Whether the spec gives @p state a progress.
[[nodiscard]] constexpr bool takesProgress(ProgramState state) noexcept
{
    return state == ProgramState::Working || state == ProgramState::Blocked;
}

/// Why a program is blocked (OSC 7501 `kind`); meaningful only with ProgramState::Blocked.
enum class BlockedKind : uint8_t
{
    None = 0,   ///< Not blocked, or blocked for a reason the program did not name.
    Permission, ///< Waiting for the user to allow something.
    Question,   ///< Waiting for the user to answer a question.
    Auth,       ///< Waiting for a password or another credential.
};

/// One enumerator and its spelling in the sequence.
/// @tparam E The enum the row belongs to.
template <typename E>
struct ProgramStatusName
{
    E value;               ///< The enumerator.
    std::string_view name; ///< Its spelling in OSC 7501.
};

/// The states OSC 7501 knows, in enumerator order. The parser and the detection reply both read it.
inline constexpr auto ProgramStateNames = std::array {
    ProgramStatusName { ProgramState::Idle, "idle" },
    ProgramStatusName { ProgramState::Working, "working" },
    ProgramStatusName { ProgramState::Done, "done" },
    ProgramStatusName { ProgramState::Blocked, "blocked" },
    ProgramStatusName { ProgramState::Error, "error" },
};

/// The blocked kinds OSC 7501 knows, in enumerator order; BlockedKind::None has no spelling.
inline constexpr auto BlockedKindNames = std::array {
    ProgramStatusName { BlockedKind::Permission, "permission" },
    ProgramStatusName { BlockedKind::Question, "question" },
    ProgramStatusName { BlockedKind::Auth, "auth" },
};

/// The longest payload (everything after `7501;`): the spec's 4096-byte sequence less `ESC ] 7501 ;`
/// and `ESC \`.
inline constexpr std::size_t MaxProgramStatusPayloadLength = 4096 - 9;

/// The longest key.
inline constexpr std::size_t MaxProgramStatusKeyLength = 16;

/// The most records a terminal keeps (the spec's minimum; it allows up to 256).
inline constexpr std::size_t MaxProgramStatusRecords = 64;

/// One program's status: the record a report with a state other than `clear` stands for.
///
/// A report replaces its id's record whole, so every key it omits is cleared.
struct ProgramStatusRecord
{
    std::string id;                          ///< The record's path (`build/test`); empty for the root.
    ProgramState state = ProgramState::Idle; ///< What the program is doing.
    BlockedKind kind = BlockedKind::None;    ///< Why it is blocked; None unless `state` is Blocked.
    std::optional<uint8_t> progress;         ///< Percent done (0..100) while Working or Blocked; else absent.
    std::string app;                         ///< Its own program identifier; empty to inherit an ancestor's.
    std::string title;                       ///< A short label, decoded UTF-8.
    std::string message;                     ///< One line describing the status, decoded UTF-8.

    bool operator==(ProgramStatusRecord const&) const = default;
};

/// `state=clear`: removes the record @ref id names and everything beneath it; an empty id removes all.
struct ProgramStatusClear
{
    std::string id; ///< The subtree to remove; empty for every record.

    bool operator==(ProgramStatusClear const&) const = default;
};

/// `?`: the program asks whether the terminal speaks OSC 7501.
struct ProgramStatusQuery
{
    bool operator==(ProgramStatusQuery const&) const = default;
};

/// What one OSC 7501 sequence asks of the terminal.
using ProgramStatusReport = std::variant<ProgramStatusRecord, ProgramStatusClear, ProgramStatusQuery>;

/// Why a whole OSC 7501 report was discarded.
///
/// A malformed PAIR is not here: it is skipped and the rest of the report stands. These are the
/// failures the spec says discard the report whole.
enum class ProgramStatusRejection : uint8_t
{
    TooLong = 0,      ///< The payload exceeds @ref MaxProgramStatusPayloadLength.
    KeyTooLong,       ///< A key exceeds @ref MaxProgramStatusKeyLength.
    NoState,          ///< The report has no `state`.
    UnknownState,     ///< The `state` is none the protocol defines.
    BadId,            ///< The `id` is not `segment ("/" segment)*` within its limits.
    BadBase64,        ///< `msg` or `title` is not base64.
    TextTooLong,      ///< `msg` or `title` exceeds its encoded or decoded limit.
    ControlCharacter, ///< Decoded text holds a C0, DEL or C1 character.
    InvalidUtf8,      ///< Decoded text is not UTF-8.
};

/// Names a rejection for diagnostics.
///
/// A switch rather than a table, so that -Wswitch makes an enumerator without a name a build break.
[[nodiscard]] constexpr std::string_view toString(ProgramStatusRejection rejection) noexcept
{
    switch (rejection)
    {
        case ProgramStatusRejection::TooLong: return "TooLong";
        case ProgramStatusRejection::KeyTooLong: return "KeyTooLong";
        case ProgramStatusRejection::NoState: return "NoState";
        case ProgramStatusRejection::UnknownState: return "UnknownState";
        case ProgramStatusRejection::BadId: return "BadId";
        case ProgramStatusRejection::BadBase64: return "BadBase64";
        case ProgramStatusRejection::TextTooLong: return "TextTooLong";
        case ProgramStatusRejection::ControlCharacter: return "ControlCharacter";
        case ProgramStatusRejection::InvalidUtf8: return "InvalidUtf8";
    }
    return "Unknown";
}

/// Decodes one OSC 7501 payload (program-status rev 0.4).
///
/// The payload is `?` or `key=value` pairs separated by `:`. A pair without `=`, with an empty key, or
/// whose value holds a byte outside `[A-Za-z0-9_.,+/=-]` is skipped; an unknown key is ignored; a
/// repeated key keeps its last value. `kind` counts only with `blocked`, `progress` only with `working`
/// or `blocked` and only for 0..100, and an `app` that is not `[A-Za-z0-9_.+-]{1,32}` is absent.
///
/// @param payload The OSC body after `7501;`.
/// @return The report, or why it is discarded whole.
[[nodiscard]] std::expected<ProgramStatusReport, ProgramStatusRejection> parseProgramStatus(
    std::string_view payload);

/// The answer to a `?` query: the states and kinds this terminal accepts, built from
/// @ref ProgramStateNames and @ref BlockedKindNames.
/// @return `ESC ] 7501 ; ? : states=idle,working,done,blocked,error : kinds=permission,question,auth ESC \`.
[[nodiscard]] std::string_view programStatusDetectionReply() noexcept;

} // namespace vtbackend
