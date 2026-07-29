// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include <vthost/tmux/ControlOutput.h>

using vthost::tmux::ControlOutput;
using vthost::tmux::ControlOutputLimits;
using vthost::tmux::escapeOutput;
using namespace std::chrono_literals;

namespace
{

/// A harness collecting every emitted protocol line, with a controllable clock.
struct QueueHarness
{
    std::vector<std::string> lines;
    std::chrono::steady_clock::time_point now {};
    ControlOutput queue { [this](std::string line) { lines.push_back(std::move(line)); },
                          // Tiny limits so tests exercise budgeting without kilobytes.
                          ControlOutputLimits { .bufferLow = 8, .bufferHigh = 64, .writeMinimum = 4 } };

    [[nodiscard]] std::size_t countPrefix(std::string_view prefix) const
    {
        return static_cast<std::size_t>(
            std::ranges::count_if(lines, [&](auto const& line) { return line.starts_with(prefix); }));
    }
};

} // namespace

TEST_CASE("escapeOutput escapes control bytes and backslash as three octal digits", "[vthost][control]")
{
    CHECK(escapeOutput("plain") == "plain");
    CHECK(escapeOutput("\x1b[1m") == "\\033[1m");
    CHECK(escapeOutput("a\\b") == "a\\134b");
    CHECK(escapeOutput(std::string_view { "\x00\x01", 2 }) == "\\000\\001");
    CHECK(escapeOutput("\r\n") == "\\015\\012");
    // 0x7F and >= 0x80 pass through RAW — including UTF-8 sequences.
    CHECK(escapeOutput("\x7f") == "\x7f");
    CHECK(escapeOutput("caf\xc3\xa9") == "caf\xc3\xa9");
}

TEST_CASE("a notification queued before output flushes immediately", "[vthost][control]")
{
    auto h = QueueHarness {};
    h.queue.enqueueNotification("%window-add @1");
    REQUIRE(h.lines.size() == 1);
    CHECK(h.lines[0] == "%window-add @1\n");
}

TEST_CASE("a notification never lands inside or before an earlier output block", "[vthost][control]")
{
    auto h = QueueHarness {};

    // A block larger than any single pass budget (bufferHigh/1/3 = 21).
    auto const payload = std::string(50, 'x');
    h.queue.enqueueOutput(7, payload, h.now);
    h.queue.enqueueNotification("%layout-change @1 dummy dummy ");

    // Nothing emitted yet: notifications behind the block must wait.
    REQUIRE(h.lines.empty());

    h.queue.pump(0, h.now);
    // First pass: some %output, but the block is not done -> still no notification.
    REQUIRE(h.countPrefix("%output %7 ") >= 1);
    CHECK(h.countPrefix("%layout-change") == 0);

    while (h.queue.pendingBytes(7) > 0)
        h.queue.pump(0, h.now);

    // The notification flushed exactly once, AFTER the last %output line.
    CHECK(h.countPrefix("%layout-change") == 1);
    CHECK(h.lines.back() == "%layout-change @1 dummy dummy \n");

    // The re-assembled escaped payload matches the input exactly.
    auto reassembled = std::string {};
    for (auto const& line: h.lines)
        if (line.starts_with("%output %7 "))
            reassembled += line.substr(11, line.size() - 12); // strip prefix and LF
    CHECK(reassembled == payload);
}

TEST_CASE("the byte budget is shared fairly across panes with pending output", "[vthost][control]")
{
    auto h = QueueHarness {};
    h.queue.enqueueOutput(1, std::string(40, 'a'), h.now);
    h.queue.enqueueOutput(2, std::string(40, 'b'), h.now);

    h.queue.pump(0, h.now);

    // Both panes progressed in the same pass (fairness), neither finished
    // (per-pane budget = 64/2/3 = 10 bytes).
    CHECK(h.countPrefix("%output %1 ") == 1);
    CHECK(h.countPrefix("%output %2 ") == 1);
    CHECK(h.queue.pendingBytes(1) == 30);
    CHECK(h.queue.pendingBytes(2) == 30);
}

TEST_CASE("a congested connection still makes writeMinimum progress", "[vthost][control]")
{
    auto h = QueueHarness {};
    h.queue.enqueueOutput(1, std::string(40, 'a'), h.now);

    // buffered beyond bufferHigh: headroom 0, budget floors at writeMinimum.
    h.queue.pump(1000, h.now);
    CHECK(h.queue.pendingBytes(1) == 36);
}

