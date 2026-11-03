// SPDX-License-Identifier: Apache-2.0
#include <core/platform/detail/LinuxPerfPlan.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cerrno>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using core::platform::detail::combinePmuReadings;
using core::platform::detail::CorePmu;
using core::platform::detail::decideMeasure;
using core::platform::detail::discoverCorePmus;
using core::platform::detail::hardwareConfig;
using core::platform::detail::InheritMode;
using core::platform::detail::openMeasure;
using core::platform::detail::OpenOutcome;
using core::platform::detail::PerfOpener;
using core::platform::detail::perfOpenFailureReason;
using core::platform::detail::PmuReading;
using core::platform::detail::readParanoidLevel;
using core::platform::testing::InMemoryFileSystem;

namespace
{

constexpr auto Devices = "/sys/bus/event_source/devices/";

/// Adds a PMU directory with its type, and a "cpus" file when it is a core PMU.
void addPmu(InMemoryFileSystem& fs, std::string const& name, int type, bool hasCpus)
{
    fs.addFile(Devices + name + "/type", std::to_string(type) + "\n");
    if (hasCpus)
        fs.addFile(Devices + name + "/cpus", "0-7\n");
}

} // namespace

TEST_CASE("discoverCorePmus finds the plain cpu PMU", "[cpucounters]")
{
    auto fs = InMemoryFileSystem {};
    addPmu(fs, "cpu", 4, false);
    auto const pmus = discoverCorePmus(fs, "/sys");
    REQUIRE(pmus.size() == 1);
    CHECK(pmus[0].name == "cpu");
    CHECK(pmus[0].type == 4);
}

TEST_CASE("discoverCorePmus finds hybrid core PMUs and ignores the rest", "[cpucounters]")
{
    auto fs = InMemoryFileSystem {};
    addPmu(fs, "cpu_core", 4, true);
    addPmu(fs, "cpu_atom", 10, true);
    addPmu(fs, "power", 12, false);
    auto const pmus = discoverCorePmus(fs, "/sys");
    REQUIRE(pmus.size() == 2);
    CHECK(pmus[0].name == "cpu_atom");
    CHECK(pmus[0].type == 10);
    CHECK(pmus[1].name == "cpu_core");
    CHECK(pmus[1].type == 4);
}

TEST_CASE("discoverCorePmus finds ARM core PMUs", "[cpucounters]")
{
    auto fs = InMemoryFileSystem {};
    addPmu(fs, "armv8_pmuv3_0", 8, true);
    addPmu(fs, "apple_icestorm_pmu", 9, true);
    CHECK(discoverCorePmus(fs, "/sys").size() == 2);
}

TEST_CASE("discoverCorePmus finds nothing on an empty sysfs", "[cpucounters]")
{
    auto fs = InMemoryFileSystem {};
    CHECK(discoverCorePmus(fs, "/sys").empty());
}

TEST_CASE("hardwareConfig adds the PMU type only with several PMUs", "[cpucounters]")
{
    CHECK(hardwareConfig(1, CorePmu { "cpu_atom", 10 }, 2) == (1 | (10ULL << 32)));
    CHECK(hardwareConfig(1, CorePmu { "armv8_pmuv3_0", 8 }, 1) == 1);
}

TEST_CASE("combinePmuReadings adds readings that ran for the whole interval", "[cpucounters]")
{
    auto const start = std::array { PmuReading { 0, 0, 0 }, PmuReading { 0, 0, 0 } };
    auto const whole = std::array { PmuReading { 100, 10, 10 }, PmuReading { 50, 10, 10 } };
    CHECK(combinePmuReadings(start, whole) == 150);

    auto const hybrid = std::array { PmuReading { 100, 10, 6 }, PmuReading { 50, 10, 4 } };
    CHECK(combinePmuReadings(start, hybrid) == 150);
}

TEST_CASE("combinePmuReadings rejects multiplexed or missing readings", "[cpucounters]")
{
    auto const start = std::array { PmuReading { 0, 0, 0 }, PmuReading { 0, 0, 0 } };
    auto const multiplexed = std::array { PmuReading { 100, 10, 3 }, PmuReading { 50, 10, 4 } };
    CHECK(!combinePmuReadings(start, multiplexed).has_value());
    CHECK(!combinePmuReadings({}, {}).has_value());
    CHECK(!combinePmuReadings(start, std::array { PmuReading { 1, 1, 1 } }).has_value());
}

TEST_CASE("decideMeasure reports the first failure", "[cpucounters]")
{
    auto const allOpened = std::array { OpenOutcome { 0 }, OpenOutcome { 0 } };
    CHECK(decideMeasure(allOpened, 2, std::nullopt).empty());

    auto const denied = std::array { OpenOutcome { 0 }, OpenOutcome { EACCES } };
    CHECK_THAT(decideMeasure(denied, 2, 3), ContainsSubstring("perf_event_paranoid=3"));
}

