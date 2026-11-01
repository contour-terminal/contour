// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/DragEndPolicy.hpp>

#include <QtCore/QPoint>
#include <QtCore/QPointF>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtGui/QScreen>

#include <memory>

class QWindow;

namespace contour::window
{

/// Where a dragged pane or tab that no window took is to open as a window of its own.
struct DropSpot
{
    QScreen* screen = nullptr; ///< The screen; null for wherever windows open.
    QPoint at;                 ///< The position, in the global coordinates of @c screen's desktop.
};

/// Tells, once a drag no window took has ended, whether the pointer was over one of Contour's windows --
/// so a cancel there is told from a drop outside, which Qt reports alike -- and where outside it was. The
/// ambient pointer state that answers it differs per platform, so it is reached through this, and a test
/// answers it itself.
///
/// Every window's drag events are reported to it as they arrive, for an implementation that has no global
/// pointer position to read at the end and tracks the drag instead.
class DragEndProbe
{
  public:
    DragEndProbe() = default;
    virtual ~DragEndProbe() = default;
    DragEndProbe(DragEndProbe const&) = delete;
    DragEndProbe& operator=(DragEndProbe const&) = delete;
    DragEndProbe(DragEndProbe&&) = delete;
    DragEndProbe& operator=(DragEndProbe&&) = delete;

    /// A drag entered, moved over, or was dropped on @p window.
    /// @param window The window.
    /// @param local The pointer, in @p window's coordinates.
    virtual void noteDragOver(QWindow const& window, QPointF local) = 0;

    /// A drag left @p window.
    virtual void noteDragLeft(QWindow const& window) = 0;

    /// @return Whether the pointer was over a Contour window as the drag ended.
    [[nodiscard]] virtual PointerOverContour pointerOverContourAtEnd() const = 0;

    /// @return Where the drag ended, for a window opened there.
    [[nodiscard]] virtual DropSpot dropSpot() const = 0;

    /// The drag ended and has been decided: what was tracked of it goes, so the next one starts afresh.
    virtual void forgetDrag() = 0;
};

/// Reads the global pointer position at the end, as Windows, macOS and X11 keep it current through a drag.
class TopLevelProbe final: public DragEndProbe
{
  public:
    void noteDragOver(QWindow const& /*window*/, QPointF /*local*/) override {}
    void noteDragLeft(QWindow const& /*window*/) override {}
    [[nodiscard]] PointerOverContour pointerOverContourAtEnd() const override;
    [[nodiscard]] DropSpot dropSpot() const override;
    void forgetDrag() override {}
};

/// Tracks a drag through the windows it crosses, for Wayland: there the pointer position Qt keeps is stale
/// during a drag and a window has no global position, so the end is read from the last sightings
/// (@see classifyTrackedDrag). A window opened from such a drop opens on the screen it was last seen on, at
/// the last known position -- the compositor places it, as it places every Wayland window.
class TrackedDragProbe final: public DragEndProbe
{
  public:
    void noteDragOver(QWindow const& window, QPointF local) override;
    void noteDragLeft(QWindow const& window) override;
    [[nodiscard]] PointerOverContour pointerOverContourAtEnd() const override;
    [[nodiscard]] DropSpot dropSpot() const override;
    void forgetDrag() override;

    /// @return What is tracked of the drag so far, as the classifier reads it.
    [[nodiscard]] TrackedDrag tracked() const;

  private:
    /// The window the drag was last seen over, compared by identity only: it may close mid-drag.
    QWindow const* _window = nullptr;
    DragPresence _presence = DragPresence::Unseen;
    DragSighting _last {};
    QPointer<QScreen> _screen;
    QPoint _global;
};

/// Builds the probe for the platform Qt runs on.
/// @param platformName QGuiApplication::platformName(), read once the application exists.
/// @return A TrackedDragProbe on Wayland, a TopLevelProbe elsewhere.
[[nodiscard]] std::unique_ptr<DragEndProbe> makeDragEndProbe(QString const& platformName);

} // namespace contour::window
