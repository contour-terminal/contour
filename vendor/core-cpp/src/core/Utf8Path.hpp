// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A path from the UTF-8 text core-cpp's strings are spelled in, and that text back from a path.

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace core
{

/// @brief The path that UTF-8 @p text spells, where the text came from outside the program.
///
/// The way a narrow string from outside becomes a path in core-cpp: a command-line argument, an
/// environment value, a configuration value, a directory a remote shell reported. A `char` is
/// UTF-8 here, but `std::filesystem::path`'s narrow constructor reads it in the platform's native
/// narrow encoding, which on Windows is the process's ANSI code page: on a CP-1252 host the two
/// bytes of the `ü` in `C:\Users\Jürgen` arrive as two characters, naming a directory that does
/// not exist. This reads @p text as UTF-8 whatever the process code page is.
///
/// On POSIX a path is the bytes it is given, so they pass through unchanged and this never fails.
/// On Windows a path is UTF-16, so the text is converted, strictly: a stray continuation byte, a
/// truncated sequence, an overlong form, an encoded surrogate or a code point past U+10FFFF names
/// no file there, and is an error rather than a U+FFFD that would name another one. Such text is
/// an input to handle, not a defect, so it is a value and never an exception. Text the program
/// spelled itself goes through pathFromTrustedUtf8() instead.
///
/// @param text The path, in UTF-8.
/// @return The path @p text spells; @c std::errc::illegal_byte_sequence on Windows where @p text
///         is not UTF-8.
[[nodiscard]] std::expected<std::filesystem::path, std::error_code> pathFromUtf8(std::string_view text);

/// @brief The path that UTF-8 @p text spells, where the program spelled the text itself.
///
/// For text that is UTF-8 by construction: a literal, a name a test chose, a string built from a
/// path through pathToUtf8() or `normalizePath()`. Text that is not UTF-8 is then a defect at the
/// call site rather than an input, so it throws instead of handing every such caller a result it
/// could only unwrap. Anything from outside the program -- whatever it is expected to hold -- goes
/// through pathFromUtf8().
///
/// @param text The path, in UTF-8.
/// @return The path @p text spells.
/// @throws std::system_error with @c std::errc::illegal_byte_sequence on Windows if @p text is not
///         UTF-8. Never on POSIX, where a path is the bytes it is given.
[[nodiscard]] inline std::filesystem::path pathFromTrustedUtf8(std::string_view text)
{
    auto path = pathFromUtf8(text);
    if (!path)
        throw std::system_error(path.error(), "pathFromTrustedUtf8");
    return *std::move(path);
}

/// @brief @p path in UTF-8, in its native format: the inverse of pathFromUtf8().
///
/// For a message, a log line or anything else that carries a path as text. Never
/// `path::string()`, which on Windows narrows through the ANSI code page: a name the code page can
/// spell comes out in that code page rather than in UTF-8, and one it cannot spell throws, so an
/// error that names the file throws instead of being reported (core-cpp#26).
///
/// It never throws for what the path holds. On POSIX a path is bytes, which pass through unchanged
/// whether or not they are UTF-8. On Windows a path is UTF-16 code units rather than code points,
/// and NTFS takes a name with a lone surrogate, which UTF-8 cannot spell: it becomes U+FFFD and
/// the rest of the name is still spelled, where `path::u8string()` throws `std::system_error`. That
/// is the one name this does not round-trip: pathFromUtf8() reads the U+FFFD back as itself, which
/// names another file.
///
/// @param path The path.
/// @return @p path in UTF-8, with the platform's own separators.
[[nodiscard]] std::string pathToUtf8(std::filesystem::path const& path);

} // namespace core
