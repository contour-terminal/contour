#include <terminal_view/GLRenderer.h>

using namespace std;
using namespace std::chrono;
using namespace std::placeholders;

using namespace terminal;
using namespace terminal::view;

inline glm::vec4 makeColor(RGBColor const& _rgb, Opacity _opacity = Opacity::Opaque)
{
    return glm::vec4{
        _rgb.red / 255.0,
        _rgb.green / 255.0,
        _rgb.blue / 255.0,
        static_cast<unsigned>(_opacity) / 255.0
    };
}

GLRenderer::GLRenderer(GLLogger& _logger,
                       Font& _regularFont,
                       terminal::ColorProfile const& _colorProfile,
                       terminal::Opacity _backgroundOpacity,
                       glm::mat4 const& _projectionMatrix) :
    logger_{ _logger },
    colorProfile_{ _colorProfile },
    backgroundOpacity_{ _backgroundOpacity },
    regularFont_{ _regularFont },
    textShaper_{ regularFont_.get(), _projectionMatrix },
    cellBackground_{
        glm::vec2{
            regularFont_.get().maxAdvance(),
            regularFont_.get().lineHeight()
        },
        _projectionMatrix
    },
    cursor_{
        glm::ivec2{
            regularFont_.get().maxAdvance(),
            regularFont_.get().lineHeight(),
        },
        _projectionMatrix,
        CursorShape::Block, // TODO: should not be hard-coded; actual value be passed via render(terminal, now);
        makeColor(colorProfile_.cursor)
    }
{
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void GLRenderer::setFont(Font& _font)
{
    auto const fontSize = regularFont_.get().fontSize();
    regularFont_ = _font;
    regularFont_.get().setFontSize(fontSize);
    textShaper_.setFont(regularFont_.get());
}

bool GLRenderer::setFontSize(unsigned int _fontSize)
{
    if (_fontSize == regularFont_.get().fontSize())
        return false;

    regularFont_.get().setFontSize(_fontSize);
    // TODO: other font styles
    textShaper_.clearGlyphCache();
    cellBackground_.resize(glm::ivec2{regularFont_.get().maxAdvance(), regularFont_.get().lineHeight()});
    cursor_.resize(glm::ivec2{regularFont_.get().maxAdvance(), regularFont_.get().lineHeight()});
    // TODO update margins?

    return true;
}

void GLRenderer::setProjection(glm::mat4 const& _projectionMatrix)
{
    cellBackground_.setProjection(_projectionMatrix);
    textShaper_.setProjection(_projectionMatrix);
    cursor_.setProjection(_projectionMatrix);
}

void GLRenderer::setBackgroundOpacity(terminal::Opacity _opacity)
{
    backgroundOpacity_ = _opacity;
}

void GLRenderer::setCursorColor(terminal::RGBColor const& _color)
{
	cursor_.setColor(makeColor(_color));
}

void GLRenderer::render(Terminal const& _terminal, steady_clock::time_point _now)
{
    _terminal.render(bind(&GLRenderer::fillCellGroup, this, _1, _2, _3, _terminal.screenSize()), _now);
    renderCellGroup(_terminal.screenSize());

    // TODO: check if CursorStyle has changed, and update render context accordingly.
    if (_terminal.shouldDisplayCursor() && _terminal.scrollOffset() + _terminal.cursor().row <= _terminal.screenSize().rows)
    {
        cursor_.setShape(_terminal.cursorShape());
        cursor_.render(makeCoords(_terminal.cursor().column, _terminal.cursor().row + static_cast<cursor_pos_t>(_terminal.scrollOffset()), _terminal.screenSize()));
    }

    if (_terminal.isSelectionAvailable())
    {
        auto const color = makeColor(colorProfile_.selection, static_cast<terminal::Opacity>(0xC0));
        for (Selector::Range const& range : _terminal.selection())
        {
            if (_terminal.isAbsoluteLineVisible(range.line))
            {
                cursor_pos_t const row = range.line - static_cast<cursor_pos_t>(_terminal.historyLineCount() - _terminal.scrollOffset());

                for (cursor_pos_t col = range.fromColumn; col <= range.toColumn; ++col)
                    cellBackground_.render(makeCoords(col, row, _terminal.screenSize()), color);
            }
        }
    }
}

void GLRenderer::fillCellGroup(cursor_pos_t _row, cursor_pos_t _col, Screen::Cell const& _cell, WindowSize const& _screenSize)
{
    if (pendingDraw_.lineNumber == _row && pendingDraw_.attributes == _cell.attributes)
        pendingDraw_.text.push_back(_cell.character);
    else
    {
        if (!pendingDraw_.text.empty())
            renderCellGroup(_screenSize);

        pendingDraw_.reset(_row, _col, _cell.attributes, _cell.character);
    }
}

void GLRenderer::renderCellGroup(WindowSize const& _screenSize)
{
    auto const [fgColor, bgColor] = makeColors(pendingDraw_.attributes);
    auto const textStyle = FontStyle::Regular;

    if (pendingDraw_.attributes.styles & CharacterStyleMask::Bold)
    {
        // TODO: switch font
    }

    if (pendingDraw_.attributes.styles & CharacterStyleMask::Italic)
    {
        // TODO: *Maybe* update transformation matrix to have chars italic *OR* change font (depending on bold-state)
    }

    if (pendingDraw_.attributes.styles & CharacterStyleMask::Blinking)
    {
        // TODO: update textshaper's shader to blink
    }

    if (pendingDraw_.attributes.styles & CharacterStyleMask::CrossedOut)
    {
        // TODO: render centered horizontal bar through the cell rectangle (we could reuse the TextShaper and a Unicode character for that, respecting opacity!)
    }

    if (pendingDraw_.attributes.styles & CharacterStyleMask::DoublyUnderlined)
    {
        // TODO: render lower-bound horizontal bar through the cell rectangle (we could reuse the TextShaper and a Unicode character for that, respecting opacity!)
    }
    else if (pendingDraw_.attributes.styles & CharacterStyleMask::Underline)
    {
        // TODO: render lower-bound double-horizontal bar through the cell rectangle (we could reuse the TextShaper and a Unicode character for that, respecting opacity!)
    }

    // TODO: stretch background to number of characters instead.
    for (cursor_pos_t i = 0; i < pendingDraw_.text.size(); ++i)
        cellBackground_.render(makeCoords(pendingDraw_.startColumn + i, pendingDraw_.lineNumber, _screenSize), bgColor);

    textShaper_.render(
        makeCoords(pendingDraw_.startColumn, pendingDraw_.lineNumber, _screenSize),
        pendingDraw_.text,
        fgColor,
        textStyle
    );
}

glm::ivec2 GLRenderer::makeCoords(cursor_pos_t col, cursor_pos_t row, WindowSize const& _screenSize) const
{
    constexpr int LeftMargin = 0;
    constexpr int BottomMargin = 0;

    return glm::ivec2{
        LeftMargin + (col - 1) * regularFont_.get().maxAdvance(),
        BottomMargin + (_screenSize.rows - row) * regularFont_.get().lineHeight()
    };
}

std::pair<glm::vec4, glm::vec4> GLRenderer::makeColors(ScreenBuffer::GraphicsAttributes const& _attributes) const
{
    float const opacity = [=]() {
        if (_attributes.styles & CharacterStyleMask::Hidden)
            return 0.0f;
        else if (_attributes.styles & CharacterStyleMask::Faint)
            return 0.5f;
        else
            return 1.0f;
    }();

    auto const applyColor = [_attributes, this](Color const& _color, ColorTarget _target, float _opacity) -> glm::vec4
    {
        RGBColor const rgb = apply(colorProfile_, _color, _target, _attributes.styles & CharacterStyleMask::Bold);
        glm::vec4 const rgba{
            rgb.red / 255.0f,
            rgb.green / 255.0f,
            rgb.blue / 255.0f,
            _opacity
        };
        return rgba;
    };

    float const backgroundOpacity =
		holds_alternative<DefaultColor>(_attributes.backgroundColor)
			? static_cast<float>(backgroundOpacity_) / 255.0f
			: 1.0f;

    return (_attributes.styles & CharacterStyleMask::Inverse)
        ? pair{ applyColor(_attributes.backgroundColor, ColorTarget::Background, opacity * backgroundOpacity),
                applyColor(_attributes.foregroundColor, ColorTarget::Foreground, opacity) }
        : pair{ applyColor(_attributes.foregroundColor, ColorTarget::Foreground, opacity),
                applyColor(_attributes.backgroundColor, ColorTarget::Background, opacity * backgroundOpacity) };
}
