// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/InputGenerator.h>
#include <vtbackend/primitives.h>

#include <optional>
#include <string_view>
#include <vector>

namespace vtbackend
{

class Image;

struct FontDef
{
    double size;
    std::string regular;
    std::string bold;
    std::string italic;
    std::string boldItalic;
    std::string emoji;
};

class ScreenEvents
{
  public:
    virtual ~ScreenEvents() = default;

    virtual void requestCaptureBuffer(int /*_absoluteStartLine*/, int /*_lineCount*/) {}
    virtual void bell() {}
    virtual void bufferChanged(ScreenType) {}
    virtual void scrollbackBufferCleared() {}
    virtual void screenUpdated() {}
    virtual FontDef getFontDef() { return {}; }
    virtual void setFontDef(FontDef const& /*_fontDef*/) {}
    virtual void copyToClipboard(std::string_view /*_data*/) {}
    virtual void inspect() {}
    virtual void notify(std::string_view /*_title*/, std::string_view /*_body*/) {}
    virtual void reply(std::string_view /*_response*/) {}
    virtual void resizeWindow(PageSize) {}
    virtual void resizeWindow(ImageSize) {}
    virtual void setApplicationkeypadMode(bool /*_enabled*/) {}
    virtual void setBracketedPaste(bool /*_enabled*/) {}
    virtual void setCursorStyle(CursorDisplay, CursorShape) {}
    virtual void setCursorVisibility(bool /*_visible*/) {}
    virtual void setGenerateFocusEvents(bool /*_enabled*/) {}
    virtual void setMouseProtocol(MouseProtocol, bool) {}
    virtual void setMouseTransport(MouseTransport) {}
    virtual void setMouseWheelMode(InputGenerator::MouseWheelMode) {}
    virtual void setWindowTitle(std::string_view /*_title*/) {}
    virtual void useApplicationCursorKeys(bool /*_enabled*/) {}
    virtual void hardReset() {}
    virtual void markCellDirty(CellLocation /*_position*/) noexcept {}
    virtual void markRegionDirty(Rect /*_region*/) noexcept {}
    virtual void synchronizedOutput(bool /*_enabled*/) {}
    virtual void onBufferScrolled(LineCount /*_n*/) noexcept {}

    // Invoked by screen buffer when an image is not being referenced by any grid cell anymore.
    virtual void discardImage(Image const&) {}

    /// Invoked upon `DCS $ p <profile-name> ST` to change terminal's currently active profile name.
    virtual void setTerminalProfile(std::string const& /*_configProfileName*/) {}
};

class MockScreenEvents: public ScreenEvents
{
  public:
    void reply(std::string_view response) override { replyData += response; }

    template <typename... T>
    void reply(fmt::format_string<T...> fmt, T&&... args)
    {
        reply(fmt::vformat(fmt, fmt::make_format_args(args...)));
    }

    void setWindowTitle(std::string_view title) override { windowTitle = title; }

    std::string replyData;
    std::string windowTitle;
};

} // namespace vtbackend
