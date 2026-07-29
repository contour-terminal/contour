// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Resolution of the preshared token that authenticates a TCP client.
///
/// The token is a secret, and `--token SECRET` puts it in the process's argv — readable through
/// `/proc/<pid>/cmdline` and `ps` by anyone sharing the machine. `--token-file` exists so the
/// secret can live somewhere with permissions on it instead. Both spellings resolve here, so the
/// daemon and the client cannot disagree about precedence or about what counts as "no token".

#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace vthost
{

/// Reads a preshared token from a file.
///
/// The file is read in BINARY and stripped of carriage returns, because a token file authored on
/// Windows (or fetched over a protocol that translates line endings) otherwise carries a `\r` that
/// silently becomes part of the secret and fails every comparison. Trailing whitespace goes the
/// same way: `echo secret > token` appends a newline, and nobody expects that newline to matter.
///
/// Interior whitespace is preserved — a passphrase may legitimately contain spaces.
///
/// @param path The file to read the token from.
/// @return The token, or a human-readable reason it could not be read.
[[nodiscard]] inline std::expected<std::string, std::string> readTokenFile(std::filesystem::path const& path)
{
    auto file = std::ifstream { path, std::ios::in | std::ios::binary };
    if (!file.is_open())
        return std::unexpected(std::string { "cannot open token file '" } + path.string() + "'");

    auto token = std::string { std::istreambuf_iterator<char> { file }, {} };
    std::erase(token, '\r');
    while (!token.empty() && (token.back() == '\n' || token.back() == ' ' || token.back() == '\t'))
        token.pop_back();

    if (token.empty())
        return std::unexpected(std::string { "token file '" } + path.string() + "' is empty");

    return token;
}

/// Resolves the effective token from the two mutually exclusive CLI spellings.
///
/// Passing both is rejected rather than silently preferring one: a caller who supplies two
/// different secrets has a bug, and picking a winner would hide it until an authentication
/// failure much later.
///
/// @param token The `--token` value, or empty.
/// @param tokenFile The `--token-file` value, or empty.
/// @return The token (empty when neither option was given), or a human-readable error.
[[nodiscard]] inline std::expected<std::string, std::string> resolveToken(std::string_view token,
                                                                          std::string_view tokenFile)
{
    if (!token.empty() && !tokenFile.empty())
        return std::unexpected(std::string { "pass either --token or --token-file, not both" });

    if (!tokenFile.empty())
        return readTokenFile(std::filesystem::path { tokenFile });

    return std::string { token };
}

} // namespace vthost
