// This file is part of the "x0" project, http://github.com/christianparpart/x0>
//   (c) 2009-2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#pragma once

#include <cstdint>
#include <sstream>
#include <string>

namespace regex_dfa::util::literals
{

/**
 * Strips a multiline string's indentation prefix.
 *
 * Example:
 * \code
 * string s = R"(|line one
 *               |line two
 *               |line three
 *               )"_multiline;
 * fmt::print(s);
 * \endcode
 *
 * This prints three lines: @c "line one\nline two\nline three\n"
 */
inline std::string operator""_multiline(const char* text, size_t /*size*/)
{
    if (!*text)
        return {};

    enum class State
    {
        LineData,
        SkipUntilPrefix,
    };

    constexpr char LF = '\n';
    State state = State::LineData;
    std::stringstream sstr;
    char sep = *text++;

    while (*text)
    {
        switch (state)
        {
            case State::LineData:
                if (*text == LF)
                {
                    state = State::SkipUntilPrefix;
                    sstr << *text++;
                }
                else
                    sstr << *text++;
                break;
            case State::SkipUntilPrefix:
                if (*text == sep)
                {
                    state = State::LineData;
                    text++;
                }
                else
                    text++;
                break;
        }
    }

    return sstr.str();
}

} // namespace regex_dfa::util::literals
