// SPDX-License-Identifier: Apache-2.0
#include <core/platform/CpuCounters.hpp>
#include <core/platform/NativeFileSystem.hpp>
#include <core/platform/detail/LinuxPerfPlan.hpp>
#include <core/platform/detail/UnavailableCpuCounterSource.hpp>

#include <sys/syscall.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

#include <linux/perf_event.h>

namespace core::platform
{

namespace
{
    constexpr auto SysRoot = std::string_view { "/sys" };
    constexpr auto ParanoidPath = std::string_view { "/proc/sys/kernel/perf_event_paranoid" };

    /// The reason a measure outside HardwareEvents is not reported.
    constexpr auto NotProvided = std::string_view { "not provided on Linux" };

    /// The note recorded when the kernel cannot restrict inheritance to threads.
    constexpr auto ChildrenCountedNote = std::string_view { "child processes are counted (kernel < 5.13)" };

    /// The measures this backend reports, and the hardware event that counts each.
    struct MeasureEvent
    {
        CpuCounter counter;
        std::uint64_t event; ///< PERF_COUNT_HW_*.
    };

    constexpr auto HardwareEvents = std::array {
        MeasureEvent { CpuCounter::Instructions, PERF_COUNT_HW_INSTRUCTIONS },
        MeasureEvent { CpuCounter::Cycles, PERF_COUNT_HW_CPU_CYCLES },
    };

    /// Owns a file descriptor.
    class UniqueFd
    {
      public:
        UniqueFd() noexcept = default;
        explicit UniqueFd(int fd) noexcept: _fd { fd } {}
        UniqueFd(UniqueFd const&) = delete;
        UniqueFd& operator=(UniqueFd const&) = delete;
        UniqueFd(UniqueFd&& other) noexcept: _fd { std::exchange(other._fd, -1) } {}
        UniqueFd& operator=(UniqueFd&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                _fd = std::exchange(other._fd, -1);
            }
            return *this;
        }
        ~UniqueFd() { reset(); }

        [[nodiscard]] int get() const noexcept { return _fd; }

      private:
        void reset() noexcept
        {
            if (_fd >= 0)
                ::close(_fd);
            _fd = -1;
        }

        int _fd = -1;
    };

    /// What read() returns for an event opened with PERF_FORMAT_TOTAL_TIME_ENABLED | RUNNING.
    struct ReadFormat
    {
        std::uint64_t value;
        std::uint64_t timeEnabled;
        std::uint64_t timeRunning;
    };

    /// Opens a hardware event that counts the calling process, every thread of it, in user mode.
    /// @return The descriptor, or the errno.
    [[nodiscard]] std::expected<UniqueFd, int> openEvent(std::uint64_t config, detail::InheritMode mode)
    {
        auto attr = perf_event_attr {};
        attr.type = PERF_TYPE_HARDWARE;
        attr.size = sizeof(attr);
        attr.config = config;
        attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
        attr.exclude_kernel = 1;
        attr.exclude_hv = 1;
        attr.inherit = 1;
        attr.inherit_thread = mode == detail::InheritMode::ThreadsOnly ? 1 : 0;

        // glibc has no perf_event_open() wrapper; the pid/cpu pair (0, -1) is this process on any CPU.
        auto const fd =
            static_cast<int>(::syscall(SYS_perf_event_open, &attr, 0, -1, -1, PERF_FLAG_FD_CLOEXEC));
        if (fd < 0)
            return std::unexpected(errno);
        return UniqueFd { fd };
    }

    /// The events of one measure, one per core PMU, and why they could not all be opened.
    struct OpenMeasure
    {
        std::vector<UniqueFd> fds;
        std::string reason;                   ///< Empty when every event is open.
        std::vector<detail::PmuReading> last; ///< What each event reported at the previous read.
    };

    /// Counts the instructions and cycles of this process with perf_event_open(2).
    class LinuxCpuCounterSource final: public ICpuCounterSource
    {
      public:
        LinuxCpuCounterSource()
        {
            auto pmus = detail::discoverCorePmus(NativeFileSystem::instance(), SysRoot);
            if (pmus.empty())
                pmus.push_back(detail::CorePmu { .name = {}, .type = PERF_TYPE_HARDWARE });
            auto const paranoid = detail::readParanoidLevel(NativeFileSystem::instance(), ParanoidPath);

            // The first measure decides whether the kernel can restrict inheritance to threads; the rest
            // follow it, so every descriptor counts the same thing.
            auto mode = detail::InheritMode::ThreadsOnly;
            auto const opener = detail::PerfOpener<UniqueFd> { openEvent };
            for (auto const& [counter, event]: HardwareEvents)
            {
                auto opening = detail::openMeasure<UniqueFd>(pmus, event, mode, opener, paranoid);
                mode = opening.mode;
                auto& measure = _measures[std::to_underlying(counter)];
                measure.fds = std::move(opening.fds);
                measure.reason = std::move(opening.reason);
            }
            if (mode == detail::InheritMode::Everything)
                _note = ChildrenCountedNote;

            // The baseline of the first interval: the events as they stand now, not as of an open() that
            // may long since have been multiplexed or idle.
            for (auto& measure: _measures)
                if (auto readings = readEvents(measure))
                    measure.last = std::move(*readings);
        }

        [[nodiscard]] CpuCounterSample read() override
        {
            auto sample = CpuCounterSample {};
            for (auto const& [counter, event]: HardwareEvents)
            {
                auto& measure = _measures[std::to_underlying(counter)];
                if (!measure.reason.empty())
                    continue;
                if (auto const total = readTotal(measure))
                    sample.set(counter, *total);
            }
            return sample;
        }

        [[nodiscard]] std::string_view unavailableReason(CpuCounter counter) const noexcept override
        {
            auto const reported =
                std::ranges::any_of(HardwareEvents, [counter](MeasureEvent const& measured) {
                    return measured.counter == counter;
                });
            if (!reported)
                return NotProvided;
            return _measures[std::to_underlying(counter)].reason;
        }

        [[nodiscard]] std::string_view note() const noexcept override { return _note; }

      private:
        /// Reads every event of a measure.
        /// @return One reading per event, or nothing if any read failed.
        [[nodiscard]] static std::optional<std::vector<detail::PmuReading>> readEvents(
            OpenMeasure const& measure)
        {
            auto readings = std::vector<detail::PmuReading> {};
            for (auto const& fd: measure.fds)
            {
                auto raw = ReadFormat {};
                if (::read(fd.get(), &raw, sizeof(raw)) != static_cast<ssize_t>(sizeof(raw)))
                    return std::nullopt;
                readings.push_back(detail::PmuReading {
                    .value = raw.value, .timeEnabled = raw.timeEnabled, .timeRunning = raw.timeRunning });
            }
            return readings;
        }

        /// Sums a measure over its PMUs, if it was not multiplexed since the previous read; the readings
        /// become the baseline of the next interval either way.
        [[nodiscard]] static std::optional<std::uint64_t> readTotal(OpenMeasure& measure)
        {
            auto readings = readEvents(measure);
            if (!readings)
                return std::nullopt;
            auto const total = detail::combinePmuReadings(measure.last, *readings);
            measure.last = std::move(*readings);
            return total;
        }

        std::array<OpenMeasure, CpuCounterCount> _measures {};
        std::string _note;
    };
} // namespace

std::unique_ptr<ICpuCounterSource> makeDefaultCpuCounterSource()
{
    return std::make_unique<LinuxCpuCounterSource>();
}

} // namespace core::platform
