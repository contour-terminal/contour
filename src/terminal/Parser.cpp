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
#include <terminal/logging.h>

#include <crispy/assert.h>
#include <crispy/escape.h>
#include <crispy/indexed.h>
#include <crispy/overloaded.h>
#include <crispy/utils.h>

#include <unicode/scan.h>
#include <unicode/utf8.h>

#include <fmt/format.h>

#include <array>
#include <cctype>
#include <cstdio>
#include <map>
#include <ostream>
#include <string_view>

#if defined(__SSE2__)
    #include <immintrin.h>
#endif

using namespace std;

namespace terminal::parser
{

namespace detail
{
    constexpr uint8_t operator"" _b(unsigned long long _value)
    {
        return static_cast<uint8_t>(_value);
    }
} // namespace detail

struct ParserTable
{
    //! State transition map from (State, Byte) to (State).
    std::array<std::array<State, 256>, std::numeric_limits<State>::size()> transitions {
        std::array<State, 256> { State::Ground /*XXX or Undefined?*/ }
    };

    //! actions to be invoked upon state entry
    std::array<Action, std::numeric_limits<Action>::size()> entryEvents { Action::Undefined };

    //! actions to be invoked upon state exit
    std::array<Action, std::numeric_limits<Action>::size()> exitEvents { Action::Undefined };

    //! actions to be invoked for a given (State, Byte) pair.
    std::array<std::array<Action, 256>, std::numeric_limits<Action>::size()> events;

    //! Standard state machine tables parsing VT225 to VT525.
    static constexpr ParserTable get();

    // {{{ implementation detail
    struct Range
    {
        uint8_t first;
        uint8_t last;
    };

    constexpr void entry(State _state, Action _action)
    {
        entryEvents.at(static_cast<size_t>(_state)) = _action;
    }

    constexpr void exit(State _state, Action _action)
    {
        exitEvents.at(static_cast<size_t>(_state)) = _action;
    }

    // Events
    constexpr void event(State _state, Action _action, uint8_t _input)
    {
        events.at(static_cast<size_t>(_state)).at(_input) = _action;
    }

    constexpr void event(State _state, Action _action, Range _input)
    {
        for (unsigned input = _input.first; input <= _input.last; ++input)
            event(_state, _action, static_cast<uint8_t>(input));
    }

    template <typename Arg, typename Arg2, typename... Args>
    constexpr void event(State s, Action a, Arg a1, Arg2 a2, Args... more)
    {
        event(s, a, a1);
        event(s, a, a2, more...);
    }

    // Transitions *with* actions
    constexpr void transition(State _from, State _to, Action _action, uint8_t _input)
    {
        event(_from, _action, _input);
        transitions[static_cast<size_t>(_from)][_input] = _to;
    }

    constexpr void transition(State _from, State _to, Action _action, Range _input)
    {
        event(_from, _action, _input);
        for (unsigned input = _input.first; input <= _input.last; ++input)
            transitions[static_cast<size_t>(_from)][input] = _to;
    }

    // template <typename Arg, typename Arg2, typename... Args>
    // constexpr void transition(State s, State t, Action a, Arg a1, Arg2 a2, Args... more)
    // {
    //     transition(s, t, a, a1);
    //     transition(s, t, a, a2, more...);
    // }

    // Transitions *without* actions
    constexpr void transition(State _from, State _to, uint8_t _input)
    {
        event(_from, Action::Ignore, _input);
        transitions[static_cast<size_t>(_from)][_input] = _to;
    }

    constexpr void transition(State _from, State _to, Range _input)
    {
        event(_from, Action::Ignore, _input);
        for (unsigned input = _input.first; input <= _input.last; ++input)
            transitions[static_cast<size_t>(_from)][input] = _to;
    }

    // template <typename Arg, typename Arg2, typename... Args>
    // constexpr void transition(State s, State t, Arg a1, Arg2 a2, Args... more)
    // {
    //     transition(s, t, a1);
    //     transition(s, t, a2, more...);
    // }

