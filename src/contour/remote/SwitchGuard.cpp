// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/SwitchGuard.hpp>

#include <memory>
#include <string_view>
#include <utility>

namespace contour::remote
{

namespace
{
    /// @return How the prompt names @p daemon: its label or host name; empty for one no switch can name.
    [[nodiscard]] std::string nameOf(DaemonAddress const& daemon)
    {
        return daemon.kind == vthost::proto::SwitchDaemonKind::Same ? std::string {} : daemon.name;
    }

    /// Has @p switching perform the allowed @p request, and tells @p done its outcome.
    void perform(GuardedSwitch const& request, session::WindowSwitching& switching, GuardedSwitchDone done)
    {
        switching.request(
            request.window,
            request.target,
            session::SwitchMode::Switch,
            [done = std::move(done)](std::expected<vthost::proto::SwitchOutcome, std::string> outcome) {
                if (outcome)
                    done(*outcome, {});
                else
                    done(vthost::proto::SwitchOutcome::Failed, std::move(outcome.error()));
            });
    }
} // namespace

void runGuardedSwitch(GuardedSwitch const& request,
                      SwitchGrants& grants,
                      SwitchAsker const& ask,
                      session::WindowSwitching& switching,
                      SwitchTimer const& timer,
                      GuardedSwitchDone done)
{
    if (auto const remembered = grants.remembered(request.source, request.target))
    {
        switch (*remembered)
        {
            case session::PermissionDecision::Deny:
                done(vthost::proto::SwitchOutcome::Refused, std::string { SwitchRefusedDetail });
                return;
            case session::PermissionDecision::Allow: perform(request, switching, std::move(done)); return;
        }
    }

    // Shared by the question and this frame: an ask that fails hands done back here, unspent.
    auto shared = std::make_shared<GuardedSwitchDone>(std::move(done));
    auto const window = request.window;
    auto question = session::SwitchAsk {
        .target = request.target,
        .sourceDaemon = nameOf(request.source),
        .remembering = rememberingFor(request.target),
        .answered =
            [request, grants = &grants, switching = &switching, shared](session::PermissionDecision decision,
                                                                        session::PermissionMemory memory) {
                if (!*shared)
                    return;
                auto told = std::exchange(*shared, {});
                if (memory != session::PermissionMemory::Once)
                    grants->remember(request.source, request.target, decision);
                switch (decision)
                {
                    case session::PermissionDecision::Deny:
                        told(vthost::proto::SwitchOutcome::Refused, std::string { SwitchRefusedDetail });
                        return;
                    case session::PermissionDecision::Allow:
                        perform(request, *switching, std::move(told));
                        return;
                }
            },
    };
    auto asked = ask(window, std::move(question));
    if (!asked)
    {
        if (*shared)
            std::exchange(*shared, {})(vthost::proto::SwitchOutcome::Failed, std::move(asked.error()));
        return;
    }
    // Before the daemon gives up waiting: an answer after that would switch a window nobody waits for.
    timer(SwitchQuestionExpiry, [shared, withdraw = *std::move(asked)] {
        if (!*shared)
            return;
        auto told = std::exchange(*shared, {});
        if (withdraw)
            withdraw();
        told(vthost::proto::SwitchOutcome::Refused, std::string { SwitchUnansweredDetail });
    });
}

} // namespace contour::remote
