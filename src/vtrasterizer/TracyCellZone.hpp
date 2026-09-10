// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <tracy/Tracy.hpp>

/// Zone marker for the per-cell render path, compiled out unless `CONTOUR_TRACY_CELL_ZONES` is set.
///
/// These sites run once per cell per renderer per frame -- three of them across
/// BackgroundRenderer, DecorationRenderer and TextRenderer -- so a full-screen workload emits tens
/// of millions of them in a minute. Each costs about 20 ns to record, which is cheap; what is not
/// cheap is the trace they produce. A one-minute `notcurses-demo` capture reaches 345 MB and takes
/// the profiler minutes and several gigabytes of RSS to load, and every `tracy-csvexport` pass has
/// to decode all of them to answer a question about frames.
///
/// They earned that cost once, by establishing that per-cell work is not a bottleneck (about 20 ns
/// per cell, with `renderCells` under 1.5% of a run) -- @see docs/internals/performance-backlog.md.
/// Turning them back on is for re-testing that specific claim, not for general profiling.
#ifdef CONTOUR_TRACY_CELL_ZONES
    #define CONTOUR_TRACY_CELL_ZONE() ZoneScoped
#else
    #define CONTOUR_TRACY_CELL_ZONE() ((void) 0)
#endif
