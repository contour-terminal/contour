// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>

namespace vtpty
{

/// An errno value, and what a diagnostic says for it.
struct ErrnoName
{
    int code;              ///< The errno value.
    std::string_view text; ///< What it means, as strerror() says it in English.
};

/// What a child between fork() and exec() says for the errno values chdir() and exec report. A table rather
/// than strerror(), strerror_r() or std::generic_category(): none of those is async-signal-safe -- glibc's
/// strerror_r() translates through dcgettext(), which takes the locale lock and may load a catalogue -- and
/// the child of a multi-threaded process may call only async-signal-safe functions until it execs (fork(2)
/// NOTES; POSIX XSH fork()).
inline constexpr auto ErrnoNames = std::array {
    ErrnoName { .code = ENOENT, .text = "No such file or directory" },
    ErrnoName { .code = EACCES, .text = "Permission denied" },
    ErrnoName { .code = ENOEXEC, .text = "Exec format error" },
    ErrnoName { .code = ENOTDIR, .text = "Not a directory" },
    ErrnoName { .code = ELOOP, .text = "Too many levels of symbolic links" },
    ErrnoName { .code = ENAMETOOLONG, .text = "File name too long" },
    ErrnoName { .code = E2BIG, .text = "Argument list too long" },
    ErrnoName { .code = ETXTBSY, .text = "Text file busy" },
    ErrnoName { .code = ENOMEM, .text = "Cannot allocate memory" },
};

/// Describes an errno value without allocating, locking or asking the locale: async-signal-safe.
/// @param errorCode The errno value.
/// @param buffer Where the text of a value the table does not name is written; must outlive the result.
/// @return The table's text for @p errorCode, else "error <errorCode>" in @p buffer (just "error" where
///         @p buffer is too small for the number).
[[nodiscard]] constexpr std::string_view errnoText(int errorCode, std::span<char> buffer) noexcept
{
    if (auto const named = std::ranges::find(ErrnoNames, errorCode, &ErrnoName::code);
        named != ErrnoNames.end())
        return named->text;

    constexpr auto Prefix = std::string_view { "error " };
    if (buffer.size() <= Prefix.size())
        return "error";
    std::ranges::copy(Prefix, buffer.begin());
    auto const digits = buffer.subspan(Prefix.size());
    auto const [end, failure] = std::to_chars(digits.data(), std::to_address(digits.end()), errorCode);
    if (failure != std::errc {})
        return "error";
    return std::string_view { buffer.data(), end };
}

} // namespace vtpty
