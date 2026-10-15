// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Text between the UTF-8 core-cpp's strings are spelled in and the UTF-16 the wide Win32 API
/// takes and answers. Windows only: a `wchar_t` is UTF-16 there and UTF-32 elsewhere, so on any
/// other platform this header declares nothing. Its definitions are in base's SOURCES_WINDOWS.
///
/// Every wide Win32 call in core-cpp converts through these two, so that no name or value goes
/// through the ANSI code page, which cannot spell most of Unicode (core-cpp#7), and a consumer
/// making wide calls of its own converts the same way rather than keeping a copy. The system's
/// message for an error is read that way too.

#ifdef _WIN32

    #include <expected>
    #include <string>
    #include <string_view>
    #include <system_error>

namespace core
{

/// @brief Converts UTF-8, as core-cpp's strings are spelled, to the UTF-16 a wide Win32 API takes.
///
/// Strict, unlike toUtf8(): text that is not UTF-8 -- a stray continuation byte, a truncated
/// sequence, an overlong form, an encoded surrogate, a code point past U+10FFFF -- fails the whole
/// conversion, because read as U+FFFD it would name another file, variable or value than the one
/// asked for. All of @p text is converted, an embedded NUL included.
///
/// @pre @p text is shorter than `INT_MAX` bytes, the most one Win32 conversion takes; every text a
///      wide Win32 API reads is.
/// @param text UTF-8 text.
/// @return @p text in UTF-16, empty when @p text is; @c std::errc::illegal_byte_sequence where it
///         is not UTF-8.
[[nodiscard]] std::expected<std::wstring, std::error_code> toUtf16(std::string_view text);

/// @brief Converts UTF-16, as a wide Win32 API answers, to the UTF-8 core-cpp's strings are spelled in.
///
/// Lenient, unlike toUtf16(), because what it reads comes from the system rather than from the
/// program: a name or value there is UTF-16 code units, not code points, and may hold a lone
/// surrogate, which UTF-8 cannot spell. That becomes U+FFFD, and the rest of @p text is still
/// converted. A surrogate pair becomes one four-byte sequence. All of @p text is converted, an
/// embedded NUL included.
///
/// @pre @p text is shorter than `INT_MAX` code units, the most one Win32 conversion takes; every
///      text a wide Win32 API answers is.
/// @param text UTF-16 text.
/// @return @p text in UTF-8; empty when @p text is.
[[nodiscard]] std::string toUtf8(std::wstring_view text);

/// @brief The system's message for a Win32 error, in UTF-8.
///
/// What `FormatMessageW()` answers for @p errorCode, converted by toUtf8(), in the language Windows
/// picks for the calling thread's UI -- the user's, unless the thread chose another -- or, where
/// the system has no message in it, in the user's, the system's or US English. The narrow
/// `FormatMessageA()` spells a message in the ANSI code page, which is not UTF-8 beyond ASCII: the
/// German one for `ERROR_INVALID_HANDLE` holds "ungültig", whose "ü" is a byte UTF-8 has no reading
/// for. `std::system_category().message()` in MSVC's library is that narrow call, asking for US
/// English first. The line break the system ends a message with is removed, so the message can
/// stand inside a line of its own.
///
/// @param errorCode The error, as `GetLastError()` reports it: a `DWORD`, spelled without
///        `<windows.h>`.
/// @return The message; `unknown error <errorCode>`, in decimal, where the system has none.
[[nodiscard]] std::string systemErrorMessage(unsigned long errorCode);

} // namespace core

#endif
