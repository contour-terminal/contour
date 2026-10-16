// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/TaskbarProgressRouter.hpp>

#include <cstdint>
#include <memory>

class QWindow;

namespace contour::platform
{

/// Receives window-scoped progress on the GUI thread. Owns native-window readiness and replay.
class TaskbarProgressSink
{
  public:
    virtual ~TaskbarProgressSink() = default;

    /// @return Whether this adapter can publish progress; unsupported platforms need no update timer.
    [[nodiscard]] virtual bool isSupported() const noexcept = 0;

    /// Observes a window before its first show. Rebinding replaces its previous native attachment.
    /// @param window Stable workspace window identity.
    /// @param nativeWindow Qt window whose taskbar button receives progress.
    virtual void bindWindow(uint64_t window, QWindow& nativeWindow) = 0;

    /// Retains the desired progress, including while the taskbar button is not ready.
    /// @param window Workspace window identity.
    /// @param progress Aggregated progress for this window only.
    virtual void setProgress(uint64_t window, TaskbarProgress progress) = 0;

    /// Stops observing a window and clears its taskbar progress if its button still exists.
    /// @param window Workspace window identity.
    virtual void removeWindow(uint64_t window) = 0;
};

/// Creates the native Windows adapter, or a no-op adapter on other platforms.
/// @return Application-owned taskbar sink; native resources are acquired after a window is bound.
[[nodiscard]] std::unique_ptr<TaskbarProgressSink> makeTaskbarProgressSink();

} // namespace contour::platform
