// SPDX-License-Identifier: Apache-2.0
#include <core/tui/Canvas.hpp>
#include <core/tui/Component.hpp>
#include <core/tui/CursorShape.hpp>
#include <core/tui/HeadlessSurface.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/Rect.hpp>
#include <core/tui/Responsive.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/TestHelpers.hpp>
#include <core/tui/layout/Style.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace core::tui;
using core::tui::test::canvasToString;

namespace
{

/// A framed panel with a title, and the size hints a layout reads.
class Panel final: public Component
{
  public:
    explicit Panel(std::string title, Size preferred = {}, Size minimum = {}):
        _title(std::move(title)), _preferred(preferred), _minimum(minimum)
    {
    }

    void render(Canvas& canvas) override
    {
        canvas.drawBox({ .x = 0, .y = 0, .width = canvas.width(), .height = canvas.height() },
                       BorderStyle::Rounded,
                       Style {},
                       _title);
    }

    [[nodiscard]] Size preferredSize() const override { return _preferred; }
    [[nodiscard]] Size minSize() const override { return _minimum; }

  private:
    std::string _title;
    Size _preferred;
    Size _minimum;
};

/// One line of text: the key hints under a view.
class HintLine final: public Component
{
  public:
    explicit HintLine(std::string text): _text(std::move(text)) {}

    void render(Canvas& canvas) override { canvas.putString(0, 0, _text, Style {}); }

  private:
    std::string _text;
};

constexpr auto HintText = std::string_view { "q quit  enter open  tab details" };

/// >= 100 columns: the list and its details side by side, the hints below.
auto wideView() -> std::unique_ptr<Component>
{
    auto view = std::make_unique<FlexBox>(layout::Style { .direction = layout::Direction::Column });
    auto body = std::make_unique<FlexBox>(layout::Style {});
    // No basis: the list's own preferred width, 40, is its basis, and it does not shrink.
    body->add(std::make_unique<Panel>("Agents", Size { .width = 40, .height = 0 }), { .shrink = 0 });
    body->add(std::make_unique<Panel>("Details"), { .grow = 1 });
    view->add(std::move(body), { .grow = 1 });
    view->add(std::make_unique<HintLine>(std::string(HintText)));
    return view;
}

/// 60-99 columns: the list over its details, the hints below.
auto mediumView() -> std::unique_ptr<Component>
{
    auto view = std::make_unique<FlexBox>(layout::Style { .direction = layout::Direction::Column });
    view->add(std::make_unique<Panel>("Agents"), { .grow = 1, .basis = 0 });
    view->add(std::make_unique<Panel>("Details"), { .grow = 1, .basis = 0 });
    view->add(std::make_unique<HintLine>(std::string(HintText)));
    return view;
}

/// < 60 columns: the list alone, the hints below.
auto narrowView() -> std::unique_ptr<Component>
{
    auto view = std::make_unique<FlexBox>(layout::Style { .direction = layout::Direction::Column });
    view->add(std::make_unique<Panel>("Agents"), { .grow = 1, .basis = 0 });
    view->add(std::make_unique<HintLine>(std::string(HintText)));
    return view;
}

/// How often each band's layout was built.
struct BuildCounts
{
    std::array<int, 3> builds {};
};

auto dashboardBreakpoints(BuildCounts& counts) -> std::vector<Breakpoint>
{
    return {
        Breakpoint { .minColumns = 0,
                     .minRows = 0,
                     .make =
                         [&counts] {
                             ++counts.builds[0];
                             return narrowView();
                         } },
        Breakpoint { .minColumns = 60,
                     .minRows = 0,
                     .make =
                         [&counts] {
                             ++counts.builds[1];
                             return mediumView();
                         } },
        Breakpoint { .minColumns = 100,
                     .minRows = 0,
                     .make =
                         [&counts] {
                             ++counts.builds[2];
                             return wideView();
                         } },
    };
}

/// A headless screen whose root holds one Responsive dashboard.
class Dashboard
{
  public:
    explicit Dashboard(Size size):
        _surface(size, [](std::string_view) {}), _screen(_surface), _responsive(dashboardBreakpoints(_counts))
    {
        _screen.root().addChild(_responsive, { .flex = layout::Style { .grow = 1 } });
    }

