// SPDX-License-Identifier: Apache-2.0
#include <vtextension/HostState.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <variant>

namespace vtextension
{

namespace
{
    /// Visits a variant with one lambda per alternative.
    template <typename... Fs>
    struct Overloaded: Fs...
    {
        using Fs::operator()...;
    };
} // namespace

void recordEvent(StateSnapshot& state, Event const& event, std::size_t maxUserVars)
{
    std::visit(Overloaded {
                   [&](TitleChanged const& e) { state.title = e.title; },
                   [&](CwdChanged const& e) { state.cwd = e.cwd; },
                   [&](ForegroundProcessChanged const& e) { state.foregroundProcess = e.name; },
                   [&](InputModeChanged const& e) { state.inputMode = e.mode; },
                   [&](TabsChanged const& e) { state.tabs = e.tabs; },
                   [&](UserVarChanged const& e) {
                       auto& vars = state.userVars;
                       auto const found =
                           std::ranges::find(vars, e.name, &std::pair<std::string, std::string>::first);
                       if (found != vars.end())
                           found->second = e.value;
                       else if (vars.size() < maxUserVars)
                           vars.emplace_back(e.name, e.value);
                   },
                   [&](ProgressChanged const& e) { state.progress = e.progress; },
                   [&](FocusChanged const& e) { state.focus = e.state; },
                   [&](FontChanged const& e) { state.font = e.metrics; },
                   [&](Resized const& e) { state.size = e.size; },
                   [](TimerFired const&) {},
                   [](ReplyBytes const&) {},
               },
               event);
}

} // namespace vtextension
