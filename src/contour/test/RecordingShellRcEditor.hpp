// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/ShellRcEditor.hpp>

#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace contour::test
{

/// A shell rc editor that touches no file: what inspect() answers is scripted per shell, and every install
/// is recorded instead of written. The one TestApp holds, so no test run can append to a real `~/.zshrc`.
class RecordingShellRcEditor final: public platform::IShellRcEditor
{
  public:
    /// One install the editor was asked for.
    struct Install
    {
        config::ShellKind shell = config::ShellKind::Unknown;
        std::string line {};
        platform::RcEnvironment sessionEnvironment {};

        [[nodiscard]] bool operator==(Install const&) const = default;
    };

    /// What inspect() answers for a shell; a shell without an entry is not inspectable (RcState::Unknown), as
    /// the null editor says, so no card is offered unless a test scripts one.
    std::map<config::ShellKind, std::expected<platform::RcInspection, platform::ShellRcError>> inspections;

    /// What every install() answers.
    std::expected<platform::RcInstall, platform::ShellRcError> installResult = platform::RcInstall::Appended;

    /// Every install() asked for, in order.
    std::vector<Install> installs;

    /// The session environment of every inspect() asked for, in order.
    mutable std::vector<platform::RcEnvironment> inspectedEnvironments;

    [[nodiscard]] std::expected<platform::RcInspection, platform::ShellRcError> inspect(
        config::ShellKind shell, platform::RcEnvironment const& sessionEnvironment) const override
    {
        inspectedEnvironments.push_back(sessionEnvironment);
        if (auto const found = inspections.find(shell); found != inspections.end())
            return found->second;
        return platform::RcInspection {};
    }

    [[nodiscard]] std::expected<platform::RcInstall, platform::ShellRcError> install(
        config::ShellKind shell,
        std::string_view line,
        platform::RcEnvironment const& sessionEnvironment) override
    {
        installs.push_back(Install {
            .shell = shell, .line = std::string { line }, .sessionEnvironment = sessionEnvironment });
        return installResult;
    }
};

} // namespace contour::test