TEST_CASE("perfOpenFailureReason phrases the usual failures", "[cpucounters]")
{
    CHECK_THAT(perfOpenFailureReason(ENOENT, std::nullopt), ContainsSubstring("no hardware PMU"));
    CHECK_THAT(perfOpenFailureReason(ENOSYS, std::nullopt), ContainsSubstring("seccomp"));
    CHECK_THAT(perfOpenFailureReason(EACCES, 2), ContainsSubstring("permission denied"));
    CHECK_THAT(perfOpenFailureReason(EPERM, 2),
               ContainsSubstring("operation not permitted (container seccomp?"));
    CHECK_THAT(perfOpenFailureReason(EPERM, 2), ContainsSubstring("perf_event_paranoid=2"));
    CHECK_THAT(perfOpenFailureReason(EMFILE, std::nullopt), ContainsSubstring("perf_event_open: "));
}

TEST_CASE("decideMeasure reports the first failure, not a later one", "[cpucounters]")
{
    auto const outcomes = std::array { OpenOutcome { EACCES }, OpenOutcome { ENOENT } };
    CHECK_THAT(decideMeasure(outcomes, 2, 2), ContainsSubstring("permission denied"));
}

TEST_CASE("decideMeasure blames the kernel, not a VM, for a missing extended type", "[cpucounters]")
{
    auto const outcomes = std::array { OpenOutcome { ENOENT } };
    CHECK_THAT(decideMeasure(outcomes, 2, std::nullopt), ContainsSubstring("Linux < 6.6"));
    CHECK_THAT(decideMeasure(outcomes, 1, std::nullopt), ContainsSubstring("no hardware PMU"));
}

TEST_CASE("perfOpenFailureReason copes with an unreadable paranoid level and errno 0", "[cpucounters]")
{
    CHECK_THAT(perfOpenFailureReason(EACCES, std::nullopt),
               ContainsSubstring("perf_event_paranoid unreadable"));
    CHECK(perfOpenFailureReason(0, 2).empty());
}

TEST_CASE("combinePmuReadings accepts a running time within 1 % of the enabled time", "[cpucounters]")
{
    auto const start = std::array { PmuReading { 0, 0, 0 } };
    CHECK(combinePmuReadings(start, std::array { PmuReading { 7, 100, 99 } }) == 7);
    CHECK(!combinePmuReadings(start, std::array { PmuReading { 7, 100, 98 } }).has_value());
}

