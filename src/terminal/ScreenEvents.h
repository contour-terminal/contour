#pragma once

#include <terminal/Commands.h>
#include <terminal/InputGenerator.h>
#include <terminal/ScreenBuffer.h> // ScreenBuffer::Type

#include <optional>
#include <string_view>
#include <vector>

namespace terminal {

class ScreenEvents {
  public:
    virtual ~ScreenEvents() = default;

    virtual std::optional<RGBColor> requestDynamicColor(DynamicColorName /*_name*/) { return std::nullopt; }
    virtual void bell() {}
    virtual void bufferChanged(ScreenBuffer::Type) {}
    virtual void commands(CommandList const& /*_commands*/) {}
    virtual void copyToClipboard(std::string_view const& /*_data*/) {}
    virtual void dumpState() {}
    virtual void notify(std::string_view const& /*_title*/, std::string_view const& /*_body*/) {}
    virtual void reply(std::string_view const& /*_response*/) {}
    virtual void resetDynamicColor(DynamicColorName /*_name*/) {}
    virtual void resizeWindow(unsigned /*_width*/, unsigned /*_height*/, bool /*_unitInPixels*/) {}
    virtual void setApplicationkeypadMode(bool /*_enabled*/) {}
    virtual void setBracketedPaste(bool /*_enabled*/) {}
    virtual void setCursorStyle(CursorDisplay, CursorShape) {}
    virtual void setDynamicColor(DynamicColorName, RGBColor const&) {}
    virtual void setGenerateFocusEvents(bool /*_enabled*/) {}
    virtual void setMouseProtocol(MouseProtocol, bool) {}
    virtual void setMouseTransport(MouseTransport) {}
    virtual void setMouseWheelMode(InputGenerator::MouseWheelMode) {}
    virtual void setWindowTitle(std::string_view const& /*_title*/) {}
    virtual void useApplicationCursorKeys(bool /*_enabled*/) {}
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