    /// Resizes the surface and draws a frame at the new size.
    void resize(Size size)
    {
        _surface.resize(size);
        _screen.draw();
    }

    /// The last frame, each row without its trailing blanks.
    [[nodiscard]] auto frame() -> std::string
    {
        _screen.draw();
        auto text = canvasToString(_screen.renderedBuffer());
        auto trimmed = std::string {};
        for (auto const row: std::views::split(text, '\n'))
        {
            auto line = std::string(std::string_view(row.begin(), row.end()));
            while (line.ends_with(' '))
                line.pop_back();
            trimmed += line + '\n';
        }
        return trimmed;
    }

    [[nodiscard]] auto responsive() -> Responsive& { return _responsive; }
    [[nodiscard]] auto counts() const -> BuildCounts const& { return _counts; }
    [[nodiscard]] auto screen() -> Screen& { return _screen; }

  private:
    BuildCounts _counts;
    HeadlessSurface _surface;
    Screen _screen;
    Responsive _responsive;
};

/// A golden screen as runs of identical rows: each pair is how many times a row repeats, and the row.
auto golden(std::initializer_list<std::pair<int, std::string_view>> runs) -> std::string
{
    auto text = std::string {};
    for (auto const& [count, row]: runs)
        for ([[maybe_unused]] auto const repeat: std::views::iota(0, count))
            text += std::string(row) + '\n';
    return text;
}

/// A panel's size hints.
struct Hints
{
    Size preferred;
    Size minimum;
};

/// A row FlexBox of the given width holding two panels with the given hints, laid out.
struct TwoPanels
{
    FlexBox box { layout::Style {} };
    Panel first;
    Panel second;

    TwoPanels(Hints a, Hints b, layout::Style firstItem, layout::Style secondItem, int width):
        first("A", a.preferred, a.minimum), second("B", b.preferred, b.minimum)
    {
        box.addChild(first, { .flex = firstItem });
        box.addChild(second, { .flex = secondItem });
        box.setArea({ .x = 0, .y = 0, .width = width, .height = 3 });
    }
};

} // namespace

TEST_CASE("Responsive: picks the last breakpoint whose minima the size meets")
{
    auto const columnBands = std::vector<Breakpoint> {
        Breakpoint { .minColumns = 0, .minRows = 0, .make = {} },
        Breakpoint { .minColumns = 60, .minRows = 0, .make = {} },
        Breakpoint { .minColumns = 100, .minRows = 0, .make = {} },
    };
    // A row band beneath a column band: the key-hint line goes first under 20 rows.
    auto const rowBands = std::vector<Breakpoint> {
        Breakpoint { .minColumns = 0, .minRows = 0, .make = {} },
        Breakpoint { .minColumns = 0, .minRows = 20, .make = {} },
        Breakpoint { .minColumns = 100, .minRows = 20, .make = {} },
    };

    struct Row
    {
        std::vector<Breakpoint> const* breakpoints = nullptr;
        Size size;
        std::optional<std::size_t> chosen;
    };
    auto const rows = std::array {
        Row { &columnBands, { .width = 0, .height = 0 }, 0 },
        Row { &columnBands, { .width = 59, .height = 40 }, 0 },
        Row { &columnBands, { .width = 60, .height = 1 }, 1 },
        Row { &columnBands, { .width = 99, .height = 24 }, 1 },
        Row { &columnBands, { .width = 100, .height = 0 }, 2 },
        Row { &columnBands, { .width = 500, .height = 9 }, 2 },
        Row { &rowBands, { .width = 120, .height = 19 }, 0 },
        Row { &rowBands, { .width = 80, .height = 20 }, 1 },
        Row { &rowBands, { .width = 120, .height = 20 }, 2 },
        Row { &rowBands, { .width = 99, .height = 40 }, 1 },
    };
    for (auto const& row: rows)
    {
        INFO(row.size.width << "x" << row.size.height);
        CHECK(Responsive::choose(*row.breakpoints, row.size) == row.chosen);
    }

    // No breakpoint met, or none at all: nothing chosen.
    auto const wideOnly =
        std::vector<Breakpoint> { Breakpoint { .minColumns = 100, .minRows = 0, .make = {} } };
    CHECK_FALSE(Responsive::choose(wideOnly, { .width = 99, .height = 40 }).has_value());
    CHECK_FALSE(Responsive::choose({}, { .width = 80, .height = 24 }).has_value());
}

