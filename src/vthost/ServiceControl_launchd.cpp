// SPDX-License-Identifier: Apache-2.0
#include <vthost/ServiceControl_launchd.hpp>

#include <core/Utf8Path.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <ranges>
#include <tuple>
#include <utility>

namespace vthost
{

namespace
{
    /// The characters a plist's XML text must not carry, and what stands for each.
    constexpr auto XmlEscapes = std::array {
        std::pair { '&', std::string_view { "&amp;" } },   std::pair { '<', std::string_view { "&lt;" } },
        std::pair { '>', std::string_view { "&gt;" } },    std::pair { '"', std::string_view { "&quot;" } },
        std::pair { '\'', std::string_view { "&apos;" } },
    };

    constexpr auto LabelPrefix = std::string_view { "org.contourterminal." };
    constexpr auto StringOpen = std::string_view { "<string>" };
    constexpr auto StringClose = std::string_view { "</string>" };

    [[nodiscard]] std::string xmlEscape(std::string_view text)
    {
        auto result = std::string {};
        for (auto const ch: text)
        {
            auto const escape = std::ranges::find(XmlEscapes, ch, &decltype(XmlEscapes)::value_type::first);
            if (escape != XmlEscapes.end())
                result += escape->second;
            else
                result += ch;
        }
        return result;
    }

    [[nodiscard]] std::string xmlUnescape(std::string_view text)
    {
        auto result = std::string {};
        while (!text.empty())
        {
            auto const escape = std::ranges::find_if(
                XmlEscapes, [&](auto const& entry) { return text.starts_with(entry.second); });
            if (escape != XmlEscapes.end())
            {
                result += escape->first;
                text.remove_prefix(escape->second.size());
            }
            else
            {
                result += text.front();
                text.remove_prefix(1);
            }
        }
        return result;
    }

    [[nodiscard]] std::string_view trimLeadingWhitespace(std::string_view text)
    {
        auto const first = text.find_first_not_of(" \t");
        return first == std::string_view::npos ? std::string_view {} : text.substr(first);
    }

    [[nodiscard]] ServiceError failure(std::string context)
    {
        return ServiceError { .code = ServiceErrorCode::Backend,
                              .systemCode = 0,
                              .context = std::move(context) };
    }
} // namespace

std::string launchdLabelFor(std::string_view registrationName)
{
    return std::format("{}{}", LabelPrefix, registrationName);
}

std::filesystem::path launchAgentPath(std::filesystem::path const& home, std::string_view label)
{
    return home / "Library" / "LaunchAgents" / std::format("{}.plist", label);
}

std::string launchdGuiDomain(unsigned userId)
{
    return std::format("gui/{}", userId);
}

std::string renderLaunchAgentPlist(std::string_view label, ServiceInstallRequest const& request)
{
    auto arguments = std::string {};
    for (auto const& argument: request.commandLine)
        arguments += std::format("        <string>{}</string>\n", xmlEscape(argument));
    return std::format(R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>{}</string>
    <key>ProgramArguments</key>
    <array>
{}    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <dict>
        <key>SuccessfulExit</key>
        <false/>
    </dict>
    <key>ProcessType</key>
    <string>Interactive</string>
</dict>
</plist>
)",
                       xmlEscape(label),
                       arguments);
}

std::vector<std::string> parseLaunchAgentArguments(std::string_view plist)
{
    constexpr auto Key = std::string_view { "<key>ProgramArguments</key>" };
    constexpr auto ArrayClose = std::string_view { "</array>" };

    auto arguments = std::vector<std::string> {};
    auto const keyAt = plist.find(Key);
    if (keyAt == std::string_view::npos)
        return arguments;
    auto rest = plist.substr(keyAt + Key.size());
    rest = rest.substr(0, rest.find(ArrayClose));
    for (auto open = rest.find(StringOpen); open != std::string_view::npos; open = rest.find(StringOpen))
    {
        rest.remove_prefix(open + StringOpen.size());
        auto const close = rest.find(StringClose);
        if (close == std::string_view::npos)
            break;
        arguments.push_back(xmlUnescape(rest.substr(0, close)));
        rest.remove_prefix(close + StringClose.size());
    }
    return arguments;
}

