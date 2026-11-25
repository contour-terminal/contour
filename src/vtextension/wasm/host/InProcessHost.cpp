// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/host/InProcessHost.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <ranges>
#include <thread>
#include <utility>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/remote/HostConnector.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>

namespace vtextension::wasm::host
{

using remote::transport::Channel;
using remote::transport::makeChannelPair;

/// The hosts of one connector that are alive: each registers itself while it lives.
class InProcessHostRegistry
{
  public:
    /// Lists @p host, alive from now on.
    void add(InProcessHost& host)
    {
        auto const lock = std::scoped_lock { _mutex };
        _hosts.push_back(&host);
    }

    /// Unlists @p host, about to be destroyed.
    void remove(InProcessHost& host) noexcept
    {
        auto const lock = std::scoped_lock { _mutex };
        std::erase(_hosts, &host);
    }

    /// Calls @p visit with every host alive, under the registry's lock: a host is not destroyed meanwhile.
    template <typename Visit>
    void forEach(Visit const& visit) const
    {
        auto const lock = std::scoped_lock { _mutex };
        for (auto* host: _hosts)
            visit(*host);
    }

  private:
    mutable std::mutex _mutex;
    std::vector<InProcessHost*> _hosts;
};

InProcessHost::InProcessHost(std::shared_ptr<InProcessHostRegistry> registry,
                             std::unique_ptr<IHostedEngine> engine,
                             std::vector<Channel> lanes,
                             Channel control,
                             HostServerConfig config):
    _registry { std::move(registry) },
    _engine { std::move(engine) },
    _server { *_engine, std::move(lanes), std::move(control), std::move(config) },
    _thread { [this] {
        std::ignore = _server.run();
        _state.store(ServingState::Ended);
        // Last: onDeath may drop the last reference to this host (~InProcessHost()).
        if (auto const onDeath = runReturned())
            onDeath();
    } }
{
    _registry->add(*this);
}

InProcessHost::~InProcessHost()
{
    _registry->remove(*this);
    terminate();
    if (_thread.get_id() == std::this_thread::get_id())
        _thread.detach(); // in its onDeath, which touches nothing of this host once it returns
    else
        _thread.join();
}

void InProcessHost::interrupt(uint32_t instanceId) noexcept
{
    _server.interrupt(instanceId);
}

void InProcessHost::terminate() noexcept
{
    _state.store(ServingState::Ended);
    _server.stop();
}

remote::StepWatch InProcessHost::watch(remote::HostStep /*step*/, std::chrono::milliseconds /*budget*/)
{
    return remote::StepWatch {};
}

void InProcessHost::attach(remote::HostAttachment attachment)
{
    {
        auto const lock = std::scoped_lock { _attachmentMutex };
        if (_run == RunState::Running)
        {
            _onDeath = std::move(attachment.onDeath);
            return;
        }
    }
    if (attachment.onDeath)
        attachment.onDeath();
}

std::function<void()> InProcessHost::runReturned()
{
    auto const lock = std::scoped_lock { _attachmentMutex };
    _run = RunState::Returned;
    return std::exchange(_onDeath, nullptr);
}

InProcessHost::ServingState InProcessHost::state() const noexcept
{
    return _state.load();
}

InProcessHostConnector::InProcessHostConnector(EngineFactory engines, HostServerConfig config):
    _engines { std::move(engines) },
    _config { std::move(config) },
    _registry { std::make_shared<InProcessHostRegistry>() },
    _connects { 0 }
{
}

std::expected<remote::HostConnection, ExtensionError> InProcessHostConnector::connect()
{
    auto engine = _engines();
    if (!engine)
        return std::unexpected(ExtensionError::HostUnavailable);
    auto control = makeChannelPair();
    if (!control)
        return std::unexpected(ExtensionError::HostUnavailable);
    auto parentLanes = std::vector<Channel> {};
    auto helperLanes = std::vector<Channel> {};
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, remote::MaxLaneChannels))
    {
        auto lane = makeChannelPair();
        if (!lane)
            return std::unexpected(ExtensionError::HostUnavailable);
        parentLanes.push_back(std::move(lane->parent));
        helperLanes.push_back(std::move(lane->helper));
    }
    auto handle = std::make_unique<InProcessHost>(
        _registry, std::move(engine), std::move(helperLanes), std::move(control->helper), _config);
    _connects.fetch_add(1);
    return remote::HostConnection { .control = std::move(control->parent),
                                    .lanes = std::move(parentLanes),
                                    .handle = std::move(handle) };
}

std::size_t InProcessHostConnector::connects() const noexcept
{
    return _connects.load();
}

std::size_t InProcessHostConnector::liveHosts() const noexcept
{
    auto count = std::size_t { 0 };
    _registry->forEach([&](InProcessHost const& host) {
        if (host.state() == InProcessHost::ServingState::Serving)
            ++count;
    });
    return count;
}

std::size_t InProcessHostConnector::terminateAll() noexcept
{
    auto count = std::size_t { 0 };
    _registry->forEach([&](InProcessHost& host) {
        if (host.state() == InProcessHost::ServingState::Serving)
        {
            host.terminate();
            ++count;
        }
    });
    return count;
}

} // namespace vtextension::wasm::host