TEST_CASE("Responsive: golden screens of the three column bands")
{
    SECTION(">= 100 columns: list and details side by side")
    {
        auto dashboard = Dashboard { { .width = 120, .height = 40 } };
        CHECK(dashboard.frame()
              == golden({ { 1,
                            "╭─Agents───────────────────────────────╮╭─Details───────────────────────────────"
                            "───────────────────────────────────────╮" },
                          { 37,
                            "│                                      ││                                       "
                            "                                       │" },
                          { 1,
                            "╰──────────────────────────────────────╯╰───────────────────────────────────────"
                            "───────────────────────────────────────╯" },
                          { 1, "q quit  enter open  tab details" } }));
        CHECK(dashboard.responsive().breakpoint() == 2);
    }

    SECTION("60-99 columns: list over details")
    {
        auto dashboard = Dashboard { { .width = 80, .height = 24 } };
        CHECK(
            dashboard.frame()
            == golden(
                { { 1, "╭─Agents───────────────────────────────────────────────────────────────────────╮" },
                  { 10, "│                                                                              │" },
                  { 1, "╰──────────────────────────────────────────────────────────────────────────────╯" },
                  { 1, "╭─Details──────────────────────────────────────────────────────────────────────╮" },
                  { 9, "│                                                                              │" },
                  { 1, "╰──────────────────────────────────────────────────────────────────────────────╯" },
                  { 1, "q quit  enter open  tab details" } }));
        CHECK(dashboard.responsive().breakpoint() == 1);
    }

    SECTION("< 60 columns: the list alone")
    {
        auto dashboard = Dashboard { { .width = 50, .height = 20 } };
        CHECK(dashboard.frame()
              == golden({ { 1, "╭─Agents─────────────────────────────────────────╮" },
                          { 17, "│                                                │" },
                          { 1, "╰────────────────────────────────────────────────╯" },
                          { 1, "q quit  enter open  tab details" } }));
        CHECK(dashboard.responsive().breakpoint() == 0);
    }
}

TEST_CASE("Responsive: a resize across a band swaps the layout, one within a band does not rebuild")
{
    auto dashboard = Dashboard { { .width = 120, .height = 40 } };
    dashboard.screen().draw();
    auto const* const wide = dashboard.responsive().current();
    REQUIRE(wide != nullptr);
    CHECK(dashboard.counts().builds == std::array { 0, 0, 1 });

    // Within the band: the same layout, laid out again at the new size.
    dashboard.resize({ .width = 110, .height = 30 });
    CHECK(dashboard.responsive().current() == wide);
    CHECK(dashboard.counts().builds == std::array { 0, 0, 1 });
    CHECK(wide->area() == Rect { .x = 0, .y = 0, .width = 110, .height = 30 });
    auto const* const body = wide->children()[0];
    CHECK(body->children()[1]->area() == Rect { .x = 40, .y = 0, .width = 70, .height = 29 });

    // Across a band: the medium layout replaces it.
    dashboard.resize({ .width = 80, .height = 24 });
    CHECK(dashboard.responsive().breakpoint() == 1);
    CHECK(dashboard.counts().builds == std::array { 0, 1, 1 });
    CHECK(dashboard.responsive().children().size() == 1);
    CHECK(dashboard.responsive().children()[0] == dashboard.responsive().current());

    // Down, and back up: each crossing builds once more, each in-band step not at all.
    dashboard.resize({ .width = 50, .height = 20 });
    dashboard.resize({ .width = 55, .height = 22 });
    dashboard.resize({ .width = 100, .height = 40 });
    CHECK(dashboard.counts().builds == std::array { 1, 1, 2 });
}

