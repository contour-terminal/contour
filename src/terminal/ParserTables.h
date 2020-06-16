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

#include <terminal/ControlCode.h>
#include <terminal/Parser.h>

#include <array>
#include <variant>

namespace terminal {

struct ParserTable {
    using State = Parser::State;
    using Action = Parser::Action;
    enum UnicodeCodepoint { Value = 256 };

    //! State transition map from (State, Byte) to (State).
    std::array<std::array<State, 257>, std::numeric_limits<State>::size()> transitions{
        std::array<State, 257>{State::Ground /*XXX or Undefined?*/}};

    //! actions to be invoked upon state entry
    std::array<Action, std::numeric_limits<Action>::size()> entryEvents{Action::Undefined};

    //! actions to be invoked upon state exit
    std::array<Action, std::numeric_limits<Action>::size()> exitEvents{Action::Undefined};

    //! actions to be invoked for a given (State, Byte) pair.
    std::array<std::array<Action, 257>, std::numeric_limits<Action>::size()> events;

    //! Standard state machine tables parsing VT225 to VT525.
    static constexpr ParserTable get();

    // {{{ implementation detail
    struct Range {
        uint8_t first;
        uint8_t last;
    };

    constexpr void entry(State _state, Action _action)
    {
        entryEvents[static_cast<size_t>(_state)] = _action;
    }

    constexpr void exit(State _state, Action _action)
    {
        exitEvents[static_cast<size_t>(_state)] = _action;
    }

    // Events
    constexpr void event(State _state, Action _action, uint8_t _input)
    {
        events[static_cast<size_t>(_state)][_input] = _action;
    }

