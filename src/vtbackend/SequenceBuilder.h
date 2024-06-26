// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Sequence.h>
#include <vtbackend/SixelParser.h>
#include <vtbackend/logging.h>

#include <vtparser/Parser.h>
#include <vtparser/ParserExtension.h>

#include <concepts>
#include <memory>
#include <string_view>

namespace vtbackend
{

template <typename T>
concept InstructionCounterConcept = requires(T t, size_t n) {
    { t() } -> std::same_as<void>;
    { t(n) } -> std::same_as<void>;
};

struct NoOpInstructionCounter
{
    void operator()(size_t /*increment*/ = 1) const noexcept {}
};

/// SequenceBuilder - The semantic VT analyzer layer.
///
/// SequenceBuilder implements the translation from VT parser events, forming a higher level Sequence,
/// that can be matched against FunctionDefinition objects and then handled on the currently active Screen.
template <SequenceHandlerConcept Handler, InstructionCounterConcept IncrementInstructionCounter>
class SequenceBuilder
{
  public:
    explicit SequenceBuilder(Handler handler, IncrementInstructionCounter incrementInstructionCounter):
        _sequence {},
        _parameterBuilder { _sequence.parameters() },
        _incrementInstructionCounter { std::move(incrementInstructionCounter) },
        _handler { std::move(handler) }
    {
    }

    // {{{ ParserEvents interface
    void error(std::string_view errorString)
    {
        if (vtParserLog)
            vtParserLog()("Parser error: {}", errorString);
    }
    void print(char32_t codepoint)
    {
        if (vtParserLog)
        {
            if (codepoint < 0x80 && std::isprint(static_cast<char>(codepoint)))
                vtParserLog()("Print: '{}'", static_cast<char>(codepoint));
            else
                vtParserLog()("Print: U+{:X}", (unsigned) codepoint);
        }
        _incrementInstructionCounter();
        _handler.writeText(codepoint);
    }

    size_t print(std::string_view chars, size_t cellCount)
    {
        if (vtParserLog)
            vtParserLog()("Print: ({}) '{}'", cellCount, crispy::escape(chars));

        assert(!chars.empty());

        _incrementInstructionCounter(cellCount);
        _handler.writeText(chars, cellCount);
        return _handler.maxBulkTextSequenceWidth();
    }

    void printEnd()
    {
        if (vtParserLog)
            vtParserLog()("PrintEnd");

        _handler.writeTextEnd();
    }

    void execute(char controlCode) { _handler.executeControlCode(controlCode); }

    void clear() noexcept
    {
        _sequence.clearExceptParameters();
        _parameterBuilder.reset();
    }

    void collect(char ch) { _sequence.intermediateCharacters().push_back(ch); }

    void collectLeader(char leader) noexcept { _sequence.setLeader(leader); }

    void param(char ch) noexcept
    {
        switch (ch)
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
            case '9': paramDigit(ch); break;
            default: crispy::unreachable();
        }
    }

    void paramDigit(char ch) noexcept
    {
        _parameterBuilder.multiplyBy10AndAdd(static_cast<uint8_t>(ch - '0'));
    }

    void paramSeparator() noexcept { _parameterBuilder.nextParameter(); }
    void paramSubSeparator() noexcept { _parameterBuilder.nextSubParameter(); }

    void dispatchESC(char finalChar)
    {
        _sequence.setCategory(FunctionCategory::ESC);
        _sequence.setFinalChar(finalChar);
        handleSequence();
    }

    void dispatchCSI(char finalChar)
    {
        _sequence.setCategory(FunctionCategory::CSI);
        _sequence.setFinalChar(finalChar);
        handleSequence();
    }

    void startOSC() { _sequence.setCategory(FunctionCategory::OSC); }

    void putOSC(char ch)
    {
        if (_sequence.intermediateCharacters().size() + 1 < Sequence::MaxOscLength)
            _sequence.intermediateCharacters().push_back(ch);
    }

    void dispatchOSC()
    {
        auto const [code, skipCount] = vtparser::extractCodePrefix(_sequence.intermediateCharacters());
        _parameterBuilder.set(static_cast<Sequence::Parameter>(code));
        _sequence.intermediateCharacters().erase(0, skipCount);
        handleSequence();
        clear();
    }

    void hook(char finalChar)
    {
        _incrementInstructionCounter();
        _sequence.setCategory(FunctionCategory::DCS);
        _sequence.setFinalChar(finalChar);

        handleSequence();
    }
    void put(char ch)
    {
        if (_hookedParser)
            _hookedParser->pass(ch);
    }
    void unhook()
    {
        if (_hookedParser)
        {
            _hookedParser->finalize();
            _hookedParser.reset();
        }
    }
    void startAPC() {}
    void putAPC(char) {}
    void dispatchAPC() {}
    void startPM() {}
    void putPM(char) {}
    void dispatchPM() {}

    void hookParser(std::unique_ptr<ParserExtension> parserExtension) noexcept
    {
        _hookedParser = std::move(parserExtension);
    }

    [[nodiscard]] size_t maxBulkTextSequenceWidth() const noexcept
    {
        return _handler.maxBulkTextSequenceWidth();
    }
    // }}}

  private:
    void handleSequence()
    {
        _parameterBuilder.fixiate();
        _handler.processSequence(_sequence);
    }

    Sequence _sequence {};
    SequenceParameterBuilder _parameterBuilder;
    IncrementInstructionCounter _incrementInstructionCounter;
    Handler _handler;

    std::unique_ptr<ParserExtension> _hookedParser {};
};

template <SequenceHandlerConcept Handler, InstructionCounterConcept IncrementInstructionCounter>
SequenceBuilder(Handler&,
                IncrementInstructionCounter) -> SequenceBuilder<Handler, IncrementInstructionCounter>;

} // namespace vtbackend