TEST_CASE("Responsive: nothing is built before a size, nor where no breakpoint is met")
{
    auto builds = 0;
    auto responsive = Responsive { { Breakpoint { .minColumns = 60, .minRows = 0, .make = [&builds] {
                                                     ++builds;
                                                     return std::make_unique<Panel>("Wide");
                                                 } } } };
    CHECK(builds == 0);
    CHECK(responsive.current() == nullptr);
    CHECK_FALSE(responsive.breakpoint().has_value());
    CHECK(responsive.preferredSize() == Size {});

    responsive.setArea({ .x = 0, .y = 0, .width = 59, .height = 10 });
    CHECK(builds == 0);
    CHECK(responsive.current() == nullptr);

    responsive.setArea({ .x = 0, .y = 0, .width = 60, .height = 10 });
    CHECK(builds == 1);
    REQUIRE(responsive.current() != nullptr);
    CHECK(responsive.current()->area() == Rect { .x = 0, .y = 0, .width = 60, .height = 10 });

    // Back under the minimum: the layout is destroyed and nothing is shown.
    responsive.setArea({ .x = 0, .y = 0, .width = 40, .height = 10 });
    CHECK(responsive.current() == nullptr);
    CHECK(responsive.children().empty());
}

TEST_CASE("Layout pass: a child's preferredSize and minSize hints are honoured")
{
    SECTION("the preferred size is the basis when the item gives none")
    {
        auto panels = TwoPanels { Hints { .preferred = { .width = 20, .height = 1 }, .minimum = {} },
                                  Hints {},
                                  layout::Style {},
                                  layout::Style { .grow = 1 },
                                  50 };
        CHECK(panels.first.area() == Rect { .x = 0, .y = 0, .width = 20, .height = 3 });
        CHECK(panels.second.area() == Rect { .x = 20, .y = 0, .width = 30, .height = 3 });
    }

    SECTION("an explicit basis wins over the preferred size")
    {
        auto panels = TwoPanels { Hints { .preferred = { .width = 20, .height = 1 }, .minimum = {} },
                                  Hints {},
                                  layout::Style { .basis = 10 },
                                  layout::Style { .grow = 1 },
                                  50 };
        CHECK(panels.first.area().width == 10);
        CHECK(panels.second.area().width == 40);
    }

    SECTION("a shrinking child stops at its minSize hint, the other gives up the rest")
    {
        // 40 preferred over 25: a deficit of 15, shared 8/7 by equal weights, would leave A at 12 --
        // below its hint of 18, so A freezes there and B takes the remaining deficit.
        auto panels = TwoPanels { Hints { .preferred = { .width = 20, .height = 1 },
                                          .minimum = { .width = 18, .height = 0 } },
                                  Hints { .preferred = { .width = 20, .height = 1 }, .minimum = {} },
                                  layout::Style {},
                                  layout::Style {},
                                  25 };
        CHECK(panels.first.area().width == 18);
        CHECK(panels.second.area().width == 7);
    }

    SECTION("the larger of the item's minSize and the hint holds")
    {
        auto panels = TwoPanels { Hints { .preferred = { .width = 20, .height = 1 },
                                          .minimum = { .width = 18, .height = 0 } },
                                  Hints { .preferred = { .width = 20, .height = 1 }, .minimum = {} },
                                  layout::Style { .minSize = { .width = 19, .height = 0 } },
                                  layout::Style {},
                                  25 };
        CHECK(panels.first.area().width == 19);
        CHECK(panels.second.area().width == 6);
    }
}