    constexpr void event(State _state, Action _action, UnicodeCodepoint)
    {
        events[static_cast<size_t>(_state)][UnicodeCodepoint::Value] = _action;
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

constexpr ParserTable ParserTable::get()
{
    auto t = ParserTable{};

    // Ground
    t.event(State::Ground, Action::Execute, Range{0x00, 0x17}, 0x19, Range{0x1C, 0x1F});
    t.event(State::Ground, Action::Print, Range{0x20, 0x7F});
    t.event(State::Ground, Action::Print, Range{0xA0, 0xFF});
    t.event(State::Ground, Action::Print, UnicodeCodepoint::Value);
    //t.event(State::Ground, Action::Print, );

    // EscapeIntermediate
    t.event(State::EscapeIntermediate, Action::Execute, Range{0x00, 0x17}, 0x19, Range{0x1C, 0x1F});
    t.event(State::EscapeIntermediate, Action::Collect, Range{0x20, 0x2F});
    t.event(State::EscapeIntermediate, Action::Ignore, 0x7F);
    t.transition(State::EscapeIntermediate, State::Ground, Action::ESC_Dispatch, Range{0x30, 0x7E});

    // Escape
    t.entry(State::Escape, Action::Clear);
    t.event(State::Escape, Action::Execute, Range{0x00, 0x17}, 0x19, Range{0x1C, 0x1F});
    t.event(State::Escape, Action::Ignore, 0x7F);
    t.transition(State::Escape, State::SOS_PM_APC_String, 0x58);
    t.transition(State::Escape, State::SOS_PM_APC_String, 0x5E);
    t.transition(State::Escape, State::SOS_PM_APC_String, 0x5F);
    t.transition(State::Escape, State::DCS_Entry, 0x50);
    t.transition(State::Escape, State::OSC_String, 0x5D);
    t.transition(State::Escape, State::CSI_Entry, 0x5B);
    t.transition(State::Escape, State::Ground, Action::ESC_Dispatch, Range{0x30, 0x4F});
    t.transition(State::Escape, State::Ground, Action::ESC_Dispatch, Range{0x51, 0x57});
    t.transition(State::Escape, State::Ground, Action::ESC_Dispatch, 0x59);
    t.transition(State::Escape, State::Ground, Action::ESC_Dispatch, 0x5A);
    t.transition(State::Escape, State::Ground, Action::ESC_Dispatch, 0x5C);
    t.transition(State::Escape, State::Ground, Action::ESC_Dispatch, Range{0x60, 0x7E});
    t.transition(State::Escape, State::EscapeIntermediate, Action::Collect, Range{0x20, 0x2F});

    // SOS_PM_APC_String
    t.event(State::SOS_PM_APC_String, Action::Ignore, Range{0x00, 0x17}, 0x19, Range{0x1C, 0x1F});
    t.transition(State::SOS_PM_APC_String, State::Ground, 0x9C);

    // DCS_Entry
    t.entry(State::DCS_Entry, Action::Clear);
    t.event(State::DCS_Entry, Action::Ignore, Range{0x00, 0x17}, 0x19, Range{0x1C, 0x1F});
    t.event(State::DCS_Entry, Action::Ignore, 0x7F);
    t.transition(State::DCS_Entry, State::DCS_Intermediate, Action::Collect, Range{0x20, 0x2F});
    t.transition(State::DCS_Entry, State::DCS_Ignore, 0x3A);
    t.transition(State::DCS_Entry, State::DCS_Param, Action::Param, Range{0x30, 0x39});
    t.transition(State::DCS_Entry, State::DCS_Param, Action::Param, 0x3B);
    t.transition(State::DCS_Entry, State::DCS_Param, Action::CollectLeader, Range{0x3C, 0x3F});
    t.transition(State::DCS_Entry, State::DCS_PassThrough, Range{0x40, 0x7E});

    // DCS_Ignore
    t.event(State::DCS_Ignore, Action::Ignore, Range{0x00, 0x17}, 0x19, Range{0x1C, 0x1F}, Range{0x20, 0x7F});
    t.transition(State::DCS_Ignore, State::Ground, 0x9C);

    // DCS_Intermediate
    t.event(State::DCS_Intermediate, Action::Ignore, Range{0x00, 0x17}, 0x19, Range{0x1C, 0x1F});
    t.event(State::DCS_Intermediate, Action::Collect, Range{0x20, 0x2F});
    t.event(State::DCS_Intermediate, Action::Ignore, 0x7F);
    t.transition(State::DCS_Intermediate, State::DCS_PassThrough, Range{0x40, 0x7E});

    // DCS_PassThrough
    t.entry(State::DCS_PassThrough, Action::Hook);
    t.event(State::DCS_PassThrough, Action::Put, Range{0x00, 0x17}, 0x19, Range{0x1C, 0x1F}, Range{0x20, 0x7E});
    t.event(State::DCS_PassThrough, Action::Ignore, 0x7F);
    t.exit(State::DCS_PassThrough, Action::Unhook);
    t.transition(State::DCS_PassThrough, State::Ground, 0x9C);

    // DCS_Param
    t.event(State::DCS_Param, Action::Execute, Range{0x00, 0x17}, 0x19, Range{0x1C, 0x1F});
    t.event(State::DCS_Param, Action::Param, Range{0x30, 0x39}, 0x3B);
    t.event(State::DCS_Param, Action::Ignore, 0x7F);
    t.transition(State::DCS_Param, State::DCS_Ignore, 0x3A);
    t.transition(State::DCS_Param, State::DCS_Ignore, Range{0x3C, 0x3F});
    t.transition(State::DCS_Param, State::DCS_Intermediate, Range{0x20, 0x2F});
    t.transition(State::DCS_Param, State::DCS_PassThrough, Range{0x40, 0x7E});

    // OSC_String
	// (xterm extension to also allow BEL (0x07) as OSC terminator)
    t.entry(State::OSC_String, Action::OSC_Start);
    t.event(State::OSC_String, Action::Ignore, Range{0x00, 0x06}, Range{0x08, 0x17}, 0x19, Range{0x1C, 0x1F});
    t.event(State::OSC_String, Action::OSC_Put, Range{0x20, 0x7F});
    t.event(State::OSC_String, Action::OSC_Put, Range{0xA0, 0xFF});
    t.event(State::OSC_String, Action::OSC_Put, UnicodeCodepoint::Value);
    t.exit(State::OSC_String, Action::OSC_End);
    t.transition(State::OSC_String, State::Ground, 0x9C);
    t.transition(State::OSC_String, State::Ground, 0x07);

    // CSI_Entry
    t.entry(State::CSI_Entry, Action::Clear);
    t.event(State::CSI_Entry, Action::Execute, Range{0x00, 0x17}, 0x19, Range{0x1C, 0x1F});
    t.event(State::CSI_Entry, Action::Ignore, 0x7F);
    t.transition(State::CSI_Entry, State::Ground, Action::CSI_Dispatch, Range{0x40, 0x7E});
    t.transition(State::CSI_Entry, State::CSI_Intermediate, Action::Collect, Range{0x20, 0x2F});
    t.transition(State::CSI_Entry, State::CSI_Ignore, 0x3A);
    t.transition(State::CSI_Entry, State::CSI_Param, Action::Param, Range{0x30, 0x39});
    t.transition(State::CSI_Entry, State::CSI_Param, Action::Param, 0x3B);
    t.transition(State::CSI_Entry, State::CSI_Param, Action::CollectLeader, Range{0x3C, 0x3F});

    // CSI_Param
    t.event(State::CSI_Param, Action::Execute, Range{0x00, 0x17}, 0x19, Range{0x1C, 0x1F});
    t.event(State::CSI_Param, Action::Param, Range{0x30, 0x39});
    t.event(State::CSI_Param, Action::Param, 0x3A);
    t.event(State::CSI_Param, Action::Param, 0x3B);
    t.event(State::CSI_Param, Action::Ignore, 0x7F);
    t.transition(State::CSI_Param, State::CSI_Ignore, Range{0x3C, 0x3F});
    t.transition(State::CSI_Param, State::CSI_Intermediate, Action::Collect, Range{0x20, 0x2F});
    t.transition(State::CSI_Param, State::Ground, Action::CSI_Dispatch, Range{0x40, 0x7E});

    // CSI_Ignore
    t.event(State::CSI_Ignore, Action::Execute, Range{0x00, 0x17}, 0x19, Range{0x1C, 0x1F});
    t.event(State::CSI_Ignore, Action::Ignore, Range{0x20, 0x3F}, 0x7F);
    t.transition(State::CSI_Ignore, State::Ground, Range{0x40, 0x7E});

    // CSI_Intermediate
    t.event(State::CSI_Intermediate, Action::Execute, Range{0x00, 0x17}, 0x19, Range{0x1C, 0x1F});
    t.event(State::CSI_Intermediate, Action::Collect, Range{0x20, 0x2F});
    t.event(State::CSI_Intermediate, Action::Ignore, 0x7F);
    t.transition(State::CSI_Intermediate, State::CSI_Ignore, Range{0x30, 0x3F});
    t.transition(State::CSI_Intermediate, State::Ground, Action::CSI_Dispatch, Range{0x40, 0x7E});

    // Ground -> *
    for (State anywhere = std::numeric_limits<State>::min(); anywhere <= std::numeric_limits<State>::max(); ++anywhere)
    {
        t.transition(anywhere, State::Ground, 0x18);
        t.transition(anywhere, State::Ground, 0x1A);
        t.transition(anywhere, State::Ground, 0x9C);
        t.transition(anywhere, State::Ground, Range{0x80, 0x8F});
        t.transition(anywhere, State::Ground, Range{0x91, 0x97});

        t.transition(anywhere, State::Escape, 0x1B);

        t.transition(anywhere, State::DCS_Entry, 0x90);

        t.transition(anywhere, State::SOS_PM_APC_String, 0x98);
        t.transition(anywhere, State::SOS_PM_APC_String, 0x9E);
        t.transition(anywhere, State::SOS_PM_APC_String, 0x9F);
    }

    // TODO: verify the above is correct (programatically as much as possible)
    // TODO: see if we can reduce the preassure on L2 caches (is this even an issue?)

    return t;
}

void dot(std::ostream& _os, ParserTable const& _table);

}  // namespace terminal
