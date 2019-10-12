/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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
#include <terminal_view/TerminalView.h>
#include <terminal_view/GLLogger.h>
#include <terminal_view/FontManager.h>

#include <terminal/Util.h>

#include <ground/overloaded.h>
#include <ground/UTF8.h>

#include <GL/glew.h>
#include <glm/glm.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <utility>

using namespace std;
using namespace std::placeholders;
using namespace terminal;

auto const envvars = terminal::Process::Environment{
    {"TERM", "xterm-256color"},
    {"COLORTERM", "xterm"},
    {"COLORFGBG", "15;0"},
    {"LINES", ""},
    {"COLUMNS", ""},
    {"TERMCAP", ""}
};

inline glm::vec4 makeColor(terminal::RGBColor const& _rgb, terminal::Opacity _opacity = terminal::Opacity::Opaque)
{
    return glm::vec4{
        _rgb.red / 255.0,
        _rgb.green / 255.0,
        _rgb.blue / 255.0,
        static_cast<unsigned>(_opacity) / 255.0
    };
}

TerminalView::TerminalView(WindowSize const& _winSize,
                           optional<size_t> _maxHistoryLineCount,
                           std::string const& _wordDelimiters,
                           Font& _regularFont,
                           CursorShape _cursorShape,
						   bool _cursorBlinking,
                           terminal::ColorProfile const& _colorProfile,
                           terminal::Opacity _backgroundOpacity,
                           string const& _shell,
                           glm::mat4 const& _projectionMatrix,
                           function<void()> _onScreenUpdate,
                           function<void()> _onWindowTitleChanged,
                           function<void(unsigned int, unsigned int, bool)> _resizeWindow,
						   std::function<void(std::function<void()>)> _post,
                           GLLogger& _logger) :
    logger_{ _logger },
    updated_{ false },
    colorProfile_{ _colorProfile },
    backgroundOpacity_{ _backgroundOpacity },
    wordDelimiters_{ utf8::decode(_wordDelimiters) },
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
        _cursorShape,
        makeColor(colorProfile_.cursor)
    },
	cursorBlinking_{ _cursorBlinking },
	cursorBlinkState_{ 1 },
    lastCursorBlink_{ chrono::steady_clock::now() },
    terminal_{
        _winSize,
        move(_maxHistoryLineCount),
        move(_onWindowTitleChanged),
        move(_resizeWindow),
		bind(&TerminalView::onSetCursorStyle, this, _1, _2),
        [this](terminal::LogEvent const& _event) { logger_(_event); },
        bind(&TerminalView::onScreenUpdateHook, this, _1),
    },
    process_{ terminal_, _shell, {_shell}, envvars },
    processExitWatcher_{ [this]() { wait(); }},
    onScreenUpdate_{ move(_onScreenUpdate) },
	post_{ move(_post) }
{
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

TerminalView::~TerminalView()
{
    wait();
    processExitWatcher_.join();
}

bool TerminalView::alive() const
{
    return alive_;
}

void TerminalView::setMaxHistoryLineCount(std::optional<size_t> _maxHistoryLineCount)
{
    terminal_.setMaxHistoryLineCount(_maxHistoryLineCount);
}

void TerminalView::writeToScreen(std::string_view const& _text)
{
    terminal_.writeToScreen(_text.data(), _text.size());
}

bool TerminalView::send(terminal::InputEvent const& _inputEvent, chrono::steady_clock::time_point _now)
{
    return visit(overloaded{
        [this, _now, &_inputEvent](KeyInputEvent const& _key) -> bool {
            logger_.keyPress(_key.key, _key.modifier);
            if (selector_ && _key.key == terminal::Key::Escape)
            {
                clearSelection();
                return true;
            }
            else
			{
				cursorBlinkState_ = 1;
				lastCursorBlink_ = _now;
                return terminal_.send(_inputEvent);
			}
        },
        [this, _now, &_inputEvent](CharInputEvent const& _char) -> bool {
			cursorBlinkState_ = 1;
			lastCursorBlink_ = _now;
            logger_.keyPress(_char.value, _char.modifier);
            return terminal_.send(_inputEvent);
        },
        [this, _now, &_inputEvent](MousePressEvent const& _mouse) -> bool {
            // TODO: anything else? logging?

			if (terminal_.send(_inputEvent))
				return true;

            if (_mouse.button == MouseButton::Left)
            {
				double const diff_ms = chrono::duration<double, milli>(_now - lastClick_).count();
                lastClick_ = _now;
                speedClicks_ = diff_ms >= 10.0 && diff_ms <= 500.0 ? speedClicks_ + 1 : 1;

                if (_mouse.modifier == Modifier::None || _mouse.modifier == Modifier::Control)
                {
					Selector::Mode const selectionMode = [](int _speedClicks, Modifier _modifier) {
						if (_speedClicks == 3)
							return Selector::Mode::FullLine;
						else if (_modifier == Modifier::Control)
							return Selector::Mode::Rectangular;
						else if (_speedClicks == 2)
							return Selector::Mode::LinearWordWise;
						else
							return Selector::Mode::Linear;
					}(speedClicks_, _mouse.modifier);

					selector_ = make_unique<Selector>(
						selectionMode,
						bind(&Terminal::absoluteAt, &terminal_, _1),
						wordDelimiters_,
						terminal_.size().rows + static_cast<cursor_pos_t>(terminal_.historyLineCount()),
						terminal_.size(),
						absoluteCoordinate(currentMousePosition_)
					);
					updated_.store(true);
					cout << fmt::format("start-selection: {}\n", *selector_);

                    return true;
                }
            }
            return false;
        },
        [this](MouseMoveEvent const& _mouseMove) -> bool {
            // receives column/row coordinates in pixels (not character cells)
            auto const cellWidth = static_cast<int>(regularFont().maxAdvance());
            auto const cellHeight = static_cast<int>(regularFont().lineHeight());

            auto const col = 1 + (_mouseMove.column - static_cast<int>(margin_.left)) / cellWidth;
            auto const row = 1 + (_mouseMove.row - static_cast<int>(margin_.bottom)) / cellHeight;

            auto const newPosition = terminal::Coordinate{
                static_cast<cursor_pos_t>(max(row, 0)),
                static_cast<cursor_pos_t>(max(col, 0))
            };

            //printf("mouse position: %d, %d; %d, %d\n", _mouseMove.row, _mouseMove.column, row, col);
            currentMousePosition_ = newPosition;

            if (selector_ && selector_->state() != Selector::State::Complete)
            {
				selector_->extend(absoluteCoordinate(newPosition));
                updated_.store(true);
            }

            return true;
        },
        [this](MouseReleaseEvent const& _mouseRelease) -> bool {
            if (selector_)
            {
                if (selector_->state() == Selector::State::InProgress)
                    selector_->stop();
                else
                    selector_.reset();
            }

            return true;
        },
    }, _inputEvent);
}

Coordinate TerminalView::absoluteCoordinate(Coordinate _viewportCoordinate) const noexcept
{
    return terminal_.absoluteCoordinate(_viewportCoordinate, scrollOffset_);
}

void TerminalView::sendPaste(std::string_view const& _text)
{
    terminal_.sendPaste(_text);
}

std::string TerminalView::screenshot() const
{
    return terminal_.screenshot();
}

void TerminalView::resize(unsigned _width, unsigned _height)
{
    auto const newSize = terminal::WindowSize{
        static_cast<unsigned short>(_width / regularFont_.get().maxAdvance()),
        static_cast<unsigned short>(_height / regularFont_.get().lineHeight())
    };

    auto const computeMargin = [this](WindowSize const& ws, unsigned _width, unsigned _height)
    {
        auto const usedHeight = ws.rows * regularFont_.get().lineHeight();
        auto const usedWidth = ws.columns * regularFont_.get().maxAdvance();
        auto const freeHeight = _height - usedHeight;
        auto const freeWidth = _width - usedWidth;
        auto const bottomMargin = freeHeight / 2;
        auto const leftMargin = freeWidth / 2;
        return Margin{leftMargin, bottomMargin};
    };

    WindowSize const oldSize = terminal_.size();
    bool const doResize = newSize != oldSize; // terminal_.size();
    if (doResize)
        terminal_.resize(newSize);

    margin_ = computeMargin(newSize, _width, _height);

    if (doResize)
    {
        clearSelection();

        cout << fmt::format(
            "Resized to {}x{} ({}x{}) (margin: {}x{}) (CharBox: {}x{})\n",
            newSize.columns, newSize.rows,
            _width, _height,
            margin_.left, margin_.bottom,
            regularFont_.get().maxAdvance(), regularFont_.get().lineHeight()
        );
    }
}

void TerminalView::setWordDelimiters(string const& _wordDelimiters)
{
    wordDelimiters_ = utf8::decode(_wordDelimiters);
}

void TerminalView::setFont(Font& _font)
{
    auto const fontSize = regularFont_.get().fontSize();
    regularFont_ = _font;
    regularFont_.get().setFontSize(fontSize);
    textShaper_.setFont(regularFont_.get());
}

bool TerminalView::setFontSize(unsigned int _fontSize)
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

bool TerminalView::setTerminalSize(terminal::WindowSize const& _newSize)
{
    if (terminal_.size() == _newSize)
        return false;

    terminal_.resize(_newSize);
    margin_ = {0, 0};
    return true;
}


void TerminalView::setProjection(glm::mat4 const& _projectionMatrix)
{
    cellBackground_.setProjection(_projectionMatrix);
    textShaper_.setProjection(_projectionMatrix);
    cursor_.setProjection(_projectionMatrix);
}

bool TerminalView::shouldRender(chrono::steady_clock::time_point const& _now)
{
	return updated_.load() || (
		cursorBlinking_ &&
		chrono::duration_cast<chrono::milliseconds>(_now - lastCursorBlink_) >= cursorBlinkInterval());
}

bool TerminalView::scrollUp(size_t _numLines)
{
    if (auto const newOffset = min(scrollOffset_ + _numLines, terminal_.historyLineCount()); newOffset != scrollOffset_)
    {
        scrollOffset_ = newOffset;
        return true;
    }
    else
        return false;
}

bool TerminalView::scrollDown(size_t _numLines)
{
    if (auto const newOffset = scrollOffset_ >= _numLines ? scrollOffset_ - _numLines : 0; newOffset != scrollOffset_)
    {
        scrollOffset_ = newOffset;
        return true;
    }
    else
        return false;
}

bool TerminalView::scrollToTop()
{
    if (auto top = terminal_.historyLineCount(); top != scrollOffset_)
    {
        scrollOffset_ = top;
        return true;
    }
    else
        return false;
}

bool TerminalView::scrollToBottom()
{
    if (scrollOffset_ != 0)
    {
        scrollOffset_ = 0;
        return true;
    }
    else
        return false;
}

void TerminalView::render(chrono::steady_clock::time_point const& _now)
{
	updated_.store(false);

    terminal_.render(bind(&TerminalView::fillCellGroup, this, _1, _2, _3), scrollOffset_);
    renderCellGroup();

	{
		auto const diff = chrono::duration_cast<chrono::milliseconds>(_now - lastCursorBlink_);
		if (diff >= cursorBlinkInterval())
		{
			lastCursorBlink_ = _now;
			cursorBlinkState_ = (cursorBlinkState_ + 1) % 2;
		}

		if (!cursorBlinking_ || cursorBlinkState_)
			if (terminal_.cursor().visible && scrollOffset_ + terminal_.cursor().row <= terminal_.size().rows)
				cursor_.render(makeCoords(terminal_.cursor().column, terminal_.cursor().row + static_cast<cursor_pos_t>(scrollOffset_)));
	}

    if (selector_ && selector_->state() != Selector::State::Waiting)
    {
        auto const color = makeColor(colorProfile_.selection, static_cast<terminal::Opacity>(0xC0));
        for (Selector::Range const& range : selection())
        {
            if (isAbsoluteLineVisible(range.line))
            {
                cursor_pos_t const row = range.line - static_cast<cursor_pos_t>(terminal_.historyLineCount() - scrollOffset_);

                for (cursor_pos_t col = range.fromColumn; col <= range.toColumn; ++col)
                    cellBackground_.render(makeCoords(col, row), color);
            }
        }
    }
}

bool TerminalView::isAbsoluteLineVisible(cursor_pos_t _row) const noexcept
{
    return _row >= terminal_.historyLineCount() - scrollOffset_
        && _row <= terminal_.historyLineCount() - scrollOffset_ + terminal_.size().rows;
}


void TerminalView::fillCellGroup(terminal::cursor_pos_t _row, terminal::cursor_pos_t _col, terminal::Screen::Cell const& _cell)
{
    if (pendingDraw_.lineNumber == _row && pendingDraw_.attributes == _cell.attributes)
        pendingDraw_.text.push_back(_cell.character);
    else
    {
        if (!pendingDraw_.text.empty())
            renderCellGroup();

        pendingDraw_.reset(_row, _col, _cell.attributes, _cell.character);
    }
}

void TerminalView::renderCellGroup()
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
        cellBackground_.render(makeCoords(pendingDraw_.startColumn + i, pendingDraw_.lineNumber), bgColor);

    textShaper_.render(
        makeCoords(pendingDraw_.startColumn, pendingDraw_.lineNumber),
        pendingDraw_.text,
        fgColor,
        textStyle
    );
}

