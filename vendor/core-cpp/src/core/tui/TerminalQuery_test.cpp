// SPDX-License-Identifier: Apache-2.0
#include <core/platform/Clock.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/MockTerminalOutput.hpp>
#include <core/tui/Terminal.hpp>
#include <core/tui/TerminalInput.hpp>
#include <core/tui/TerminalOutput.hpp>
#include <core/tui/VtParser.hpp>
#include <core/tui/runtime/TerminalInputSource.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <deque>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

using core::platform::ManualClock;
using core::tui::CellSizeReport;
using core::tui::ColorScheme;
using core::tui::ColorSchemeReport;
using core::tui::CursorPositionReport;
using core::tui::DecModeReport;
using core::tui::DecModeStatus;
using core::tui::DeviceAttributesReport;
using core::tui::InputEvent;
using core::tui::KeyEvent;
using core::tui::MockTerminalOutput;
using core::tui::QueryUnanswered;
using core::tui::Terminal;
using core::tui::TerminalInput;
using core::tui::TerminalQueryInput;

namespace
{

/// A query input that replays scripted reads and never waits in real time.
///
/// Each poll() hands out the next scripted batch. Once the script is exhausted a poll models a real
/// timeout: it advances the ManualClock by exactly the timeout it was asked for and returns nothing.
/// So a query driven by the injected clock ends deterministically, while one that read the real clock
/// would spin against the exhausted script for the whole timeout -- which the recorded timeouts show.
class ScriptedQueryInput final: public TerminalQueryInput
{
  public:
    /// @param clock The clock a scripted timeout advances.
    explicit ScriptedQueryInput(ManualClock& clock) noexcept: _clock(clock) {}

    /// Appends one read's worth of events.
    /// @param events The events the next poll returns.
    void pushRead(std::vector<InputEvent> events) { _reads.push_back(std::move(events)); }

    [[nodiscard]] auto poll(int timeoutMs) -> std::vector<InputEvent> override
    {
        _timeouts.push_back(timeoutMs);
        if (_reads.empty())
        {
            _clock.advance(std::chrono::milliseconds(timeoutMs));
            return {};
        }
        auto events = std::move(_reads.front());
        _reads.pop_front();
        return events;
    }

    void unread(std::vector<InputEvent> events) override
    {
        _handedBack.insert(_handedBack.end(),
                           std::make_move_iterator(events.begin()),
                           std::make_move_iterator(events.end()));
    }

    /// @return The timeout every poll() was called with, in order.
    [[nodiscard]] auto timeouts() const -> std::vector<int> const& { return _timeouts; }

    /// @return Everything the query handed back, in order.
    [[nodiscard]] auto handedBack() const -> std::vector<InputEvent> const& { return _handedBack; }

  private:
    ManualClock& _clock;
    std::deque<std::vector<InputEvent>> _reads;
    std::vector<int> _timeouts;
    std::vector<InputEvent> _handedBack;
};

/// A key event carrying @p codepoint.
auto key(char32_t codepoint) -> InputEvent
{
    return KeyEvent { .codepoint = codepoint };
}

/// The codepoints of @p events, with 0 standing for anything that is not a key.
auto codepoints(std::vector<InputEvent> const& events) -> std::vector<char32_t>
{
    auto result = std::vector<char32_t> {};
    for (auto const& event: events)
    {
        auto const* keyEvent = std::get_if<KeyEvent>(&event);
        result.push_back(keyEvent != nullptr ? keyEvent->codepoint : char32_t { 0 });
    }
    return result;
}

} // namespace

TEST_CASE("A key typed before a query's reply survives the query", "[TerminalQuery]")
{
    auto clock = ManualClock {};
    auto input = ScriptedQueryInput { clock };
    auto terminal = Terminal { std::make_unique<MockTerminalOutput>(), input, clock };
    input.pushRead({ key(U'a'), CellSizeReport { .height = 20, .width = 10 } });

    auto const size = terminal.queryCellSize();

    REQUIRE(size.has_value());
    CHECK(size->first == 10);
    CHECK(size->second == 20);
    CHECK(codepoints(input.handedBack()) == std::vector<char32_t> { U'a' });
}

TEST_CASE("A key read after a query's reply, in the same read, survives the query", "[TerminalQuery]")
{
    auto clock = ManualClock {};
    auto input = ScriptedQueryInput { clock };
    auto terminal = Terminal { std::make_unique<MockTerminalOutput>(), input, clock };
    input.pushRead({ CursorPositionReport { .row = 3, .column = 7 }, key(U'c') });

    auto const cursor = terminal.queryCursorPosition();

    REQUIRE(cursor.has_value());
    CHECK(cursor->first == 3);
    CHECK(cursor->second == 7);
    CHECK(codepoints(input.handedBack()) == std::vector<char32_t> { U'c' });
}

