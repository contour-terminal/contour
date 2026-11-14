// SPDX-License-Identifier: Apache-2.0
#include <vthost/keeper/RestartPolicy.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace vthost::keeper
{

namespace
{
    struct EndClass
    {
        DaemonEndState state;
        DaemonExitKind kind;
    };

    /// The end classification, by the state the process ended in.
    constexpr auto EndClasses = std::array {
        EndClass { DaemonEndState::StartingFirst, DaemonExitKind::FailedToStart },
        EndClass { DaemonEndState::StartingRestart, DaemonExitKind::Crashed },
        EndClass { DaemonEndState::Serving, DaemonExitKind::Crashed },
        EndClass { DaemonEndState::ShutdownAnnounced, DaemonExitKind::ShutDown },
        EndClass { DaemonEndState::StopForwarded, DaemonExitKind::ShutDown },
        EndClass { DaemonEndState::HungKilled, DaemonExitKind::Hung },
        EndClass { DaemonEndState::FirstStartKilled, DaemonExitKind::FailedToStart },
    };
} // namespace

DaemonExitKind classifyExit(DaemonEndState state) noexcept
{
    auto const row = std::ranges::find(EndClasses, state, &EndClass::state);
    return row != EndClasses.end() ? row->kind : DaemonExitKind::Crashed;
}

CrashRestartPolicy::CrashRestartPolicy(RestartPolicyConfig config): _config { config }
{
}

RestartDecision CrashRestartPolicy::onExit(DaemonExitKind kind, core::platform::SteadyTimePoint now)
{
    switch (kind)
    {
        case DaemonExitKind::FailedToStart: return { .action = RestartAction::ExitWithStatus };
        case DaemonExitKind::ShutDown: return { .action = RestartAction::ExitClean };
        case DaemonExitKind::Crashed:
        case DaemonExitKind::Hung: break;
    }

    // A crash of the one retry ends everything: the newest image did no better.
    if (_phase == GiveUpPhase::Retrying)
        return { .action = RestartAction::EndAll };

    std::erase_if(_crashes, [&](auto crash) { return now - crash >= _config.giveUpWindow; });
    _crashes.push_back(now);
    auto const quick = _lastCrash && now - *_lastCrash < _config.quickRepeat;
    _lastCrash = now;

    if (_phase == GiveUpPhase::GivenUp || std::cmp_greater_equal(_crashes.size(), _config.giveUpCrashes))
    {
        _phase = GiveUpPhase::GivenUp;
        return { .action = RestartAction::GiveUp, .delay = _config.grace };
    }
    if (!quick)
    {
        _backoff = {};
        return { .action = RestartAction::RestartNow };
    }
    _backoff = _backoff == std::chrono::milliseconds {} ? _config.backoffStart
                                                        : std::min(_backoff * 2, _config.backoffCap);
    return { .action = RestartAction::RestartAfter, .delay = _backoff };
}

RestartDecision CrashRestartPolicy::onGraceElapsed(core::platform::SteadyTimePoint /*now*/)
{
    switch (_phase)
    {
        case GiveUpPhase::Running: return { .action = RestartAction::Ignore };
        case GiveUpPhase::GivenUp:
            _phase = GiveUpPhase::Retrying;
            return { .action = RestartAction::RetryNewestImage };
        case GiveUpPhase::Retrying: break;
    }
    return { .action = RestartAction::EndAll };
}

void CrashRestartPolicy::onStable(core::platform::SteadyTimePoint /*now*/)
{
    _crashes.clear();
    _lastCrash.reset();
    _backoff = {};
    _phase = GiveUpPhase::Running;
}

} // namespace vthost::keeper
