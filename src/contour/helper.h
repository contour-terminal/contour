// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Config.h>
#include <contour/WindowGeometry.h>

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
#include <map>
#include <string>
#include <string_view>

namespace vtrasterizer
{
class Renderer;
}

namespace contour
{

auto inline const displayLog =
    logstore::category("gui.display", "Logs display driver details (e.g. OpenGL).");
auto inline const inputLog =
    logstore::category("gui.input", "Logs input driver details (e.g. GUI input events).");
auto inline const sessionLog = logstore::category("gui.session", "VT terminal session logs");
auto inline const managerLog = logstore::category("gui.session_manager", "Sessions manager logs");
auto inline const startupLog = logstore::category("gui.startup", "Logs startup timing information.");

/// RAII utility that logs the elapsed time of a scope to a given log category.
class ScopedTimer
{
  public:
    /// Constructs a ScopedTimer that logs the elapsed duration on destruction.
    ///
    /// @param category The logstore category to log the timing to.
    /// @param label A human-readable label identifying the timed section.
    ScopedTimer(logstore::category const& category, std::string_view label):
        _category { category }, _label { label }, _start { std::chrono::steady_clock::now() }
    {
    }

    ~ScopedTimer()
    {
        if (_category.is_enabled())
        {
            auto const elapsed = std::chrono::steady_clock::now() - _start;
            auto const ms =
                static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count())
                / 1000.0;
            _category()("{}: {:.1f} ms", _label, ms);
        }
    }

    ScopedTimer(ScopedTimer const&) = delete;
    ScopedTimer& operator=(ScopedTimer const&) = delete;
    ScopedTimer(ScopedTimer&&) = delete;
    ScopedTimer& operator=(ScopedTimer&&) = delete;

  private:
    logstore::category const& _category;
    std::string_view _label;
    std::chrono::steady_clock::time_point _start;
};

enum class MouseCursorShape : uint8_t
{
    Hidden,
    PointingHand,
    IBeam,
    Arrow,
};

/// Posts a functor to be executed on the event loop of the thread that owns @p obj.
///
/// Uses QMetaObject::invokeMethod with Qt::QueuedConnection to ensure the functor
/// is invoked via a QMetaCallEvent, which Qt's event loop processes promptly.
template <typename F>
void postToObject(QObject* obj, F fun)
{
    QMetaObject::invokeMethod(obj, std::forward<F>(fun), Qt::QueuedConnection);
}

constexpr bool isModifier(Qt::Key key)
{
    switch (key)
    {
        case Qt::Key_Alt:
        case Qt::Key_Control:
        case Qt::Key_Shift:
        case Qt::Key_Meta: return true;
        default: return false;
    }
}

constexpr char32_t makeChar(Qt::Key key, Qt::KeyboardModifiers mods)
{
    auto const value = static_cast<int>(key);
    if (value >= 'A' && value <= 'Z')
    {
        if (mods & Qt::ShiftModifier)
            return static_cast<char32_t>(value);
        else
            return static_cast<char32_t>(std::tolower(value));
    }
    return 0;
}

/// Creates the VT keyboard modifier state from Qt modifiers and native platform modifiers.
/// The chord is taken from the Qt modifiers, the CapsLock and NumLock state from the
/// platform-specific native modifiers.
/// @param qtModifiers Standard Qt keyboard modifiers
/// @param nativeModifiers Platform-specific value from QKeyEvent::nativeModifiers()
/// @param stripAltGr When true (default), removes the Ctrl+Alt combination on Windows
///                   that represents AltGr. Set to false for Win32 Input Mode which
///                   needs the raw modifier state.
/// @return The chord being held, plus the latched lock keys.
vtbackend::KeyboardModifiers makeModifiers(Qt::KeyboardModifiers qtModifiers,
                                           quint32 nativeModifiers = 0,
                                           bool stripAltGr = true);

constexpr vtbackend::MouseButton makeMouseButton(Qt::MouseButton button)
{
    switch (button)
    {
        case Qt::MouseButton::RightButton: return vtbackend::MouseButton::Right;
        case Qt::MiddleButton: return vtbackend::MouseButton::Middle;
        case Qt::LeftButton: [[fallthrough]];
        default: // d'oh
            return vtbackend::MouseButton::Left;
    }
}

/// Maps a US-ASCII "shifted" character back to the base character its physical key produces without
/// Shift (e.g. '<' -> ',', '?' -> '/', '_' -> '-', '@' -> '2'). Returns @p ch unchanged when it is not
/// a shifted symbol.
///
/// Keyboard shortcuts are written with the base key label (e.g. `Ctrl+Shift+,`) and stored as the base
/// character, but a Shift+punctuation chord is delivered by Qt as the *shifted* symbol (comma+Shift is
/// reported as '<' on a US layout). Matching the delivered shifted codepoint against the base char a
/// binding is stored under is what lets such shortcuts fire as the user intended.
/// @param ch The (possibly shifted) input codepoint.
/// @return The un-shifted base codepoint, or @p ch if it is not a recognized shifted symbol.
[[nodiscard]] char32_t unshiftedCodepoint(char32_t ch) noexcept;

/// Maps Qt's scroll phase onto the platform-independent vtbackend enum.
///
/// Shared rather than local to the QWheelEvent path, because the QML wheel handler needs the same
/// mapping: QML hands a phase across as a plain int, and the two enumerations agreeing numerically
/// today is a coincidence to translate through, not one to rely on.
///
/// @param phase The phase Qt reported.
/// @return The corresponding vtbackend phase; NoPhase for anything unrecognized.
[[nodiscard]] vtbackend::ScrollPhase mapScrollPhase(Qt::ScrollPhase phase) noexcept;

class TerminalSession;
bool sendKeyEvent(QKeyEvent* keyEvent, vtbackend::KeyboardEventType eventType, TerminalSession& session);
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

constexpr Qt::CursorShape toQtMouseShape(MouseCursorShape shape)
{
    switch (shape)
    {
        case contour::MouseCursorShape::Hidden: return Qt::CursorShape::BlankCursor;
        case contour::MouseCursorShape::Arrow: return Qt::CursorShape::ArrowCursor;
        case contour::MouseCursorShape::IBeam: return Qt::CursorShape::IBeamCursor;
        case contour::MouseCursorShape::PointingHand: return Qt::CursorShape::PointingHandCursor;
    }

    // should never be reached
    return Qt::CursorShape::ArrowCursor;
}

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
/// This is primarily updated by two independant threads, the rendering thread and the I/O
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
