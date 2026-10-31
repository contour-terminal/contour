// SPDX-License-Identifier: Apache-2.0
#include <vthost/ConfigReload.hpp>

#include <vthost/SessionHost.hpp>

namespace vthost
{

ConfigReloadWatcher::ConfigReloadWatcher(core::net::EventLoop& loop,
                                         SessionHost& host,
                                         ConfigReloadHook hook):
    _loop { loop }, _host { host }, _hook { std::move(hook) }
{
    arm();
}

void ConfigReloadWatcher::arm()
{
    if (!_hook.poll)
        return;
    _timer.reset();
    _timer.emplace(_loop, _loop.clock().now() + _hook.interval, &ConfigReloadWatcher::fire, this);
}

void ConfigReloadWatcher::fire(void* self)
{
    auto& watcher = *static_cast<ConfigReloadWatcher*>(self);
    if (auto catalog = watcher._hook.poll())
        watcher._host.replaceProfiles(std::move(catalog));
    // Re-armed from inside its own callback, which DeadlineTimer allows: it is settled before it is
    // called.
    watcher.arm();
}

} // namespace vthost
