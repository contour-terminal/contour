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
#pragma once

#include <terminal/Parser.h>
#include <crispy/assert.h>

#if defined(__SSE2__)
#include <immintrin.h>
#endif

namespace terminal::parser
{

namespace detail
{
    constexpr uint8_t operator "" _b(unsigned long long  _value)
    {
        return static_cast<uint8_t>(_value);
    }

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

        // NOTE: It seems like it is natural to have the following two lines
        // in order to improve speed, but it's in fact slower.
        //
        // while (input != _end && *input >= 0x20 && (*input & 0x80) == 0)
        //     ++input;

        return static_cast<size_t>(std::distance(_begin, input));
        #else
        return 0;
        #endif
    }

}

constexpr ParserTable ParserTable::get() // {{{
{
    using namespace detail;

    auto constexpr UnicodeRange = Range{0x80, 0xFF};

    auto t = ParserTable{};

    // Ground
    t.event(State::Ground, Action::Execute, Range{0x00_b, 0x17_b}, 0x19_b, Range{0x1C_b, 0x1F_b});
    t.event(State::Ground, Action::Print, Range{0x20_b, 0x7F_b});
    t.event(State::Ground, Action::Print, Range{0xA0_b, 0xFF_b});
    t.event(State::Ground, Action::Print, UnicodeRange);

    // EscapeIntermediate
    t.event(State::EscapeIntermediate, Action::Execute, Range{0x00_b, 0x17_b}, 0x19_b, Range{0x1C_b, 0x1F_b});
    t.event(State::EscapeIntermediate, Action::Collect, Range{0x20_b, 0x2F_b});
    t.event(State::EscapeIntermediate, Action::Ignore, 0x7F_b);
    t.transition(State::EscapeIntermediate, State::Ground, Action::ESC_Dispatch, Range{0x30_b, 0x7E_b});

    // Escape
    t.entry(State::Escape, Action::Clear);
    t.event(State::Escape, Action::Execute, Range{0x00_b, 0x17_b}, 0x19_b, Range{0x1C_b, 0x1F_b});
    t.event(State::Escape, Action::Ignore, 0x7F_b);
    t.transition(State::Escape, State::IgnoreUntilST, 0x58_b); // SOS (start of string): ESC X
    t.transition(State::Escape, State::PM_String, 0x5E_b);     // PM (private message): ESC ^
    t.transition(State::Escape, State::APC_String, 0x5F_b);    // APC (application program command): ESC _
    t.transition(State::Escape, State::DCS_Entry, 0x50_b);
    t.transition(State::Escape, State::OSC_String, 0x5D_b);
    t.transition(State::Escape, State::CSI_Entry, 0x5B_b);
    t.transition(State::Escape, State::Ground, Action::ESC_Dispatch, Range{0x30_b, 0x4F_b});
    t.transition(State::Escape, State::Ground, Action::ESC_Dispatch, Range{0x51_b, 0x57_b});
    t.transition(State::Escape, State::Ground, Action::ESC_Dispatch, 0x59_b);
    t.transition(State::Escape, State::Ground, Action::ESC_Dispatch, 0x5A_b);
    t.transition(State::Escape, State::Ground, Action::Ignore, 0x5C_b); // ST for OSC, DCS, ...
    t.transition(State::Escape, State::Ground, Action::ESC_Dispatch, Range{0x60_b, 0x7E_b});
    t.transition(State::Escape, State::EscapeIntermediate, Action::Collect, Range{0x20_b, 0x2F_b});

    // IgnoreUntilST
    t.event(State::IgnoreUntilST, Action::Ignore, Range{0x00_b, 0x17_b}, 0x19_b, Range{0x1C_b, 0x1F_b});
    t.transition(State::IgnoreUntilST, State::Ground, 0x9C_b);

    // DCS_Entry
    t.entry(State::DCS_Entry, Action::Clear);
    t.event(State::DCS_Entry, Action::Ignore, Range{0x00_b, 0x17_b}, 0x19_b, Range{0x1C_b, 0x1F_b});
    t.event(State::DCS_Entry, Action::Ignore, 0x7F_b);
    t.transition(State::DCS_Entry, State::DCS_Intermediate, Action::Collect, Range{0x20_b, 0x2F_b});
    t.transition(State::DCS_Entry, State::DCS_Ignore, 0x3A_b);
    t.transition(State::DCS_Entry, State::DCS_Param, Action::Param, Range{0x30_b, 0x39_b});
    t.transition(State::DCS_Entry, State::DCS_Param, Action::Param, 0x3B_b);
    t.transition(State::DCS_Entry, State::DCS_Param, Action::CollectLeader, Range{0x3C_b, 0x3F_b});
    t.transition(State::DCS_Entry, State::DCS_PassThrough, Range{0x40_b, 0x7E_b});

    // DCS_Ignore
    t.event(State::DCS_Ignore, Action::Ignore, Range{0x00_b, 0x17_b}, 0x19_b, Range{0x1C_b, 0x1F_b}, Range{0x20_b, 0x7F_b});
    t.event(State::DCS_Ignore, Action::Print, Range{0xA0_b, 0xFF_b});
    t.event(State::DCS_Ignore, Action::Print, UnicodeRange);
    t.transition(State::DCS_Ignore, State::Ground, 0x9C_b);

    // DCS_Intermediate
    t.event(State::DCS_Intermediate, Action::Ignore, Range{0x00_b, 0x17_b}, 0x19_b, Range{0x1C_b, 0x1F_b});
    t.event(State::DCS_Intermediate, Action::Collect, Range{0x20_b, 0x2F_b});
    t.event(State::DCS_Intermediate, Action::Ignore, 0x7F_b);
    t.transition(State::DCS_Intermediate, State::DCS_PassThrough, Range{0x40_b, 0x7E_b});

    // DCS_PassThrough
    t.entry(State::DCS_PassThrough, Action::Hook);
    t.event(State::DCS_PassThrough, Action::Put, Range{0x00_b, 0x17_b}, 0x19_b, Range{0x1C_b, 0x1F_b}, Range{0x20_b, 0x7E_b});
    t.event(State::DCS_PassThrough, Action::Ignore, 0x7F_b);
    t.exit(State::DCS_PassThrough, Action::Unhook);
    t.transition(State::DCS_PassThrough, State::Ground, 0x9C_b);

    // DCS_Param
    t.event(State::DCS_Param, Action::Execute, Range{0x00_b, 0x17_b}, 0x19_b, Range{0x1C_b, 0x1F_b});
    t.event(State::DCS_Param, Action::Param, Range{0x30_b, 0x39_b}, 0x3B_b);
    t.event(State::DCS_Param, Action::Ignore, 0x7F_b);
    t.transition(State::DCS_Param, State::DCS_Ignore, 0x3A_b);
    t.transition(State::DCS_Param, State::DCS_Ignore, Range{0x3C_b, 0x3F_b});
    t.transition(State::DCS_Param, State::DCS_Intermediate, Range{0x20_b, 0x2F_b});
    t.transition(State::DCS_Param, State::DCS_PassThrough, Range{0x40_b, 0x7E_b});

    // OSC_String
	// (xterm extension to also allow BEL (0x07) as OSC terminator)
    t.entry(State::OSC_String, Action::OSC_Start);
    t.event(State::OSC_String, Action::Ignore, Range{0x00_b, 0x06_b}, Range{0x08_b, 0x17_b}, 0x19_b, Range{0x1C_b, 0x1F_b});
    t.event(State::OSC_String, Action::OSC_Put, Range{0x20_b, 0x7F_b});
    t.event(State::OSC_String, Action::OSC_Put, Range{0xA0_b, 0xFF_b});
    t.event(State::OSC_String, Action::OSC_Put, UnicodeRange);
    t.exit(State::OSC_String, Action::OSC_End);
    t.transition(State::OSC_String, State::Ground, 0x9C_b);
    t.transition(State::OSC_String, State::Ground, 0x07_b);

    // APC_String
    // APC := ESC _ ... ST
    t.entry(State::APC_String, Action::APC_Start);
    t.event(State::APC_String, Action::APC_Put, Range{0x20_b, 0x7F_b});
    t.event(State::APC_String, Action::APC_Put, Range{0xA0_b, 0xFF_b});
    t.event(State::APC_String, Action::APC_Put, UnicodeRange);
    t.exit(State::APC_String, Action::APC_End);
    t.transition(State::APC_String, State::Ground, 0x9C_b); // ST
    t.transition(State::APC_String, State::Ground, 0x07_b); // BEL

    // PM_String
    // PM := ESC ^ ... ST
    t.entry(State::PM_String, Action::PM_Start);
    t.event(State::PM_String, Action::PM_Put, Range{0x00_b, 0x17_b}, 0x19_b,
                                              Range{0x1C_b, 0x1F_b},
                                              Range{0x20_b, 0x7F_b},
                                              Range{0xA0_b, 0xFF_b});
    t.event(State::PM_String, Action::PM_Put, UnicodeRange);
    t.exit(State::PM_String, Action::PM_End);
    t.transition(State::PM_String, State::Ground, 0x9C_b); // ST
    t.transition(State::PM_String, State::Ground, 0x07_b); // BEL

    // CSI_Entry
    t.entry(State::CSI_Entry, Action::Clear);
    t.event(State::CSI_Entry, Action::Execute, Range{0x00_b, 0x17_b}, 0x19_b, Range{0x1C_b, 0x1F_b});
    t.event(State::CSI_Entry, Action::Ignore, 0x7F_b);
    t.transition(State::CSI_Entry, State::Ground, Action::CSI_Dispatch, Range{0x40_b, 0x7E_b});
    t.transition(State::CSI_Entry, State::CSI_Intermediate, Action::Collect, Range{0x20_b, 0x2F_b});
    t.transition(State::CSI_Entry, State::CSI_Ignore, 0x3A_b);
    t.transition(State::CSI_Entry, State::CSI_Param, Action::Param, Range{0x30_b, 0x39_b});
    t.transition(State::CSI_Entry, State::CSI_Param, Action::Param, 0x3B_b);
    t.transition(State::CSI_Entry, State::CSI_Param, Action::CollectLeader, Range{0x3C_b, 0x3F_b});

    // CSI_Param
    t.event(State::CSI_Param, Action::Execute, Range{0x00_b, 0x17_b}, 0x19_b, Range{0x1C_b, 0x1F_b});
    t.event(State::CSI_Param, Action::Param, Range{0x30_b, 0x39_b});
    t.event(State::CSI_Param, Action::Param, 0x3A_b);
    t.event(State::CSI_Param, Action::Param, 0x3B_b);
    t.event(State::CSI_Param, Action::Ignore, 0x7F_b);
    t.transition(State::CSI_Param, State::CSI_Ignore, Range{0x3C_b, 0x3F_b});
    t.transition(State::CSI_Param, State::CSI_Intermediate, Action::Collect, Range{0x20_b, 0x2F_b});
    t.transition(State::CSI_Param, State::Ground, Action::CSI_Dispatch, Range{0x40_b, 0x7E_b});

    // CSI_Ignore
    t.event(State::CSI_Ignore, Action::Execute, Range{0x00_b, 0x17_b}, 0x19_b, Range{0x1C_b, 0x1F_b});
    t.event(State::CSI_Ignore, Action::Ignore, Range{0x20_b, 0x3F_b}, 0x7F_b);
    t.transition(State::CSI_Ignore, State::Ground, Range{0x40_b, 0x7E_b});

    // CSI_Intermediate
    t.event(State::CSI_Intermediate, Action::Execute, Range{0x00_b, 0x17_b}, 0x19_b, Range{0x1C_b, 0x1F_b});
    t.event(State::CSI_Intermediate, Action::Collect, Range{0x20_b, 0x2F_b});
    t.event(State::CSI_Intermediate, Action::Ignore, 0x7F_b);
    t.transition(State::CSI_Intermediate, State::CSI_Ignore, Range{0x30_b, 0x3F_b});
    t.transition(State::CSI_Intermediate, State::Ground, Action::CSI_Dispatch, Range{0x40_b, 0x7E_b});

    // * -> Ground, ...
    for (State anywhere = std::numeric_limits<State>::min(); anywhere <= std::numeric_limits<State>::max(); ++anywhere)
    {
        t.transition(anywhere, State::Ground, 0x18_b);
        t.transition(anywhere, State::Ground, 0x1A_b);
        t.transition(anywhere, State::Escape, 0x1B_b);

        // C1 control need special 2-byte treatment due to this Parser
        // being UTF-8.
        // t.transition(anywhere, State::Ground, 0x9C_b);
        // t.transition(anywhere, State::Ground, Range{0x80_b, 0x8F_b});
        // t.transition(anywhere, State::Ground, Range{0x91_b, 0x97_b});
        // t.transition(anywhere, State::DCS_Entry, 0x90_b);     // C1: DCS
        // t.transition(anywhere, State::IgnoreUntilST, 0x98_b); // C1: SOS
        // t.transition(anywhere, State::PM_String, 0x9E_b);     // C1: PM
        // t.transition(anywhere, State::APC_String, 0x9F_b);    // C1: APC
    }

    // TODO: verify the above is correct (programatically as much as possible)
    // TODO: see if we can reduce the preassure on L2 caches (is this even an issue?)

    return t;
} // }}}

