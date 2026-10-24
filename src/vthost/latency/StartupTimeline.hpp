// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The startup timeline: when each step from process start to the first painted frame happened.
///
/// The steps are a table, so a new one is a row. The timeline is stamped from several threads (the
/// GUI thread, the reactor thread, the render thread); the first stamp of each step wins.

#include <core/platform/Clock.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace vthost::latency
{

/// A step of the startup, in the order it is reported.
enum class StartupMark : uint8_t
{
    ProcessStart = 0,
    ConfigLoaded,
    ConnectBegin,
    Connected,
    LayoutReceived,
    FirstSessionDiscovered,
    QmlEngineReady,
    WindowCreated,
    FirstFrameSwapped,
    FirstPromptSwapped,
};

/// One row of the table naming a StartupMark.
struct StartupMarkRow
{
    StartupMark mark;
    std::string_view name;
};

/// Every StartupMark, in enumerator order, with the name it is reported and parsed under.
inline constexpr auto StartupMarkTable = std::array {
    StartupMarkRow { .mark = StartupMark::ProcessStart, .name = "process-start" },
    StartupMarkRow { .mark = StartupMark::ConfigLoaded, .name = "config-loaded" },
    StartupMarkRow { .mark = StartupMark::ConnectBegin, .name = "connect-begin" },
    StartupMarkRow { .mark = StartupMark::Connected, .name = "connected" },
    StartupMarkRow { .mark = StartupMark::LayoutReceived, .name = "layout-received" },
    StartupMarkRow { .mark = StartupMark::FirstSessionDiscovered, .name = "first-session-discovered" },
    StartupMarkRow { .mark = StartupMark::QmlEngineReady, .name = "qml-engine-ready" },
    StartupMarkRow { .mark = StartupMark::WindowCreated, .name = "window-created" },
    StartupMarkRow { .mark = StartupMark::FirstFrameSwapped, .name = "first-frame-swapped" },
    StartupMarkRow { .mark = StartupMark::FirstPromptSwapped, .name = "first-prompt-swapped" },
};

/// Records the first instant of each StartupMark. Thread-safe.
class StartupTimeline
{
  public:
    /// @param clock Where instants are read from. Borrowed: it must outlive the timeline.
    /// @param processStart When the process began; stamps StartupMark::ProcessStart.
    StartupTimeline(core::platform::IClock const& clock, core::platform::SteadyTimePoint processStart);

    /// Stamps @p mark with the clock's now, unless it was stamped before.
    /// @param mark The step that was just reached.
    void mark(StartupMark mark);

    /// @param mark The step asked about.
    /// @return When it was first reached, or nullopt if it was not.
    [[nodiscard]] std::optional<core::platform::SteadyTimePoint> at(StartupMark mark) const;

    /// @return One line per recorded mark in table order: its name and the milliseconds since the
    ///         process start.
    [[nodiscard]] std::string describe() const;

  private:
    core::platform::IClock const& _clock;
    mutable std::mutex _mutex;
    std::array<std::optional<core::platform::SteadyTimePoint>, StartupMarkTable.size()> _stamps;
};

} // namespace vthost::latency
