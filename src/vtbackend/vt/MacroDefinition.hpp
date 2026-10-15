// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtparser/ParserExtension.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace vtbackend
{

/// How many macros there are room for: DECDMAC's Pid is "a number between 0 and 63" (DEC STD 070).
constexpr int MacroCount = 64;

/// DECDMAC's Pdt: which macro definitions are deleted before the new one is stored.
enum class MacroDeletion : uint8_t
{
    SameId, ///< 0, or omitted: only the macro with the id being defined.
    All,    ///< 1: every macro.
};

/// DECDMAC's Pen: how the body is spelled in the control string.
enum class MacroEncoding : uint8_t
{
    Text,     ///< 0, or omitted: as the characters themselves.
    HexPairs, ///< 1: as two hex digits per character.
};

/// Why a DECDMAC defines nothing.
enum class MacroDefinitionError : uint8_t
{
    InvalidId,       ///< Pid is not between 0 and 63.
    InvalidDeletion, ///< Pdt is neither 0 nor 1.
    InvalidEncoding, ///< Pen is neither 0 nor 1.
    TooLarge,        ///< The body outgrew the room there was for it.
    NotHex,          ///< A hex body holds what is neither a hex pair nor a repeat sequence.
};

/// DECDMAC's parameters: Pid ; Pdt ; Pen.
struct MacroDefinitionParameters
{
    int id = 0;                                     ///< Pid, the macro being defined.
    MacroDeletion deletion = MacroDeletion::SameId; ///< Pdt.
    MacroEncoding encoding = MacroEncoding::Text;   ///< Pen.

    bool operator==(MacroDefinitionParameters const&) const = default;
};

/// Reads DECDMAC's parameters, an omitted one being zero. DEC STD 070: "Pid must be a number between 0 and
/// 63; Pdt 0 or 1; and Pen 0 or 1; otherwise the control will be ignored."
/// @param id Pid.
/// @param deletion Pdt.
/// @param encoding Pen.
/// @return The parameters, or which of them is out of range.
[[nodiscard]] std::expected<MacroDefinitionParameters, MacroDefinitionError> decodeMacroDefinitionParameters(
    unsigned id, unsigned deletion, unsigned encoding) noexcept;

/// Collects a DECDMAC body, decoding it as it arrives and holding no more of it than the room there is.
///
/// A body that outgrows the room, or a hex body holding what is not hex, is refused rather than cut: what was
/// held of it is dropped, the rest of the control string is consumed without being stored, and the body is
/// handed on as the MacroDefinitionError that refused it.
class MacroBodyCollector final: public ParserExtension
{
  public:
    /// Receives the decoded body once the control string ends.
    using Done = std::function<void(std::expected<std::string, MacroDefinitionError>)>;

    /// @param encoding How the body is spelled.
    /// @param room The most bytes the decoded body may take.
    /// @param done Receives the body.
    MacroBodyCollector(MacroEncoding encoding, size_t room, Done done);

    using ParserExtension::pass;
    void pass(char ch) override;
    void finalize() override;

    /// How many bytes of the decoded body it holds: never more than its room.
    ///
    /// A test seam, with no caller in production: what it shows -- that a refused body is dropped rather
    /// than held while the rest of the control string streams past -- is a bound on memory, which nothing
    /// else the collector does makes observable.
    [[nodiscard]] size_t heldBytes() const noexcept { return _body ? _body->size() : 0; }

  private:
    /// What the next character of a hex-encoded body is read as.
    enum class HexState : uint8_t
    {
        FirstDigit,  ///< The first digit of a pair -- or `!` opening a repeat, or `;` closing one.
        SecondDigit, ///< The second digit of a pair.
        RepeatCount, ///< A digit of a repeat's count, or the `;` that ends it.
    };

    /// A repeat sequence whose bytes are being decoded, to be repeated once its closing `;` comes.
    struct PendingRepeat
    {
        size_t count = 0; ///< How many times the sequence is to be in the body; 0 is as once.
        size_t start = 0; ///< Where in the body the sequence begins.
    };

    /// Decodes one character of a hex-encoded body.
    void passHex(char ch);

    /// Adds the copies a pending repeat asks for beyond the one already in the body.
    void applyPendingRepeat();

    /// Adds one decoded byte, or refuses the body once it would outgrow the room.
    void append(char byte);

    /// Refuses the body: what was held of it is dropped, and what follows only consumed.
    void refuse(MacroDefinitionError error);

    MacroEncoding _encoding;
    size_t _room;
    Done _done;

    /// The body decoded so far, or why it was refused.
    std::expected<std::string, MacroDefinitionError> _body = std::string {};

    HexState _hexState = HexState::FirstDigit;
    uint8_t _highNibble = 0; ///< The first digit of the pair in SecondDigit.
    size_t _repeatCount = 0; ///< The count read so far in RepeatCount.
    std::optional<PendingRepeat> _pendingRepeat;
};

} // namespace vtbackend
