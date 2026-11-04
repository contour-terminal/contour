// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The text encodings a shell startup file comes in, and the conversion between them and the UTF-8 the
/// rc-file editor reads and writes in. Pure: bytes in, bytes out, no I/O.
///
/// POSIX shells read their startup files as bytes, which in practice is UTF-8. PowerShell profiles are
/// written by editors and by PowerShell itself: Windows PowerShell 5.1's `>` writes UTF-16LE with a byte
/// order mark, and many Windows editors save UTF-8 with one. A line appended to such a file must be
/// spelled in the file's own encoding, so the encoding is read once, kept, and used again for the write.

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace contour::platform
{

/// How a startup file's text is stored.
enum class RcEncoding : uint8_t
{
    Utf8 = 0,   ///< UTF-8 with no byte order mark, or bytes in no particular encoding: kept as they are.
    Utf8Bom,    ///< UTF-8 after the byte order mark EF BB BF.
    Utf16LeBom, ///< UTF-16, little endian, after the byte order mark FF FE.
};

/// Why a startup file's text could not be read.
enum class RcTextError : uint8_t
{
    /// A byte order mark of an encoding no shell profile is written in (UTF-16BE, UTF-32).
    UnsupportedEncoding = 0,
    /// UTF-16LE that is not: an odd number of bytes, or an unpaired surrogate.
    Malformed,
};

/// A startup file's text, in UTF-8, and the encoding it was stored in.
struct RcText
{
    RcEncoding encoding = RcEncoding::Utf8; ///< How the file stored the text.
    std::string utf8 {};                    ///< The text, without any byte order mark.
};

/// Reads a startup file's bytes as text.
///
/// The byte order mark decides: EF BB BF is UTF-8, FF FE is UTF-16LE (unless FF FE 00 00, which is
/// UTF-32LE). Without one the bytes are taken as UTF-8 as they are, unvalidated, as a POSIX shell reads
/// them.
///
/// @param bytes The file's contents.
/// @return The text in UTF-8 and its encoding; UnsupportedEncoding for a UTF-16BE or UTF-32 mark;
///         Malformed for UTF-16LE of odd length or with an unpaired surrogate.
[[nodiscard]] std::expected<RcText, RcTextError> decodeRcText(std::string_view bytes);

/// Spells UTF-8 text in @p encoding, for appending to a file stored in it.
///
/// @pre @p utf8 is valid UTF-8: text Contour composes, or decodeRcText() produced from UTF-16.
/// @param utf8 The text.
/// @param encoding The encoding to spell it in.
/// @return The bytes, without a byte order mark, which belongs only at the start of a file.
[[nodiscard]] std::string encodeRcText(std::string_view utf8, RcEncoding encoding);

/// The byte order mark a file in @p encoding starts with.
///
/// @param encoding The encoding.
/// @return The mark; empty for RcEncoding::Utf8.
[[nodiscard]] constexpr std::string_view byteOrderMark(RcEncoding encoding) noexcept
{
    switch (encoding)
    {
        case RcEncoding::Utf8: return {};
        case RcEncoding::Utf8Bom: return "\xEF\xBB\xBF";
        case RcEncoding::Utf16LeBom: return "\xFF\xFE";
    }
    return {};
}

} // namespace contour::platform
