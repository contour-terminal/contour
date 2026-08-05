// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/WindowGeometry.h>
#include <contour/config/Config.h>
#include <contour/input/KeyboardLayout.h>

#include <vtbackend/InputGenerator.h>
#include <vtbackend/Terminal.h> // vtbackend::ScrollPhase

#include <vtrasterizer/GridMetrics.h>

#include <crispy/logstore.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/Qt>
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickWindow>

#include <cctype>
#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace vtrasterizer
{
class Renderer;
}

namespace contour
{

auto inline const sessionLog = logstore::category("gui.session", "VT terminal session logs");
auto inline const managerLog = logstore::category("gui.session_manager", "Sessions manager logs");
auto inline const startupLog = logstore::category("gui.startup", "Logs startup timing information.");

class TerminalSession;

/// Translates a Qt key event into terminal input and hands it to @p session.
///
/// @param keyEvent The Qt event.
/// @param eventType Whether this is a press, a repeat or a release.
/// @param session The session to send the resulting input to.
/// @param keyboardLayout Resolves the event's native key identifier to the codepoint the key
///                       carries unmodified. Passed in rather than queried, because it reads the
///                       user's active input source -- an ambient resource tests must be able to
///                       substitute.
/// @return Whether the event was translated into input; false leaves it for Qt to route on.
bool sendKeyEvent(QKeyEvent* keyEvent,
                  vtbackend::KeyboardEventType eventType,
                  TerminalSession& session,
                  input::KeyboardLayout const& keyboardLayout);
void sendWheelEvent(QWheelEvent* event, TerminalSession& session);
void sendMousePressEvent(QMouseEvent* event, TerminalSession& session);
void sendMouseMoveEvent(QMouseEvent* event, TerminalSession& session);
void sendMouseMoveEvent(QHoverEvent* event, TerminalSession& session);
void sendMouseReleaseEvent(QMouseEvent* event, TerminalSession& session);

/// Result of computing auto-scroll parameters from the mouse position.
struct AutoScrollInfo
{
    int direction = 0;    ///< -1 = up (into history), 0 = inactive, +1 = down
    int linesPerTick = 0; ///< Number of lines to scroll per timer tick.
};

/// Computes auto-scroll direction and speed based on mouse pixel position vs content area bounds.
///
/// @return AutoScrollInfo with direction and speed; direction == 0 means mouse is inside content area.
AutoScrollInfo computeAutoScrollInfo(QMouseEvent const* event, TerminalSession const& session) noexcept;

/// A resolved "spawn a new contour process" command: the program to run plus its argument list.
struct SpawnTerminalCommand
{
    QString program;
    QStringList arguments;
};

/// Builds the command line for spawning a new detached contour process (pure; no launching), so the
/// argument assembly (config/profile/working-directory flags and the cwd-URL host filtering) is
/// unit-testable without starting a process.
/// @param programPath The contour executable path.
/// @param configPath The active config file (added as `config <path>` when non-empty).
/// @param profileName The profile to open (added as `profile <name>` when non-empty).
/// @param cwdUrl The working directory as a file URL; only a local-host path is forwarded.
/// @return The program + arguments to hand to a detached-process launcher.
[[nodiscard]] SpawnTerminalCommand buildSpawnTerminalCommand(std::string const& programPath,
                                                             std::string const& configPath,
                                                             std::string const& profileName,
                                                             std::string const& cwdUrl);

vtbackend::FontDef getFontDefinition(vtrasterizer::Renderer& renderer);

vtrasterizer::FontDescriptions sanitizeFontDescription(vtrasterizer::FontDescriptions fonts,
                                                       text::DPI screenDPI);

/// Adapts configured window margins to the geometry module's margin type.
/// @param margins Configured margins (logical or device pixels — the unit passes through unchanged).
/// @return The same margins as geometry::Margins.
constexpr geometry::Margins toGeometryMargins(config::WindowMargins margins) noexcept
{
    return { .horizontal = unbox<int>(margins.horizontal), .vertical = unbox<int>(margins.vertical) };
}

void applyResize(vtbackend::ImageSize newPixelSize,
                 TerminalSession& session,
                 vtrasterizer::Renderer& renderer);

bool applyFontDescription(text::DPI dpi,
                          vtrasterizer::Renderer& renderer,
                          vtrasterizer::FontDescriptions fontDescriptions);

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

} // namespace contour

template <>
struct std::formatter<contour::RenderState>: std::formatter<string_view>
{
    using State = contour::RenderState;
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
