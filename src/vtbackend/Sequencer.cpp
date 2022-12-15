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
#include <vtbackend/Screen.h>
#include <vtbackend/Sequencer.h>
#include <vtbackend/SixelParser.h>
#include <vtbackend/Terminal.h>
#include <vtbackend/logging.h>
#include <vtbackend/primitives.h>

#include <string_view>
#include <utility>

using std::get;
using std::holds_alternative;
using std::string_view;

using namespace std::string_view_literals;

namespace terminal
{

Sequencer::Sequencer(Terminal& _terminal):
    terminal_ { _terminal }, parameterBuilder_ { sequence_.parameters() }
{
}

void Sequencer::error(std::string_view _errorString)
{
    if (VTParserLog)
        VTParserLog()("Parser error: {}", _errorString);
}

void Sequencer::print(char32_t codepoint)
{
    terminal_.state().instructionCounter++;
    terminal_.activeDisplay().writeText(codepoint);
}

size_t Sequencer::print(string_view _chars, size_t cellCount)
{
    assert(_chars.size() != 0);

    terminal_.state().instructionCounter += _chars.size();
    terminal_.activeDisplay().writeText(_chars, cellCount);

    return terminal_.state().pageSize.columns.as<size_t>()
           - terminal_.state().cursor.position.column.as<size_t>();
}

void Sequencer::execute(char controlCode)
{
    terminal_.activeDisplay().executeControlCode(controlCode);
}

void Sequencer::collect(char _char)
{
    sequence_.intermediateCharacters().push_back(_char);
}

void Sequencer::collectLeader(char _leader) noexcept
{
    sequence_.setLeader(_leader);
}

void Sequencer::param(char _char) noexcept
{
    switch (_char)
    {
        case ';': paramSeparator(); break;
        case ':': paramSubSeparator(); break;
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9': paramDigit(_char); break;
    }
}

void Sequencer::dispatchESC(char _finalChar)
{
    sequence_.setCategory(FunctionCategory::ESC);
    sequence_.setFinalChar(_finalChar);
    handleSequence();
}

void Sequencer::dispatchCSI(char _finalChar)
{
    sequence_.setCategory(FunctionCategory::CSI);
    sequence_.setFinalChar(_finalChar);
    handleSequence();
}

void Sequencer::startOSC()
{
    sequence_.setCategory(FunctionCategory::OSC);
}

void Sequencer::putOSC(char _char)
{
    if (sequence_.intermediateCharacters().size() + 1 < Sequence::MaxOscLength)
        sequence_.intermediateCharacters().push_back(_char);
}

void Sequencer::dispatchOSC()
{
    auto const [code, skipCount] = parser::extractCodePrefix(sequence_.intermediateCharacters());
    parameterBuilder_.set(static_cast<Sequence::Parameter>(code));
    sequence_.intermediateCharacters().erase(0, skipCount);
    handleSequence();
    clear();
}

void Sequencer::hook(char _finalChar)
{
    terminal_.state().instructionCounter++;
    sequence_.setCategory(FunctionCategory::DCS);
    sequence_.setFinalChar(_finalChar);

    handleSequence();
}

void Sequencer::put(char _char)
{
    if (hookedParser_)
        hookedParser_->pass(_char);
}

void Sequencer::unhook()
{
    if (hookedParser_)
    {
        hookedParser_->finalize();
        hookedParser_.reset();
    }
}

size_t Sequencer::maxBulkTextSequenceWidth() const noexcept
{
    if (!terminal_.isPrimaryScreen())
        return 0;

    if (!terminal_.primaryScreen().currentLine().isTrivialBuffer())
        return 0;

    assert(terminal_.currentScreen().margin().horizontal.to >= terminal_.cursor().position.column);
    return unbox<size_t>(terminal_.currentScreen().margin().horizontal.to
                         - terminal_.cursor().position.column);
}

void Sequencer::handleSequence()
{
    parameterBuilder_.fixiate();
    terminal_.activeDisplay().processSequence(sequence_);
}

} // namespace terminal
