// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>
#include <vtpty/test/PtyReading.hpp>

#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace std::chrono_literals;
using vtpty::testing::readUntil;

namespace
{

/// A PTY that hands out what it was given one byte per read, as a program writing in many small pieces
/// reaches its reader, and answers at once with an end of file once it has nothing left, as a PTY whose
/// far end has gone may.
class TricklingPty final: public vtpty::MockPty
{
  public:
    /// @param output What to hand out.
    /// @param onRead What each read does besides, e.g. let time pass on a test clock.
    TricklingPty(std::string_view output, std::function<void()> onRead):
        MockPty { vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) } },
        _onRead { std::move(onRead) }
    {
        appendStdOutBuffer(output);
    }

    [[nodiscard]] std::optional<ReadResult> read(crispy::BufferObject<char>& storage,
                                                 std::optional<std::chrono::milliseconds> timeout,
                                                 [[maybe_unused]] size_t size) override
    {
        ++_reads;
        _onRead();
        return MockPty::read(storage, timeout, 1);
    }

    /// @return How many reads were made.
    [[nodiscard]] size_t reads() const noexcept { return _reads; }

  private:
    std::function<void()> _onRead;
    size_t _reads = 0;
};

/// A PTY whose every read comes back at once with nothing, and says why in errno: EAGAIN for nothing yet,
/// as a read whose wait was cut short does; EIO for a far end that is gone, as a PTY whose child exited
/// does.
class EmptyHandedPty final: public vtpty::MockPty
{
  public:
    /// @param error The errno each read leaves.
    explicit EmptyHandedPty(int error):
        MockPty { vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) } }, _error { error }
    {
    }

    [[nodiscard]] std::optional<ReadResult> read(crispy::BufferObject<char>& /*storage*/,
                                                 std::optional<std::chrono::milliseconds> /*timeout*/,
                                                 size_t /*size*/) override
    {
        ++_reads;
        errno = _error;
        return std::nullopt;
    }

    /// @return How many reads were made.
    [[nodiscard]] size_t reads() const noexcept { return _reads; }

  private:
    int _error;
    size_t _reads = 0;
};

/// @return A Wait that lets the time pass on @p clock instead of in the world.
vtpty::testing::Wait passingOn(core::platform::ManualClock& clock)
{
    return [&clock](std::chrono::milliseconds duration) {
        clock.advance(duration);
    };
}

} // namespace

TEST_CASE("readUntil reads for as long as it takes, however many reads that is", "[vtpty][readUntil]")
{
    // 2000 single bytes, 10 ms apart: 20 seconds, well inside the deadline -- and far more reads than
    // a bound on their number would allow.
    auto clock = core::platform::ManualClock {};
    auto pty = TricklingPty { std::string(2000, 'x'), [&clock] { clock.advance(10ms); } };

    auto const text = readUntil(
        pty, [](std::string_view soFar) { return soFar.size() == 2000; }, 30s, clock, passingOn(clock));

    CHECK(text.size() == 2000);
    CHECK(pty.reads() == 2000);
}

TEST_CASE("readUntil gives up at its deadline", "[vtpty][readUntil]")
{
    // A program that never stops writing, 10 ms a byte: 30 seconds hold 3000 reads.
    auto clock = core::platform::ManualClock {};
    auto pty = TricklingPty { std::string(5000, 'x'), [&clock] { clock.advance(10ms); } };

    auto const text = readUntil(pty, [](std::string_view) { return false; }, 30s, clock, passingOn(clock));

    CHECK(text.size() == 3000);
    CHECK(pty.reads() == 3000);
}

TEST_CASE("readUntil stops at once when the far end is gone", "[vtpty][readUntil]")
{
    // Whatever the deadline: a test whose shell died fails at once rather than after it.
    auto clock = core::platform::ManualClock {};

    SECTION("an end of file")
    {
        auto pty = TricklingPty { "abc", [&clock] { clock.advance(10ms); } };
        CHECK(readUntil(pty, [](std::string_view) { return false; }, 1s, clock, passingOn(clock)) == "abc");
        CHECK(pty.reads() == 4);
    }

    SECTION("a read that fails for good")
    {
        auto pty = EmptyHandedPty { EIO };
        CHECK(readUntil(pty, [](std::string_view) { return false; }, 1s, clock, passingOn(clock)).empty());
        CHECK(pty.reads() == 1);
    }
}

TEST_CASE("readUntil waits out a read that came back early with nothing", "[vtpty][readUntil]")
{
    // Every read answers at once with nothing yet. Each one waits out its poll interval rather than
    // reading again at once, so 250 ms hold five reads rather than as many as the CPU can make.
    constexpr auto Timeout = 250ms;
    auto clock = core::platform::ManualClock {};
    auto pty = EmptyHandedPty { EAGAIN };

    auto const text =
        readUntil(pty, [](std::string_view) { return false; }, Timeout, clock, passingOn(clock));

    CHECK(text.empty());
    CHECK(pty.reads() == static_cast<size_t>(Timeout / vtpty::testing::ReadPollInterval));
}
