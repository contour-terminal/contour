// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstdint>
#include <format>
#include <string_view>

namespace contour::display
{

/// Declares the screen-dirtiness-vs-rendering state.
enum class RenderState : uint8_t
{
    CleanIdle,     //!< No screen updates and no rendering currently in progress.
    DirtyIdle,     //!< Screen updates pending and no rendering currently in progress.
    CleanPainting, //!< No screen updates and rendering currently in progress.
    DirtyPainting  //!< Screen updates pending and rendering currently in progress.
};

/// Defines the current screen-dirtiness-vs-rendering state.
///
/// This is primarily updated by two independent threads, the rendering thread and the I/O
/// thread.
/// The rendering thread constantly marks the rendering state CleanPainting whenever it is about
/// to render and, depending on whether new screen changes happened, in the frameSwapped()
/// callback either DirtyPainting and continues to rerender or CleanIdle if no changes came in
/// since last render.
///
/// The I/O thread constantly marks the state dirty whenever new data has arrived,
/// either DirtyIdle if no painting is currently in progress, DirtyPainting otherwise.
struct RenderStateManager
{
    std::atomic<RenderState> state = RenderState::CleanIdle;
    bool renderingPressure = false;

    RenderState fetchAndClear() { return state.exchange(RenderState::CleanPainting); }

    bool touch()
    {
        for (;;)
        {
            auto stateTmp = state.load();
            switch (stateTmp)
            {
                case RenderState::CleanIdle:
                    if (state.compare_exchange_strong(stateTmp, RenderState::DirtyIdle))
                        return true;
                    break;
                case RenderState::CleanPainting:
                    if (state.compare_exchange_strong(stateTmp, RenderState::DirtyPainting))
                        return false;
                    break;
                case RenderState::DirtyIdle:
                case RenderState::DirtyPainting: return false;
            }
        }
    }

    /// @retval true finished rendering, nothing pending yet. So please start update timer.
    /// @retval false more data pending. Rerun paint() immediately.
    bool finish()
    {
        for (;;)
        {
            auto stateTmp = state.load();
            switch (stateTmp)
            {
                case RenderState::DirtyIdle:
                    // assert(!"The impossible happened, painting but painting. Shakesbeer.");
                    // qDebug() << "The impossible happened, onFrameSwapped() called in wrong state
                    // DirtyIdle.";
                    [[fallthrough]];
                case RenderState::DirtyPainting: return false;
                case RenderState::CleanPainting:
                    if (!state.compare_exchange_strong(stateTmp, RenderState::CleanIdle))
                        break;
                    [[fallthrough]];
                case RenderState::CleanIdle: renderingPressure = false; return true;
            }
        }
    }
};

} // namespace contour::display

template <>
struct std::formatter<contour::display::RenderState>: std::formatter<string_view>
{
    using State = contour::display::RenderState;
    auto format(State state, auto& ctx) const
    {
        string_view name;
        switch (state)
        {
            case State::CleanIdle: name = "clean-idle"; break;
            case State::CleanPainting: name = "clean-painting"; break;
            case State::DirtyIdle: name = "dirty-idle"; break;
            case State::DirtyPainting: name = "dirty-painting"; break;
        }
        return std::formatter<string_view>::format(name, ctx);
    }
};