TEST_CASE("combinePmuReadings judges the interval since the previous read, not the lifetime", "[cpucounters]")
{
    // Open for 30 s at full speed, then a 200 ms window in which the event ran half the time: the
    // cumulative ratio is still above 99 %, the interval's is 50 %.
    constexpr auto Second = std::uint64_t { 1'000'000'000 };
    constexpr auto Window = Second / 5;
    constexpr auto Long = 30 * Second;
    auto const before = std::array { PmuReading { 3000, Long, Long } };
    auto const after = std::array { PmuReading { 3010, Long + Window, Long + (Window / 2) } };
    CHECK(!combinePmuReadings(before, after).has_value());

    // The reverse: a multiplexed history does not poison a clean window.
    auto const flaky = std::array { PmuReading { 10, Long, Long / 2 } };
    auto const clean = std::array { PmuReading { 20, Long + Window, (Long / 2) + Window } };
    CHECK(combinePmuReadings(flaky, clean) == 20);
}

TEST_CASE("combinePmuReadings rejects readings that went backwards", "[cpucounters]")
{
    auto const before = std::array { PmuReading { 50, 10, 10 } };
    CHECK(!combinePmuReadings(before, std::array { PmuReading { 40, 12, 12 } }).has_value());
    CHECK(!combinePmuReadings(before, std::array { PmuReading { 60, 9, 9 } }).has_value());
}

TEST_CASE("discoverCorePmus prefers cpu over PMUs that have a cpus file", "[cpucounters]")
{
    auto fs = InMemoryFileSystem {};
    addPmu(fs, "cpu", 4, true);
    addPmu(fs, "uncore_imc", 20, true);
    auto const pmus = discoverCorePmus(fs, "/sys");
    REQUIRE(pmus.size() == 1);
    CHECK(pmus[0].name == "cpu");
}

TEST_CASE("discoverCorePmus skips a PMU whose type is not a number", "[cpucounters]")
{
    auto fs = InMemoryFileSystem {};
    fs.addFile(std::string { Devices } + "cpu_core/type", "garbage\n");
    fs.addFile(std::string { Devices } + "cpu_core/cpus", "0\n");
    addPmu(fs, "cpu_atom", 10, true);
    fs.addFile(std::string { Devices } + "cpu_nofile/cpus", "0\n");
    auto const pmus = discoverCorePmus(fs, "/sys");
    REQUIRE(pmus.size() == 1);
    CHECK(pmus[0].name == "cpu_atom");
}

TEST_CASE("readParanoidLevel parses the kernel's file", "[cpucounters]")
{
    auto fs = InMemoryFileSystem {};
    CHECK(!readParanoidLevel(fs, "/p").has_value());
    fs.addFile("/p", "2\n");
    CHECK(readParanoidLevel(fs, "/p") == 2);
    fs.addFile("/q", "-1\n");
    CHECK(readParanoidLevel(fs, "/q") == -1);
    fs.addFile("/r", "garbage\n");
    CHECK(!readParanoidLevel(fs, "/r").has_value());
}

namespace
{

/// A descriptor double that counts how many are alive.
class CountedFd
{
  public:
    explicit CountedFd(int* alive): _alive { alive } { ++*_alive; }
    CountedFd(CountedFd&& other) noexcept: _alive { std::exchange(other._alive, nullptr) } {}
    CountedFd(CountedFd const&) = delete;
    CountedFd& operator=(CountedFd const&) = delete;
    CountedFd& operator=(CountedFd&&) = delete;
    ~CountedFd()
    {
        if (_alive)
            --*_alive;
    }

  private:
    int* _alive;
};

using Result = std::expected<CountedFd, int>;

} // namespace

TEST_CASE("openMeasure keeps ThreadsOnly where the kernel allows it", "[cpucounters]")
{
    auto alive = 0;
    auto const opener =
        PerfOpener<CountedFd> { [&](std::uint64_t, InheritMode) -> Result { return CountedFd { &alive }; } };
    auto const pmus = std::array { CorePmu { "cpu", 4 } };
    auto const opening = openMeasure<CountedFd>(pmus, 1, InheritMode::ThreadsOnly, opener, 2);
    CHECK(opening.reason.empty());
    CHECK(opening.mode == InheritMode::ThreadsOnly);
    CHECK(opening.fds.size() == 1);
}

TEST_CASE("openMeasure falls back to Everything on EINVAL", "[cpucounters]")
{
    auto alive = 0;
    auto const opener = PerfOpener<CountedFd> { [&](std::uint64_t, InheritMode mode) -> Result {
        if (mode == InheritMode::ThreadsOnly)
            return std::unexpected(EINVAL);
        return CountedFd { &alive };
    } };
    auto const pmus = std::array { CorePmu { "cpu", 4 } };
    auto const opening = openMeasure<CountedFd>(pmus, 1, InheritMode::ThreadsOnly, opener, 2);
    CHECK(opening.reason.empty());
    CHECK(opening.mode == InheritMode::Everything);
}

TEST_CASE("openMeasure is unavailable, and keeps its mode, when the fallback fails too", "[cpucounters]")
{
    auto const opener = PerfOpener<CountedFd> { [&](std::uint64_t, InheritMode mode) -> Result {
        return std::unexpected(mode == InheritMode::ThreadsOnly ? EINVAL : EACCES);
    } };
    auto const pmus = std::array { CorePmu { "cpu", 4 } };
    auto const opening = openMeasure<CountedFd>(pmus, 1, InheritMode::ThreadsOnly, opener, 3);
    CHECK_THAT(opening.reason, ContainsSubstring("perf_event_paranoid=3"));
    CHECK(opening.mode == InheritMode::ThreadsOnly);
    CHECK(opening.fds.empty());
}

TEST_CASE("openMeasure never mixes modes across the PMUs of a hybrid machine", "[cpucounters]")
{
    auto alive = 0;
    auto const opener = PerfOpener<CountedFd> { [&](std::uint64_t config, InheritMode mode) -> Result {
        auto const isAtom = (config >> 32) == 10;
        if (isAtom && mode == InheritMode::ThreadsOnly)
            return std::unexpected(EINVAL);
        return CountedFd { &alive };
    } };
    auto const pmus = std::array { CorePmu { "cpu_atom", 10 }, CorePmu { "cpu_core", 4 } };
    {
        auto const opening = openMeasure<CountedFd>(pmus, 1, InheritMode::ThreadsOnly, opener, 2);
        CHECK(opening.reason.empty());
        CHECK(opening.mode == InheritMode::Everything);
        CHECK(opening.fds.size() == 2);
        CHECK(alive == 2); // the discarded ThreadsOnly descriptor was closed
    }
    CHECK(alive == 0);
}

TEST_CASE("openMeasure closes the descriptors of an unsupported measure", "[cpucounters]")
{
    auto alive = 0;
    auto const opener = PerfOpener<CountedFd> { [&](std::uint64_t config, InheritMode) -> Result {
        if ((config >> 32) == 10)
            return std::unexpected(EACCES);
        return CountedFd { &alive };
    } };
    auto const pmus = std::array { CorePmu { "cpu_core", 4 }, CorePmu { "cpu_atom", 10 } };
    auto const opening = openMeasure<CountedFd>(pmus, 1, InheritMode::ThreadsOnly, opener, 2);
    CHECK(!opening.reason.empty());
    CHECK(alive == 0);
}
