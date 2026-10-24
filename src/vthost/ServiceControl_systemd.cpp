// SPDX-License-Identifier: Apache-2.0
#include <vthost/ServiceControl_systemd.hpp>

#include <core/Utf8Path.hpp>
#include <core/platform/UserPaths.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <ranges>
#include <tuple>
#include <utility>
#include <vector>

namespace vthost
{

namespace
{
    /// Each character systemd treats specially inside a double-quoted word, and what it becomes.
    constexpr auto QuoteEscapes = std::array {
        std::pair { '\\', std::string_view { "\\\\" } },
        std::pair { '"', std::string_view { "\\\"" } },
        std::pair { '%', std::string_view { "%%" } },
        std::pair { '$', std::string_view { "$$" } },
    };

    constexpr auto ExecStartKey = std::string_view { "ExecStart=" };

    /// The keys `status()` asks `systemctl show` for.
    constexpr auto ShowProperties =
        std::array { std::string_view { "LoadState" }, std::string_view { "ActiveState" } };

    [[nodiscard]] std::string loadStateOf(std::string_view showOutput)
    {
        auto const properties = parseSystemctlShow(showOutput);
        auto const found = properties.find("LoadState");
        return found != properties.end() ? found->second : std::string {};
    }

    [[nodiscard]] std::string trimTrailingNewline(std::string_view text)
    {
        while (text.ends_with('\n') || text.ends_with('\r'))
            text.remove_suffix(1);
        return std::string { text };
    }
} // namespace

std::string systemdQuote(std::string_view argument)
{
    auto result = std::string { "\"" };
    for (auto const ch: argument)
    {
        auto const escape = std::ranges::find(QuoteEscapes, ch, &decltype(QuoteEscapes)::value_type::first);
        if (escape != QuoteEscapes.end())
            result += escape->second;
        else
            result += ch;
    }
    result += '"';
    return result;
}

namespace
{
    /// @return @p text as one line of a unit file value: control characters dropped, `%` doubled (a
    ///         specifier introducer in every value).
    [[nodiscard]] std::string unitLineValue(std::string_view text)
    {
        auto result = std::string {};
        for (auto const ch: text)
        {
            if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f)
                continue;
            result += ch;
            if (ch == '%')
                result += '%';
        }
        return result;
    }
} // namespace

std::string renderSystemdUnit(ServiceInstallRequest const& request)
{
    auto execStart = std::string {};
    for (auto const& argument: request.commandLine)
    {
        if (!execStart.empty())
            execStart += ' ';
        execStart += systemdQuote(argument);
    }
    return std::format("[Unit]\n"
                       "Description={}\n"
                       "Documentation=https://contour-terminal.org/persistent-sessions/\n"
                       "\n"
                       "[Service]\n"
                       "Type=simple\n"
                       "ExecStart={}\n"
                       "Restart=on-failure\n"
                       "\n"
                       "[Install]\n"
                       "WantedBy=default.target\n",
                       unitLineValue(request.displayName),
                       execStart);
}

std::filesystem::path systemdUserUnitDirectory(core::Environment const& environment)
{
    if (auto const config = core::platform::configHome(environment))
        return *config / "systemd" / "user";
    return {};
}

std::map<std::string, std::string, std::less<>> parseSystemctlShow(std::string_view output)
{
    auto properties = std::map<std::string, std::string, std::less<>> {};
    for (auto const line: std::views::split(output, '\n'))
    {
        auto const text = std::string_view { line.begin(), line.end() };
        if (auto const equals = text.find('='); equals != std::string_view::npos)
            properties.insert_or_assign(std::string { text.substr(0, equals) },
                                        trimTrailingNewline(text.substr(equals + 1)));
    }
    return properties;
}

SystemdUserBackend::SystemdUserBackend(SystemdUnitConfig config, ServiceBackendServices const& services):
    _config { std::move(config) }, _fileSystem { services.fileSystem }, _commands { services.commands }
{
}

DaemonLaunchStyle SystemdUserBackend::launchStyle() const noexcept
{
    return DaemonLaunchStyle::Foreground;
}

