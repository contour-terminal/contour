// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/SettingsEditSession.hpp>

#include <QtCore/QObject>
#include <QtCore/QTimer>

#include <chrono>
#include <functional>
#include <utility>

namespace contour::window
{

/// The Settings save model's time source in the app: one single-shot QTimer on the GUI thread, restarted by
/// every schedule() (a debounce).
class QtSaveScheduler final: public config::SaveScheduler
{
  public:
    QtSaveScheduler()
    {
        _timer.setSingleShot(true);
        QObject::connect(&_timer, &QTimer::timeout, [this] {
            if (auto fire = std::exchange(_fire, nullptr))
                fire();
        });
    }

    void schedule(std::chrono::milliseconds delay, std::function<void()> fire) override
    {
        _fire = std::move(fire);
        _timer.start(delay);
    }

    void cancel() override
    {
        _timer.stop();
        _fire = nullptr;
    }

  private:
    QTimer _timer;
    std::function<void()> _fire;
};

} // namespace contour::window
