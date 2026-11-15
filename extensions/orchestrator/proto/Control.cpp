// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/proto/Control.hpp>

#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>
#include <orchestrator/core/Engine.hpp>
#include <orchestrator/proto/HostJson.hpp>
#include <orchestrator/proto/Spawn.hpp>

namespace orchestrator::proto
{

namespace
{
    constexpr auto ReadOptions = glz::opts { .null_terminated = false, .error_on_unknown_keys = false };

    /// @return A message saying what became of a call that did not exit 0.
    [[nodiscard]] std::string failureOf(ProcessOutcome const& outcome)
    {
        if (outcome.start != StartResult::Started)
            return { "contour could not be started" };
        if (outcome.completion == Completion::TimedOut)
            return { "contour timed out" };
        auto why = outcome.stderrText.empty() ? outcome.stdoutText : outcome.stderrText;
        while (!why.empty() && why.back() == '\n')
            why.pop_back();
        return why.empty() ? std::format("contour exited with {}", outcome.exitCode.value_or(-1)) : why;
    }
} // namespace

ControlApi::ControlApi(std::string contour,
                       std::string daemon,
                       std::vector<std::string> environment,
                       std::string cwd):
    _contour { std::move(contour) },
    _daemon { std::move(daemon) },
    _environment { std::move(environment) },
    _cwd { std::move(cwd) }
{
}

std::expected<std::string, ControlFailure> ControlApi::call(std::string_view method,
                                                            std::string request) const
{
    auto const outcome = runToCompletion(
        SpawnRequest {
            .argv = { _contour, "api", "call", "daemon", _daemon, std::string(method), std::move(request) },
            .environment = _environment,
            .cwd = _cwd,
            .timeout = ShortTimeout,
        },
        monotonicNowMs());
    if (outcome.start != StartResult::Started || outcome.completion != Completion::Exited
        || outcome.exitCode != 0 || outcome.extent != OutputExtent::Whole)
        return std::unexpected { ControlFailure { .why = failureOf(outcome) } };
    return outcome.stdoutText;
}

std::expected<std::string, ControlFailure> ControlApi::openTab(std::string const& cwd,
                                                               std::string const& label,
                                                               std::vector<std::string> command) const
{
    auto request = std::string {};
    if (glz::write_json(detail::JsonCreateTab { .placement = "existing",
                                                .cwd = cwd,
                                                .command = std::move(command),
                                                .label = label,
                                                .focus = "keep" },
                        request))
        return std::unexpected { ControlFailure { .why = "the request could not be written" } };
    return call("tab.create", std::move(request)).and_then([](std::string const& reply) {
        auto created = detail::JsonCreatedTab {};
        if (glz::read<ReadOptions>(created, reply) || created.pane.empty())
            return std::expected<std::string, ControlFailure> { std::unexpected {
                ControlFailure { .why = "tab.create replied without a pane" } } };
        return std::expected<std::string, ControlFailure> { created.pane };
    });
}

std::expected<void, ControlFailure> ControlApi::closePane(std::string const& controlPane) const
{
    auto request = std::string {};
    if (glz::write_json(detail::JsonClosePane { .pane = controlPane }, request))
        return std::unexpected { ControlFailure { .why = "the request could not be written" } };
    return call("pane.close", std::move(request)).transform([](std::string const&) {});
}

std::expected<std::string, ControlFailure> ControlApi::listWorkspaces() const
{
    return call("workspace.list", "{}");
}

} // namespace orchestrator::proto
