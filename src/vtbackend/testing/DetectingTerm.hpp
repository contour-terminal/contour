// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/links/LinkIndex.hpp>
#include <vtbackend/screen/LinkDetector.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <core/async/testing/ManualExecutor.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace vtbackend::test
{

/// A MockTerm with passive link detection, driven frame by frame as a display drives it.
///
/// The jobs run on manual executors, which a test drains, and look paths up in an in-memory filesystem: no
/// thread of its own and no file on disk.
struct DetectingTerm
{
    /// @param size The page size.
    /// @param chosen Detection's timing and bounds.
    /// @param history The scrollback; none by default.
    explicit DetectingTerm(PageSize size = PageSize { LineCount(5), ColumnCount(40) },
                           LinkDetectorTuning chosen = {},
                           HistoryLimits history = {}):
        tuning { chosen },
        mock { size,
               history,
               1024,
               std::nullopt,
               &fixedOffsetLocalTime<>,
               LinkDetectionServices { .matchExecutor = match,
                                       .probeExecutor = probe,
                                       .fileSystem = files,
                                       .clock = probeClock,
                                       .tuning = chosen } }
    {
    }

    LinkDetectorTuning tuning;                         ///< As the terminal's detector was given it.
    core::async::testing::ManualExecutor match;        ///< Runs the match jobs when drained.
    core::async::testing::ManualExecutor probe;        ///< Runs the probe jobs when drained.
    core::platform::testing::InMemoryFileSystem files; ///< What the probes and the terminal look paths up in.
    core::platform::ManualClock probeClock;            ///< Ages probe answers.
    MockTerm<> mock;                                   ///< The terminal, which owns the detector.

    /// One frame, as the display draws one: the fill that feeds detection.
    void frame() { mock.terminal.refreshRenderBuffer(); }

    /// A whole detection cycle: a frame that sees the change, the quiet time, the frame that collects, the
    /// match job, the frame that adopts it, the probe job, and the frame that adopts that.
    void cycle()
    {
        frame();
        mock.terminal.tick(mock.terminal.currentTime() + tuning.debounce);
        frame();
        match.drain();
        frame();
        probe.drain();
        frame();
    }

    /// Writes @p text, and runs a detection cycle over it.
    /// @param text What the application prints.
    void print(std::string_view text)
    {
        mock.writeToScreen(text);
        cycle();
    }

    /// The link detected at a cell of the page on display.
    /// @param line The row on the page.
    /// @param column The column.
    /// @return The link, or nullptr where none is, or none is detected for that page. Valid until the next
    ///         frame.
    [[nodiscard]] DetectedLink const* linkAt(int line, int column)
    {
        auto const _ = std::scoped_lock { mock.terminal };
        auto const* index = mock.terminal.detectedLinks();
        if (!index)
            return nullptr;
        auto const id = mock.terminal.displayedPage().grid().stableLineIdOf(LineOffset(line));
        return index->at(id, ColumnOffset(column));
    }

    /// The target of the link detected at a cell of the page on display.
    /// @param line The row on the page.
    /// @param column The column.
    /// @return The target, empty where no link is detected.
    [[nodiscard]] std::string targetAt(int line, int column)
    {
        auto const* link = linkAt(line, column);
        return link ? link->target.target : std::string {};
    }
};

} // namespace vtbackend::test
