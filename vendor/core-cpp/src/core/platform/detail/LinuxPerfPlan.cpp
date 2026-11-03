// SPDX-License-Identifier: Apache-2.0
#include <core/platform/detail/LinuxPerfPlan.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <format>
#include <limits>
#include <ranges>
#include <string_view>
#include <system_error>

namespace core::platform::detail
{

namespace
{
    constexpr auto DevicesPath = std::string_view { "bus/event_source/devices" };

    /// The bit position of a PMU's type within an extended hardware event config.
    constexpr auto PmuTypeShift = 32;

    /// A running time may fall this far short of the enabled time before the counter counts as
    /// multiplexed.
    constexpr auto RunningTimeTolerance = 0.99;

    /// Reads the number a sysfs file holds.
    /// @return The number, or nothing if the file is unreadable or not a number.
    [[nodiscard]] std::optional<std::uint32_t> readNumber(FileSystem const& fs,
                                                          std::filesystem::path const& path)
    {
        auto const content = fs.readFile(path);
        if (!content)
            return std::nullopt;
        auto const end = std::ranges::find_if(
            *content, [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });
        auto value = std::uint32_t {};
        auto const first = content->data();
        auto const [rest, error] = std::from_chars(first, first + (end - content->begin()), value);
        if (error != std::errc {} || rest == first)
            return std::nullopt;
        return value;
    }

    /// One way perf_event_open() fails, and how it is phrased.
    struct FailureRow
    {
        std::array<int, 3> errors; ///< The errnos it covers; 0 entries are unused.
        std::string_view text;     ///< A std::format string taking the paranoid level's phrase.
    };

    constexpr auto FailureRows = std::array {
        FailureRow { { EACCES, 0, 0 }, "perf_event_open: permission denied ({})" },
        FailureRow { { EPERM, 0, 0 }, "perf_event_open: operation not permitted (container seccomp? {})" },
        FailureRow { { ENOENT, ENODEV, EOPNOTSUPP }, "no hardware PMU exposed (virtual machine?)" },
        FailureRow { { ENOSYS, 0, 0 }, "perf_event_open blocked (seccomp?)" },
    };
} // namespace

std::vector<CorePmu> discoverCorePmus(FileSystem const& fs, std::filesystem::path const& sysRoot)
{
    auto pmus = std::vector<CorePmu> {};
    auto const devices = sysRoot / DevicesPath;

    auto const addPmu = [&](std::string name) {
        if (auto const type = readNumber(fs, devices / name / "type"))
            pmus.push_back(CorePmu { .name = std::move(name), .type = *type });
    };

    if (fs.exists(devices / "cpu"))
    {
        addPmu("cpu");
        return pmus;
    }

    auto const entries = fs.listDirectory(devices);
    if (!entries)
        return pmus;
    for (auto const& entry: *entries)
        if (fs.exists(entry.path / "cpus"))
            addPmu(entry.path.filename().string());
    std::ranges::sort(pmus, {}, &CorePmu::name);
    return pmus;
}

std::uint64_t hardwareConfig(std::uint64_t event, CorePmu const& pmu, std::size_t pmuCount) noexcept
{
    if (pmuCount <= 1)
        return event;
    return event | (static_cast<std::uint64_t>(pmu.type) << PmuTypeShift);
}

std::optional<std::uint64_t> combinePmuReadings(std::span<PmuReading const> previous,
                                                std::span<PmuReading const> current) noexcept
{
    if (current.empty() || previous.size() != current.size())
        return std::nullopt;
    auto sum = std::uint64_t {};
    auto running = std::uint64_t {};
    auto enabled = std::numeric_limits<std::uint64_t>::max();
    for (auto const index: std::views::iota(std::size_t { 0 }, current.size()))
    {
        auto const& from = previous[index];
        auto const& to = current[index];
        if (to.value < from.value || to.timeEnabled < from.timeEnabled || to.timeRunning < from.timeRunning)
            return std::nullopt;
        sum += to.value;
        running += to.timeRunning - from.timeRunning;
        enabled = std::min(enabled, to.timeEnabled - from.timeEnabled);
    }
    if (static_cast<double>(running) < static_cast<double>(enabled) * RunningTimeTolerance)
        return std::nullopt;
    return sum;
}

std::string perfOpenFailureReason(int error, std::optional<int> paranoidLevel)
{
    if (error == 0)
        return {};
    auto const level = paranoidLevel ? std::format("perf_event_paranoid={}", *paranoidLevel)
                                     : std::string { "perf_event_paranoid unreadable" };
    for (auto const& row: FailureRows)
        if (std::ranges::find(row.errors, error) != std::ranges::end(row.errors))
            return std::vformat(row.text, std::make_format_args(level));
    return "perf_event_open: " + std::generic_category().message(error);
}

std::optional<int> readParanoidLevel(FileSystem const& fs, std::filesystem::path const& path)
{
    auto const content = fs.readFile(path);
    if (!content)
        return std::nullopt;
    auto level = 0;
    auto const first = content->data();
    auto const [rest, error] = std::from_chars(first, first + content->size(), level);
    if (error != std::errc {} || rest == first)
        return std::nullopt;
    return level;
}

std::string decideMeasure(std::span<OpenOutcome const> outcomes,
                          std::size_t pmuCount,
                          std::optional<int> paranoidLevel)
{
    auto const failed = std::ranges::find_if(outcomes, [](OpenOutcome const& o) { return o.error != 0; });
    if (failed == outcomes.end())
        return {};
    if (pmuCount > 1 && failed->error == ENOENT)
        return "hybrid PMU events not supported by this kernel (Linux < 6.6?)";
    return perfOpenFailureReason(failed->error, paranoidLevel);
}

} // namespace core::platform::detail