TEST_CASE("A key typed during a query that times out survives the query", "[TerminalQuery]")
{
    auto clock = ManualClock {};
    auto input = ScriptedQueryInput { clock };
    auto terminal = Terminal { std::make_unique<MockTerminalOutput>(), input, clock };
    input.pushRead({ key(U'b') });

    auto const size = terminal.queryCellSize();

    REQUIRE_FALSE(size.has_value());
    CHECK(size.error() == QueryUnanswered::NoReply);
    CHECK(codepoints(input.handedBack()) == std::vector<char32_t> { U'b' });
}

TEST_CASE("A ManualClock drives a query's timeout with no real time elapsing", "[TerminalQuery]")
{
    auto clock = ManualClock {};
    auto input = ScriptedQueryInput { clock };
    auto terminal = Terminal { std::make_unique<MockTerminalOutput>(), input, clock };
    auto const start = clock.now();

    auto const size = terminal.queryCellSize();

    REQUIRE_FALSE(size.has_value());
    CHECK(size.error() == QueryUnanswered::NoReply);
    // One wait for the whole timeout, and the query ended when the injected clock said so. A query
    // reading the real clock spins against the exhausted script instead, so it polls many times.
    CHECK(input.timeouts() == std::vector<int> { 100 });
    CHECK(clock.now() - start == std::chrono::milliseconds(100));
    CHECK(input.handedBack().empty());
}

TEST_CASE("An early empty read does not end a query before its deadline", "[TerminalQuery]")
{
    // A resize, a wakeup or half of an escape sequence returns a poll early with nothing decoded.
    // Only the deadline may end the wait, or the reply that follows is lost to a false NoReply.
    auto clock = ManualClock {};
    auto input = ScriptedQueryInput { clock };
    auto terminal = Terminal { std::make_unique<MockTerminalOutput>(), input, clock };
    input.pushRead({});
    input.pushRead({ CellSizeReport { .height = 20, .width = 10 } });

    auto const size = terminal.queryCellSize();

    REQUIRE(size.has_value());
    CHECK(size->first == 10);
    CHECK(input.timeouts().size() == 2);
}

TEST_CASE("A query that was never sent is told apart from one nobody answered", "[TerminalQuery]")
{
    // `(0, 0)` used to answer both, and is not even a legal cursor position.
    auto notAsked = Terminal { std::make_unique<MockTerminalOutput>() };
    REQUIRE_FALSE(notAsked.queryCellSize().has_value());
    CHECK(notAsked.queryCellSize().error() == QueryUnanswered::NotAsked);
    REQUIRE_FALSE(notAsked.queryDeviceAttributes().has_value());
    CHECK(notAsked.queryDeviceAttributes().error() == QueryUnanswered::NotAsked);

    auto clock = ManualClock {};
    auto input = ScriptedQueryInput { clock };
    auto silent = Terminal { std::make_unique<MockTerminalOutput>(), input, clock };
    REQUIRE_FALSE(silent.queryCursorPosition().has_value());
    CHECK(silent.queryCursorPosition().error() == QueryUnanswered::NoReply);
}

TEST_CASE("A mock terminal with no query input answers the cursor position from the mock", "[TerminalQuery]")
{
    auto terminal = Terminal { std::make_unique<MockTerminalOutput>() };

    auto const cursor = terminal.queryCursorPosition();

    REQUIRE(cursor.has_value());
    CHECK(cursor->first == 1);
    CHECK(cursor->second == 1);
}

namespace
{

/// An output that is not a MockTerminalOutput but says it is a simulation: what the terminal reads
/// now is that property, not the output's type.
class SimulatedOutput final: public core::tui::TerminalOutput
{
  public:
    [[nodiscard]] auto kind() const noexcept -> core::tui::OutputKind override
    {
        return core::tui::OutputKind::Simulation;
    }

    [[nodiscard]] auto simulatedCursorPosition() const noexcept -> std::optional<std::pair<int, int>> override
    {
        return std::pair { 4, 9 };
    }

    /// @return Every byte that reached the destination.
    [[nodiscard]] auto captured() const noexcept -> std::string const& { return _captured; }

  protected:
    void writeToDestination(std::string_view bytes) override { _captured.append(bytes); }