glm::ivec2 TerminalView::makeCoords(cursor_pos_t col, cursor_pos_t row) const
{
    return glm::ivec2{
        margin_.left + (col - 1) * regularFont_.get().maxAdvance(),
        margin_.bottom + (terminal_.size().rows - row) * regularFont_.get().lineHeight()
    };
}

std::pair<glm::vec4, glm::vec4> TerminalView::makeColors(ScreenBuffer::GraphicsAttributes const& _attributes) const
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

void TerminalView::wait()
{
    if (!alive_)
        return;

    using terminal::Process;

    while (true)
        if (visit(overloaded{[&](Process::NormalExit) { return true; },
                             [&](Process::SignalExit) { return true; },
                             [&](Process::Suspend) { return false; },
                             [&](Process::Resume) { return false; },
                  },
                  process_.wait()))
            break;

    terminal_.close();
    terminal_.wait();
    alive_ = false;
}

void TerminalView::setTabWidth(unsigned int _tabWidth)
{
    terminal_.setTabWidth(_tabWidth);
}

void TerminalView::setBackgroundOpacity(terminal::Opacity _opacity)
{
    backgroundOpacity_ = _opacity;
}

void TerminalView::setCursorColor(terminal::RGBColor const& _color)
{
	cursor_.setColor(makeColor(_color));
}