std::expected<void, ServiceError> SystemdUserBackend::install(ServiceInstallRequest const& request)
{
    // An existing unit is rewritten, not refused: installing again is how the command line is updated.
    auto const fail = [&](std::string const& reason) {
        return std::unexpected(ServiceError {
            .code = ServiceErrorCode::Backend,
            .systemCode = 0,
            .context = std::format("writing {}: {}", core::pathToUtf8(_config.unitPath), reason) });
    };
    if (auto const created = _fileSystem.createDirectories(_config.unitPath.parent_path()); !created)
        return fail(created.error());
    if (auto const written = _fileSystem.writeFile(_config.unitPath, renderSystemdUnit(request)); !written)
        return fail(written.error());

    return systemctl({ "daemon-reload" })
        .and_then([&](auto const&) { return systemctl({ "enable", _config.unitName }); })
        .transform([](auto const&) {});
}

std::expected<void, ServiceError> SystemdUserBackend::uninstall()
{
    auto const fileExists = _fileSystem.exists(_config.unitPath);
    // A systemctl that cannot be started says nothing about the unit: that is an error, not "not installed".
    auto const shown = _commands.run(showArgv());
    if (!shown)
        return std::unexpected(ServiceError { .code = ServiceErrorCode::Backend,
                                              .systemCode = shown.error().value(),
                                              .context = "systemctl not found" });
    auto const loaded = shown->exitCode == 0 && loadStateOf(shown->standardOutput) != "not-found";
    if (!fileExists && !loaded)
        return std::unexpected(ServiceError { .code = ServiceErrorCode::NotInstalled });

    // Whether the unit was enabled or running is of no interest to a removal: the file goes either way.
    std::ignore = systemctl({ "disable", "--now", _config.unitName });
    if (fileExists)
        if (auto const removed = _fileSystem.remove(_config.unitPath); !removed)
            return std::unexpected(ServiceError {
                .code = ServiceErrorCode::Backend,
                .systemCode = 0,
                .context =
                    std::format("removing {}: {}", core::pathToUtf8(_config.unitPath), removed.error()) });
    return systemctl({ "daemon-reload" }).transform([](auto const&) {});
}

std::expected<void, ServiceError> SystemdUserBackend::start()
{
    return systemctl({ "start", _config.unitName }).transform([](auto const&) {});
}

std::expected<void, ServiceError> SystemdUserBackend::stop()
{
    return systemctl({ "stop", _config.unitName }).transform([](auto const&) {});
}

std::expected<CommandOutcome, ServiceError> SystemdUserBackend::systemctl(
    std::vector<std::string> arguments) const
{
    auto const context = std::format("systemctl --user {}", arguments.front());
    auto argv = std::vector<std::string> { "systemctl", "--user" };
    argv.insert(
        argv.end(), std::make_move_iterator(arguments.begin()), std::make_move_iterator(arguments.end()));
    return runServiceCommand(_commands, argv, context);
}

std::vector<std::string> SystemdUserBackend::showArgv() const
{
    auto argv = std::vector<std::string> { "systemctl", "--user", "show", _config.unitName };
    for (auto const property: ShowProperties)
    {
        argv.emplace_back("-p");
        argv.emplace_back(property);
    }
    return argv;
}

std::expected<CommandOutcome, ServiceError> SystemdUserBackend::show() const
{
    return runServiceCommand(_commands, showArgv(), "systemctl --user show");
}

std::expected<ServiceStatus, ServiceError> SystemdUserBackend::status() const
{
    return show().transform([&](CommandOutcome const& outcome) {
        auto const properties = parseSystemctlShow(outcome.standardOutput);
        auto const value = [&](std::string_view key) {
            auto const found = properties.find(key);
            return found != properties.end() ? std::string_view { found->second } : std::string_view {};
        };

        auto status = ServiceStatus { .mode = ServiceStartMode::Logon };
        if (value("LoadState") == "not-found")
            status.state = ServiceRunState::NotInstalled;
        else if (value("ActiveState") == "active")
            status.state = ServiceRunState::Running;
        else
            status.state = ServiceRunState::Stopped;

        if (auto const unit = _fileSystem.readFile(_config.unitPath))
            for (auto const line: std::views::split(*unit, '\n'))
                if (auto const text = std::string_view { line.begin(), line.end() };
                    text.starts_with(ExecStartKey))
                    status.commandLine = text.substr(ExecStartKey.size());
        return status;
    });
}

} // namespace vthost