  private:
    std::string _captured;
};

/// A device output whose destination is a string: a terminal stands behind it, so it is asked.
class DeviceOutput final: public core::tui::TerminalOutput
{
  public:
    /// @param captured Where every byte that reaches the destination is appended; must outlive this.
    explicit DeviceOutput(std::string& captured) noexcept: _captured(captured) {}

  protected:
    void writeToDestination(std::string_view bytes) override { _captured.append(bytes); }

  private:
    std::string& _captured;
};

} // namespace

TEST_CASE("An output that says it is a simulation is not asked, whatever its type", "[TerminalQuery]")
{
    auto output = std::make_unique<SimulatedOutput>();
    auto const& written = output->captured();
    auto terminal = Terminal { std::move(output) };

    // Site 3: the cursor comes from the simulation, without a query on the wire.
    auto const cursor = terminal.queryCursorPosition();
    REQUIRE(cursor.has_value());
    CHECK(*cursor == std::pair { 4, 9 });
    // Sites 1 and 2: no query input and a simulation, so nothing is asked at all.
    CHECK(terminal.queryCellSize().error() == QueryUnanswered::NotAsked);
    CHECK(written.empty());
}

TEST_CASE("A device output is asked, and answers from the reply rather than a model", "[TerminalQuery]")
{
    auto clock = ManualClock {};
    auto input = ScriptedQueryInput { clock };
    input.pushRead({ CursorPositionReport { .row = 12, .column = 34 } });
    auto written = std::string {};
    auto terminal = Terminal { std::make_unique<DeviceOutput>(written), input, clock };

    auto const cursor = terminal.queryCursorPosition();

    REQUIRE(cursor.has_value());
    CHECK(*cursor == std::pair { 12, 34 });
    CHECK(written.contains("\033[6n")); // The DSR went out.
}

TEST_CASE("A color scheme report read during a query is dispatched, not handed back", "[TerminalQuery]")
{
    auto clock = ManualClock {};
    auto input = ScriptedQueryInput { clock };
    auto terminal = Terminal { std::make_unique<MockTerminalOutput>(), input, clock };
    input.pushRead({ ColorSchemeReport { .mode = 2 }, CellSizeReport { .height = 20, .width = 10 } });

    std::ignore = terminal.queryCellSize();

    CHECK(terminal.colorScheme() == ColorScheme::Light);
    CHECK(input.handedBack().empty());
}

TEST_CASE("A DA1 reply decodes to the attributes the terminal listed, as a protocol report",
          "[TerminalQuery]")
{
    auto parser = core::tui::VtParser {};

    auto const events = parser.feed("\033[?62;4;22c");

    REQUIRE(events.size() == 1);
    auto const* report = std::get_if<DeviceAttributesReport>(events.data());
    REQUIRE(report != nullptr);
    CHECK(report->attributes == std::vector<int> { 62, 4, 22 });
    CHECK(core::tui::isProtocolReport(events.front()));
}

TEST_CASE("A cell size reply as the terminal writes it, height first, answers the query width first",
          "[TerminalQuery]")
{
    // The reply to CSI 16 t is CSI 6 ; height ; width t, and the query answers (width, height). A swap
    // on either side passes every case that scripts an already-decoded report, and sizes each image
    // with its sides exchanged, so this one starts from the bytes.
    auto parser = core::tui::VtParser {};
    auto events = parser.feed("\033[6;20;10t");
    REQUIRE(events.size() == 1);
    CHECK(core::tui::isProtocolReport(events.front()));

    auto clock = ManualClock {};
    auto input = ScriptedQueryInput { clock };
    auto terminal = Terminal { std::make_unique<MockTerminalOutput>(), input, clock };
    input.pushRead(std::move(events));

    auto const size = terminal.queryCellSize();

    REQUIRE(size.has_value());
    CHECK(size->first == 10);
    CHECK(size->second == 20);
    CHECK(input.timeouts().size() == 1);
}

TEST_CASE("A DA1 query reports whether the terminal advertises Sixel, and hands back input read meanwhile",
          "[TerminalQuery]")
{
    auto clock = ManualClock {};
    auto sixelInput = ScriptedQueryInput { clock };
    auto sixel = Terminal { std::make_unique<MockTerminalOutput>(), sixelInput, clock };
    sixelInput.pushRead({ key(U's'), DeviceAttributesReport { .attributes = { 62, 4, 22 } } });

    auto const withSixel = sixel.queryDeviceAttributes();

    REQUIRE(withSixel.has_value());
    CHECK(core::tui::advertisesSixel(*withSixel));
    CHECK(codepoints(sixelInput.handedBack()) == std::vector<char32_t> { U's' });

    auto plainInput = ScriptedQueryInput { clock };
    auto plain = Terminal { std::make_unique<MockTerminalOutput>(), plainInput, clock };
    plainInput.pushRead({ DeviceAttributesReport { .attributes = { 62, 22 } } });

    auto const withoutSixel = plain.queryDeviceAttributes();

    REQUIRE(withoutSixel.has_value());
    CHECK_FALSE(core::tui::advertisesSixel(*withoutSixel));
}