TEST_CASE("pause-after drops the backlog and emits one %pause; %continue resumes", "[vthost][control]")
{
    auto h = QueueHarness {};
    h.queue.setPauseAfter(100ms);

    h.queue.enqueueOutput(3, "stale data", h.now);
    h.now += 200ms; // the client did not drain in time
    h.queue.pump(0, h.now);

    CHECK(h.queue.isPaused(3));
    CHECK(h.countPrefix("%pause %3") == 1);
    CHECK(h.queue.pendingBytes(3) == 0);
    CHECK(h.countPrefix("%output %3 ") == 0); // the backlog was dropped, not sent

    // While paused, new output is discarded at the source.
    h.queue.enqueueOutput(3, "dropped", h.now);
    CHECK(h.queue.pendingBytes(3) == 0);

    h.queue.continuePane(3);
    CHECK_FALSE(h.queue.isPaused(3));
    CHECK(h.countPrefix("%continue %3") == 1);

    h.queue.enqueueOutput(3, "fresh", h.now);
    h.queue.pump(0, h.now);
    CHECK(h.countPrefix("%output %3 fresh") == 1);
}

TEST_CASE("a paused pane's dropped blocks do not gate later notifications", "[vthost][control]")
{
    auto h = QueueHarness {};
    h.queue.setPauseAfter(100ms);

    h.queue.enqueueOutput(4, std::string(50, 'z'), h.now);
    h.queue.enqueueNotification("%sessions-changed");
    REQUIRE(h.lines.empty()); // gated behind the block

    h.now += 200ms;
    h.queue.pump(0, h.now); // pauses pane 4, discards its block

    // The discarded block no longer blocks the queue: both lines flushed.
    CHECK(h.countPrefix("%pause %4") == 1);
    CHECK(h.countPrefix("%sessions-changed") == 1);
}

TEST_CASE("extended output carries the block age before the colon", "[vthost][control]")
{
    auto h = QueueHarness {};
    h.queue.setExtendedOutput(true);

    h.queue.enqueueOutput(5, "late", h.now);
    h.now += 250ms;
    h.queue.pump(0, h.now);

    REQUIRE(h.lines.size() == 1);
    CHECK(h.lines[0] == "%extended-output %5 250 : late\n");

    // Freshly queued output reports age zero.
    h.queue.enqueueOutput(5, "now", h.now);
    h.queue.pump(0, h.now);
    CHECK(h.lines.back() == "%extended-output %5 0 : now\n");
}

TEST_CASE("pausePane force-pauses with one %pause and drops the backlog", "[vthost][control]")
{
    auto h = QueueHarness {};

    h.queue.enqueueOutput(6, "pending", h.now);
    h.queue.pausePane(6);

    CHECK(h.queue.isPaused(6));
    CHECK(h.countPrefix("%pause %6") == 1);
    CHECK(h.queue.pendingBytes(6) == 0);

    h.queue.pausePane(6); // idempotent while paused
    CHECK(h.countPrefix("%pause %6") == 1);

    h.queue.continuePane(6);
    CHECK(h.countPrefix("%continue %6") == 1);
    h.queue.enqueueOutput(6, "fresh", h.now);
    h.queue.pump(0, h.now);
    CHECK(h.countPrefix("%output %6 fresh") == 1);
}

TEST_CASE("a disabled pane is silenced without any pause handshake", "[vthost][control]")
{
    auto h = QueueHarness {};

    h.queue.enqueueOutput(7, "pending", h.now);
    h.queue.setPaneEnabled(7, false);
    CHECK(h.queue.pendingBytes(7) == 0);

    // While off, new output is dropped at the source — silently.
    h.queue.enqueueOutput(7, "dropped", h.now);
    h.queue.pump(0, h.now);
    CHECK(h.countPrefix("%output %7 ") == 0);
    CHECK(h.countPrefix("%pause %7") == 0);

    h.queue.setPaneEnabled(7, true);
    h.queue.enqueueOutput(7, "visible", h.now);
    h.queue.pump(0, h.now);
    CHECK(h.countPrefix("%output %7 visible") == 1);
}
