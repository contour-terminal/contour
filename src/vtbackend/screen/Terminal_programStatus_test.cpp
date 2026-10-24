// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/ProgramStatusSet.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using vtbackend::ColumnCount;
using vtbackend::LineCount;
using vtbackend::MockTerm;
using vtbackend::ProgramState;
using vtbackend::ProgramStatusReason;

TEST_CASE("Terminal stores OSC 7501 and raises programStatusChanged", "[vtbackend][programstatus]")
{
    auto mock = MockTerm<> { ColumnCount(40), LineCount(5) };
    auto const& status = mock.terminal.programStatus();

    mock.writeToScreen("\033]7501;state=working:app=brew\033\\");
    REQUIRE(status.records().size() == 1);
    CHECK(status.records().front().app == "brew");
    REQUIRE(mock.programStatusChanges.size() == 1);
    CHECK(mock.programStatusChanges.back().reason == ProgramStatusReason::Report);
    CHECK(mock.programStatusChanges.back().updated == std::vector<std::string> { "" });

    SECTION("a prompt drops it")
    {
        mock.writeToScreen("\033]133;A\007");
        CHECK(status.records().empty());
        REQUIRE(mock.programStatusChanges.size() == 2);
        CHECK(mock.programStatusChanges.back().reason == ProgramStatusReason::Prompt);

        // A second prompt has nothing to drop, and says nothing.
        mock.writeToScreen("\033]133;A\007");
        CHECK(mock.programStatusChanges.size() == 2);
    }

    SECTION("done survives a prompt, DECSTR and the alternate screen; RIS removes it")
    {
        mock.writeToScreen("\033]7501;state=done\033\\");
        mock.writeToScreen("\033]133;A\007");
        mock.writeToScreen("\033[!p");
        mock.writeToScreen("\033[?1049h");
        mock.writeToScreen("\033[?1049l");
        REQUIRE(status.records().size() == 1);
        CHECK(status.records().front().state == ProgramState::Done);
        auto const changesBefore = mock.programStatusChanges.size();

        mock.writeToScreen("\033c");
        CHECK(status.records().empty());
        REQUIRE(mock.programStatusChanges.size() == changesBefore + 1);
        CHECK(mock.programStatusChanges.back().reason == ProgramStatusReason::Reset);
    }

    SECTION("the process's exit drops working")
    {
        mock.writeToScreen("\033]7501;state=error:id=lint\033\\");
        mock.terminal.markProcessExited();
        REQUIRE(status.records().size() == 1);
        CHECK(status.records().front().id == "lint");
        CHECK(mock.programStatusChanges.back().reason == ProgramStatusReason::ProcessExit);
    }

    SECTION("a clear removes the subtree")
    {
        mock.writeToScreen("\033]7501;state=working:id=a/b\033\\");
        mock.writeToScreen("\033]7501;state=clear\033\\");
        CHECK(status.records().empty());
        CHECK(mock.programStatusChanges.back().removed.size() == 2);
    }

    SECTION("a rejected report changes nothing")
    {
        mock.writeToScreen("\033]7501;state=bogus\033\\");
        mock.writeToScreen("\033]7501;state=idle:id=a//b\033\\");
        CHECK(status.records().size() == 1);
        CHECK(mock.programStatusChanges.size() == 1);
    }

    SECTION("a mirror replaces the set")
    {
        auto replacement = std::vector { vtbackend::ProgramStatusRecord {
            .id = "host",
            .state = ProgramState::Blocked,
            .kind = vtbackend::BlockedKind::Question,
            .progress = std::nullopt,
            .app = {},
            .title = {},
            .message = "Continue?",
        } };
        {
            auto const _ = std::lock_guard { mock.terminal };
            mock.terminal.replaceProgramStatus(std::move(replacement));
        }
        REQUIRE(status.records().size() == 1);
        CHECK(status.records().front().id == "host");
        CHECK(mock.programStatusChanges.back().reason == ProgramStatusReason::Mirror);
        CHECK(mock.programStatusChanges.back().removed == std::vector<std::string> { "" });
    }
}

TEST_CASE("Terminal answers OSC 7501 detection", "[vtbackend][programstatus]")
{
    auto mock = MockTerm<> { ColumnCount(40), LineCount(5) };
    mock.writeToScreen("\033]7501;?\033\\");
    mock.terminal.flushInput();
    CHECK(mock.replyData()
          == "\033]7501;?:states=idle,working,done,blocked,error:kinds=permission,question,auth\033\\");
    CHECK(mock.programStatusChanges.empty());
}

TEST_CASE("Terminal: a loop of fresh OSC 7501 ids stays at the cap", "[vtbackend][programstatus]")
{
    auto mock = MockTerm<> { ColumnCount(40), LineCount(5) };
    for (auto const index: std::views::iota(0, 1000))
        mock.writeToScreen(std::format("\033]7501;state=working:id=job{}\033\\", index));
    CHECK(mock.terminal.programStatus().records().size() == vtbackend::MaxProgramStatusRecords);
    CHECK(mock.terminal.programStatus().records().back().id == "job999");
}

TEST_CASE("Terminal.markProcessExited waits for the state lock", "[vtbackend][programstatus][threading]")
{
    // The PTY's owner calls it from its own thread, while the parser thread may be inside a parse: it must
    // take the lock the parser holds, not race it.
    auto mock = MockTerm<> { ColumnCount(40), LineCount(5) };
    mock.writeToScreen("\033]7501;state=working\033\\");

    auto parserHold = std::unique_lock { mock.terminal };
    auto started = std::atomic<bool> { false };
    auto completed = std::atomic<bool> { false };
    auto worker = std::thread { [&]() {
        started.store(true, std::memory_order_release);
        mock.terminal.markProcessExited();
        completed.store(true, std::memory_order_release);
    } };

    while (!started.load(std::memory_order_acquire))
        std::this_thread::yield();
    std::this_thread::sleep_for(200ms);
    CHECK_FALSE(completed.load(std::memory_order_acquire));
    CHECK(mock.terminal.programStatus().records().size() == 1);

    parserHold.unlock();
    worker.join();

    CHECK(completed.load(std::memory_order_acquire));
    auto const _ = std::lock_guard { mock.terminal };
    CHECK(mock.terminal.programStatus().records().empty());
}

TEST_CASE("Terminal: program status read under the lock while the parser writes it",
          "[vtbackend][programstatus][threading]")
{
    // The GUI reads the records under the lock while the parser thread changes them: every read sees a
    // whole set, never one being changed.
    auto mock = MockTerm<> { ColumnCount(40), LineCount(5) };
    auto done = std::atomic<bool> { false };
    auto writer = std::thread { [&]() {
        for (auto const index: std::views::iota(0, 500))
            mock.writeToScreen(std::format("\033]7501;state=working:id=w{}:app=writer\033\\", index));
        done.store(true, std::memory_order_release);
    } };

    auto largest = std::size_t { 0 };
    while (!done.load(std::memory_order_acquire))
    {
        auto const _ = std::lock_guard { mock.terminal };
        for (auto const& record: mock.terminal.programStatus().records())
            CHECK(record.app == "writer");
        largest = std::max(largest, mock.terminal.programStatus().records().size());
    }
    writer.join();
    CHECK(largest <= vtbackend::MaxProgramStatusRecords);
}
