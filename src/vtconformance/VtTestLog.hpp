// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vtconformance
{

/// The record kinds vttest writes to its `-l <logfile>` transcript.
///
/// vttest defines these tags in its own `vttest.h`; they are the only stable, machine-readable
/// window into what it sent and what the terminal answered.
enum class VtTestRecordKind : std::uint8_t
{
    Note, ///< vttest's own commentary, e.g. "Derived terminal-id: 100".
    Send, ///< Control sequence bytes vttest wrote to the terminal.
    Data, ///< Test payload bytes vttest wrote to the terminal.
    Text, ///< Human-readable test instructions and verdicts vttest printed.
    Read, ///< Bytes vttest read back — either a menu keystroke, or the TERMINAL'S REPLY.
    Wait, ///< vttest suspended replay and is now waiting for a terminal reply, by id.
    Done, ///< The wait with this id finished.
    Skip, ///< Replay debugging marker.
};

/// One line of a vttest transcript.
struct VtTestRecord
{
    VtTestRecordKind kind;

    /// For Note/Text: the text verbatim.
    /// For Send/Data/Read: the DECODED raw bytes (`<27>` escapes resolved back to 0x1B, etc).
    /// For Wait/Done/Skip: the id, rendered as text.
    std::string payload;

    /// 1-based line number in the transcript, for diagnostics.
    size_t lineNumber = 0;
};

/// A query vttest issued and the terminal's answer to it.
///
/// vttest brackets every reply-read with `Wait:`/`Done:` (it pauses command-file replay so the
/// answer always comes from the live terminal). That bracketing is what lets us pair them up.
struct Query
{
    /// The bytes vttest sent, e.g. `ESC [ 0 c`.
    std::string request;

    /// The bytes the terminal answered with. **Empty means the terminal never answered** — which is
    /// an unambiguous conformance failure needing no golden file.
    std::string reply;

    size_t lineNumber = 0;

    [[nodiscard]] bool answered() const noexcept { return !reply.empty(); }
};

/// Decodes one vttest byte-encoded field (`<27> [ 0 c`) back into raw bytes.
///
/// Tokens are space-separated; a token of the form `<NNN>` is a decimal byte value, any other
/// single-character token is that literal byte.
///
/// @param encoded The encoded field, i.e. everything after the record tag.
/// @return The decoded raw bytes.
[[nodiscard]] std::string decodeBytes(std::string_view encoded);

/// Parses a complete vttest `-l` transcript.
///
/// @param transcript The full file contents.
/// @return One record per recognised line; unrecognised lines are skipped.
[[nodiscard]] std::vector<VtTestRecord> parseVtTestLog(std::string_view transcript);

/// Pairs every `Send:` immediately preceding a `Wait:` with the `Read:` that answers it.
///
/// @param records A parsed transcript.
/// @return The query/reply pairs, in transcript order.
[[nodiscard]] std::vector<Query> extractQueries(std::vector<VtTestRecord> const& records);

/// A verdict vttest itself rendered about the terminal, scraped from its `Text:` output.
struct Verdict
{
    /// The full text line vttest printed.
    std::string text;

    /// Whether the line reports success. vttest has no exit code, so its printed verdicts are the
    /// only self-check signal it exposes.
    bool passed = false;

    size_t lineNumber = 0;
};

/// Scrapes vttest's self-check verdicts out of a parsed transcript.
///
/// Only lines matching one of vttest's known verdict phrasings are returned; ordinary instruction
/// text is ignored.
///
/// vttest records a verdict in one of two places, and both count. `show_result()` writes
/// `Note: result <verdict>` (main.c:2104-2109); a test that prints its verdict inline instead leaves
/// it in the `Text:` prose. Reading only `Text:` finds the minority -- 2 of chapter 06's 22, and none
/// of chapter 07's or 11.1's -- and a self-checking scenario that reads a tenth of its own verdicts
/// is not judged by them.
[[nodiscard]] std::vector<Verdict> extractVerdicts(std::vector<VtTestRecord> const& records);

/// Renders raw bytes the way this project's logs do: printable ASCII verbatim, everything else as a
/// mnemonic (`ESC`, `CSI`, ...) or `<0xNN>`. Used to make reports readable.
[[nodiscard]] std::string prettyBytes(std::string_view bytes);

} // namespace vtconformance
