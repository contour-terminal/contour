// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/InputEvent.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace core::tui
{

/// @brief Feed-based incremental state machine for parsing VT input sequences.
///
/// Accepts raw bytes from stdin via feed() and produces InputEvent objects.
/// Handles CSI sequences, CSIu (Kitty keyboard protocol), SS3 sequences,
/// SGR mouse reporting, bracketed paste, UTF-8 multi-byte sequences,
/// and bare ESC disambiguation via timeout().
class VtParser
{
  public:
    /// @brief The largest bracketed paste delivered whole.
    ///
    /// The bytes come from stdin, which is untrusted: a `ESC[200~` whose `ESC[201~` never
    /// arrives would otherwise grow this parser for as long as bytes keep coming. Past the cap
    /// the parser emits what it has and returns to Ground, so the remainder of such a paste is
    /// read as ordinary input -- which is what happens anyway when the terminator is lost. No
    /// interactive paste reaches 4 MiB; a file that large is not typed into a prompt.
    ///
    /// A paste of exactly this many bytes still ends at its own `ESC[201~`: the buffer holds the
    /// terminator too while it arrives, and that room is reserved ON TOP of the cap. Without the
    /// reservation the last bytes a paste can carry cleanly would be the cap minus the length of
    /// a terminator that is not part of the paste at all.
    static constexpr std::size_t MaxPasteLength = std::size_t { 4 } * 1024 * 1024;

    /// @brief The largest CSI parameter string assembled before the sequence is abandoned.
    ///
    /// DEC's own limit is 16 parameters, and the longest sequence core::tui answers
    /// (a DECRQM reply, a Kitty key) is a few dozen bytes. 256 leaves room for anything
    /// well-formed, and a longer one is malformed with nothing worth emitting. This cap needs no
    /// room reserved above it: a CSI ends at its final byte, which is dispatched rather than
    /// collected, so a parameter string of exactly this length still dispatches.
    static constexpr std::size_t MaxCsiParamLength = 256;

    /// @brief The largest DCS payload delivered whole.
    ///
    /// A DCS here carries a terminal's answer -- XTGETTCAP, DECRQSS -- which is small. 64 KiB is
    /// far above any of them, and a payload past it has lost its ST. As with the paste cap, the
    /// room the ST (`ESC \`) needs while it arrives is reserved above this, so a payload of
    /// exactly this length still ends at its own terminator.
    static constexpr std::size_t MaxDcsLength = std::size_t { 64 } * 1024;

    /// @brief The largest OSC payload delivered whole.
    ///
    /// An OSC reply a program waits for -- an OSC 5522 status, a colour report -- is a few dozen
    /// bytes. The same reasoning and the same reserved room for the ST apply as for DCS.
    static constexpr std::size_t MaxOscLength = std::size_t { 64 } * 1024;

    /// @brief What `ESC ]` introduces.
    enum class OscRecognition : std::uint8_t
    {
        AltKey,   ///< Alt+] followed by ordinary input: what a terminal sends when the user types it.
        Response, ///< An OSC reply, delivered as @c OscResponse once its ST or BEL arrives.
    };

    /// @brief How a parser reads ambiguous input, fixed at construction.
    struct Options
    {
        /// Response only where a program has asked the terminal something it answers with an
        /// OSC. An interactive prompt keeps AltKey: there, reading `ESC ]` as an OSC would
        /// swallow everything typed after Alt+] until a BEL.
        OscRecognition osc = OscRecognition::AltKey;
    };

    /// @brief Constructs a parser with the default options.
    VtParser() = default;

    /// @brief Constructs a parser with @p options.
    /// @param options How to read ambiguous input.
    explicit VtParser(Options options) noexcept: _options(options) {}

    /// @brief Feeds raw bytes and produces zero or more parsed events.
    /// @param data Raw bytes from stdin.
    /// @return Vector of parsed input events.
    [[nodiscard]] auto feed(std::string_view data) -> std::vector<InputEvent>;

    /// @brief Call after a timeout (e.g. 50ms) with no new data.
    ///
    /// Resolves ambiguous sequences like a bare ESC that could be the start
    /// of a longer escape sequence.
    /// @return Vector of resolved events (typically 0 or 1).
    [[nodiscard]] auto timeout() -> std::vector<InputEvent>;

  private:
    /// @brief Parser state machine states.
    enum class State : std::uint8_t
    {
        Ground,       ///< Default state, expecting new input.
        Escape,       ///< Received ESC, waiting for sequence type.
        CsiEntry,     ///< Received ESC[, waiting for params or final byte.
        CsiParam,     ///< Parsing CSI parameter digits and semicolons.
        Ss3,          ///< Received ESC O, waiting for final byte.
        PasteBody,    ///< Inside bracketed paste (ESC[200~), collecting text.
        Utf8Sequence, ///< Collecting UTF-8 continuation bytes.
        DcsEntry,     ///< Received ESC P, collecting parameter/intermediate bytes.
        DcsBody,      ///< Inside DCS body, collecting data until ST (ESC \).
        OscBody,      ///< Inside an OSC reply, collecting data until ST (ESC \) or BEL.
    };

    Options _options;

    State _state = State::Ground;
    std::string _paramBuf;  ///< Buffer for CSI parameter bytes.
    std::string _utf8Buf;   ///< Buffer for UTF-8 multi-byte sequence.
    std::string _pasteBuf;  ///< Buffer for bracketed paste content.
    std::string _dcsBuf;    ///< Buffer for DCS (Device Control String) payload.
    std::string _oscBuf;    ///< Buffer for an OSC reply's payload.
    int _utf8Remaining = 0; ///< Expected remaining UTF-8 continuation bytes.

    /// A Win32-input-mode key's high surrogate, waiting for the key that carries its low half, or 0.
    /// The console reports a character outside the BMP as two keys, one per UTF-16 unit, and they
    /// may arrive in two reads (core-cpp#20).
    char16_t _pendingHighSurrogate = 0;

    /// @brief Abandons a sequence whose buffer has grown past its cap.
    ///
    /// Clears @p buffer, gives its storage back and returns the parser to Ground, so a sequence
    /// whose terminator never arrives costs a bounded amount of memory rather than an unbounded
    /// one. The bytes collected so far are dropped: a sequence this long is malformed.
    ///
    /// @param buffer The buffer to test and, when it is over the cap, clear.
    /// @param cap The largest size @p buffer may reach.
    /// @return true when the parser was reset, so the caller must stop processing this byte.
    [[nodiscard]] auto abandonIfOverlong(std::string& buffer, std::size_t cap) -> bool;

    /// @brief Processes a single byte in the Ground state.
    void processGround(std::uint8_t byte, std::vector<InputEvent>& events);

    /// @brief Processes a single byte in the Escape state.
    void processEscape(std::uint8_t byte, std::vector<InputEvent>& events);

    /// @brief Processes a single byte in the CsiEntry/CsiParam state.
    void processCsi(std::uint8_t byte, std::vector<InputEvent>& events);

    /// @brief Processes a single byte in the Ss3 state.
    void processSs3(std::uint8_t byte, std::vector<InputEvent>& events);

    /// @brief Processes a single byte in the PasteBody state.
    void processPaste(std::uint8_t byte, std::vector<InputEvent>& events);

    /// @brief Processes a single byte in the Utf8Sequence state.
    void processUtf8(std::uint8_t byte, std::vector<InputEvent>& events);

    /// @brief Processes a single byte in the DcsEntry state.
    void processDcsEntry(std::uint8_t byte, std::vector<InputEvent>& events);

    /// @brief Processes a single byte in the DcsBody state.
    void processDcsBody(std::uint8_t byte, std::vector<InputEvent>& events);

    /// @brief Processes a single byte in the OscBody state.
    void processOscBody(std::uint8_t byte, std::vector<InputEvent>& events);

    /// @brief Parses a complete CSI sequence from _paramBuf and the final byte.
    void dispatchCsi(char finalByte, std::vector<InputEvent>& events);

    /// @brief Emits a KeyEvent for a single Unicode codepoint.
    static void emitCodepoint(char32_t cp, std::vector<InputEvent>& events);

    /// @brief Decodes a complete UTF-8 sequence to a codepoint and emits a KeyEvent.
    void emitUtf8(std::vector<InputEvent>& events);
};

} // namespace core::tui