TEST_CASE("Layout pass: runs when flex children come, go, show or hide, and leaves the others alone")
{
    auto box = FlexBox { layout::Style {} };
    box.setArea({ .x = 0, .y = 0, .width = 30, .height = 2 });

    auto a = Panel { "A" };
    auto b = Panel { "B" };
    auto manual = Panel { "M" };
    box.addChild(a, { .flex = layout::Style { .grow = 1 } });
    CHECK(a.area() == Rect { .x = 0, .y = 0, .width = 30, .height = 2 });

    box.addChild(b, { .flex = layout::Style { .grow = 1 } });
    CHECK(a.area().width == 15);
    CHECK(b.area() == Rect { .x = 15, .y = 0, .width = 15, .height = 2 });

    // A child without a flex item keeps the area it was given, and takes no room from the others.
    box.addChild(manual, { .area = { .x = 3, .y = 1, .width = 4, .height = 1 } });
    CHECK(manual.area() == Rect { .x = 3, .y = 1, .width = 4, .height = 1 });
    CHECK(a.area().width == 15);

    // A hidden flex child takes no room; shown again, it takes its share back.
    b.setVisible(false);
    CHECK(a.area().width == 30);
    b.setVisible(true);
    CHECK(a.area().width == 15);

    box.removeChild(b);
    CHECK(a.area().width == 30);

    // A resize lays the children out again.
    box.setArea({ .x = 5, .y = 5, .width = 12, .height = 2 });
    CHECK(a.area() == Rect { .x = 0, .y = 0, .width = 12, .height = 2 });
    CHECK(manual.area() == Rect { .x = 3, .y = 1, .width = 4, .height = 1 });
}

TEST_CASE("Layout pass: the screen's root is the viewport, and lays out flex children on every resize")
{
    auto surface = HeadlessSurface { { .width = 20, .height = 4 }, [](std::string_view) {} };
    auto screen = Screen { surface };
    auto panel = Panel { "P" };
    auto manual = Panel { "M" };
    screen.root().addChild(panel, { .flex = layout::Style { .grow = 1 } });
    screen.root().addChild(manual, { .area = { .x = 1, .y = 1, .width = 3, .height = 2 }, .zIndex = 1 });
    screen.draw();
    CHECK(panel.area() == Rect { .x = 0, .y = 0, .width = 20, .height = 4 });

    surface.resize({ .width = 30, .height = 6 });
    CHECK(screen.dispatchEvent(ResizeEvent { .columns = 30, .rows = 6 }) == EventResult::Handled);
    CHECK(panel.area() == Rect { .x = 0, .y = 0, .width = 30, .height = 6 });
    CHECK(manual.area() == Rect { .x = 1, .y = 1, .width = 3, .height = 2 });
}

namespace
{

/// A focusable leaf that counts the keys it is handed and asks for a bar cursor.
class Field final: public Component
{
  public:
    explicit Field(int& keys): _keys(&keys) {}

    void render(Canvas& /*canvas*/) override {}
    [[nodiscard]] bool focusable() const override { return true; }
    [[nodiscard]] CursorShape cursorShape() const override { return CursorShape::SteadyBar; }
    [[nodiscard]] EventResult onEvent(InputEvent const& event) override
    {
        if (std::holds_alternative<KeyEvent>(event))
        {
            ++*_keys;
            return EventResult::Handled;
        }
        return EventResult::Ignored;
    }

  private:
    int* _keys;
};

} // namespace

TEST_CASE(
    "Responsive: a swap that destroys the focused and hovered layout leaves the screen pointing at nothing")
{
    // Run under ASan: before the screen forgot what a detached component was, the draw after the swap asked
    // the destroyed field for its cursor shape and the next key went to it.
    auto keys = 0;
    auto surface = HeadlessSurface { { .width = 80, .height = 24 }, [](std::string_view) {} };
    auto screen = Screen { surface };
    auto responsive = Responsive { {
        Breakpoint {
            .minColumns = 0, .minRows = 0, .make = [] { return std::make_unique<Panel>("Narrow"); } },
        Breakpoint {
            .minColumns = 60, .minRows = 0, .make = [&keys] { return std::make_unique<Field>(keys); } },
    } };
    screen.root().addChild(responsive, { .flex = layout::Style { .grow = 1 } });
    screen.draw();

    auto* const field = responsive.current();
    REQUIRE(field != nullptr);
    screen.setFocus(field);
    CHECK(screen.dispatchEvent(test::mouseMove(5, 5)) == EventResult::Ignored);
    REQUIRE(screen.focusedComponent() == field);
    REQUIRE(screen.hoverState().currentHover().has_value());
    REQUIRE(screen.hoverState().currentHover()->target == field);
    CHECK(screen.dispatchEvent(test::charKey('a')) == EventResult::Handled);
    CHECK(keys == 1);

    surface.resize({ .width = 50, .height = 20 });
    screen.draw();
    CHECK(responsive.breakpoint() == 0);
    CHECK(screen.focusedComponent() == nullptr);
    CHECK_FALSE(screen.hoverState().currentHover().has_value());
    CHECK(screen.dispatchEvent(test::charKey('b')) == EventResult::Ignored);
    CHECK(keys == 1);
    screen.draw();
}

