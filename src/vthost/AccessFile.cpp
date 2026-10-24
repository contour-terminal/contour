// SPDX-License-Identifier: Apache-2.0
#include <vthost/AccessFile.hpp>

#include <core/Utf8Path.hpp>

#include <algorithm>
#include <format>
#include <ranges>

#include <vthost/Token.hpp>

namespace vthost
{

std::expected<std::vector<AcceptedToken>, std::string> parseAccessFile(std::string_view text,
                                                                       std::string_view fullToken)
{
    auto entries = std::vector<AcceptedToken> {};
    auto lineNumbers = std::vector<std::size_t> {};
    for (auto const [index, rawLine]: std::views::split(text, '\n') | std::views::enumerate)
    {
        auto const lineNumber = static_cast<std::size_t>(index) + 1;
        // Indentation is not part of the grant list: `  # note` is a comment, `  read tok` an entry.
        auto const cleaned = cleanedToken(std::string { rawLine.begin(), rawLine.end() });
        auto const line = cleaned.substr(std::min(cleaned.find_first_not_of(" \t"), cleaned.size()));
        if (line.empty() || line.starts_with('#'))
            continue;

        auto const space = line.find(' ');
        auto const token = space == std::string::npos ? std::string {} : line.substr(space + 1);
        if (token.empty())
            return std::unexpected(std::format("access file line {}: the token is empty", lineNumber));

        auto const grants = vtcontrol::parseGrants(std::string_view { line }.substr(0, space));
        if (!grants)
            return std::unexpected(
                std::format("access file line {}: {}", lineNumber, grants.error().message));

        if (!fullToken.empty() && token == fullToken)
            return std::unexpected(std::format("access file line {}: repeats --token, which already grants "
                                               "everything; give the line a token "
                                               "of its own",
                                               lineNumber));

        if (auto const earlier = std::ranges::find(entries, token, &AcceptedToken::token);
            earlier != entries.end())
            return std::unexpected(
                std::format("access file line {}: repeats the token of line {}",
                            lineNumber,
                            lineNumbers[static_cast<std::size_t>(earlier - entries.begin())]));

        entries.push_back(AcceptedToken { .token = token, .grants = *grants });
        lineNumbers.push_back(lineNumber);
    }
    return entries;
}

std::expected<std::vector<AcceptedToken>, std::string> readAccessFile(
    core::platform::FileSystem const& fileSystem,
    std::filesystem::path const& path,
    std::string_view fullToken)
{
    auto const text = fileSystem.readFile(path);
    if (!text)
        return std::unexpected(
            std::format("cannot read access file '{}': {}", core::pathToUtf8(path), text.error()));
    return parseAccessFile(*text, fullToken).transform_error([&](std::string const& message) {
        return std::format("{}: {}", core::pathToUtf8(path), message);
    });
}

} // namespace vthost
