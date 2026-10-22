// SPDX-License-Identifier: Apache-2.0
#include <vthost/DaemonExtensions.hpp>

#include <algorithm>
#include <array>
#include <utility>

#include <vtextension/ExtensionConsent.hpp>
#include <vthost/Logging.hpp>

namespace vthost
{

namespace
{
    /// One answer, as the wire spells it and as the grant store keeps it.
    struct GrantMapping
    {
        GrantState wire;
        vtextension::GrantState stored;
    };

    constexpr auto GrantMappings = std::array {
        GrantMapping { .wire = GrantState::Pending, .stored = vtextension::GrantState::Pending },
        GrantMapping { .wire = GrantState::Granted, .stored = vtextension::GrantState::Granted },
        GrantMapping { .wire = GrantState::Denied, .stored = vtextension::GrantState::Denied },
    };

    /// Hands a session's finished builds to its host's loop.
    class PostingOwner final: public vtextension::IOwnerThread
    {
      public:
        explicit PostingOwner(OwnerPost post): _post { std::move(post) } {}

        void post(std::function<void()> job) override { _post(std::move(job)); }

      private:
        OwnerPost _post;
    };

    /// What an extension logs, in the daemon's log. Called on the extension's lane.
    void logExtensionMessage(std::string_view name, vtextension::LogLevel level, std::string_view message)
    {
        // Asked first, so that a chatty guest costs nothing while nobody reads the category.
        if (!extensionLog.isEnabled())
            return;
        extensionLog()("{}", vtextension::extensionLogLine(name, level, message));
    }

    /// One hosted session's extensions: vtextension's per-pane bundle, built on the runtime's build thread.
    class DaemonSessionExtensions final: public ISessionExtensions
    {
      public:
        DaemonSessionExtensions(vtextension::DeclaredExtensions declared,
                                vtextension::SessionExtensionServices services,
                                SessionExtensionEvents events,
                                OwnerPost post):
            _declared { std::move(declared) },
            _services { services },
            _extensions { vtextension::SessionExtensionCallbacks {
                              .onPermissionNeeded =
                                  [onNeeded = std::move(events.onPermissionNeeded)](
                                      std::string_view hostId, vtextension::Manifest const& manifest) {
                                      onNeeded(permissionRequestOf(hostId, manifest));
                                  },
                              .onLog = &logExtensionMessage,
                              .onReplaced = std::move(events.onReplaced) },
                          std::make_shared<PostingOwner>(std::move(post)) }
        {
        }

        [[nodiscard]] vtbackend::StatusLineExtensionServices terminalServices() noexcept override
        {
            return _extensions.terminalServices();
        }

        void start() override
        {
            // No cell size: the daemon draws in no pixels, and a take-over's graphics are sized by none.
            _extensions.rebuild(_declared, {}, _services);
        }

        [[nodiscard]] vtbackend::StatusLineEventKinds subscriptions() const noexcept override
        {
            return _extensions.subscriptions();
        }

      private:
        vtextension::DeclaredExtensions _declared;
        vtextension::SessionExtensionServices _services;
        vtextension::SessionExtensions _extensions;
    };
} // namespace

proto::PermissionRequest permissionRequestOf(std::string_view hostId, vtextension::Manifest const& manifest)
{
    auto request = proto::PermissionRequest { .session = 0,
                                              .hostId = std::string { hostId },
                                              .name = manifest.name,
                                              .version = manifest.version,
                                              .digest = manifest.digest,
                                              .commands = {} };
    request.commands.reserve(manifest.commands.size());
    for (auto const& command: manifest.commands)
        request.commands.push_back(
            proto::WireDeclaredCommand { .name = command.name, .argv = command.argvTemplate });
    return request;
}

vtextension::GrantState storedGrantStateOf(GrantState answer) noexcept
{
    auto const mapping = std::ranges::find(GrantMappings, answer, &GrantMapping::wire);
    // Unreachable for a valid GrantState; no answer is the safe reading of anything else.
    return mapping != GrantMappings.end() ? mapping->stored : vtextension::GrantState::Pending;
}

GrantState wireGrantStateOf(vtextension::GrantState state) noexcept
{
    auto const mapping = std::ranges::find(GrantMappings, state, &GrantMapping::stored);
    return mapping != GrantMappings.end() ? mapping->wire : GrantState::Pending;
}

DaemonExtensions::DaemonExtensions(DaemonExtensionsConfig config, DaemonExtensionServices services):
    _declared { std::move(config.declared) },
    _runtimeConfig { std::move(config.runtime) },
    _services { services }
{
}

vtextension::ExtensionRuntime& DaemonExtensions::runtime()
{
    if (!_runtime)
    {
        daemonLog()("Starting the status-line extension runtime.");
        _runtime = std::make_unique<vtextension::ExtensionRuntime>(_runtimeConfig, _services.runtime);
    }
    return *_runtime;
}

std::unique_ptr<ISessionExtensions> DaemonExtensions::createSession(SessionExtensionEvents events,
                                                                    OwnerPost post)
{
    if (_declared.extensions.empty() || _stopped)
        return nullptr;
    return std::make_unique<DaemonSessionExtensions>(
        _declared, runtime().sessionServices(), std::move(events), std::move(post));
}

void DaemonExtensions::answer(std::string_view hostId, std::string_view digest, GrantState answer)
{
    // A dismissed prompt records nothing: the store keeps answers only, and Pending would forget one.
    if (answer == GrantState::Pending)
        return;
    runtime().grants().record(hostId, digest, storedGrantStateOf(answer));
}

GrantState DaemonExtensions::lookup(std::string_view hostId, std::string_view digest) const
{
    return _runtime ? wireGrantStateOf(_runtime->grants().lookup(hostId, digest)) : GrantState::Pending;
}

void DaemonExtensions::releaseRetired() noexcept
{
    if (_runtime)
        _runtime->releaseRetired();
}

void DaemonExtensions::stop() noexcept
{
    _stopped = true;
    // First, so that the joins below wait for no command: each run sees the request and ends.
    _services.commandShutdown.request_stop();
    if (_runtime)
        _runtime->stop();
}

} // namespace vthost
