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

} // namespace vtpty
