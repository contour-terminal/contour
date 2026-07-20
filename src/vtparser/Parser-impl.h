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

// clang-format off
/// Spells a byte constant in the parser tables below, e.g. @c 0x1B_b.
constexpr uint8_t operator""_b(unsigned long long value)
{
    return static_cast<uint8_t>(value);
}
// clang-format on

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
    // 8-bit ST (0x9C) is handled in processOnceViaStateMachine, at a UTF-8 boundary.

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
    t.transition(State::DCS_Param,
                 State::DCS_Intermediate,
                 Action::Collect,
                 Range { .first = 0x20_b, .last = 0x2F_b });
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
    t.transition(State::OSC_String, State::Ground, 0x07_b);

    // APC_String
    // APC := ESC _ ... ST
    t.entry(State::APC_String, Action::APC_Start);
    t.event(State::APC_String, Action::APC_Put, Range { .first = 0x20_b, .last = 0x7F_b });
    t.event(State::APC_String, Action::APC_Put, Range { .first = 0xA0_b, .last = 0xFF_b });
    t.event(State::APC_String, Action::APC_Put, UnicodeRange);
    t.exit(State::APC_String, Action::APC_End);
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
    auto const* input = data.data();
    auto const* const end = data.data() + data.size();

    while (input != end)
    {
        auto const [processKind, processedByteCount] = _state == State::DCS_PassThrough
                                                           ? parseBulkDcsPassThrough(input, end)
                                                           : parseBulkText(input, end);
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

/// True for the states that collect a string body (OSC/APC/PM/SOS/DCS), where an 8-bit ST must be
/// distinguished from a UTF-8 continuation byte.
constexpr bool isStringCollectingState(State s) noexcept
{
    switch (s)
    {
        case State::OSC_String:
        case State::APC_String:
        case State::PM_String:
        case State::IgnoreUntilST:
        case State::DCS_PassThrough:
        case State::DCS_Ignore: return true;
        default: return false;
    }
}

template <ParserEventsConcept EventListener, bool TraceStateChanges>
auto Parser<EventListener, TraceStateChanges>::parseBulkDcsPassThrough(char const* begin,
                                                                       char const* end) noexcept
    -> std::tuple<ProcessKind, size_t>
{
    // The passthrough state hands every printable byte to the handler unchanged and acts on nothing
    // else, so a run of them needs no per-byte decision at all. Only these are plain payload:
    // anything below 0x20 can terminate or be executed, and 0x7F is ignored rather than passed --
    // so both stop the run and let the state machine have the byte.
    auto const* input = begin;
    while (input != end && static_cast<uint8_t>(*input) >= 0x20 && static_cast<uint8_t>(*input) < 0x7F)
        ++input;

    auto const byteCount = static_cast<size_t>(std::distance(begin, input));
    if (byteCount == 0)
        return { ProcessKind::FallbackToFSM, 0 };

    auto const payload = std::string_view { begin, byteCount };
    // Offering the bulk form is optional: the concept only asks for put(char), so a listener that
    // has nothing better to do with a run still gets it a byte at a time -- just without the state
    // machine in between, which is where the cost was.
    if constexpr (requires { _eventListener.put(payload); })
        _eventListener.put(payload);
    else
        for (auto const ch: payload)
            _eventListener.put(ch);

    return { ProcessKind::ContinueBulk, byteCount };
}

template <ParserEventsConcept EventListener, bool TraceStateChanges>
void Parser<EventListener, TraceStateChanges>::processOnceViaStateMachine(uint8_t ch)
{
    ParserTable static constexpr Table = ParserTable::get();

    // VT52 has its own escape grammar. processVT52() consumes the byte when it is part of a VT52
    // command (ESC, the command letter, or the ESC Y coordinates); a byte it does not consume (a
    // printable or a C0 control in VT52 ground) falls through to the normal Ground handling below.
    if (_vt52Mode && processVT52(ch))
        return;

    // Inside a string body an 8-bit ST (0x9C) terminates the string, but only at a UTF-8 character
    // boundary: string content may be UTF-8, and 0x9C is a valid continuation byte (e.g. the middle
    // byte of U+2705, 0xE2 0x9C 0x85). Track the continuation bytes still expected so the terminator is
    // told apart from the content.
    if (isStringCollectingState(_state))
    {
        // A byte only advances an in-progress multi-byte sequence when it is a genuine UTF-8
        // continuation byte (0x80-0xBF). Trusting the pending count alone is a desync bug: a malformed
        // lead (a lead byte not followed by continuation bytes) would keep the count > 0 and let it
        // mask a following 8-bit ST as content, so the string would never terminate. On any
        // non-continuation byte, abandon the sequence and judge this byte on its own.
        auto const isContinuationByte = (ch & 0xC0) == 0x80;
        if (_stringUtf8Pending != 0 && isContinuationByte)
            --_stringUtf8Pending; // a continuation byte; fall through to collect it
        else
        {
            _stringUtf8Pending = 0;
            if (ch == 0x9C)
            {
                // 8-bit ST at a character boundary: leave the string exactly as the 7-bit ESC \ does.
                handle(ActionClass::Leave, Table.exitEvents[static_cast<size_t>(_state)], ch);
                _state = State::Ground;
                handle(ActionClass::Enter, Table.entryEvents[static_cast<size_t>(State::Ground)], ch);
                return;
            }
            if (ch >= 0xF0)
                _stringUtf8Pending = 3; // start of a 4-byte UTF-8 sequence
            else if (ch >= 0xE0)
                _stringUtf8Pending = 2; // 3-byte
            else if (ch >= 0xC0)
                _stringUtf8Pending = 1; // 2-byte
        }
    }

    auto const s = static_cast<size_t>(_state);

    if (auto const t = Table.transitions[s][ch]; t != State::Undefined)
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
bool Parser<EventListener, TraceStateChanges>::processVT52(uint8_t ch)
{
    switch (_vt52State)
    {
        case Vt52State::Ground:
            if (ch == 0x1B) // ESC begins a VT52 command.
            {
                _vt52State = Vt52State::Escape;
                return true;
            }
            return false; // printable text or a C0 control: handle it as in ANSI ground.
        case Vt52State::Escape:
            if (ch == 'Y') // ESC Y <row> <col>: direct cursor address.
            {
                _vt52State = Vt52State::CursorRow;
                return true;
            }
            if (ch >= 0x20 && ch <= 0x2F) // ESC <space..slash>: an unimplemented 2-byte VT52 sequence.
            {
                _vt52State = Vt52State::Ignore;
                return true;
            }
            // Every other byte is a complete single-character VT52 command (including '<' to leave
            // VT52). Unknown ones are dispatched too; the handler treats them as no-ops.
            _eventListener.dispatchVT52(static_cast<char>(ch), 0, 0);
            _vt52State = Vt52State::Ground;
            return true;
        case Vt52State::CursorRow:
            _vt52CursorRow = ch;
            _vt52State = Vt52State::CursorColumn;
            return true;
        case Vt52State::CursorColumn: {
            // Each coordinate byte encodes value + 0x20; recover the 1-based row/column. A byte below
            // 0x20 is out of range and clamps to the first row/column.
            auto const row =
                1u + (_vt52CursorRow >= 0x20 ? static_cast<unsigned>(_vt52CursorRow - 0x20) : 0u);
            auto const column = 1u + (ch >= 0x20 ? static_cast<unsigned>(ch - 0x20) : 0u);
            _eventListener.dispatchVT52('Y', row, column);
            _vt52State = Vt52State::Ground;
            return true;
        }
        case Vt52State::Ignore: // swallow the second byte of the unimplemented sequence.
            _vt52State = Vt52State::Ground;
            return true;
    }
    return false;
}

template <ParserEventsConcept EventListener, bool TraceStateChanges>
auto Parser<EventListener, TraceStateChanges>::parseBulkText(char const* begin, char const* end) noexcept
    -> std::tuple<ProcessKind, size_t>
{
    auto const* input = begin;
    if (_state != State::Ground)
        return { ProcessKind::FallbackToFSM, 0 };

    // In VT52 mode, once an ESC has been seen the following byte(s) form a VT52 command, not text, so
    // they must go through the state machine (processVT52) rather than the bulk text scanner.
    if (_vt52State != Vt52State::Ground)
        return { ProcessKind::FallbackToFSM, 0 };

    // If we have pending incomplete UTF-8 from a previous parse call, fall back to FSM.
    // This is necessary because unicode::scan_text assumes contiguous memory when
    // continuing an incomplete sequence, but parse calls can span different buffers.
    // The FSM's printUtf8Byte function correctly handles cross-buffer UTF-8 sequences.
    if (_scanState.utf8.expectedLength != 0)
        return { ProcessKind::FallbackToFSM, 0 };

    // A leading 8-bit C1 control (0x80..0x9F) is not text: the bulk scanner would consume it as an
    // invalid UTF-8 byte and print U+FFFD, so route it through the state machine, where printUtf8Byte
    // recognises it as the control it is. (It cannot be a UTF-8 continuation byte here -- we are in the
    // Ground state with no pending sequence.)
    if (auto const first = static_cast<uint8_t>(*input); first >= 0x80 && first <= 0x9F)
        return { ProcessKind::FallbackToFSM, 0 };

    auto const maxCharCount = _eventListener.maxBulkTextSequenceWidth();
    if (!maxCharCount)
        return { ProcessKind::FallbackToFSM, 0 };

    _scanState.next = nullptr;
    // scan_text() stops at a mid-run 8-bit C1 control and leaves it for the state machine (libunicode
    // >= 0.9.1, guaranteed by the CMake version floor), so the whole buffer is handed to it.
    auto const chunk = std::string_view(input, static_cast<size_t>(end - input));
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

    // Always print the complete text portion (subStart to subEnd), even if there's
    // incomplete UTF-8 at the end. The incomplete bytes are between subEnd and
    // _scanState.next and will be completed on the next parse call.
    // BUG FIX: Previously, when utf8.expectedLength != 0, the entire text was
    // skipped but bytes were still consumed, causing text corruption.
    if (!text.empty())
    {
        _eventListener.print(text, cellCount);

        // Update lastCodepointHint for REP (CSI Ps b) support.
        // scan_text's ASCII fast-path (scan_for_text_ascii) doesn't update this field.
        // The non-ASCII path (scan_for_text_nonascii) already sets it correctly.
        if (auto const lastByte = static_cast<uint8_t>(text.back()); lastByte < 0x80)
            _scanState.lastCodepointHint = static_cast<char32_t>(lastByte);
    }

    if (_scanState.utf8.expectedLength == 0)
    {
        // Process trailing C0 control characters inline, bypassing the FSM.
        // This handles the common (TEXT C0)+ pattern (e.g. text followed by LF, HT, CR)
        // with a significant throughput improvement (~50x for cat-like workloads).
        //
        // C0 Execute range: 0x00-0x17, 0x19, 0x1C-0x1F (bits in a 32-bit mask)
        // Excluded: 0x18 (CAN) and 0x1A (SUB) — trigger state transitions (Ground + Ignore)
        // Excluded: 0x1B (ESC) — transitions to Escape state
        constexpr auto C0ExecuteMask =
            uint32_t { 0xFFFFFFFF } & ~(1u << 0x18) & ~(1u << 0x1A) & ~(1u << 0x1B);
        auto const* current = _scanState.next;
        while (current != end)
        {
            auto const ch = static_cast<uint8_t>(*current);
            if (ch < 0x20 && (C0ExecuteMask & (1u << ch)) != 0)
            {
                _eventListener.execute(*current);
                ++current;
            }
            else
                break;
        }
        _scanState.next = current;
    }

    auto const count = static_cast<size_t>(std::distance(input, _scanState.next));
    return { ProcessKind::ContinueBulk, count };
}

template <ParserEventsConcept EventListener, bool TraceStateChanges>
void Parser<EventListener, TraceStateChanges>::printUtf8Byte(char ch)
{
    auto const byte = static_cast<uint8_t>(ch);

    // An 8-bit C1 control (0x80..0x9F) that *begins* a character -- i.e. is not a UTF-8 continuation
    // byte in a multi-byte sequence -- is a control, not text. DEC terminals and esctest transmit
    // these as raw single bytes (Latin-1), which are not valid UTF-8, so they must be caught here
    // before from_utf8() would fold them to U+FFFD. A C1 control is exactly ESC followed by
    // (byte - 0x40), so replay that pair through the state machine: this reuses every bit of the 7-bit
    // ESC handling (CSI/DCS/OSC entry, single dispatch, string collection) with no duplication.
    if (_scanState.utf8.expectedLength == 0 && byte >= 0x80 && byte <= 0x9F)
    {
        processOnceViaStateMachine(0x1B);                              // ESC
        processOnceViaStateMachine(static_cast<uint8_t>(byte - 0x40)); // the equivalent 7-bit byte
        return;
    }

    unicode::ConvertResult const r = unicode::from_utf8(_scanState.utf8, byte);
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
        case Action::GroundStart:
            _scanState.lastCodepointHint = 0;
            _stringUtf8Pending = 0; // a string can only start from Ground, so this is a clean slate
            break;
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
