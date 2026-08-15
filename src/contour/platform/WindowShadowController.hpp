// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/WindowShadow.hpp>

#include <QtGui/QColor>
#include <QtGui/QWindow>

#include <memory>
#include <utility>

namespace contour::platform
{

/// What @p visibility means for a shadow.
///
/// A pure mapping rather than a member mirrored at every transition. The mirror this replaced needed
/// a writer at each of seven call sites, and a refactor silently left it with NONE -- so the window
/// stayed permanently "Windowed" and never withdrew its shadow on maximize. Deriving it from the
/// window cannot be forgotten, and being pure it is testable without a window at all.
///
/// Minimized and Hidden map to Windowed deliberately: an unmapped window shows no shadow whatever we
/// publish, so withdrawing one would be churn for no visible difference, and republishing on the way
/// back out is a transition we would then have to handle.
[[nodiscard]] constexpr WindowPresentation presentationFor(QWindow::Visibility visibility) noexcept
{
    switch (visibility)
    {
        case QWindow::Maximized: return WindowPresentation::Maximized;
        case QWindow::FullScreen: return WindowPresentation::FullScreen;
        case QWindow::Windowed:
        case QWindow::AutomaticVisibility:
        case QWindow::Minimized:
        case QWindow::Hidden: break;
    }
    return WindowPresentation::Windowed;
}

/// Keeps one window's published shadow in step with the state the window is actually in.
///
/// The window's owner calls @ref refresh whenever anything the answer depends on moves -- the
/// window is shown, maximized, restored, its decoration is toggled, the configuration is reloaded.
/// Everything else lives here: whether a shadow is wanted at all (@ref shadowVisibilityFor), what
/// it should look like (@ref shadowMetricsFor), and whether the tiles for it have already been
/// rendered.
///
/// Constructed with its attachment rather than making one, so a test drives the whole sequence
/// against a recording double with no compositor anywhere.
class WindowShadowController
{
  public:
    /// @param shadow The attachment to publish through, @see makeWindowShadow. Never null.
    explicit WindowShadowController(std::unique_ptr<WindowShadow> shadow): _shadow { std::move(shadow) } {}

    /// Re-publishes the shadow for the state the window is now in.
    ///
    /// Cheap to call repeatedly: the tiles are re-rendered only when the size actually changes, and
    /// the attachment itself skips an upload whose geometry matches what it already published.
    void refresh(WindowPresentation presentation, WindowDecoration decoration, ShadowSize size)
    {
        if (shadowVisibilityFor(presentation, decoration, size) == ShadowVisibility::Hidden)
        {
            _shadow->withdraw();
            return;
        }

        if (_renderedFor != size)
        {
            // Black, because that is what a shadow is. Left as a literal rather than a constructor
            // parameter until something actually wants to vary it -- a knob no caller turns reads
            // as configurable when it is not.
            _tiles = renderWindowShadowTiles(shadowMetricsFor(size), Qt::black);
            _renderedFor = size;
        }
        _shadow->apply(_tiles);
    }

    /// Withdraws the shadow. Called before the window goes away, while its handle is still live.
    void withdraw() { _shadow->withdraw(); }

  private:
    std::unique_ptr<WindowShadow> _shadow;
    WindowShadowTiles _tiles {};
    // No optional: refresh() returns early for None, so None can never be a RENDERED size and
    // doubles perfectly as "nothing rendered yet".
    ShadowSize _renderedFor = ShadowSize::None;
};

} // namespace contour::platform