    // }}}
};

constexpr ParserTable ParserTable::get() // {{{
{
    using namespace detail;

    auto constexpr UnicodeRange = Range { 0x80, 0xFF };

    auto t = ParserTable {};

    // Ground
    t.entry(State::Ground, Action::GroundStart);
    t.event(State::Ground, Action::Execute, Range { 0x00_b, 0x17_b }, 0x19_b, Range { 0x1C_b, 0x1F_b });
    t.event(State::Ground, Action::Print, Range { 0x20_b, 0x7F_b });
    t.event(State::Ground, Action::Print, Range { 0xA0_b, 0xFF_b });
    t.event(State::Ground, Action::Print, UnicodeRange);

    // EscapeIntermediate
    t.event(State::EscapeIntermediate,
            Action::Execute,
            Range { 0x00_b, 0x17_b },
            0x19_b,
            Range { 0x1C_b, 0x1F_b });
    t.event(State::EscapeIntermediate, Action::Collect, Range { 0x20_b, 0x2F_b });
    t.event(State::EscapeIntermediate, Action::Ignore, 0x7F_b);
    t.transition(State::EscapeIntermediate, State::Ground, Action::ESC_Dispatch, Range { 0x30_b, 0x7E_b });

    // Escape
    t.entry(State::Escape, Action::Clear);
    t.event(State::Escape, Action::Execute, Range { 0x00_b, 0x17_b }, 0x19_b, Range { 0x1C_b, 0x1F_b });
    t.event(State::Escape, Action::Ignore, 0x7F_b);
    t.transition(State::Escape, State::IgnoreUntilST, 0x58_b); // SOS (start of string): ESC X
    t.transition(State::Escape, State::PM_String, 0x5E_b);     // PM (private message): ESC ^
    t.transition(State::Escape, State::APC_String, 0x5F_b);    // APC (application program command): ESC _
    t.transition(State::Escape, State::DCS_Entry, 0x50_b);
    t.transition(State::Escape, State::OSC_String, 0x5D_b);
    t.transition(State::Escape, State::CSI_Entry, 0x5B_b);
    t.transition(State::Escape, State::Ground, Action::ESC_Dispatch, Range { 0x30_b, 0x4F_b });
    t.transition(State::Escape, State::Ground, Action::ESC_Dispatch, Range { 0x51_b, 0x57_b });
    t.transition(State::Escape, State::Ground, Action::ESC_Dispatch, 0x59_b);
    t.transition(State::Escape, State::Ground, Action::ESC_Dispatch, 0x5A_b);
    t.transition(State::Escape, State::Ground, Action::Ignore, 0x5C_b); // ST for OSC, DCS, ...
    t.transition(State::Escape, State::Ground, Action::ESC_Dispatch, Range { 0x60_b, 0x7E_b });
    t.transition(State::Escape, State::EscapeIntermediate, Action::Collect, Range { 0x20_b, 0x2F_b });

    // IgnoreUntilST
    t.event(State::IgnoreUntilST, Action::Ignore, Range { 0x00_b, 0x17_b }, 0x19_b, Range { 0x1C_b, 0x1F_b });
    // t.transition(State::IgnoreUntilST, State::Ground, 0x9C_b);

    // DCS_Entry
    t.entry(State::DCS_Entry, Action::Clear);
    t.event(State::DCS_Entry, Action::Ignore, Range { 0x00_b, 0x17_b }, 0x19_b, Range { 0x1C_b, 0x1F_b });
    t.event(State::DCS_Entry, Action::Ignore, 0x7F_b);
    t.transition(State::DCS_Entry, State::DCS_Intermediate, Action::Collect, Range { 0x20_b, 0x2F_b });
    t.transition(State::DCS_Entry, State::DCS_Ignore, 0x3A_b);
    t.transition(State::DCS_Entry, State::DCS_Param, Action::Param, Range { 0x30_b, 0x39_b });
    t.transition(State::DCS_Entry, State::DCS_Param, Action::Param, 0x3B_b);
    t.transition(State::DCS_Entry, State::DCS_Param, Action::CollectLeader, Range { 0x3C_b, 0x3F_b });
    t.transition(State::DCS_Entry, State::DCS_PassThrough, Range { 0x40_b, 0x7E_b });

    // DCS_Ignore
    t.event(State::DCS_Ignore,
            Action::Ignore,
            Range { 0x00_b, 0x17_b },
            0x19_b,
            Range { 0x1C_b, 0x1F_b },
            Range { 0x20_b, 0x7F_b });
    t.event(State::DCS_Ignore, Action::Print, Range { 0xA0_b, 0xFF_b });
    t.event(State::DCS_Ignore, Action::Print, UnicodeRange);
    // t.transition(State::DCS_Ignore, State::Ground, 0x9C_b);

    // DCS_Intermediate
    t.event(
        State::DCS_Intermediate, Action::Ignore, Range { 0x00_b, 0x17_b }, 0x19_b, Range { 0x1C_b, 0x1F_b });
    t.event(State::DCS_Intermediate, Action::Collect, Range { 0x20_b, 0x2F_b });
    t.event(State::DCS_Intermediate, Action::Ignore, 0x7F_b);
    t.transition(State::DCS_Intermediate, State::DCS_PassThrough, Range { 0x40_b, 0x7E_b });

    // DCS_PassThrough
    t.entry(State::DCS_PassThrough, Action::Hook);
    t.event(State::DCS_PassThrough,
            Action::Put,
            Range { 0x00_b, 0x17_b },
            0x19_b,
            Range { 0x1C_b, 0x1F_b },
            Range { 0x20_b, 0x7E_b });
    t.event(State::DCS_PassThrough, Action::Ignore, 0x7F_b);
    t.exit(State::DCS_PassThrough, Action::Unhook);
    // t.transition(State::DCS_PassThrough, State::Ground, 0x9C_b);

    // DCS_Param
    t.event(State::DCS_Param, Action::Execute, Range { 0x00_b, 0x17_b }, 0x19_b, Range { 0x1C_b, 0x1F_b });
    t.event(State::DCS_Param, Action::Param, Range { 0x30_b, 0x39_b }, 0x3B_b);
    t.event(State::DCS_Param, Action::Ignore, 0x7F_b);
    t.transition(State::DCS_Param, State::DCS_Ignore, 0x3A_b);
    t.transition(State::DCS_Param, State::DCS_Ignore, Range { 0x3C_b, 0x3F_b });
    t.transition(State::DCS_Param, State::DCS_Intermediate, Range { 0x20_b, 0x2F_b });
    t.transition(State::DCS_Param, State::DCS_PassThrough, Range { 0x40_b, 0x7E_b });

    // OSC_String
    // (xterm extension to also allow BEL (0x07) as OSC terminator)
    t.entry(State::OSC_String, Action::OSC_Start);
    t.event(State::OSC_String,
            Action::Ignore,
            Range { 0x00_b, 0x06_b },
            Range { 0x08_b, 0x17_b },
            0x19_b,
            Range { 0x1C_b, 0x1F_b });
    t.event(State::OSC_String, Action::OSC_Put, Range { 0x20_b, 0x7F_b });
    t.event(State::OSC_String, Action::OSC_Put, Range { 0xA0_b, 0xFF_b });
    t.event(State::OSC_String, Action::OSC_Put, UnicodeRange);
    t.exit(State::OSC_String, Action::OSC_End);
    // t.transition(State::OSC_String, State::Ground, 0x9C_b);
    t.transition(State::OSC_String, State::Ground, 0x07_b);

    // APC_String
    // APC := ESC _ ... ST
    t.entry(State::APC_String, Action::APC_Start);
    t.event(State::APC_String, Action::APC_Put, Range { 0x20_b, 0x7F_b });
    t.event(State::APC_String, Action::APC_Put, Range { 0xA0_b, 0xFF_b });
    t.event(State::APC_String, Action::APC_Put, UnicodeRange);
    t.exit(State::APC_String, Action::APC_End);
    // t.transition(State::APC_String, State::Ground, 0x9C_b); // ST
    t.transition(State::APC_String, State::Ground, 0x07_b); // BEL

    // PM_String
    // PM := ESC ^ ... ST
    t.entry(State::PM_String, Action::PM_Start);
    t.event(State::PM_String,
            Action::PM_Put,
            Range { 0x00_b, 0x17_b },
            0x19_b,
            Range { 0x1C_b, 0x1F_b },
            Range { 0x20_b, 0x7F_b },
            Range { 0xA0_b, 0xFF_b });
    t.event(State::PM_String, Action::PM_Put, UnicodeRange);
    t.exit(State::PM_String, Action::PM_End);
    // t.transition(State::PM_String, State::Ground, 0x9C_b); // ST
    t.transition(State::PM_String, State::Ground, 0x07_b); // BEL

    // CSI_Entry
    t.entry(State::CSI_Entry, Action::Clear);
    t.event(State::CSI_Entry, Action::Execute, Range { 0x00_b, 0x17_b }, 0x19_b, Range { 0x1C_b, 0x1F_b });
    t.event(State::CSI_Entry, Action::Ignore, 0x7F_b);
    t.transition(State::CSI_Entry, State::Ground, Action::CSI_Dispatch, Range { 0x40_b, 0x7E_b });
    t.transition(State::CSI_Entry, State::CSI_Intermediate, Action::Collect, Range { 0x20_b, 0x2F_b });
    t.transition(State::CSI_Entry, State::CSI_Ignore, 0x3A_b);
    t.transition(State::CSI_Entry, State::CSI_Param, Action::ParamDigit, Range { 0x30_b, 0x39_b });
    t.transition(State::CSI_Entry, State::CSI_Param, Action::ParamSeparator, 0x3B_b);
    t.transition(State::CSI_Entry, State::CSI_Param, Action::CollectLeader, Range { 0x3C_b, 0x3F_b });

    // CSI_Param
    t.event(State::CSI_Param, Action::Execute, Range { 0x00_b, 0x17_b }, 0x19_b, Range { 0x1C_b, 0x1F_b });
    t.event(State::CSI_Param, Action::ParamDigit, Range { 0x30_b, 0x39_b });
    t.event(State::CSI_Param, Action::ParamSubSeparator, 0x3A_b);
    t.event(State::CSI_Param, Action::ParamSeparator, 0x3B_b);
    t.event(State::CSI_Param, Action::Ignore, 0x7F_b);
    t.transition(State::CSI_Param, State::CSI_Ignore, Range { 0x3C_b, 0x3F_b });
    t.transition(State::CSI_Param, State::CSI_Intermediate, Action::Collect, Range { 0x20_b, 0x2F_b });
    t.transition(State::CSI_Param, State::Ground, Action::CSI_Dispatch, Range { 0x40_b, 0x7E_b });

    // CSI_Ignore
    t.event(State::CSI_Ignore, Action::Execute, Range { 0x00_b, 0x17_b }, 0x19_b, Range { 0x1C_b, 0x1F_b });
    t.event(State::CSI_Ignore, Action::Ignore, Range { 0x20_b, 0x3F_b }, 0x7F_b);
    t.transition(State::CSI_Ignore, State::Ground, Range { 0x40_b, 0x7E_b });

    // CSI_Intermediate
    t.event(
        State::CSI_Intermediate, Action::Execute, Range { 0x00_b, 0x17_b }, 0x19_b, Range { 0x1C_b, 0x1F_b });
    t.event(State::CSI_Intermediate, Action::Collect, Range { 0x20_b, 0x2F_b });
    t.event(State::CSI_Intermediate, Action::Ignore, 0x7F_b);
    t.transition(State::CSI_Intermediate, State::CSI_Ignore, Range { 0x30_b, 0x3F_b });
    t.transition(State::CSI_Intermediate, State::Ground, Action::CSI_Dispatch, Range { 0x40_b, 0x7E_b });

    // * -> Ground, ...
    for (State anywhere = std::numeric_limits<State>::min(); anywhere <= std::numeric_limits<State>::max();
         ++anywhere)
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

template <typename EventListener, bool TraceStateChanges>
void Parser<EventListener, TraceStateChanges>::parseFragment(std::string_view const _data)
{
    auto input = _data.data();
    auto const end = _data.data() + _data.size();

    while (input != end)
    {
        auto const maxCharCount = eventListener_.maxBulkTextSequenceWidth();
        if (state_ == State::Ground && maxCharCount)
        {
            auto const chunk = std::string_view(input, static_cast<size_t>(std::distance(input, end)));
            auto const [cellCount, next, subStart, subEnd] =
                unicode::scan_for_text(scanState_, chunk, maxCharCount);

            if (next != input)
            {
                // We do not test on cellCount>0 because the scan could contain only a ZWJ (zero width
                // joiner), and that would be misleading.

                assert(subStart <= subEnd);
                auto const byteCount = static_cast<size_t>(std::distance(subStart, subEnd));
                assert(cellCount <= maxCharCount);
                assert(subEnd <= chunk.data() + chunk.size());
                assert(next <= chunk.data() + chunk.size());

#if defined(LIBTERMINAL_LOG_TRACE)
                if (VTTraceParserLog)
                    VTTraceParserLog()("[Unicode] Scanned text: {}/{} cells; \"{}\"",
                                       cellCount,
                                       maxCharCount,
                                       crispy::escape(std::string_view { input, byteCount }));
#endif

                auto const text = std::string_view { subStart, byteCount };
                if (scanState_.utf8.expectedLength == 0)
                {
                    if (!text.empty())
                    {
                        eventListener_.print(text, cellCount);
                    }
                }
                else
                {
                    // fmt::print("Parser.text: incomplete UTF-8 sequence at end: {}/{}\n",
                    //            scanState_.utf8.currentLength,
                    //            scanState_.utf8.expectedLength);

                    // for (char const ch: text)
                    //     printUtf8Byte(ch);
                }

                input = next;

                // This optimization is for the `cat`-people.
                // It further optimizes the throughput performance by bypassing
                // the FSM for the `(TEXT LF+)+`-case.
                //
                // As of bench-headless, the performance incrrease is about 50x.
                if (input != end && *input == '\n')
                {
                    eventListener_.execute(*input++);
                }
                continue;
            }
        }

        auto const ch = static_cast<uint8_t>(*input++);
        auto const s = static_cast<size_t>(state_);
        ParserTable static constexpr table = ParserTable::get();

        if (auto const t = table.transitions[s][static_cast<uint8_t>(ch)]; t != State::Undefined)
        {
            // fmt::print("VTParser: Transitioning from {} to {}", state_, t);
            // handle(_actionClass, _action, currentChar());
            handle(ActionClass::Leave, table.exitEvents[s], ch);
            handle(ActionClass::Transition, table.events[s][static_cast<size_t>(ch)], ch);
            state_ = t;
            handle(ActionClass::Enter, table.entryEvents[static_cast<size_t>(t)], ch);
        }
        else if (Action const a = table.events[s][ch]; a != Action::Undefined)
            handle(ActionClass::Event, a, ch);
        else
            eventListener_.error(
                fmt::format("Parser Error: Unknown action for state/input pair ({}, '{}' 0x{:02X})",
                            state_,
                            ch,
                            static_cast<unsigned>(ch)));
    }
}

template <typename EventListener, bool TraceStateChanges>
void Parser<EventListener, TraceStateChanges>::printUtf8Byte(char ch)
{
    unicode::ConvertResult const r = unicode::from_utf8(scanState_.utf8, (uint8_t) ch);
    if (std::holds_alternative<unicode::Incomplete>(r))
        return;

    auto constexpr ReplacementCharacter = char32_t { 0xFFFD };
    auto const codepoint = std::holds_alternative<unicode::Success>(r) ? std::get<unicode::Success>(r).value
                                                                       : ReplacementCharacter;
    eventListener_.print(codepoint);
    scanState_.lastCodepointHint = codepoint;
}

template <typename EventListener, bool TraceStateChanges>
void Parser<EventListener, TraceStateChanges>::handle(ActionClass _actionClass,
                                                      Action _action,
                                                      uint8_t codepoint)
{
    (void) _actionClass;
    auto const ch = static_cast<char>(codepoint);

#if defined(LIBTERMINAL_LOG_TRACE)
    if constexpr (TraceStateChanges)
        if (VTTraceParserLog && _action != Action::Ignore && _action != Action::Undefined)
            VTTraceParserLog()("handle: {} {} {} {}",
                               state_,
                               _actionClass,
                               _action,
                               crispy::escape(static_cast<uint8_t>(ch)));
#endif

    switch (_action)
    {
        case Action::GroundStart: scanState_.lastCodepointHint = 0; break;
        case Action::Clear: eventListener_.clear(); break;
        case Action::CollectLeader: eventListener_.collectLeader(ch); break;
        case Action::Collect: eventListener_.collect(ch); break;
        case Action::Param: eventListener_.param(ch); break;
        case Action::ParamDigit: eventListener_.paramDigit(ch); break;
        case Action::ParamSeparator: eventListener_.paramSeparator(); break;
        case Action::ParamSubSeparator: eventListener_.paramSubSeparator(); break;
        case Action::Execute: eventListener_.execute(ch); break;
        case Action::ESC_Dispatch: eventListener_.dispatchESC(ch); break;
        case Action::CSI_Dispatch: eventListener_.dispatchCSI(ch); break;
        case Action::Print: printUtf8Byte(ch); break;
        case Action::OSC_Start: eventListener_.startOSC(); break;
        case Action::OSC_Put: eventListener_.putOSC(ch); break;
        case Action::OSC_End: eventListener_.dispatchOSC(); break;
        case Action::Hook: eventListener_.hook(ch); break;
        case Action::Put: eventListener_.put(ch); break;
        case Action::Unhook: eventListener_.unhook(); break;
        case Action::APC_Start: eventListener_.startAPC(); break;
        case Action::APC_Put: eventListener_.putAPC(ch); break;
        case Action::APC_End: eventListener_.dispatchAPC(); break;
        case Action::PM_Start: eventListener_.startPM(); break;
        case Action::PM_Put: eventListener_.putPM(ch); break;
        case Action::PM_End: eventListener_.dispatchPM(); break;
        case Action::Ignore:
        case Action::Undefined: break;
    }
}

void parserTableDot(std::ostream& _os) // {{{
{
    using Transition = pair<State, State>;
    using Range = ParserTable::Range;
    using RangeSet = std::vector<Range>;

    ParserTable const& table = ParserTable::get();
    // (State, Byte) -> State
    auto transitions = map<Transition, RangeSet> {};
    for ([[maybe_unused]] auto const&& [sourceState, sourceTransitions]: crispy::indexed(table.transitions))
    {
        for (auto const [i, targetState]: crispy::indexed(sourceTransitions))
        {
            auto const ch = static_cast<uint8_t>(i);
            if (targetState != State::Undefined)
            {
                //_os << fmt::format("({}, 0x{:0X}) -> {}\n", static_cast<State>(sourceState), ch,
                // targetState);
                auto const t = Transition { static_cast<State>(sourceState), targetState };
                if (!transitions[t].empty() && ch == transitions[t].back().last + 1)
                    transitions[t].back().last++;
                else
                    transitions[t].emplace_back(Range { ch, ch });
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

    for (auto const& t: transitions)
    {
        auto const sourceState = t.first.first;
        auto const targetState = t.first.second;

        if (sourceState == State::Undefined)
            continue;

        auto const targetStateName = targetState == State::Ground && targetState != sourceState
                                         ? fmt::format("{}_{}", targetState, ++groundCount)
                                         : fmt::format("{}", targetState);

        // if (isReachableFromAnywhere(targetState))
        //     _os << fmt::format("  {} [style=dashed, style=\"rounded, filled\", fillcolor=yellow];\n",
        //     sourceStateName);

        if (targetState == State::Ground && sourceState != State::Ground)
            _os << fmt::format("  \"{}\" [style=\"dashed, filled\", fillcolor=gray, label=\"ground\"];\n",
                               targetStateName);

        _os << fmt::format(R"(  "{}" -> "{}" )", sourceState, targetStateName);
        _os << "[";
        _os << "label=\"";
        for (auto const&& [rangeCount, u]: crispy::indexed(t.second))
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
    for (auto const state: { State::CSI_Entry, State::DCS_Entry, State::OSC_String })
        _os << fmt::format(R"("{}"; )", state);
    _os << "};\n";

    _os << "  { rank=same; ";
    for (auto const state: { State::CSI_Param, State::DCS_Param, State::OSC_String })
        _os << fmt::format(R"("{}"; )", state);
    _os << "};\n";

    _os << "}\n";
}
// }}}

} // namespace terminal::parser

#include <terminal/Sequencer.h>
template class terminal::parser::Parser<terminal::Sequencer>;
template class terminal::parser::Parser<terminal::ParserEvents>;