TEST_CASE("Layout pass: a child added with a fixed area lays itself out")
{
    auto parent = FlexBox { layout::Style {} };

    SECTION("a Responsive builds for the area it is added with")
    {
        auto responsive = Responsive { { Breakpoint {
            .minColumns = 60, .minRows = 0, .make = [] { return std::make_unique<Panel>("Wide"); } } } };
        parent.addChild(responsive, { .area = { .x = 0, .y = 0, .width = 80, .height = 24 } });
        REQUIRE(responsive.current() != nullptr);
        CHECK(responsive.current()->area() == Rect { .x = 0, .y = 0, .width = 80, .height = 24 });
    }

    SECTION("a FlexBox lays its children out over the area it is added with")
    {
        auto box = FlexBox { layout::Style {} };
        auto& panel = box.add(std::make_unique<Panel>("P"), { .grow = 1 });
        CHECK(panel.area() == Rect {});
        parent.addChild(box, { .area = { .x = 2, .y = 1, .width = 30, .height = 4 } });
        CHECK(panel.area() == Rect { .x = 0, .y = 0, .width = 30, .height = 4 });
    }
}

TEST_CASE("Layout pass: a sibling gets the right share after a Responsive swaps, with no further resize")
{
    // A column: the Responsive takes its layout's preferred height, the sibling the rest. The swap a resize
    // causes changes the Responsive's hints, and the same frame lays the column out again.
    auto surface = HeadlessSurface { { .width = 80, .height = 20 }, [](std::string_view) {} };
    auto screen = Screen { surface };
    auto column = FlexBox { layout::Style { .direction = layout::Direction::Column } };
    auto responsive = Responsive { {
        Breakpoint {
            .minColumns = 0,
            .minRows = 0,
            .make = [] { return std::make_unique<Panel>("Short", Size { .width = 0, .height = 3 }); } },
        Breakpoint {
            .minColumns = 60,
            .minRows = 0,
            .make = [] { return std::make_unique<Panel>("Tall", Size { .width = 0, .height = 6 }); } },
    } };
    auto sibling = Panel { "Rest" };
    column.addChild(responsive, { .flex = layout::Style {} });
    column.addChild(sibling, { .flex = layout::Style { .grow = 1 } });
    screen.root().addChild(column, { .flex = layout::Style { .grow = 1 } });

    screen.draw();
    CHECK(responsive.breakpoint() == 1);
    CHECK(responsive.area() == Rect { .x = 0, .y = 0, .width = 80, .height = 6 });
    CHECK(sibling.area() == Rect { .x = 0, .y = 6, .width = 80, .height = 14 });
    CHECK_FALSE(column.layoutPending());

    surface.resize({ .width = 50, .height = 20 });
    screen.draw();
    CHECK(responsive.breakpoint() == 0);
    CHECK(responsive.area() == Rect { .x = 0, .y = 0, .width = 50, .height = 3 });
    CHECK(sibling.area() == Rect { .x = 0, .y = 3, .width = 50, .height = 17 });

    // Without a screen, the mark waits for the parent's next layout pass.
    auto box = FlexBox { layout::Style { .direction = layout::Direction::Column } };
    auto standalone =
        Responsive { { Breakpoint { .minColumns = 0, .minRows = 0, .make = [] {
                                       return std::make_unique<Panel>("P", Size { .width = 0, .height = 2 });
                                   } } } };
    box.addChild(standalone, { .flex = layout::Style {} });
    box.setArea({ .x = 0, .y = 0, .width = 10, .height = 10 });
    CHECK(box.layoutPending());
    box.layoutChildren();
    CHECK_FALSE(box.layoutPending());
    CHECK(standalone.area() == Rect { .x = 0, .y = 0, .width = 10, .height = 2 });
}

