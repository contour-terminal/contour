// SPDX-License-Identifier: Apache-2.0
#include <vthost/tmux/CommandLine.hpp>

namespace vthost::tmux
{

std::vector<std::string> splitCommandLine(std::string_view line)
{
    auto arguments = std::vector<std::string> {};
    auto current = std::string {};
    auto inWord = false;
    auto quote = '\0';

    // Consumes the line from the front, so a character that takes the next one with it just takes it.
    auto rest = line;
    while (!rest.empty())
    {
        auto const ch = rest.front();
        rest.remove_prefix(1);
        if (quote != '\0')
        {
            if (ch == quote)
                quote = '\0';
            else if (quote == '"' && ch == '\\' && !rest.empty()
                     && (rest.front() == '"' || rest.front() == '\\'))
            {
                current += rest.front();
                rest.remove_prefix(1);
            }
            else
                current += ch;
            continue;
        }
        if (ch == '\\' && !rest.empty())
        {
            // Outside quotes a backslash makes the next character literal, which is how tmux spells a
            // single quote inside a single-quoted word: 'it'\''s'.
            current += rest.front();
            rest.remove_prefix(1);
            inWord = true;
            continue;
        }
        if (ch == '\'' || ch == '"')
        {
            quote = ch;
            inWord = true;
            continue;
        }
        if (ch == ' ' || ch == '\t')
        {
            if (inWord)
            {
                arguments.push_back(std::move(current));
                current.clear();
                inWord = false;
            }
            continue;
        }
        current += ch;
        inWord = true;
    }
    if (inWord)
        arguments.push_back(std::move(current));
    return arguments;
}

std::string quoteArgument(std::string_view argument)
{
    auto quoted = std::string { "'" };
    for (auto const character: argument)
    {
        if (character == '\'')
            quoted += "'\\''";
        else
            quoted += character;
    }
    quoted += '\'';
    return quoted;
}

} // namespace vthost::tmux