TEST_CASE("A terminal that never answers DA1 reports NoReply at the deadline instead of hanging",
          "[TerminalQuery]")
{
    auto clock = ManualClock {};
    auto input = ScriptedQueryInput { clock };
    auto terminal = Terminal { std::make_unique<MockTerminalOutput>(), input, clock };
    input.pushRead({ key(U'k') });

    auto const attributes = terminal.queryDeviceAttributes();

    REQUIRE_FALSE(attributes.has_value());
    CHECK(attributes.error() == QueryUnanswered::NoReply);
    CHECK(input.timeouts() == std::vector<int> { 100, 100 });
    CHECK(codepoints(input.handedBack()) == std::vector<char32_t> { U'k' });
}

TEST_CASE("A DEC mode query that was never sent is told apart from one the terminal declined",
          "[TerminalQuery]")
{
    auto notAsked = Terminal { std::make_unique<MockTerminalOutput>() };
    CHECK(notAsked.queryDecMode(2035) == DecModeStatus::NotAsked);

    auto clock = ManualClock {};
    auto declinedInput = ScriptedQueryInput { clock };
    auto declined = Terminal { std::make_unique<MockTerminalOutput>(), declinedInput, clock };
    declinedInput.pushRead({ DecModeReport { .mode = 2035, .status = 0 } });
    CHECK(declined.queryDecMode(2035) == DecModeStatus::NotRecognized);

    auto silentInput = ScriptedQueryInput { clock };
    auto silent = Terminal { std::make_unique<MockTerminalOutput>(), silentInput, clock };
    CHECK(silent.queryDecMode(2035) == DecModeStatus::NoReply);
}

TEST_CASE("A DEC mode query reports each DECRPM status as its own answer", "[TerminalQuery]")
{
    auto const expected = std::vector<std::pair<int, DecModeStatus>> {
        { 1, DecModeStatus::Set },
        { 2, DecModeStatus::Reset },
        { 3, DecModeStatus::PermanentlySet },
        { 4, DecModeStatus::PermanentlyReset },
    };
    for (auto const& [status, answer]: expected)
    {
        auto clock = ManualClock {};
        auto input = ScriptedQueryInput { clock };
        auto terminal = Terminal { std::make_unique<MockTerminalOutput>(), input, clock };
        input.pushRead({ DecModeReport { .mode = 2029, .status = status } });
        CHECK(terminal.queryDecMode(2029) == answer);
    }
}

TEST_CASE("A DEC mode report for another mode is not the reply", "[TerminalQuery]")
{
    auto clock = ManualClock {};
    auto input = ScriptedQueryInput { clock };
    auto terminal = Terminal { std::make_unique<MockTerminalOutput>(), input, clock };
    input.pushRead({ DecModeReport { .mode = 2029, .status = 1 }, key(U'k') });

    CHECK(terminal.queryDecMode(2035) == DecModeStatus::NoReply);
    // The stale report is a protocol report and is consumed; the key is input and is handed back.
    CHECK(codepoints(input.handedBack()) == std::vector<char32_t> { U'k' });
}

TEST_CASE("Events handed back to TerminalInput are the next poll's, in arrival order", "[TerminalQuery]")
{
    auto input = TerminalInput {};
    input.unread({ key(U'x') });
    input.unread({ key(U'y') });

    auto const events = input.poll(0);

    CHECK(codepoints(events) == std::vector<char32_t> { U'x', U'y' });
    CHECK(input.takePending().empty());
}

TEST_CASE("The runtime's terminal input source hands back what a query did not want", "[TerminalQuery]")
{
    // The adapter's whole job on this path: what a query read and did not consume is taken
    // through `takePending`, so the runtime can deliver it without waiting on a handle that will
    // never become readable on its account.
    auto terminal = Terminal { std::make_unique<MockTerminalOutput>() };
    terminal.input().unread({ key(U'z') });
    auto source = core::tui::runtime::TerminalInputSource { terminal };

    CHECK(codepoints(source.takePending()) == std::vector<char32_t> { U'z' });
    CHECK(source.takePending().empty());
}
