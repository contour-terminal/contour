// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/session/DisplaySurface.hpp>

#include <vtbackend/vt/Screenshot.hpp>

#include <expected>
#include <functional>
#include <utility>

namespace contour::display
{

/// Answers one `OSC 533` capture exactly once, whatever becomes of it.
///
/// The render target holds a SINGLE pending screenshot callback, and there are two reachable ways it
/// goes away without ever being called: the scene graph invalidates and destroys the render target
/// mid-capture (a window re-parent, a lost GPU context — @see vtrasterizer::Renderable::detachRenderTarget),
/// and a second capture overwrites the first, which the screenshot key bindings and the at-exit state
/// dump can do just as well as another `OSC 533` can.
///
/// Either way the application that asked is left blocking on a read for a reply that is never coming —
/// the one outcome this extension promises cannot happen. So silence is not a state this path may end
/// in: whichever way the capture is lost, @ref ~ScreenshotAnswer turns it into
/// @ref vtbackend::screenshot::Status::Unavailable, which is exactly the status that says "not now,
/// ask again".
///
/// Hold it through a @c shared_ptr so the copies @c std::function makes of a capturing lambda share one
/// answer, and the destructor runs when the last of them goes.
///
/// The thread hop is injected rather than reached for: both paths run on the render thread — the
/// readback completes there, and so does the renderer's destruction — while the answer has to be
/// delivered on the GUI thread. Taking it as a parameter is also what lets this be tested without a
/// window, a scene graph or a GPU.
class ScreenshotAnswer
{
  public:
    /// Runs work on the thread the answer must be delivered on.
    using Post = std::function<void(std::function<void()> const&)>;

    /// @param post    How to reach the delivering thread. @see TerminalDisplay::post.
    /// @param onReady Receives the answer, exactly once.
    ScreenshotAnswer(Post post, session::DisplaySurface::ScreenshotCaptureCallback onReady):
        _post { std::move(post) }, _onReady { std::move(onReady) }
    {
    }

    ScreenshotAnswer(ScreenshotAnswer const&) = delete;
    ScreenshotAnswer& operator=(ScreenshotAnswer const&) = delete;
    ScreenshotAnswer(ScreenshotAnswer&&) = delete;
    ScreenshotAnswer& operator=(ScreenshotAnswer&&) = delete;

    /// Answers @c Unavailable if the capture never arrived. @see ScreenshotAnswer.
    ~ScreenshotAnswer() { deliver(std::unexpected { vtbackend::screenshot::Status::Unavailable }); }

    /// Delivers @p result, if nothing has been delivered yet.
    /// @param result The capture, or the status to refuse the request with.
    void deliver(vtbackend::screenshot::CaptureResult result)
    {
        if (!_onReady)
            return;

        _post([onReady = std::exchange(_onReady, {}), result = std::move(result)]() mutable {
            onReady(std::move(result));
        });
    }

  private:
    Post _post;
    session::DisplaySurface::ScreenshotCaptureCallback _onReady;
};

} // namespace contour::display
