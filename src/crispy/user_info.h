// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>

namespace crispy
{

/// The fields we care about from the calling user's entry in the password database.
struct password_entry
{
    std::string name;          ///< Login name, e.g. "jane".
    std::string homeDirectory; ///< Home directory, e.g. "/home/jane".
    std::string shell;         ///< Login shell, e.g. "/bin/zsh".
};

/// Reads the calling user's password-database entry.
///
/// Uses the reentrant `getpwuid_r()` rather than `getpwuid()`, whose result lives in a shared
/// static buffer that concurrent callers would race on.
///
/// @return The entry, or std::nullopt if the user has none or it could not be read.
[[nodiscard]] std::optional<password_entry> currentUserPasswordEntry();

/// Convenience wrapper around currentUserPasswordEntry().
///
/// @return The calling user's home directory, or an empty string if it is unknown.
[[nodiscard]] std::string userHomeDirectory();

} // namespace crispy
