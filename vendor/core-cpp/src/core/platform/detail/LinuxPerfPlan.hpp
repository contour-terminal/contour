// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/FileSystem.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace core::platform::detail
{

/// A core PMU: the performance monitoring unit that counts the instructions and cycles of a CPU core.
struct CorePmu
{
    std::string name;   ///< Its directory name below /sys/bus/event_source/devices.
    std::uint32_t type; ///< The perf_event_attr type the kernel assigned to it.
};

/// Finds the core PMUs under <sysRoot>/bus/event_source/devices: "cpu" if present, otherwise every PMU
/// directory that has a "cpus" file (how perf identifies core PMUs: cpu_core/cpu_atom, armv8_*,
/// apple_*_pmu, ...). Each one's type is read from its "type" file.
/// @param fs Where sysfs is read from.
/// @param sysRoot The sysfs mount point, "/sys" on a running system.
/// @return The core PMUs, by name; empty when none is found or readable.
[[nodiscard]] std::vector<CorePmu> discoverCorePmus(FileSystem const& fs,
                                                    std::filesystem::path const& sysRoot);

/// The perf_event_attr config that selects a hardware event on a core PMU.
/// With one PMU that is the plain event (pre-6.6 arm64 rejects the extended type); with several it is the
/// event with the PMU's type in the upper 32 bits.
/// @param event The PERF_COUNT_HW_* value.
/// @param pmu The PMU to count on.
/// @param pmuCount How many core PMUs the machine has.
[[nodiscard]] std::uint64_t hardwareConfig(std::uint64_t event,
                                           CorePmu const& pmu,
                                           std::size_t pmuCount) noexcept;

/// What one perf event file descriptor reports.
struct PmuReading
{
    std::uint64_t value;       ///< The count.
    std::uint64_t timeEnabled; ///< Nanoseconds the event was enabled.
    std::uint64_t timeRunning; ///< Nanoseconds it was counting on a PMU.
};

/// Adds the per-PMU readings of one measure, judged over the interval since the previous read.
/// A hybrid machine's PMUs each run only while the thread is on their cores, so over an interval their
/// running times add up to the enabled time; a total short of that by more than 1 % means the counter was
/// multiplexed away during the interval, and the count is not trustworthy. Only the interval counts: a
/// counter that ran cleanly for a long time before a multiplexed window must not pass for clean.
/// @param previous One reading per core PMU, from the previous read (or from opening the events).
/// @param current One reading per core PMU, from this read.
/// @return The sum of the current values (cumulative, so deltas of consecutive results are counts); nothing
///         if there are no readings, the sizes differ, a reading went backwards, or the interval's running
///         times fall short of its smallest enabled time by more than 1 %.
[[nodiscard]] std::optional<std::uint64_t> combinePmuReadings(std::span<PmuReading const> previous,
                                                              std::span<PmuReading const> current) noexcept;

/// The result of opening one measure on one PMU.
struct OpenOutcome
{
    int error; ///< errno of the failed perf_event_open(); 0 when it opened.
};

/// Reads the paranoid level, which says how much perf the kernel lets an unprivileged process do.
/// @param fs Where the file is read from.
/// @param path The perf_event_paranoid file, /proc/sys/kernel/perf_event_paranoid on a running system.
/// @return The level, or nothing if the file is unreadable or not a number.
[[nodiscard]] std::optional<int> readParanoidLevel(FileSystem const& fs, std::filesystem::path const& path);

/// Whether a perf event inherits to the children of a thread, or only to its threads.
enum class InheritMode : std::uint8_t
{
    ThreadsOnly, ///< Linux 5.13 and later (inherit_thread).
    Everything,  ///< Older kernels cannot tell threads from child processes.
};

/// Opens one perf event.
/// @tparam Fd A movable owner of the descriptor, closing it on destruction.
template <typename Fd>
using PerfOpener = std::function<std::expected<Fd, int>(std::uint64_t config, InheritMode mode)>;

/// The result of opening one measure on every core PMU.
template <typename Fd>
struct MeasureOpening
{
    std::vector<Fd> fds; ///< One per core PMU; empty unless the measure is supported.
    std::string reason;  ///< Empty when supported.
    InheritMode mode =
        InheritMode::ThreadsOnly; ///< The mode the descriptors were opened in; the caller's if unsupported.
};

/// Phrases why perf_event_open() failed for a measure, taking the PMU count into account.
/// @param outcomes One outcome per core PMU.
/// @param pmuCount How many core PMUs the machine has.
/// @param paranoidLevel The paranoid level, if it could be read.
/// @return Empty when every open succeeded.
[[nodiscard]] std::string decideMeasure(std::span<OpenOutcome const> outcomes,
                                        std::size_t pmuCount,
                                        std::optional<int> paranoidLevel);

/// Opens one measure on every core PMU, in one inheritance mode throughout.
/// Opens in @p preferred. If that mode is ThreadsOnly and any open fails with EINVAL (kernel < 5.13), the
/// whole measure is reopened in Everything mode, so the PMUs of a hybrid machine never mix modes; the mode
/// changes only if that reopening succeeded. Descriptors of an unsupported measure are closed.
/// @param pmus The core PMUs.
/// @param event The PERF_COUNT_HW_* value.
/// @param preferred The mode to try first.
/// @param opener Opens one event: a descriptor, or the errno.
/// @param paranoidLevel The paranoid level, if it could be read.
template <typename Fd>
[[nodiscard]] MeasureOpening<Fd> openMeasure(std::span<CorePmu const> pmus,
                                             std::uint64_t event,
                                             InheritMode preferred,
                                             PerfOpener<Fd> const& opener,
                                             std::optional<int> paranoidLevel)
{
    struct Attempt
    {
        std::vector<Fd> fds;
        std::vector<OpenOutcome> outcomes;
    };
    auto const attempt = [&](InheritMode mode) {
        auto result = Attempt {};
        for (auto const& pmu: pmus)
        {
            auto opened = opener(hardwareConfig(event, pmu, pmus.size()), mode);
            result.outcomes.push_back(OpenOutcome { .error = opened ? 0 : opened.error() });
            if (opened)
                result.fds.push_back(std::move(*opened));
        }
        return result;
    };
    auto const needsFallback = [](Attempt const& a) {
        return std::ranges::any_of(a.outcomes, [](OpenOutcome const& o) { return o.error == EINVAL; });
    };

    auto first = attempt(preferred);
    if (preferred == InheritMode::ThreadsOnly && needsFallback(first))
    {
        first.fds.clear();
        auto second = attempt(InheritMode::Everything);
        auto reason = decideMeasure(second.outcomes, pmus.size(), paranoidLevel);
        if (reason.empty())
            return MeasureOpening<Fd> { std::move(second.fds), {}, InheritMode::Everything };
        return MeasureOpening<Fd> { {}, std::move(reason), preferred };
    }
    auto reason = decideMeasure(first.outcomes, pmus.size(), paranoidLevel);
    if (!reason.empty())
        first.fds.clear();
    return MeasureOpening<Fd> { std::move(first.fds), std::move(reason), preferred };
}

/// Phrases why perf_event_open() failed.
/// @param error The errno.
/// @param paranoidLevel The value of /proc/sys/kernel/perf_event_paranoid, if it could be read.
[[nodiscard]] std::string perfOpenFailureReason(int error, std::optional<int> paranoidLevel);

} // namespace core::platform::detail
