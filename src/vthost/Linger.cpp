// SPDX-License-Identifier: Apache-2.0
#include <vthost/Linger.hpp>

#include <array>
#include <string>

namespace vthost
{

LingerState lingerStateOf(CommandRunner& commands, std::string_view user)
{
    if (user.empty())
        return LingerState::Unknown;

    auto const argv =
        std::array<std::string, 5> { "loginctl", "show-user", std::string { user }, "-p", "Linger" };
    auto const outcome = commands.run(argv);
    if (!outcome || outcome->exitCode != 0)
        return LingerState::Unknown;

    auto const text = std::string_view { outcome->standardOutput };
    auto const first = text.substr(0, text.find('\n'));
    if (first == "Linger=yes")
        return LingerState::On;
    if (first == "Linger=no")
        return LingerState::Off;
    return LingerState::Unknown;
}

std::string_view lingerAdvice()
{
    return "Your login session ends the daemon: systemd stops your user services at the last logout. "
           "To keep it running, run: loginctl enable-linger";
}

std::string_view lingerHintAfterInstall(ServiceStartMode mode,
                                        CommandRunner& commands,
                                        core::Environment const& environment)
{
    if (mode != ServiceStartMode::Logon)
        return {};
    auto const user = environment.get("USER").value_or("");
    return lingerStateOf(commands, user) == LingerState::Off ? lingerAdvice() : std::string_view {};
}

} // namespace vthost
