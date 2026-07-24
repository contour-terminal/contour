// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `RoutingSessionFactory` — the app's always-installed session factory that
/// forwards to either the default (local-shell/SSH) factory or an attach-mode
/// delegate. The session manager holds ONE factory reference for its whole
/// life, so attach mode switches the route rather than the reference.

#include <contour/SessionFactory.h>

#include <memory>
#include <utility>

namespace contour
{

/// Delegating SessionFactory: attach mode installs a delegate, everything
/// else flows to the default factory.
class RoutingSessionFactory final: public SessionFactory
{
  public:
    /// @param defaultFactory The factory used while no delegate is set (owned).
    explicit RoutingSessionFactory(std::unique_ptr<SessionFactory> defaultFactory):
        _default(std::move(defaultFactory))
    {
    }

    /// Routes createPty/canCreateSession to @p delegate (not owned; nullptr
    /// restores the default factory).
    /// @note This setter exists as a documented rebinding seam: the session
    ///       factory reference is held for the app's lifetime, and attach mode
    ///       swaps in a delegate at runtime rather than replacing the factory
    ///       pointer.
    void setDelegate(SessionFactory* delegate) noexcept { _delegate = delegate; }

    [[nodiscard]] std::unique_ptr<vtpty::Pty> createPty(
        std::optional<std::string> cwd,
        std::optional<vtbackend::PageSize> pageSize = std::nullopt,
        std::optional<vtpty::Process::ExecInfo> commandOverride = std::nullopt,
        std::optional<std::string> profileName = std::nullopt) override
    {
        return active().createPty(
            std::move(cwd), pageSize, std::move(commandOverride), std::move(profileName));
    }

    [[nodiscard]] bool canCreateSession() const noexcept override { return active().canCreateSession(); }

    [[nodiscard]] bool requestRemoteTab() override { return active().requestRemoteTab(); }

    [[nodiscard]] bool requestRemoteSplit(vtpty::Pty const* actingPty, bool vertical) override
    {
        return active().requestRemoteSplit(actingPty, vertical);
    }

    [[nodiscard]] bool requestRemoteWindow() override { return active().requestRemoteWindow(); }

  private:
    [[nodiscard]] SessionFactory& active() const noexcept { return _delegate ? *_delegate : *_default; }

    std::unique_ptr<SessionFactory> _default;
    SessionFactory* _delegate = nullptr;
};

} // namespace contour
