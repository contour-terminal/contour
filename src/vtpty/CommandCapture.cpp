// SPDX-License-Identifier: Apache-2.0
#include <vtpty/CommandCapture.hpp>

#include <utility>

namespace vtpty
{

SystemCommandCapture::SystemCommandCapture(std::string shell,
                                           core::async::StopToken shutdown,
                                           [[maybe_unused]] std::optional<std::uint64_t> descriptorLimit):
    _shell { std::move(shell) },
    _shutdown { std::move(shutdown) }
#ifndef _WIN32
    ,
    _descriptorLimit { descriptorLimit }
#endif
{
}

std::string SystemCommandCapture::defaultShell([[maybe_unused]] core::Environment const& env)
{
#ifdef _WIN32
    return env.get("COMSPEC").value_or("cmd.exe");
#else
    return "/bin/sh";
#endif
}

std::string firstLineOf(std::string_view output)
{
    auto line = output.substr(0, output.find('\n')); // npos -> the whole string
    if (line.ends_with('\r'))
        line.remove_suffix(1);
    return std::string { line };
}

} // namespace vtpty
