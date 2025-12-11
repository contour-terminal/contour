// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <vtparser/Parser.h>

#include <libunicode/utf8.h>

#include <array>
#include <cassert>
#include <string_view>
#include <tuple>

namespace vtparser
{

namespace
{
    // clang-format off
    constexpr uint8_t operator""_b(unsigned long long value)
    {
        return static_cast<uint8_t>(value);
    }
    // clang-format on
} // namespace

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

    constexpr void entry(State state, Action action) noexcept
    {
        entryEvents[static_cast<size_t>(state)] = action;
    }

    constexpr void exit(State state, Action action) noexcept
    {
        exitEvents[static_cast<size_t>(state)] = action;
    }

    // Events
    constexpr void event(State state, Action action, uint8_t input) noexcept
    {
        events[static_cast<size_t>(state)][input] = action;
    }

    constexpr void event(State state, Action action, Range input) noexcept
    {
        for (unsigned ch = input.first; ch <= input.last; ++ch)
            event(state, action, static_cast<uint8_t>(ch));
    }

    template <typename Arg, typename Arg2, typename... Args>
    constexpr void event(State s, Action a, Arg a1, Arg2 a2, Args... more)
    {
        event(s, a, a1);
        event(s, a, a2, more...);
    }

    // Transitions *with* actions
    constexpr void transition(State from, State to, Action action, uint8_t input)
    {
        event(from, action, input);
        transitions[static_cast<size_t>(from)][input] = to;
    }

    constexpr void transition(State from, State to, Action action, Range input)
    {
        event(from, action, input);
        for (unsigned ch = input.first; ch <= input.last; ++ch)
            transitions[static_cast<size_t>(from)][ch] = to;
    }

    // template <typename Arg, typename Arg2, typename... Args>
    // constexpr void transition(State s, State t, Action a, Arg a1, Arg2 a2, Args... more)
    // {
    //     transition(s, t, a, a1);
    //     transition(s, t, a, a2, more...);
    // }

    // Transitions *without* actions
    constexpr void transition(State from, State to, uint8_t input)
    {
        event(from, Action::Ignore, input);
        transitions[static_cast<size_t>(from)][input] = to;
    }