TEST_CASE("Layout pass: a sized Responsive moved to a new parent builds once, for its new place")
{
    auto builds = std::array<int, 2> {};
    auto responsive = Responsive { {
        Breakpoint { .minColumns = 0,
                     .minRows = 0,
                     .make =
                         [&builds] {
                             ++builds[0];
                             return std::make_unique<Panel>("Narrow");
                         } },
        Breakpoint { .minColumns = 60,
                     .minRows = 0,
                     .make =
                         [&builds] {
                             ++builds[1];
                             return std::make_unique<Panel>("Wide");
                         } },
    } };
    auto first = FlexBox { layout::Style {} };
    first.setArea({ .x = 0, .y = 0, .width = 80, .height = 24 });
    first.addChild(responsive, { .flex = layout::Style { .grow = 1 } });
    REQUIRE(responsive.breakpoint() == 1);
    REQUIRE(builds == std::array { 0, 1 });

    SECTION("to a parent of another band: the layout of that band, and no other on the way")
    {
        auto second = FlexBox { layout::Style {} };
        second.setArea({ .x = 0, .y = 0, .width = 40, .height = 10 });
        second.addChild(responsive, { .flex = layout::Style { .grow = 1 } });
        CHECK(responsive.breakpoint() == 0);
        CHECK(builds == std::array { 1, 1 });
        CHECK(responsive.area() == Rect { .x = 0, .y = 0, .width = 40, .height = 10 });
    }

    SECTION("to a parent of the same band: the layout it has, rebuilt not at all")
    {
        auto const* const wide = responsive.current();
        auto second = FlexBox { layout::Style {} };
        second.setArea({ .x = 0, .y = 0, .width = 100, .height = 30 });
        second.addChild(responsive, { .flex = layout::Style { .grow = 1 } });
        CHECK(responsive.current() == wide);
        CHECK(builds == std::array { 0, 1 });
        CHECK(responsive.area() == Rect { .x = 0, .y = 0, .width = 100, .height = 30 });
    }
}

TEST_CASE("Layout pass: breakpoints whose layouts flip each other stop swapping after a bounded run")
{
    // A row: the Responsive takes its layout's preferred width. Narrow, its layout prefers 70 columns,
    // which choose the wide breakpoint; wide, its layout prefers 30, which choose the narrow one -- the
    // precondition Responsive documents, broken. Each frame's sweep swaps it once more.
    auto builds = 0;
    auto surface = HeadlessSurface { { .width = 80, .height = 10 }, [](std::string_view) {} };
    auto screen = Screen { surface };
    auto row = FlexBox { layout::Style {} };
    auto responsive = Responsive { {
        Breakpoint { .minColumns = 0,
                     .minRows = 0,
                     .make =
                         [&builds] {
                             ++builds;
                             return std::make_unique<Panel>("Narrow", Size { .width = 70, .height = 0 });
                         } },
        Breakpoint { .minColumns = 60,
                     .minRows = 0,
                     .make =
                         [&builds] {
                             ++builds;
                             return std::make_unique<Panel>("Wide", Size { .width = 30, .height = 0 });
                         } },
    } };
    row.addChild(responsive, { .flex = layout::Style { .shrink = 0 } });
    screen.root().addChild(row, { .flex = layout::Style { .grow = 1 } });

    auto const drawFrames = [&screen](int count) {
        for ([[maybe_unused]] auto const frame: std::views::iota(0, count))
            screen.draw();
    };

    screen.draw();
    CHECK_FALSE(screen.layoutAbandoned());
    drawFrames(30);
    auto const settled = builds;
    // The first frame builds twice (its resize, then its sweep); every chained sweep after it once more.
    CHECK(settled > 2);
    CHECK(settled <= 2 + Screen::MaxChainedLayoutSweeps + 1);
    CHECK(screen.layoutAbandoned());
    drawFrames(30);
    CHECK(builds == settled);
    CHECK(screen.layoutAbandoned());

    // Something from outside asks again: the chain runs again, and stops again.
    surface.resize({ .width = 90, .height = 10 });
    screen.draw();
    CHECK_FALSE(screen.layoutAbandoned());
    drawFrames(30);
    auto const resettled = builds;
    CHECK(resettled > settled);
    CHECK(screen.layoutAbandoned());
    drawFrames(30);
    CHECK(builds == resettled);

    // A hint changed from outside a sweep asks again too, with the viewport's size unchanged.
    responsive.hintsChanged();
    screen.draw();
    CHECK_FALSE(screen.layoutAbandoned());
    drawFrames(30);
    auto const rehinted = builds;
    CHECK(rehinted > resettled);
    CHECK(screen.layoutAbandoned());
    drawFrames(30);
    CHECK(builds == rehinted);
}

