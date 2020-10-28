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

#include <terminal/Logger.h>
#include <terminal/Parser.h>
#include <terminal/ParserExtension.h>
#include <terminal/Functions.h>
#include <terminal/Commands.h>
#include <terminal/SixelParser.h>

#include <terminal/CommandBuilder.h> // only for ApplyResult

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace terminal {

class Screen;

class Sequencer {
  public:
    using ActionClass = parser::ActionClass;
    using Action = parser::Action;

    /// Constructs the sequencer stage.
    Sequencer(Screen& _screen,
              Logger _logger,
              Size _maxImageSize,
              RGBAColor _backgroundColor,
              std::shared_ptr<ColorPalette> _imageColorPalette);

    /// Constructs a very primitive Sequencer, SHOULD be used for testing only.
    Sequencer(Screen& _screen, Logger _logger)
        : Sequencer(_screen,
                    std::move(_logger),
                    Size{800, 600},
                    RGBAColor{},
                    std::make_shared<ColorPalette>()) {}

    void operator()(ActionClass _actionClass, Action _action, char32_t _finalChar)
    {
        return handleAction(_actionClass, _action, _finalChar);
    }

    void setMaxImageSize(Size _value) { maxImageSize_ = _value; }
    void setMaxImageColorRegisters(int _value) { maxImageRegisterCount_ = _value; }
    void setUsePrivateColorRegisters(bool _value) { usePrivateColorRegisters_ = _value; }

    int64_t instructionCounter() const noexcept { return instructionCounter_; }
    void resetInstructionCounter() noexcept { instructionCounter_ = 0; }

  private:
    // helper methods
    //
    std::optional<RGBColor> static parseColor(std::string_view const& _value);

    void handleAction(ActionClass _actionClass, Action _action, char32_t _finalChar);
    void executeControlFunction(char _c0);
    void dispatchESC(char _finalChar);
    void dispatchCSI(char _finalChar);
    void dispatchOSC();
    void handleSequence();

    void hookSixel(Sequence const& _ctx);
    void hookDECRQSS(Sequence const& _ctx);

    ApplyResult apply(FunctionDefinition const& _function, Sequence const& _context);

    template <typename Event, typename... Args>
    void log(Args&&... args) const
    {
        if (logger_)
            logger_(Event{ std::forward<Args>(args)... });
    }

  private:
    Sequence sequence_{};
    Screen& screen_;
    bool batching_ = false;
    int64_t instructionCounter_ = 0;
    using Batchable = std::variant<Sequence, SixelImage>;
    std::vector<Batchable> batchedSequences_;

    Logger const logger_;

    std::unique_ptr<ParserExtension> hookedParser_;
    std::unique_ptr<SixelImageBuilder> sixelImageBuilder_;
    std::shared_ptr<ColorPalette> imageColorPalette_;
    bool usePrivateColorRegisters_ = false;
    Size maxImageSize_;
    int maxImageRegisterCount_;
    RGBAColor backgroundColor_;
};

}  // namespace terminal