void TerminalView::setCursorShape(CursorShape _shape)
{
	cursor_.setShape(_shape);
}

void TerminalView::setCursorBlinking(bool _blinking)
{
	cursorBlinking_ = _blinking;
}

void TerminalView::onScreenUpdateHook(std::vector<terminal::Command> const& _commands)
{
    logger_(TraceOutputEvent{ fmt::format("onScreenUpdate: {} instructions", _commands.size()) });

    auto const mnemonics = to_mnemonic(_commands, true, true);
    for (auto const& mnemonic : mnemonics)
        logger_(TraceOutputEvent{ mnemonic });

    updated_.store(true);

    if (onScreenUpdate_)
        onScreenUpdate_();
}

void TerminalView::onSetCursorStyle(CursorDisplay _display, CursorStyle _style)
{
	switch (_display)
	{
		case CursorDisplay::Steady:
			setCursorBlinking(false);
			break;
		case CursorDisplay::Blink:
			setCursorBlinking(true);
			break;
	}

	switch (_style)
	{
		case CursorStyle::Block:
			post([this]() { setCursorShape(CursorShape::Block); });
			break;
		case CursorStyle::Underline:
			post([this]() { setCursorShape(CursorShape::Underscore); });
			break;
		default:
			break;
	}
}

vector<Selector::Range> TerminalView::selection() const
{
    if (selector_)
		return selector_->selection();
	else
		return {};
}

void TerminalView::renderSelection(terminal::Screen::Renderer _render) const
{
	if (selector_)
		selector_->render(_render);
}

void TerminalView::clearSelection()
{
    selector_.reset();
    updated_.store(true);
}
