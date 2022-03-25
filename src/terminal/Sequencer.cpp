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
#include <terminal/Screen.h>
#include <terminal/Sequencer.h>
#include <terminal/SixelParser.h>
#include <terminal/Terminal.h>
#include <terminal/logging.h>
#include <terminal/primitives.h>

#include <array>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

using std::array;
using std::clamp;
using std::get;
using std::holds_alternative;
using std::make_shared;
using std::make_unique;
using std::nullopt;
using std::optional;
using std::pair;
using std::string_view;
using std::unique_ptr;

using namespace std::string_view_literals;

namespace terminal
{

Sequencer::Sequencer(Terminal& _terminal): terminal_ { _terminal }
{
}

void Sequencer::error(std::string_view _errorString)
{
    if (!VTParserLog)
        return;

    VTParserLog()("Parser error: {}", _errorString);
}

void Sequencer::print(char _char)
{
    unicode::ConvertResult const r = unicode::from_utf8(terminal_.state().utf8DecoderState, (uint8_t) _char);
    if (holds_alternative<unicode::Incomplete>(r))
        return;

    static constexpr char32_t ReplacementCharacter { 0xFFFD };

    terminal_.state().instructionCounter++;
    auto const codepoint =
        holds_alternative<unicode::Success>(r) ? get<unicode::Success>(r).value : ReplacementCharacter;
    terminal_.screen().writeText(codepoint);
    terminal_.state().precedingGraphicCharacter = codepoint;
}

void Sequencer::print(string_view _chars)
{
    assert(_chars.size() != 0);

    if (terminal_.state().utf8DecoderState.expectedLength == 0)
    {
        terminal_.state().instructionCounter += _chars.size();
        terminal_.screen().writeText(_chars);
        terminal_.state().precedingGraphicCharacter = static_cast<char32_t>(_chars.back());
    }
    else
        for (char const ch: _chars)
            print(ch);
}

void Sequencer::execute(char controlCode)
{
    terminal_.screen().executeControlCode(controlCode);
}

void Sequencer::clear()
{
    sequence_.clear();
    terminal_.state().utf8DecoderState = {};
}

void Sequencer::collect(char _char)
{
    sequence_.intermediateCharacters().push_back(_char);
}

void Sequencer::collectLeader(char _leader)
{
    sequence_.setLeader(_leader);
}

void Sequencer::param(char _char)
{
    if (sequence_.parameters().empty())
        sequence_.parameters().push_back({ 0 });

    switch (_char)
    {
        case ';':
            if (sequence_.parameters().size() < Sequence::MaxParameters)
                sequence_.parameters().push_back({ 0 });
            break;
        case ':':
            if (sequence_.parameters().back().size() < Sequence::MaxParameters)
                sequence_.parameters().back().push_back({ 0 });
            break;
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            sequence_.parameters().back().back() =
                sequence_.parameters().back().back() * 10 + (Sequence::Parameter)(_char - '0');
            break;
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
    sequence_.parameters().push_back({ static_cast<Sequence::Parameter>(code) });
    sequence_.intermediateCharacters().erase(0, skipCount);
    handleSequence();
    clear();
}

void Sequencer::hook(char _finalChar)
{
    terminal_.state().instructionCounter++;
    sequence_.setCategory(FunctionCategory::DCS);
    sequence_.setFinalChar(_finalChar);

#if defined(LIBTERMINAL_LOG_TRACE)
    if (VTParserTraceLog)
        VTParserTraceLog()("Handle VT sequence: {}", sequence_);
#endif

    if (FunctionDefinition const* funcSpec = sequence_.functionDefinition(); funcSpec != nullptr)
    {
        switch (funcSpec->id())
        {
            case DECSIXEL: hookedParser_ = hookSixel(sequence_); break;
            case STP: hookedParser_ = hookSTP(sequence_); break;
            case DECRQSS: hookedParser_ = hookDECRQSS(sequence_); break;
            case XTGETTCAP: hookedParser_ = hookXTGETTCAP(sequence_); break;
        }
    }
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

unique_ptr<ParserExtension> Sequencer::hookSixel(Sequence const& _seq)
{
    auto const Pa = _seq.param_or(0, 1);
    auto const Pb = _seq.param_or(1, 2);

    auto const aspectVertical = [](int Pa) {
        switch (Pa)
        {
            case 9:
            case 8:
            case 7: return 1;
            case 6:
            case 5: return 2;
            case 4:
            case 3: return 3;
            case 2: return 5;
            case 1:
            case 0:
            default: return 2;
        }
    }(Pa);

    auto const aspectHorizontal = 1;
    auto const transparentBackground = Pb == 1;

    sixelImageBuilder_ = make_unique<SixelImageBuilder>(
        terminal_.state().maxImageSize,
        aspectVertical,
        aspectHorizontal,
        transparentBackground ? RGBAColor { 0, 0, 0, 0 } : terminal_.state().colorPalette.defaultBackground,
        terminal_.state().usePrivateColorRegisters
            ? make_shared<SixelColorPalette>(terminal_.state().maxImageRegisterCount,
                                             clamp(terminal_.state().maxImageRegisterCount, 0u, 16384u))
            : terminal_.state().imageColorPalette);

    return make_unique<SixelParser>(*sixelImageBuilder_, [this]() {
        {
            terminal_.screen().sixelImage(sixelImageBuilder_->size(), move(sixelImageBuilder_->data()));
        }
    });
}

unique_ptr<ParserExtension> Sequencer::hookSTP(Sequence const& /*_seq*/)
{
    return make_unique<SimpleStringCollector>(
        [this](string_view const& _data) { terminal_.setTerminalProfile(unicode::convert_to<char>(_data)); });
}

unique_ptr<ParserExtension> Sequencer::hookXTGETTCAP(Sequence const& /*_seq*/)
{
    // DCS + q Pt ST
    //           Request Termcap/Terminfo String (XTGETTCAP), xterm.  The
    //           string following the "q" is a list of names encoded in
    //           hexadecimal (2 digits per character) separated by ; which
    //           correspond to termcap or terminfo key names.
    //           A few special features are also recognized, which are not key
    //           names:
    //
    //           o   Co for termcap colors (or colors for terminfo colors), and
    //
    //           o   TN for termcap name (or name for terminfo name).
    //
    //           o   RGB for the ncurses direct-color extension.
    //               Only a terminfo name is provided, since termcap
    //               applications cannot use this information.
    //
    //           xterm responds with
    //           DCS 1 + r Pt ST for valid requests, adding to Pt an = , and
    //           the value of the corresponding string that xterm would send,
    //           or
    //           DCS 0 + r Pt ST for invalid requests.
    //           The strings are encoded in hexadecimal (2 digits per
    //           character).

    return make_unique<SimpleStringCollector>([this](string_view const& _data) {
        auto const capsInHex = crispy::split(_data, ';');
        for (auto hexCap: capsInHex)
        {
            auto const hexCap8 = unicode::convert_to<char>(hexCap);
            if (auto const capOpt = crispy::fromHexString(string_view(hexCap8.data(), hexCap8.size())))
                terminal_.screen().requestCapability(capOpt.value());
        }
    });
}

unique_ptr<ParserExtension> Sequencer::hookDECRQSS(Sequence const& /*_seq*/)
{
    return make_unique<SimpleStringCollector>([this](string_view const& _data) {
        auto const s = [](string_view _dataString) -> optional<RequestStatusString> {
            auto const mappings = array<pair<string_view, RequestStatusString>, 9> {
                pair { "m", RequestStatusString::SGR },       pair { "\"p", RequestStatusString::DECSCL },
                pair { " q", RequestStatusString::DECSCUSR }, pair { "\"q", RequestStatusString::DECSCA },
                pair { "r", RequestStatusString::DECSTBM },   pair { "s", RequestStatusString::DECSLRM },
                pair { "t", RequestStatusString::DECSLPP },   pair { "$|", RequestStatusString::DECSCPP },
                pair { "*|", RequestStatusString::DECSNLS }
            };
            for (auto const& mapping: mappings)
                if (_dataString == mapping.first)
                    return mapping.second;
            return nullopt;
        }(_data);

        if (s.has_value())
            terminal_.screen().requestStatusString(s.value());

        // TODO: handle batching
    });
}

void Sequencer::handleSequence()
{
    terminal_.screen().process(sequence_);
}

} // namespace terminal
