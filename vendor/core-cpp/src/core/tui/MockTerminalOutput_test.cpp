// SPDX-License-Identifier: Apache-2.0
#include <core/tui/MockScreenSurface.hpp>
#include <core/tui/MockTerminalOutput.hpp>
#include <core/tui/ScreenSurface.hpp>
#include <core/tui/TerminalOutput.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using core::tui::MockScreenSurface;
using core::tui::MockTerminalOutput;
using core::tui::OutputKind;
using core::tui::QueryUnanswered;
using core::tui::TerminalOutput;

namespace
{

/// A @c TerminalOutput whose destination is a string: the double a headless renderer writes into.
class StringOutput final: public TerminalOutput
{
  public:
    /// @return Every byte written to the destination so far, in order.
    [[nodiscard]] std::string const& captured() const noexcept { return _captured; }

  protected:
    void writeToDestination(std::string_view bytes) override { _captured.append(bytes); }

  private:
    std::string _captured;
};

/// A mock a test specialises, which is still a mock: the property is inherited, as the RTTI test
/// it replaces matched every subclass of MockTerminalOutput too.
class DerivedMock final: public MockTerminalOutput
{
};

/// A mock that has overridden its simulated cursor away: still a simulation, but with no cursor
/// to report.
class CursorlessMock final: public MockTerminalOutput
{
  public:
    [[nodiscard]] auto simulatedCursorPosition() const noexcept -> std::optional<std::pair<int, int>> override
    {
        return std::nullopt;
    }
};

} // namespace

TEST_CASE("tui.TerminalOutput: a double in the headless target composes into a string")
{
    auto output = StringOutput {};

    output.moveTo(2, 5);
    output.writeRaw("hi");
    CHECK(output.captured().empty()); // Buffered until flushed.
    output.flush();

    CHECK(output.captured() == "\033[2;5Hhi");
}

TEST_CASE("tui.TerminalOutput: an output with a terminal behind it is a device, and knows no cursor")
{
    auto const output = StringOutput {};
    TerminalOutput const& base = output;

    CHECK(base.kind() == OutputKind::Device);
    CHECK_FALSE(base.simulatedCursorPosition().has_value());
}

TEST_CASE("tui.MockTerminalOutput: is a simulation, and answers its cursor 1-based")
{
    auto mock = MockTerminalOutput {};
    TerminalOutput& base = mock;

    CHECK(base.kind() == OutputKind::Simulation);
    CHECK(base.simulatedCursorPosition() == std::pair { 1, 1 });

    base.moveTo(3, 7);
    CHECK(base.simulatedCursorPosition() == std::pair { 3, 7 });
    CHECK(mock.cursorRow() == 2); // The mock's own accessors stay 0-based.
    CHECK(mock.cursorCol() == 6);
}

TEST_CASE("tui.MockTerminalOutput: a subclass of the mock is still a simulation")
{
    auto const mock = DerivedMock {};
    TerminalOutput const& base = mock;

    CHECK(base.kind() == OutputKind::Simulation);
}

TEST_CASE("tui.MockScreenSurface: answers the cursor query from the mock's cursor")
{
    auto surface = MockScreenSurface {};
    surface.mock().moveTo(4, 9);

    auto const cursor = surface.queryCursorPosition();
    REQUIRE(cursor.has_value());
    CHECK(*cursor == std::pair { 4, 9 });
}

TEST_CASE("tui.MockScreenSurface: a mock with no simulated cursor leaves the query unanswered")
{
    auto surface = MockScreenSurface { std::make_unique<CursorlessMock>() };

    auto const cursor = surface.queryCursorPosition();
    REQUIRE_FALSE(cursor.has_value());
    CHECK(cursor.error() == QueryUnanswered::NotAsked);
}
