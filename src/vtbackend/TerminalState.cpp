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

#include <vtbackend/Terminal.h>
#include <vtbackend/TerminalState.h>

#include "vtbackend/SixelParser.h"

namespace terminal
{

class Terminal;

TerminalState::TerminalState(Terminal& terminal):
    settings { terminal.settings() },
    cellPixelSize {},
    effectiveImageCanvasSize { settings.maxImageSize },
    imageColorPalette { std::make_shared<sixel_color_palette>(maxImageColorRegisters,
                                                              maxImageColorRegisters) },
    imagePool { [te = &terminal](image const* image) {
        te->discardImage(*image);
    } },
    hyperlinks { hyperlink_cache { 1024 } },
    sequencer { terminal },
    parser { std::ref(sequencer) },
    viCommands { terminal },
    inputHandler { viCommands, vi_mode::Insert }
{
}

/// Applies a FunctionDefinition to a given context, emitting the respective command.
std::string to_string(ansi_mode mode)
{
    switch (mode)
    {
        case ansi_mode::KeyboardAction: return "KeyboardAction";
        case ansi_mode::Insert: return "Insert";
        case ansi_mode::SendReceive: return "SendReceive";
        case ansi_mode::AutomaticNewLine: return "AutomaticNewLine";
    }

    return fmt::format("({})", static_cast<unsigned>(mode));
}

std::string to_string(dec_mode mode)
{
    switch (mode)
    {
        case dec_mode::UseApplicationCursorKeys: return "UseApplicationCursorKeys";
        case dec_mode::DesignateCharsetUSASCII: return "DesignateCharsetUSASCII";
        case dec_mode::Columns132: return "Columns132";
        case dec_mode::SmoothScroll: return "SmoothScroll";
        case dec_mode::ReverseVideo: return "ReverseVideo";
        case dec_mode::MouseProtocolX10: return "MouseProtocolX10";
        case dec_mode::MouseProtocolNormalTracking: return "MouseProtocolNormalTracking";
        case dec_mode::MouseProtocolHighlightTracking: return "MouseProtocolHighlightTracking";
        case dec_mode::MouseProtocolButtonTracking: return "MouseProtocolButtonTracking";
        case dec_mode::MouseProtocolAnyEventTracking: return "MouseProtocolAnyEventTracking";
        case dec_mode::SaveCursor: return "SaveCursor";
        case dec_mode::ExtendedAltScreen: return "ExtendedAltScreen";
        case dec_mode::Origin: return "Origin";
        case dec_mode::AutoWrap: return "AutoWrap";
        case dec_mode::PrinterExtend: return "PrinterExtend";
        case dec_mode::LeftRightMargin: return "LeftRightMargin";
        case dec_mode::ShowToolbar: return "ShowToolbar";
        case dec_mode::BlinkingCursor: return "BlinkingCursor";
        case dec_mode::VisibleCursor: return "VisibleCursor";
        case dec_mode::ShowScrollbar: return "ShowScrollbar";
        case dec_mode::AllowColumns80to132: return "AllowColumns80to132";
        case dec_mode::DebugLogging: return "DebugLogging";
        case dec_mode::UseAlternateScreen: return "UseAlternateScreen";
        case dec_mode::BracketedPaste: return "BracketedPaste";
        case dec_mode::FocusTracking: return "FocusTracking";
        case dec_mode::NoSixelScrolling: return "NoSixelScrolling";
        case dec_mode::UsePrivateColorRegisters: return "UsePrivateColorRegisters";
        case dec_mode::MouseExtended: return "MouseExtended";
        case dec_mode::MouseSGR: return "MouseSGR";
        case dec_mode::MouseURXVT: return "MouseURXVT";
        case dec_mode::MouseSGRPixels: return "MouseSGRPixels";
        case dec_mode::MouseAlternateScroll: return "MouseAlternateScroll";
        case dec_mode::MousePassiveTracking: return "MousePassiveTracking";
        case dec_mode::ReportGridCellSelection: return "ReportGridCellSelection";
        case dec_mode::BatchedRendering: return "BatchedRendering";
        case dec_mode::Unicode: return "Unicode";
        case dec_mode::TextReflow: return "TextReflow";
        case dec_mode::SixelCursorNextToGraphic: return "SixelCursorNextToGraphic";
    }
    return fmt::format("({})", static_cast<unsigned>(mode));
}

} // namespace terminal
