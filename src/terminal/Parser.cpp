/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <terminal/ControlCode.h>
#include <terminal/Parser.h>
#include <terminal/ParserTables.h>

#include <crispy/escape.h>
#include <crispy/overloaded.h>

#include <unicode/utf8.h>

#include <array>
#include <cctype>
#include <cstdio>
#include <iostream>

#include <fmt/format.h>

namespace terminal {

using namespace std;

using Range = ParserTable::Range;

constexpr bool includes(Range const& range, char32_t value) noexcept
{
    return range.first <= value && value <= range.last;
}

constexpr bool isExecuteChar(char32_t value) noexcept
{
    return includes(Range{0x00, 0x17}, value) || value == 0x19 || includes(Range{0x1C, 0x1F}, value);
}

constexpr bool isParamChar(char32_t value) noexcept
{
    return includes(Range{0x30, 0x3B}, value);
}

constexpr bool isC1(char32_t value) noexcept
{
    return includes(Range{0x80, 0x9F}, value);
}

constexpr bool isPrintChar(char32_t value) noexcept
{
    return includes(Range{0x20, 0x7F}, value) || (value > 0x7F && !isC1(value));
}

void Parser::parseFragment(uint8_t const* begin, uint8_t const* end)
{
    begin_ = begin;
    end_ = end;

    parse();
}

void Parser::parse()
{
    static constexpr char32_t ReplacementCharacter {0xFFFD};

    while (dataAvailable())
    {
        visit(
            overloaded{
                [&](unicode::Incomplete) {},
                [&](unicode::Invalid) {
                    if (parseError_)
                        parseError_("Invalid UTF-8 byte sequence received.");
                    currentChar_ = ReplacementCharacter;
                    handleViaTables();
                },
                [&](unicode::Success const& success) {
                    currentChar_ = success.value;
                    handleViaTables();
                },
            },
            unicode::from_utf8(utf8DecoderState_, currentByte())
        );

        advance();
    }
}

void Parser::invokeAction(ActionClass actionClass, Action action)
{
    if (actionHandler_)
        actionHandler_(actionClass, action, currentChar());
}

void Parser::handleViaTables()
{
    auto const s = static_cast<size_t>(state_);

    ParserTable static constexpr table = ParserTable::get();

    auto const ch = currentChar() < 0xFF ? currentChar() : static_cast<char32_t>(ParserTable::UnicodeCodepoint::Value);

    if (auto const t = table.transitions[s][ch]; t != State::Undefined)
    {
        invokeAction(ActionClass::Leave, table.exitEvents[s]);
        invokeAction(ActionClass::Transition, table.events[s][ch]);
        state_ = t;
        invokeAction(ActionClass::Enter, table.entryEvents[static_cast<size_t>(t)]);
    }
    else if (Action const a = table.events[s][ch]; a != Action::Undefined)
        invokeAction(ActionClass::Event, a);
    else if (parseError_)
        parseError_(fmt::format("Parser Error: Unknown action for state/input pair ({}, 0x{:02X})", state_, static_cast<uint32_t>(ch)));
}

}  // namespace terminal