namespace
{

/// A focusable field that counts its blurs and, in one, adds a component to its parent.
class BlurGrowsParent final: public Component
{
  public:
    BlurGrowsParent(Component& extra, int& blurs): _extra(&extra), _blurs(&blurs) {}

    void render(Canvas& /*canvas*/) override {}
    [[nodiscard]] bool focusable() const override { return true; }
    void onBlur() override
    {
        ++*_blurs;
        if (parent())
            parent()->addChild(*_extra, { .area = { .x = 0, .y = 1, .width = 4, .height = 1 } });
    }

  private:
    Component* _extra;
    int* _blurs;
};

} // namespace

TEST_CASE("Responsive: a swap tells the old layout's focused component it lost the focus, after the swap")
{
    // Run under ASan: the blur used to run inside the detach's loop over the old layout's children, where
    // the handler's addChild() reallocated the vector being walked, and the old layout is destroyed in the
    // same swap -- so it is retired until the layout pass is over, and the blur reaches it alive.
    auto blurs = 0;
    auto extra = Panel { "Extra" };
    auto surface = HeadlessSurface { { .width = 80, .height = 24 }, [](std::string_view) {} };
    auto screen = Screen { surface };
    auto responsive = Responsive { {
        Breakpoint {
            .minColumns = 0, .minRows = 0, .make = [] { return std::make_unique<Panel>("Narrow"); } },
        Breakpoint { .minColumns = 60,
                     .minRows = 0,
                     .make =
                         [&] {
                             auto view = std::make_unique<FlexBox>(layout::Style {});
                             view->add(std::make_unique<BlurGrowsParent>(extra, blurs), { .grow = 1 });
                             // A second child: the old loop read on past the first.
                             view->add(std::make_unique<Panel>("Side"), { .grow = 1 });
                             return view;
                         } },
    } };
    screen.root().addChild(responsive, { .flex = layout::Style { .grow = 1 } });
    screen.draw();
    REQUIRE(responsive.breakpoint() == 1);
    auto* const field = responsive.current()->children()[0];
    screen.setFocus(field);
    REQUIRE(screen.focusedComponent() == field);

    surface.resize({ .width = 50, .height = 20 });
    screen.draw();
    CHECK(responsive.breakpoint() == 0);
    CHECK(blurs == 1);
    CHECK(screen.focusedComponent() == nullptr);
    // The handler added it to the old layout, which is gone now and let it go.
    CHECK(extra.parent() == nullptr);
    CHECK(extra.screen() == nullptr);
}

TEST_CASE("Layout pass: a hidden flex child moved to a new parent takes an empty area at its origin")
{
    auto first = FlexBox { layout::Style {} };
    first.setArea({ .x = 0, .y = 0, .width = 40, .height = 10 });
    auto panel = Panel { "P" };
    first.addChild(panel, { .flex = layout::Style { .grow = 1 } });
    REQUIRE(panel.area() == Rect { .x = 0, .y = 0, .width = 40, .height = 10 });

    auto second = FlexBox { layout::Style {} };
    second.setArea({ .x = 0, .y = 0, .width = 30, .height = 5 });
    second.addChild(panel, { .visible = false, .flex = layout::Style { .grow = 1 } });
    CHECK(panel.area() == Rect {});
}
