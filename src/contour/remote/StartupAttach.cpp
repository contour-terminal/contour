// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/StartupAttach.hpp>

#include <QtCore/QPointer>

#include <cstdlib>
#include <utility>
#include <variant>

namespace contour::remote
{

DuringStartup duringStartup(actions::Action const& action) noexcept
{
    return std::holds_alternative<actions::Quit>(action) ? DuringStartup::Allowed : DuringStartup::Refused;
}

StartupAttach::StartupAttach(StartupAttachSteps steps): _steps { std::move(steps) }
{
    _deadline.setSingleShot(true);
    connect(&_deadline, &QTimer::timeout, this, &StartupAttach::progressed);
}

void StartupAttach::start()
{
    if (!_steps.prepare)
    {
        _phase = Phase::Waiting;
        QTimer::singleShot(0, this, &StartupAttach::progressed);
        return;
    }
    _phase = Phase::Preparing;
    // Told back from the event loop, and only while this wait lives.
    _steps.prepare([self = QPointer { this }](std::expected<void, std::string> started) {
        if (self)
            self->prepared(std::move(started));
    });
}

void StartupAttach::prepared(std::expected<void, std::string> started)
{
    if (_phase != Phase::Preparing)
        return;
    if (!started)
    {
        _phase = Phase::Settled;
        _steps.end(started.error(), EXIT_FAILURE);
        return;
    }
    _phase = Phase::Waiting;
    progressed();
}

void StartupAttach::progressed()
{
    if (_phase != Phase::Waiting)
        return;
    auto const settled = _steps.look();
    if (!settled)
    {
        // Looked at again on the attach's next news; and when its time is up, if that is known by now.
        if (auto const left = _steps.untilDeadline())
            _deadline.start(*left);
        return;
    }
    _deadline.stop();
    _phase = Phase::Settled;
    if (auto const opened = _steps.openFirstWindow(*settled); !opened)
        _steps.end(opened.error(), EXIT_FAILURE);
}

} // namespace contour::remote
