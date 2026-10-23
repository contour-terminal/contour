// SPDX-License-Identifier: Apache-2.0
//
// runGuarded: every way out of a guarded call disarms its deadline, and no exception escapes it.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <stdexcept>
#include <vector>

#include <vtextension/DeadlineGuard.hpp>
#include <vtextension/testing/FakeWasmBackend.hpp>

using namespace vtextension;

namespace
{

/// Records what is armed and disarmed; can be told to fail arming.
class RecordingWatchdog final: public IDeadlineWatchdog
{
  public:
    void armDeadline(WatchToken token, std::chrono::milliseconds /*timeout*/) override
    {
        if (failArming)
            throw std::runtime_error("cannot arm");
        armed.push_back(token);
    }

    void disarm(WatchToken token) override { disarmed.push_back(token); }

    std::vector<WatchToken> armed;
    std::vector<WatchToken> disarmed;
    bool failArming = false;
};

/// A token for an instance that is never called, whose deadline never expires.
[[nodiscard]] WatchToken someToken()
{
    static auto instance = vtextension::testing::FakeWasmInstance {};
    static auto state = std::atomic<DeadlineState> { DeadlineState::Running };
    return WatchToken { .instance = &instance, .state = &state };
}

constexpr auto Timeout = std::chrono::milliseconds { 50 };

} // namespace

TEST_CASE("DeadlineGuard.disarmsOnEveryExit", "[vtextension]")
{
    auto const token = someToken();

    SECTION("a value")
    {
        auto watchdog = RecordingWatchdog {};
        auto const result =
            runGuarded(watchdog, token, Timeout, [] { return std::expected<int, ExtensionError> { 42 }; });
        CHECK(result == 42);
        CHECK(watchdog.armed == std::vector { token });
        CHECK(watchdog.disarmed == std::vector { token });
    }

    SECTION("an error")
    {
        auto watchdog = RecordingWatchdog {};
        auto const result = runGuarded(watchdog, token, Timeout, [] {
            return std::expected<int, ExtensionError> { std::unexpected(ExtensionError::OutOfMemory) };
        });
        CHECK(result.error() == ExtensionError::OutOfMemory);
        CHECK(watchdog.disarmed == std::vector { token });
    }

    SECTION("an exception, which becomes a trap")
    {
        auto watchdog = RecordingWatchdog {};
        auto const result = runGuarded(
            watchdog, token, Timeout, []() -> std::expected<int, ExtensionError> { throw std::bad_alloc(); });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ExtensionError::Trapped);
        CHECK(watchdog.armed == std::vector { token });
        CHECK(watchdog.disarmed == std::vector { token });
    }

    SECTION("arming fails: nothing runs, nothing is left armed")
    {
        auto watchdog = RecordingWatchdog {};
        watchdog.failArming = true;
        auto ran = false;
        auto const result = runGuarded(watchdog, token, Timeout, [&] {
            ran = true;
            return std::expected<void, ExtensionError> {};
        });
        CHECK(result.error() == ExtensionError::Trapped);
        CHECK_FALSE(ran);
        CHECK(watchdog.disarmed.empty());
    }
}