    constexpr void transition(State from, State to, Range input)
    {
        event(from, Action::Ignore, input);
        for (unsigned ch = input.first; ch <= input.last; ++ch)
            transitions[static_cast<size_t>(from)][ch] = to;
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
    auto constexpr UnicodeRange = Range { .first = 0x80, .last = 0xFF };

    auto t = ParserTable {};

    // Ground
    t.entry(State::Ground, Action::GroundStart);
    t.event(State::Ground,
            Action::Execute,
            Range { .first = 0x00_b, .last = 0x17_b },
            0x19_b,
            Range { .first = 0x1C_b, .last = 0x1F_b });
    t.event(State::Ground, Action::Print, Range { .first = 0x20_b, .last = 0x7F_b });
    t.event(State::Ground, Action::Print, Range { .first = 0xA0_b, .last = 0xFF_b });
    t.event(State::Ground, Action::Print, UnicodeRange);
    t.exit(State::Ground, Action::PrintEnd);

    // EscapeIntermediate
    t.event(State::EscapeIntermediate,
            Action::Execute,
            Range { .first = 0x00_b, .last = 0x17_b },
            0x19_b,
            Range { .first = 0x1C_b, .last = 0x1F_b });
    t.event(State::EscapeIntermediate, Action::Collect, Range { .first = 0x20_b, .last = 0x2F_b });
    t.event(State::EscapeIntermediate, Action::Ignore, 0x7F_b);
    t.transition(State::EscapeIntermediate,
                 State::Ground,
                 Action::ESC_Dispatch,
                 Range { .first = 0x30_b, .last = 0x7E_b });

    // Escape
    t.entry(State::Escape, Action::Clear);
    t.event(State::Escape,
            Action::Execute,
            Range { .first = 0x00_b, .last = 0x17_b },
            0x19_b,
            Range { .first = 0x1C_b, .last = 0x1F_b });
    t.event(State::Escape, Action::Ignore, 0x7F_b);
    t.transition(State::Escape, State::IgnoreUntilST, 0x58_b); // SOS (start of string): ESC X
    t.transition(State::Escape, State::PM_String, 0x5E_b);     // PM (private message): ESC ^
    t.transition(State::Escape, State::APC_String, 0x5F_b);    // APC (application program command): ESC _
    t.transition(State::Escape, State::DCS_Entry, 0x50_b);
    t.transition(State::Escape, State::OSC_String, 0x5D_b);
    t.transition(State::Escape, State::CSI_Entry, 0x5B_b);
    t.transition(
        State::Escape, State::Ground, Action::ESC_Dispatch, Range { .first = 0x30_b, .last = 0x4F_b });
    t.transition(
        State::Escape, State::Ground, Action::ESC_Dispatch, Range { .first = 0x51_b, .last = 0x57_b });
    t.transition(State::Escape, State::Ground, Action::ESC_Dispatch, 0x59_b);
    t.transition(State::Escape, State::Ground, Action::ESC_Dispatch, 0x5A_b);
    t.transition(State::Escape, State::Ground, Action::Ignore, 0x5C_b); // ST for OSC, DCS, ...
    t.transition(
        State::Escape, State::Ground, Action::ESC_Dispatch, Range { .first = 0x60_b, .last = 0x7E_b });
    t.transition(
        State::Escape, State::EscapeIntermediate, Action::Collect, Range { .first = 0x20_b, .last = 0x2F_b });

    // IgnoreUntilST
    t.event(State::IgnoreUntilST,
            Action::Ignore,
            Range { .first = 0x00_b, .last = 0x17_b },
            0x19_b,
            Range { .first = 0x1C_b, .last = 0x1F_b });
    // t.transition(State::IgnoreUntilST, State::Ground, 0x9C_b);

    // DCS_Entry
    t.entry(State::DCS_Entry, Action::Clear);
    t.event(State::DCS_Entry,
            Action::Ignore,
            Range { .first = 0x00_b, .last = 0x17_b },
            0x19_b,
            Range { .first = 0x1C_b, .last = 0x1F_b });
    t.event(State::DCS_Entry, Action::Ignore, 0x7F_b);
    t.transition(State::DCS_Entry,
                 State::DCS_Intermediate,
                 Action::Collect,
                 Range { .first = 0x20_b, .last = 0x2F_b });
    t.transition(State::DCS_Entry, State::DCS_Ignore, 0x3A_b);
    t.transition(
        State::DCS_Entry, State::DCS_Param, Action::Param, Range { .first = 0x30_b, .last = 0x39_b });
    t.transition(State::DCS_Entry, State::DCS_Param, Action::Param, 0x3B_b);
    t.transition(
        State::DCS_Entry, State::DCS_Param, Action::CollectLeader, Range { .first = 0x3C_b, .last = 0x3F_b });
    t.transition(State::DCS_Entry, State::DCS_PassThrough, Range { .first = 0x40_b, .last = 0x7E_b });

    // DCS_Ignore
    t.event(State::DCS_Ignore,
            Action::Ignore,
            Range { .first = 0x00_b, .last = 0x17_b },
            0x19_b,
            Range { .first = 0x1C_b, .last = 0x1F_b },
            Range { .first = 0x20_b, .last = 0x7F_b });
    t.event(State::DCS_Ignore, Action::Print, Range { .first = 0xA0_b, .last = 0xFF_b });
    t.event(State::DCS_Ignore, Action::Print, UnicodeRange);
    // t.transition(State::DCS_Ignore, State::Ground, 0x9C_b);

    // DCS_Intermediate
    t.event(State::DCS_Intermediate,
            Action::Ignore,
            Range { .first = 0x00_b, .last = 0x17_b },
            0x19_b,
            Range { .first = 0x1C_b, .last = 0x1F_b });
    t.event(State::DCS_Intermediate, Action::Collect, Range { .first = 0x20_b, .last = 0x2F_b });
    t.event(State::DCS_Intermediate, Action::Ignore, 0x7F_b);
    t.transition(State::DCS_Intermediate, State::DCS_PassThrough, Range { .first = 0x40_b, .last = 0x7E_b });

    // DCS_PassThrough
    t.entry(State::DCS_PassThrough, Action::Hook);
    t.event(State::DCS_PassThrough,
            Action::Put,
            Range { .first = 0x00_b, .last = 0x17_b },
            0x19_b,
            Range { .first = 0x1C_b, .last = 0x1F_b },
            Range { .first = 0x20_b, .last = 0x7E_b });
    t.event(State::DCS_PassThrough, Action::Ignore, 0x7F_b);
    t.exit(State::DCS_PassThrough, Action::Unhook);
    // t.transition(State::DCS_PassThrough, State::Ground, 0x9C_b);

    // DCS_Param
    t.event(State::DCS_Param,
            Action::Execute,
            Range { .first = 0x00_b, .last = 0x17_b },
            0x19_b,
            Range { .first = 0x1C_b, .last = 0x1F_b });
    t.event(State::DCS_Param, Action::Param, Range { .first = 0x30_b, .last = 0x39_b }, 0x3B_b);
    t.event(State::DCS_Param, Action::Ignore, 0x7F_b);
    t.transition(State::DCS_Param, State::DCS_Ignore, 0x3A_b);
    t.transition(State::DCS_Param, State::DCS_Ignore, Range { .first = 0x3C_b, .last = 0x3F_b });
    t.transition(State::DCS_Param, State::DCS_Intermediate, Range { .first = 0x20_b, .last = 0x2F_b });
    t.transition(State::DCS_Param, State::DCS_PassThrough, Range { .first = 0x40_b, .last = 0x7E_b });

    // OSC_String
    // (xterm extension to also allow BEL (0x07) as OSC terminator)
    t.entry(State::OSC_String, Action::OSC_Start);
    t.event(State::OSC_String,
            Action::Ignore,
            Range { .first = 0x00_b, .last = 0x06_b },
            Range { .first = 0x08_b, .last = 0x17_b },
            0x19_b,
            Range { .first = 0x1C_b, .last = 0x1F_b });
    t.event(State::OSC_String, Action::OSC_Put, Range { .first = 0x20_b, .last = 0x7F_b });
    t.event(State::OSC_String, Action::OSC_Put, Range { .first = 0xA0_b, .last = 0xFF_b });
    t.event(State::OSC_String, Action::OSC_Put, UnicodeRange);
    t.exit(State::OSC_String, Action::OSC_End);
    // t.transition(State::OSC_String, State::Ground, 0x9C_b);
    t.transition(State::OSC_String, State::Ground, 0x07_b);

    // APC_String
    // APC := ESC _ ... ST
    t.entry(State::APC_String, Action::APC_Start);
    t.event(State::APC_String, Action::APC_Put, Range { .first = 0x20_b, .last = 0x7F_b });
    t.event(State::APC_String, Action::APC_Put, Range { .first = 0xA0_b, .last = 0xFF_b });
    t.event(State::APC_String, Action::APC_Put, UnicodeRange);
    t.exit(State::APC_String, Action::APC_End);
    // t.transition(State::APC_String, State::Ground, 0x9C_b); // ST
    t.transition(State::APC_String, State::Ground, 0x07_b); // BEL

    // PM_String
    // PM := ESC ^ ... ST
    t.entry(State::PM_String, Action::PM_Start);
    t.event(State::PM_String,
            Action::PM_Put,
            Range { .first = 0x00_b, .last = 0x17_b },
            0x19_b,
            Range { .first = 0x1C_b, .last = 0x1F_b },
            Range { .first = 0x20_b, .last = 0x7F_b },
            Range { .first = 0xA0_b, .last = 0xFF_b });
    t.event(State::PM_String, Action::PM_Put, UnicodeRange);
    t.exit(State::PM_String, Action::PM_End);
    // t.transition(State::PM_String, State::Ground, 0x9C_b); // ST
    t.transition(State::PM_String, State::Ground, 0x07_b); // BEL

    // CSI_Entry
    t.entry(State::CSI_Entry, Action::Clear);
    t.event(State::CSI_Entry,
            Action::Execute,
            Range { .first = 0x00_b, .last = 0x17_b },
            0x19_b,
            Range { .first = 0x1C_b, .last = 0x1F_b });
    t.event(State::CSI_Entry, Action::Ignore, 0x7F_b);
    t.transition(
        State::CSI_Entry, State::Ground, Action::CSI_Dispatch, Range { .first = 0x40_b, .last = 0x7E_b });
    t.transition(State::CSI_Entry,
                 State::CSI_Intermediate,
                 Action::Collect,
                 Range { .first = 0x20_b, .last = 0x2F_b });
    t.transition(State::CSI_Entry, State::CSI_Ignore, 0x3A_b);
    t.transition(
        State::CSI_Entry, State::CSI_Param, Action::ParamDigit, Range { .first = 0x30_b, .last = 0x39_b });
    t.transition(State::CSI_Entry, State::CSI_Param, Action::ParamSeparator, 0x3B_b);
    t.transition(
        State::CSI_Entry, State::CSI_Param, Action::CollectLeader, Range { .first = 0x3C_b, .last = 0x3F_b });

    // CSI_Param
    t.event(State::CSI_Param,
            Action::Execute,
            Range { .first = 0x00_b, .last = 0x17_b },
            0x19_b,
            Range { .first = 0x1C_b, .last = 0x1F_b });
    t.event(State::CSI_Param, Action::ParamDigit, Range { .first = 0x30_b, .last = 0x39_b });
    t.event(State::CSI_Param, Action::ParamSubSeparator, 0x3A_b);
    t.event(State::CSI_Param, Action::ParamSeparator, 0x3B_b);
    t.event(State::CSI_Param, Action::Ignore, 0x7F_b);
    t.transition(State::CSI_Param, State::CSI_Ignore, Range { .first = 0x3C_b, .last = 0x3F_b });
    t.transition(State::CSI_Param,
                 State::CSI_Intermediate,
                 Action::Collect,
                 Range { .first = 0x20_b, .last = 0x2F_b });
    t.transition(
        State::CSI_Param, State::Ground, Action::CSI_Dispatch, Range { .first = 0x40_b, .last = 0x7E_b });

    // CSI_Ignore
    t.event(State::CSI_Ignore,
            Action::Execute,
            Range { .first = 0x00_b, .last = 0x17_b },
            0x19_b,
            Range { .first = 0x1C_b, .last = 0x1F_b });
    t.event(State::CSI_Ignore, Action::Ignore, Range { .first = 0x20_b, .last = 0x3F_b }, 0x7F_b);
    t.transition(State::CSI_Ignore, State::Ground, Range { .first = 0x40_b, .last = 0x7E_b });

    // CSI_Intermediate
    t.event(State::CSI_Intermediate,
            Action::Execute,
            Range { .first = 0x00_b, .last = 0x17_b },
            0x19_b,
            Range { .first = 0x1C_b, .last = 0x1F_b });
    t.event(State::CSI_Intermediate, Action::Collect, Range { .first = 0x20_b, .last = 0x2F_b });
    t.event(State::CSI_Intermediate, Action::Ignore, 0x7F_b);
    t.transition(State::CSI_Intermediate, State::CSI_Ignore, Range { .first = 0x30_b, .last = 0x3F_b });
    t.transition(State::CSI_Intermediate,
                 State::Ground,
                 Action::CSI_Dispatch,
                 Range { .first = 0x40_b, .last = 0x7E_b });

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
    // TODO: see if we can reduce the pressure on L2 caches (is this even an issue?)

    return t;
} // }}}

template <ParserEventsConcept EventListener, bool TraceStateChanges>
void Parser<EventListener, TraceStateChanges>::parseFragment(gsl::span<char const> data)
{
    const auto* input = data.data();
    const auto* const end = data.data() + data.size();

    while (input != end)
    {
        auto const [processKind, processedByteCount] = parseBulkText(input, end);
        switch (processKind)
        {
            case ProcessKind::ContinueBulk:
                // clang-format off
                input += processedByteCount;
                break;
                // clang-format on
            case ProcessKind::FallbackToFSM:
                processOnceViaStateMachine(static_cast<uint8_t>(*input++));
                break;
        }
    }
}

template <ParserEventsConcept EventListener, bool TraceStateChanges>
void Parser<EventListener, TraceStateChanges>::processOnceViaStateMachine(uint8_t ch)
{
    auto const s = static_cast<size_t>(_state);
    ParserTable static constexpr Table = ParserTable::get();

    if (auto const t = Table.transitions[s][static_cast<uint8_t>(ch)]; t != State::Undefined)
    {
        // std::cout << std::format("VTParser: Transitioning from {} to {}", _state, t);
        handle(ActionClass::Leave, Table.exitEvents[s], ch);
        handle(ActionClass::Transition, Table.events[s][static_cast<size_t>(ch)], ch);
        _state = t;
        handle(ActionClass::Enter, Table.entryEvents[static_cast<size_t>(t)], ch);
    }
    else if (Action const a = Table.events[s][ch]; a != Action::Undefined)
        handle(ActionClass::Event, a, ch);
    else
        _eventListener.error("Parser error: Unknown action for state/input pair.");
}

template <ParserEventsConcept EventListener, bool TraceStateChanges>
auto Parser<EventListener, TraceStateChanges>::parseBulkText(char const* begin, char const* end) noexcept
    -> std::tuple<ProcessKind, size_t>
{
    auto const* input = begin;
    if (_state != State::Ground)
        return { ProcessKind::FallbackToFSM, 0 };

    auto const maxCharCount = _eventListener.maxBulkTextSequenceWidth();
    if (!maxCharCount)
        return { ProcessKind::FallbackToFSM, 0 };

    _scanState.next = nullptr;
    auto const chunk = std::string_view(input, static_cast<size_t>(std::distance(input, end)));
    auto const [cellCount, subStart, subEnd] = unicode::scan_text(_scanState, chunk, maxCharCount);

    if (_scanState.next == input)
        return { ProcessKind::FallbackToFSM, 0 };

    // We do not test on cellCount>0 because the scan could contain only a ZWJ (zero width
    // joiner), and that would be misleading.

    assert(subStart <= subEnd);
    auto const byteCount = static_cast<size_t>(std::distance(subStart, subEnd));
    if (byteCount == 0)
        return { ProcessKind::FallbackToFSM, 0 };

    assert(cellCount <= maxCharCount);
    assert(subEnd <= chunk.data() + chunk.size());
    assert(_scanState.next <= chunk.data() + chunk.size());

    auto const text = std::string_view { subStart, byteCount };
    if (_scanState.utf8.expectedLength == 0)
    {
        if (!text.empty())
            _eventListener.print(text, cellCount);

        // This optimization is for the `cat`-people.
        // It further optimizes the throughput performance by bypassing
        // the FSM for the `(TEXT LF+)+`-case.
        //
        // As of bench-headless, the performance incrrease is about 50x.
        if (input != end && *input == '\n')
            _eventListener.execute(*input++);
    }

    auto const count = static_cast<size_t>(std::distance(input, _scanState.next));
    return { ProcessKind::ContinueBulk, count };
}

template <ParserEventsConcept EventListener, bool TraceStateChanges>
void Parser<EventListener, TraceStateChanges>::printUtf8Byte(char ch)
{
    unicode::ConvertResult const r = unicode::from_utf8(_scanState.utf8, (uint8_t) ch);
    if (std::holds_alternative<unicode::Incomplete>(r))
        return;

    auto constexpr ReplacementCharacter = char32_t { 0xFFFD };
    auto const codepoint = std::holds_alternative<unicode::Success>(r) ? std::get<unicode::Success>(r).value
                                                                       : ReplacementCharacter;
    _eventListener.print(codepoint);
    _scanState.lastCodepointHint = codepoint;
}

template <ParserEventsConcept EventListener, bool TraceStateChanges>
void Parser<EventListener, TraceStateChanges>::handle(ActionClass actionClass,
                                                      Action action,
                                                      uint8_t codepoint)
{
    (void) actionClass;
    auto const ch = static_cast<char>(codepoint);

    switch (action)
    {
        case Action::GroundStart: _scanState.lastCodepointHint = 0; break;
        case Action::Clear: _eventListener.clear(); break;
        case Action::CollectLeader: _eventListener.collectLeader(ch); break;
        case Action::Collect: _eventListener.collect(ch); break;
        case Action::Param: _eventListener.param(ch); break;
        case Action::ParamDigit: _eventListener.paramDigit(ch); break;
        case Action::ParamSeparator: _eventListener.paramSeparator(); break;
        case Action::ParamSubSeparator: _eventListener.paramSubSeparator(); break;
        case Action::Execute: _eventListener.execute(ch); break;
        case Action::ESC_Dispatch: _eventListener.dispatchESC(ch); break;
        case Action::CSI_Dispatch: _eventListener.dispatchCSI(ch); break;
        case Action::Print: printUtf8Byte(ch); break;
        case Action::PrintEnd: _eventListener.printEnd(); break;
        case Action::OSC_Start: _eventListener.startOSC(); break;
        case Action::OSC_Put: _eventListener.putOSC(ch); break;
        case Action::OSC_End: _eventListener.dispatchOSC(); break;
        case Action::Hook: _eventListener.hook(ch); break;
        case Action::Put: _eventListener.put(ch); break;
        case Action::Unhook: _eventListener.unhook(); break;
        case Action::APC_Start: _eventListener.startAPC(); break;
        case Action::APC_Put: _eventListener.putAPC(ch); break;
        case Action::APC_End: _eventListener.dispatchAPC(); break;
        case Action::PM_Start: _eventListener.startPM(); break;
        case Action::PM_Put: _eventListener.putPM(ch); break;
        case Action::PM_End: _eventListener.dispatchPM(); break;
        case Action::Ignore:
        case Action::Undefined: break;
    }
}

} // namespace vtparser
