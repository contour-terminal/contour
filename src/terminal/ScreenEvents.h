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

#include <terminal/InputGenerator.h>
#include <terminal/Sequencer.h>

#include <optional>
#include <string_view>
#include <vector>

namespace terminal {

class Image;

enum class ScreenType {
    Main = 0,
    Alternate = 1
};

class ScreenEvents {
  public:
    virtual ~ScreenEvents() = default;

    virtual std::optional<RGBColor> requestDynamicColor(DynamicColorName /*_name*/) { return std::nullopt; }
    virtual void bell() {}
    virtual void bufferChanged(ScreenType) {}
    virtual void scrollbackBufferCleared() {}
    virtual void commands() {}
    virtual void copyToClipboard(std::string_view const& /*_data*/) {}
    virtual void dumpState() {}
    virtual void notify(std::string_view const& /*_title*/, std::string_view const& /*_body*/) {}
    virtual void reply(std::string_view const& /*_response*/) {}
    virtual void resetDynamicColor(DynamicColorName /*_name*/) {}
    virtual void resizeWindow(int /*_width*/, int /*_height*/, bool /*_unitInPixels*/) {}
    virtual void setApplicationkeypadMode(bool /*_enabled*/) {}
    virtual void setBracketedPaste(bool /*_enabled*/) {}
    virtual void setCursorStyle(CursorDisplay, CursorShape) {}
    virtual void setCursorVisibility(bool /*_visible*/) {}
    virtual void setDynamicColor(DynamicColorName, RGBColor const&) {}
    virtual void setGenerateFocusEvents(bool /*_enabled*/) {}
    virtual void setMouseProtocol(MouseProtocol, bool) {}
    virtual void setMouseTransport(MouseTransport) {}
    virtual void setMouseWheelMode(InputGenerator::MouseWheelMode) {}
    virtual void setWindowTitle(std::string_view const& /*_title*/) {}
    virtual void useApplicationCursorKeys(bool /*_enabled*/) {}

    // Invoked by screen buffer when an image is not being referenced by any grid cell anymore.
    virtual void discardImage(Image const&) {}
};

class MockScreenEvents : public ScreenEvents {
  public:
    void reply(std::string_view const& _response) override
    {
        replyData += _response;
    }

  public:
    std::string replyData;
};

} // end namespace
