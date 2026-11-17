// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SshFailureMessage.hpp>

#include <format>

namespace vtpty
{

std::string describeSshConnectFailure(SshConnectStage stage,
                                      NetworkAccess network,
                                      std::string_view target,
                                      std::string_view detail)
{
    if (network == NetworkAccess::Denied)
        return std::format("Cannot reach {}: this sandbox permits no network access. Contour's SSH "
                           "runs inside the sandbox, so it needs the \"network\" permission.",
                           target);

    auto const what = stage == SshConnectStage::Resolve ? "Failed to resolve host" : "Failed to connect to";

    // The platform's own text is often empty for a failure it has no message for; saying "Failed to
    // connect to host:22. " with nothing after it reads like something went missing.
    if (detail.empty())
        return std::format("{} {}.", what, target);

    return std::format("{} {}. {}", what, target, detail);
}

std::string describeSshChannelOpenFailure(std::string_view detail, TerminfoInstallAttempt attempt)
{
    if (attempt == TerminfoInstallAttempt::Attempted)
        return std::format("Failed to open SSH channel. {} Contour had just tried to install its terminfo "
                           "on this host; set ssh.install_terminfo: false to skip that.",
                           detail);
    return std::format("Failed to open SSH channel. {}", detail);
}

std::string describeSshConnectTimeout(std::string_view stage,
                                      std::string_view host,
                                      std::chrono::seconds limit)
{
    return std::format(
        "Connecting to {} timed out during {}: it took longer than ssh.connect_timeout ({} s). "
        "Raise it, or set it to 0 for no limit, if the host is slow to answer.",
        host,
        stage,
        limit.count());
}

std::string describeSshAgentTimeout(std::string_view host, std::chrono::seconds allowance)
{
    auto const span =
        allowance.count() % 60 == 0
            ? std::format("{} minutes", std::chrono::duration_cast<std::chrono::minutes>(allowance).count())
            : std::format("{} seconds", allowance.count());
    return std::format(
        "Authenticating to {} through the SSH agent did not finish within {}: the agent may be "
        "waiting for an approval that never came, or the host stopped answering.",
        host,
        span);
}

std::string describeSshAgentClosed(std::string_view host)
{
    return std::format(
        "The SSH agent closed its connection while authenticating to {}; trying the next way to "
        "log in.",
        host);
}

} // namespace vtpty
