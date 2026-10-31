// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Live reload of the host's configuration: a hook the daemon polls on its loop, and the watcher that polls
/// it and hands each new profile catalog to the session host.

#include <core/net/DeadlineTimer.hpp>
#include <core/net/EventLoop.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include <vthost/ProfileCatalog.hpp>

namespace vthost
{

class SessionHost;

/// How a daemon learns that its configuration changed.
struct ConfigReloadHook
{
    /// Asked every @ref interval, on the daemon's loop: a new catalog when the configuration changed and
    /// loaded, else null. Must not block for long — it runs on the loop thread.
    std::function<std::shared_ptr<ProfileCatalog const>()> poll;
    /// How often @ref poll is asked.
    std::chrono::milliseconds interval { 2000 };
};

/// Asks a ConfigReloadHook every interval and hands what it yields to SessionHost::replaceProfiles, for as
/// long as it lives.
class ConfigReloadWatcher
{
  public:
    /// Arms the first poll, one interval from now.
    /// @param loop The loop the polls run on (not owned; outlives this).
    /// @param host The host the catalogs go to (not owned; outlives this).
    /// @param hook What is polled; a null @c poll polls nothing.
    ConfigReloadWatcher(core::net::EventLoop& loop, SessionHost& host, ConfigReloadHook hook);

    ConfigReloadWatcher(ConfigReloadWatcher const&) = delete;
    ConfigReloadWatcher(ConfigReloadWatcher&&) = delete;
    ConfigReloadWatcher& operator=(ConfigReloadWatcher const&) = delete;
    ConfigReloadWatcher& operator=(ConfigReloadWatcher&&) = delete;
    ~ConfigReloadWatcher() = default;

  private:
    void arm();

    static void fire(void* self);

    core::net::EventLoop& _loop;
    SessionHost& _host;
    ConfigReloadHook _hook;
    std::optional<core::net::DeadlineTimer> _timer;
};

} // namespace vthost
