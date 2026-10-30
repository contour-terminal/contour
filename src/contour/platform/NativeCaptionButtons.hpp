// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/CaptionHitTest.hpp>

#include <functional>
#include <memory>

class QWindow;

namespace contour::platform
{

/// What a window lends the caption adapter: where its buttons are, and what to do when the OS drives one.
struct CaptionBinding
{
    std::function<CaptionLayout()> layout {};           ///< The buttons' scene rectangles now.
    std::function<void(CaptionButtonVisual)> visual {}; ///< The maximise button's new look.
    std::function<void()> toggleMaximize {};            ///< Maximise or restore, as the QML button does.
};

/// Hands a client-drawn window's maximise button to the OS, so Windows 11 shows Snap Layouts over it.
///
/// Only the maximise button: minimise and close stay QML buttons, whose hover, press, focus and
/// accessibility are the scene's own.
class INativeCaptionButtons
{
  public:
    virtual ~INativeCaptionButtons() = default;

    /// Starts answering @p window's non-client messages from @p binding. Re-attaching replaces the binding.
    /// A window with no platform window yet is answered once it has one.
    /// @pre GUI thread.
    /// @param window  The window, which must outlive the attachment or be detached first.
    /// @param binding Where its buttons are, and what to do when the OS drives the maximise button.
    virtual void attach(QWindow& window, CaptionBinding binding) = 0;

    /// Stops answering @p window's messages; a window never attached is ignored.
    /// @pre GUI thread.
    /// @param window The window.
    virtual void detach(QWindow& window) = 0;
};

/// The adapter for platforms whose caption has no native buttons to hand over: both calls do nothing.
class NullNativeCaptionButtons final: public INativeCaptionButtons
{
  public:
    /// Does nothing.
    void attach(QWindow& /*window*/, CaptionBinding /*binding*/) override {}

    /// Does nothing.
    void detach(QWindow& /*window*/) override {}
};

/// Creates the operating system's caption adapter, or the null one where there is none. Built before a
/// QGuiApplication exists, so a native adapter checks the platform plugin when it is called, not here.
/// @return The adapter; never null.
[[nodiscard]] std::unique_ptr<INativeCaptionButtons> makeNativeCaptionButtons();

} // namespace contour::platform
