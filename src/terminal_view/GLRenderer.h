#pragma once

#include <terminal/Logger.h>
#include <terminal/Terminal.h>

#include <terminal_view/CellBackground.h>
#include <terminal_view/FontManager.h>
#include <terminal_view/GLCursor.h>
#include <terminal_view/GLTextShaper.h>

#include <QPoint>
#include <QMatrix2x4>
#include <QOpenGLFunctions>

#include <chrono>
#include <vector>
#include <utility>

namespace terminal::view {

class GLRenderer : public QOpenGLFunctions {
  public:
    GLRenderer(Logger _logger,
               Font& _regularFont,
               ColorProfile const& _colorProfile,
               Opacity _backgroundOpacity,
               QMatrix4x4 const& _projectionMatrix);

    size_t cellHeight() const noexcept { return regularFont_.get().lineHeight(); }
    size_t cellWidth() const noexcept { return regularFont_.get().maxAdvance(); }

    void setBackgroundOpacity(terminal::Opacity _opacity);
    void setCursorColor(RGBColor const& _color);
    void setFont(Font& _font);
    bool setFontSize(unsigned int _fontSize);
    void setProjection(QMatrix4x4 const& _projectionMatrix);

    void render(Terminal const& _terminal, std::chrono::steady_clock::time_point _now);

  private:
    void fillCellGroup(cursor_pos_t _row, cursor_pos_t _col, ScreenBuffer::Cell const& _cell, WindowSize const& _screenSize);
    void renderCellGroup(WindowSize const& _screenSize);

    QPoint makeCoords(cursor_pos_t _col, cursor_pos_t _row, WindowSize const& _screenSize) const;
    std::pair<QVector4D, QVector4D> makeColors(ScreenBuffer::GraphicsAttributes const& _attributes) const;

  private:
    /// Holds an array of directly connected characters on a single line that all share the same visual attributes.
    struct PendingDraw {
        cursor_pos_t lineNumber{};
        cursor_pos_t startColumn{};
        ScreenBuffer::GraphicsAttributes attributes{};
        std::vector<char32_t> text{};

        void reset(cursor_pos_t _row, cursor_pos_t _col, ScreenBuffer::GraphicsAttributes const& _attributes, char32_t _char)
        {
            lineNumber = _row;
            startColumn = _col;
            attributes = _attributes;
            text.clear();
            text.push_back(_char);
        }
    };

    PendingDraw pendingDraw_;
    Margin margin_{};
    Logger logger_;

    ColorProfile const& colorProfile_;
    Opacity backgroundOpacity_;

    std::reference_wrapper<Font> regularFont_;
    GLTextShaper textShaper_;
    CellBackground cellBackground_;
    GLCursor cursor_;
};

}
