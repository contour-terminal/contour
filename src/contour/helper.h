// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Config.h>

#include <vtbackend/InputGenerator.h>

#include <vtrasterizer/GridMetrics.h>

#include <crispy/logstore.h>

#include <QtCore/QCoreApplication>
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

constexpr inline bool isModifier(Qt::Key key)
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

constexpr inline char32_t makeChar(Qt::Key key, Qt::KeyboardModifiers mods)
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

constexpr inline vtbackend::Modifiers makeModifiers(Qt::KeyboardModifiers qtModifiers)
{
    using vtbackend::Modifier;
    using vtbackend::Modifiers;

    Modifiers modifiers {};

    if (qtModifiers & Qt::GroupSwitchModifier)
        modifiers |= Modifier::NumLock;

    if (qtModifiers & Qt::AltModifier)
        modifiers |= Modifier::Alt;
    if (qtModifiers & Qt::ShiftModifier)
        modifiers |= Modifier::Shift;
    if (qtModifiers & Qt::ControlModifier)
        modifiers |= Modifier::Control;
    if (qtModifiers & Qt::MetaModifier)
        modifiers |= Modifier::Super;

#if defined(_WIN32)
    // Deal with AltGr on Windows, which is seen by the app as Ctrl+Alt, because
    // the user may alternatively press Ctrl+Alt to emulate AltGr on keyboard
    // that are missing the AltGr key.
    // Microsoft does not recommend using Ctrl+Alt modifier for shortcuts.
    auto constexpr AltGrEquivalent = Modifiers { Modifier::Alt, Modifier::Control };
    if (modifiers.contains(AltGrEquivalent))
        modifiers = modifiers.without(AltGrEquivalent);
#endif

    return modifiers;
}

constexpr inline vtbackend::MouseButton makeMouseButton(Qt::MouseButton button)
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

class TerminalSession;
bool sendKeyEvent(QKeyEvent* keyEvent, vtbackend::KeyboardEventType eventType, TerminalSession& session);
void sendWheelEvent(QWheelEvent* event, TerminalSession& session);
void sendMousePressEvent(QMouseEvent* event, TerminalSession& session);
void sendMouseMoveEvent(QMouseEvent* event, TerminalSession& session);
void sendMouseMoveEvent(QHoverEvent* event, TerminalSession& session);
void sendMouseReleaseEvent(QMouseEvent* event, TerminalSession& session);

void spawnNewTerminal(std::string const& programPath,
                      std::string const& configPath,
                      std::string const& profileName,
                      std::string const& cwdUrl);

vtbackend::FontDef getFontDefinition(vtrasterizer::Renderer& renderer);

constexpr config::WindowMargins applyContentScale(config::WindowMargins margins, double contentScale) noexcept
{
    return { .horizontal =
                 config::HorizontalMargin(static_cast<int>(unbox(margins.horizontal) * contentScale)),
             .vertical = config::VerticalMargin(static_cast<int>(unbox(margins.vertical) * contentScale)) };
}

vtrasterizer::PageMargin computeMargin(vtbackend::ImageSize cellSize,
                                       vtbackend::PageSize charCells,
                                       vtbackend::ImageSize displaySize,
                                       config::WindowMargins minimumMargins) noexcept;

vtrasterizer::FontDescriptions sanitizeFontDescription(vtrasterizer::FontDescriptions fonts,
                                                       text::DPI screenDPI);

constexpr vtbackend::PageSize pageSizeForPixels(vtbackend::ImageSize totalViewSize,
                                                vtbackend::ImageSize cellSize,
                                                config::WindowMargins margins)
{
    // NB: Multiplied by 2, because margins are applied on both sides of the terminal.
    auto const marginSize =
        vtbackend::ImageSize { vtbackend::Width::cast_from(2 * unbox(margins.horizontal)),
                               vtbackend::Height::cast_from(2 * unbox(margins.vertical)) };

    auto const usableViewSize = totalViewSize - marginSize;

    auto const result =
        vtbackend::PageSize { boxed_cast<vtbackend::LineCount>((usableViewSize / cellSize).height),
                              boxed_cast<vtbackend::ColumnCount>((usableViewSize / cellSize).width) };

    return result;
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

#define CONSUME_GL_ERRORS()                                                       \
    do                                                                            \
    {                                                                             \
        GLenum err {};                                                            \
        while ((err = glGetError()) != GL_NO_ERROR)                               \
            errorLog()("Ignoring GL error {} in {}:{}", err, __FILE__, __LINE__); \
    } while (0)

#define CHECKED_GL(code)                                            \
    [&]() -> bool {                                                 \
        (code);                                                     \
        GLenum err {};                                              \
        int fails = 0;                                              \
        while ((err = glGetError()) != GL_NO_ERROR)                 \
        {                                                           \
            errorLog()("OpenGL error {} for call: {}", err, #code); \
            fails++;                                                \
        }                                                           \
        return fails == 0;                                          \
    }()

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
