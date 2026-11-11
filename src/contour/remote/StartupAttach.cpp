// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/StartupAttach.hpp>

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
    _isWaiting = true;
    QTimer::singleShot(0, this, &StartupAttach::progressed);
}

void StartupAttach::progressed()
{
    if (!_isWaiting)
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
    _isWaiting = false;
    if (auto const opened = _steps.openFirstWindow(*settled); !opened)
        _steps.end(opened.error(), EXIT_FAILURE);
}

} // namespace contour::remote