ServiceRunState parseLaunchctlPrint(int exitCode, std::string_view output)
{
    if (exitCode != 0)
        return ServiceRunState::Stopped;
    for (auto const line: std::views::split(output, '\n'))
        if (trimLeadingWhitespace(std::string_view { line.begin(), line.end() }) == "state = running")
            return ServiceRunState::Running;
    return ServiceRunState::Stopped;
}

LaunchdBackend::LaunchdBackend(LaunchdAgentConfig config, ServiceBackendServices const& services):
    _config { std::move(config) }, _fileSystem { services.fileSystem }, _commands { services.commands }
{
}

DaemonLaunchStyle LaunchdBackend::launchStyle() const noexcept
{
    return DaemonLaunchStyle::Foreground;
}

std::string LaunchdBackend::target() const
{
    return std::format("{}/{}", _config.domain, _config.label);
}

std::expected<CommandOutcome, ServiceError> LaunchdBackend::launchctl(
    std::vector<std::string> arguments) const
{
    auto const context = std::format("launchctl {}", arguments.front());
    auto argv = std::vector<std::string> { "launchctl" };
    argv.insert(
        argv.end(), std::make_move_iterator(arguments.begin()), std::make_move_iterator(arguments.end()));
    return runServiceCommand(_commands, argv, context);
}

std::expected<void, ServiceError> LaunchdBackend::install(ServiceInstallRequest const& request)
{
    // Written, not loaded: launchd loads LaunchAgents at the next login, and `start` loads one now.
    // An existing plist is rewritten, which is how the command line is updated.
    auto const path = core::pathToUtf8(_config.plistPath);
    if (auto const created = _fileSystem.createDirectories(_config.plistPath.parent_path()); !created)
        return std::unexpected(
            failure(std::format("creating the directory of {}: {}", path, created.error())));
    if (auto const written =
            _fileSystem.writeFile(_config.plistPath, renderLaunchAgentPlist(_config.label, request));
        !written)
        return std::unexpected(failure(std::format("writing {}: {}", path, written.error())));
    return {};
}

std::expected<void, ServiceError> LaunchdBackend::uninstall()
{
    if (!_fileSystem.exists(_config.plistPath))
        return std::unexpected(ServiceError { .code = ServiceErrorCode::NotInstalled });

    // An agent that was never loaded makes bootout fail; the plist is to go either way.
    std::ignore = launchctl({ "bootout", target() });
    if (auto const removed = _fileSystem.remove(_config.plistPath); !removed)
        return std::unexpected(
            failure(std::format("removing {}: {}", core::pathToUtf8(_config.plistPath), removed.error())));
    return {};
}

std::expected<void, ServiceError> LaunchdBackend::start()
{
    if (!_fileSystem.exists(_config.plistPath))
        return std::unexpected(ServiceError { .code = ServiceErrorCode::NotInstalled });

    // bootstrap fails when the agent is already loaded (e.g. by the login that read the plist); kickstart
    // then starts the loaded one.
    return launchctl({ "bootstrap", _config.domain, core::pathToUtf8(_config.plistPath) })
        .or_else([&](ServiceError const& bootstrapError) -> std::expected<CommandOutcome, ServiceError> {
            // Report the bootstrap failure, which is the cause; a failed kickstart only says the agent
            // was not loaded either.
            if (auto kicked = launchctl({ "kickstart", target() }); !kicked)
                return std::unexpected(bootstrapError);
            return CommandOutcome {};
        })
        .transform([](auto const&) {});
}

std::expected<void, ServiceError> LaunchdBackend::stop()
{
    return launchctl({ "kill", "SIGTERM", target() }).transform([](auto const&) {});
}

std::expected<ServiceStatus, ServiceError> LaunchdBackend::status() const
{
    if (!_fileSystem.exists(_config.plistPath))
        return ServiceStatus { .state = ServiceRunState::NotInstalled };

    // `launchctl print` exits non-zero for an agent that is registered but not loaded; the outcome is
    // data here, so the run is not routed through launchctl()'s exit-code check.
    auto const printed = _commands.run(std::vector<std::string> { "launchctl", "print", target() });
    if (!printed)
        return std::unexpected(failure("launchctl not found"));

    auto status = ServiceStatus { .state = parseLaunchctlPrint(printed->exitCode, printed->standardOutput) };
    if (auto const plist = _fileSystem.readFile(_config.plistPath))
        status.commandLine =
            std::ranges::to<std::string>(parseLaunchAgentArguments(*plist) | std::views::join_with(' '));
    return status;
}

} // namespace vthost
