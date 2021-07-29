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

#include <crispy/escape.h>
#include <crispy/overloaded.h>
#include <crispy/indexed.h>

#include <unicode/utf8.h>

#include <array>
#include <cctype>
#include <cstdio>
#include <map>
#include <ostream>
#include <string_view>

#include <fmt/format.h>

#if defined(__SSE2__)
#include <immintrin.h>
#endif

namespace terminal::parser {

using namespace std;

inline int countTrailingZeroBits(unsigned int _value)
{
#if defined(_WIN32)
    return _tzcnt_u32(_value);
#else
    return __builtin_ctz(_value);
#endif
}

inline size_t countAsciiTextChars(uint8_t const* _begin, uint8_t const* _end) noexcept
{
    // TODO: Do move this functionality into libunicode?

    auto input = _begin;

#if 0 // TODO: defined(__AVX2__)
    // AVX2 to be implemented directly in NASM file.

#elif defined(__SSE2__)
    __m128i const ControlCodeMax = _mm_set1_epi8(0x20);  // 0..0x1F
    __m128i const Complex = _mm_set1_epi8(static_cast<char>(0x80));

    while (input < _end - sizeof(__m128i))
    {
        __m128i batch = _mm_loadu_si128((__m128i *)input);
        __m128i isControl = _mm_cmplt_epi8(batch, ControlCodeMax);
        __m128i isComplex = _mm_and_si128(batch, Complex);
        __m128i testPack = _mm_or_si128(isControl, isComplex);
        if (int const check = _mm_movemask_epi8(testPack); check != 0)
        {
            int advance = countTrailingZeroBits(check);
            input += advance;
            break;
        }
        input += 16;
    }

    return static_cast<size_t>(std::distance(_begin, input));
#else
    return 0;
#endif
}

void Parser::parseFragment(string_view _data)
{
    auto input = reinterpret_cast<uint8_t const*>(_data.data());
    auto end = reinterpret_cast<uint8_t const*>(_data.data() + _data.size());

    do
    {
        if (state_ == State::Ground && !utf8DecoderState_.expectedLength)
        {
            if (auto count = countAsciiTextChars(input, end); count > 0)
            {
                eventListener_.print(string_view{reinterpret_cast<char const*>(input), count});
                input += count;

                // This optimization is for the `cat`-people.
                // It further optimizes the throughput performance by bypassing
                // the FSM for the `(TEXT LF+)+`-case.
                //
                // As of bench-headless, the performance incrrease is about 50x.
                if (input != end && *input == '\n')
                {
                    eventListener_.execute(static_cast<char>(*input++));
                    continue;
                }
            }
        }

        static constexpr char32_t ReplacementCharacter {0xFFFD};

        while (input != end)
        {
            unicode::ConvertResult const r = unicode::from_utf8(utf8DecoderState_, *input);

            if (std::holds_alternative<unicode::Success>(r))
                processInput(std::get<unicode::Success>(r).value);
            else if (std::holds_alternative<unicode::Invalid>(r))
                processInput(ReplacementCharacter);

            ++input;
        }
    }
    while (input != end);
}

// {{{ dot
using Transition = pair<State, State>;
using Range = ParserTable::Range;
using RangeSet = std::vector<Range>;

void dot(std::ostream& _os, ParserTable const& _table)
{
    // (State, Byte) -> State
    auto transitions = map<Transition, RangeSet>{};
    for ([[maybe_unused]] auto const && [sourceState, sourceTransitions] : crispy::indexed(_table.transitions))
    {
        for (auto const [i, targetState] : crispy::indexed(sourceTransitions))
        {
            auto const ch = static_cast<uint8_t>(i);
            if (targetState != State::Undefined)
            {
                //_os << fmt::format("({}, 0x{:0X}) -> {}\n", static_cast<State>(sourceState), ch, targetState);
                auto const t = Transition{static_cast<State>(sourceState), targetState};
                if (!transitions[t].empty() && ch == transitions[t].back().last + 1)
                    transitions[t].back().last++;
                else
                    transitions[t].emplace_back(Range{ch, ch});
            }
        }
    }
    // TODO: isReachableFromAnywhere(targetState) to check if x can be reached from anywhere.

    _os << "digraph {\n";
    _os << "  node [shape=box];\n";
    _os << "  ranksep = 0.75;\n";
    _os << "  rankdir = LR;\n";
    _os << "  concentrate = true;\n";

    unsigned groundCount = 0;

    for (auto const& t : transitions)
    {
        auto const sourceState = t.first.first;
        auto const targetState = t.first.second;

        if (sourceState == State::Undefined)
            continue;

        auto const targetStateName = targetState == State::Ground && targetState != sourceState
            ? fmt::format("{}_{}", targetState, ++groundCount)
            : fmt::format("{}", targetState);

        // if (isReachableFromAnywhere(targetState))
        //     _os << fmt::format("  {} [style=dashed, style=\"rounded, filled\", fillcolor=yellow];\n", sourceStateName);

        if (targetState == State::Ground && sourceState != State::Ground)
            _os << fmt::format("  {} [style=\"dashed, filled\", fillcolor=gray, label=\"ground\"];\n", targetStateName);

        _os << fmt::format("  {} -> {} ", sourceState, targetStateName);
        _os << "[";
        _os << "label=\"";
        for (auto const && [rangeCount, u] : crispy::indexed(t.second))
        {
            if (rangeCount)
            {
                _os << ", ";
                if (rangeCount % 3 == 0)
                    _os << "\\n";
            }
            if (u.first == u.last)
                _os << fmt::format("{:02X}", u.first);
            else
                _os << fmt::format("{:02X}-{:02X}", u.first, u.last);
        }
        _os << "\"";
        _os << "]";
        _os << ";\n";
    }

    // equal ranks
    _os << "  { rank=same; ";
    for (auto const state : {State::CSI_Entry, State::DCS_Entry, State::OSC_String})
        _os << fmt::format("{}; ", state);
    _os << "};\n";

    _os << "  { rank=same; ";
    for (auto const state : {State::CSI_Param, State::DCS_Param, State::OSC_String})
        _os << fmt::format("{}; ", state);
    _os << "};\n";

    _os << "}\n";
}
// }}}

}  // namespace terminal