namespace detail {

} // end namespace detail

template <typename EventListener>
void Parser<EventListener>::parseFragment(std::string_view _data)
{
    auto input = reinterpret_cast<uint8_t const*>(_data.data());
    auto end = reinterpret_cast<uint8_t const*>(_data.data() + _data.size());

    do
    {
        if (state_ == State::Ground)
        {
            if (auto count = detail::countAsciiTextChars(input, end); count > 0)
            {
                eventListener_.print(std::string_view{reinterpret_cast<char const*>(input), count});
                input += count;

                // This optimization is for the `cat`-people.
                // It further optimizes the throughput performance by bypassing
                // the FSM for the `(TEXT LF+)+`-case.
                //
                // As of bench-headless, the performance incrrease is about 50x.
                if (input != end && *input == '\n')
                {
                    eventListener_.execute(static_cast<char>(*input++));
                }
                continue;
            }
        }

        auto const _ch = *input++;
        auto const s = static_cast<size_t>(state_);
        ParserTable static constexpr table = ParserTable::get();

        if (auto const t = table.transitions[s][static_cast<uint8_t>(_ch)]; t != State::Undefined)
        {
            // fmt::print("VTParser: Transitioning from {} to {}", state_, t);
            // handle(_actionClass, _action, currentChar());
            handle(ActionClass::Leave, table.exitEvents[s], _ch);
            handle(ActionClass::Transition, table.events[s][_ch], _ch);
            state_ = t;
            handle(ActionClass::Enter, table.entryEvents[static_cast<size_t>(t)], _ch);
        }
        else if (Action const a = table.events[s][static_cast<uint8_t>(_ch)]; a != Action::Undefined)
            handle(ActionClass::Event, a, _ch);
        else
            eventListener_.error(fmt::format("Parser Error: Unknown action for state/input pair ({}, '{}' 0x{:02X})", state_, _ch, static_cast<unsigned>(_ch)));
    }
    while (input != end);
}

