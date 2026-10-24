// SPDX-License-Identifier: Apache-2.0
#include <vthost/client/DaemonGrantReplies.hpp>

#include <vthost/DaemonExtensions.hpp>

namespace vthost::client
{

std::optional<vtextension::ExtensionConsentRequest> consentRequestOf(proto::PermissionRequest const& request)
{
    auto consent = vtextension::ExtensionConsentRequest {
        .hostId = request.hostId,
        .manifest =
            vtextension::Manifest {
                .name = request.name, .version = request.version, .commands = {}, .digest = request.digest },
    };
    consent.manifest.commands.reserve(request.commands.size());
    for (auto const& command: request.commands)
        consent.manifest.commands.push_back(
            vtextension::DeclaredCommand { .name = command.name, .argvTemplate = command.argv });
    consent.manifest.control.reserve(request.control.size());
    for (auto const& name: request.control)
    {
        auto const permission = vtcontrol::enumFromName<vtcontrol::Permission>(name);
        if (!permission)
            return std::nullopt;
        consent.manifest.control.push_back(*permission);
    }
    return consent;
}

vtextension::GrantState DaemonGrantReplies::lookup(std::string_view hostId, std::string_view digest) const
{
    auto const found = _answers.find(std::pair { std::string { hostId }, std::string { digest } });
    return found != _answers.end() ? found->second : vtextension::GrantState::Pending;
}

void DaemonGrantReplies::record(std::string_view hostId,
                                std::string_view digest,
                                vtextension::GrantState state)
{
    auto key = std::pair { std::string { hostId }, std::string { digest } };
    if (state == vtextension::GrantState::Pending)
        _answers.erase(key);
    else
        _answers.insert_or_assign(std::move(key), state);
    if (_reply)
        _reply(hostId, digest, wireGrantStateOf(state));
}

} // namespace vthost::client
