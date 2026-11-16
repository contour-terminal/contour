// SPDX-License-Identifier: Apache-2.0
#include <core/tui/Canvas.hpp>
#include <core/tui/Component.hpp>
#include <core/tui/HeadlessSurface.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/TestHelpers.hpp>
#include <core/tui/VtParser.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <variant>

using core::tui::Canvas;
using core::tui::Component;
using core::tui::EventResult;
using core::tui::HeadlessSurface;
using core::tui::InputEvent;
using core::tui::KeyEvent;
using core::tui::QueryUnanswered;
using core::tui::Screen;
using core::tui::SinkDestination;
using core::tui::Size;
using core::tui::VtParser;
using core::tui::test::canvasToString;

namespace
{

/// A focusable line that shows what was typed into it.
class Echo final: public Component
{
  public:
    void render(Canvas& canvas) override { canvas.putString(0, 0, _text, {}); }

    [[nodiscard]] EventResult onEvent(InputEvent const& event) override
    {
        auto const* key = std::get_if<KeyEvent>(&event);
        if (key == nullptr || key->codepoint < U' ' || key->codepoint > U'~')
            return EventResult::Ignored;
        _text += static_cast<char>(key->codepoint);
        return EventResult::Handled;
    }

    [[nodiscard]] bool focusable() const override { return true; }

  private:
    std::string _text;
};

} // namespace

TEST_CASE("HeadlessSurface: a size and a byte sink, and nobody to ask")
{
    auto bytes = std::string {};
    auto surface = HeadlessSurface { Size { .width = 12, .height = 2 },
                                     [&bytes](std::string_view chunk) { bytes += chunk; } };

    CHECK(surface.columns() == 12);
    CHECK(surface.rows() == 2);
    CHECK(surface.output().isTerminal());
    CHECK(surface.output().columns() == 12);
    REQUIRE_FALSE(surface.queryCursorPosition().has_value());
    CHECK(surface.queryCursorPosition().error() == QueryUnanswered::NotAsked);

    surface.output().writeRaw("abc");
    CHECK(bytes.empty());
    surface.output().flush();
    CHECK(bytes == "abc");

    surface.resize(Size { .width = 40, .height = 10 });
    CHECK(surface.columns() == 40);
    CHECK(surface.rows() == 10);
}

TEST_CASE("HeadlessSurface: a plain sink is not bracketed in synchronized output")
{
    auto bytes = std::string {};
    auto surface = HeadlessSurface { Size { .width = 4, .height = 1 },
                                     [&bytes](std::string_view chunk) { bytes += chunk; },
                                     SinkDestination::Plain };
    auto screen = Screen { surface };
    screen.draw();

    CHECK_FALSE(surface.output().isTerminal());
    CHECK_FALSE(bytes.contains("\033[?2026h"));
}

TEST_CASE("HeadlessSurface: a headless Screen fed bytes through VtParser renders what was typed")
{
    auto bytes = std::string {};
    auto surface = HeadlessSurface { Size { .width = 8, .height = 2 },
                                     [&bytes](std::string_view chunk) { bytes += chunk; } };
    auto screen = Screen { surface };
    auto echo = Echo {};
    screen.root().addChild(echo, { .area = { .x = 0, .y = 0, .width = 8, .height = 1 } });
    screen.setFocus(&echo);
    screen.draw();
    bytes.clear();

    auto parser = VtParser {};
    for (auto const& event: parser.feed("hi!"))
        CHECK(screen.dispatchEvent(event) == EventResult::Handled);
    screen.draw();

    CHECK(canvasToString(screen.renderedBuffer()) == "hi!     \n        ");
    // The frame went to the sink, bracketed for the terminal on the other side, as a diff: the three
    // typed cells and nothing else of the frame.
    CHECK(bytes.starts_with("\033[?2026h"));
    CHECK(bytes.contains("\033[1;1Hh"));
    CHECK(bytes.contains("\033[1;3H!"));
    CHECK_FALSE(bytes.contains("\033[2;1H"));
    CHECK(bytes.ends_with("\033[?2026l"));
}