template <typename EventListener>
void Parser<EventListener>::handle(ActionClass _actionClass, Action _action, char _char)
{
    (void) _actionClass;
    // if (_action != Action::Ignore && _action != Action::Undefined)
    //     fmt::print("Parser.handle: {} {} {} {}\n",
    //         state_,
    //         _actionClass,
    //         _action,
    //         crispy::escape(unicode::convert_to<char>(_char))
    //     );

    switch (_action)
    {
        case Action::Clear:
            eventListener_.clear();
            break;
        case Action::CollectLeader:
            eventListener_.collectLeader(static_cast<char>(_char));
            break;
        case Action::Collect:
            eventListener_.collect(static_cast<char>(_char));
            break;
        case Action::Param:
            eventListener_.param(static_cast<char>(_char));
            break;
        case Action::Execute:
            eventListener_.execute(static_cast<char>(_char));
            break;
        case Action::ESC_Dispatch:
            eventListener_.dispatchESC(static_cast<char>(_char));
            break;
        case Action::CSI_Dispatch:
            eventListener_.dispatchCSI(static_cast<char>(_char));
            break;
        case Action::Print:
            eventListener_.print(_char);
            break;
        case Action::OSC_Start:
            eventListener_.startOSC();
            break;
        case Action::OSC_Put:
            eventListener_.putOSC(_char);
            break;
        case Action::OSC_End:
            eventListener_.dispatchOSC();
            break;
        case Action::Hook:
            eventListener_.hook(static_cast<char>(_char));
            break;
        case Action::Put:
            eventListener_.put(_char);
            break;
        case Action::Unhook:
            eventListener_.unhook();
            break;
        case Action::APC_Start:
            eventListener_.startAPC();
            break;
        case Action::APC_Put:
            eventListener_.putAPC(_char);
            break;
        case Action::APC_End:
            eventListener_.dispatchAPC();
            break;
        case Action::PM_Start:
            eventListener_.startPM();
            break;
        case Action::PM_Put:
            eventListener_.putPM(_char);
            break;
        case Action::PM_End:
            eventListener_.dispatchPM();
            break;
        case Action::Ignore:
        case Action::Undefined:
            break;
    }
}

} // end namespace
