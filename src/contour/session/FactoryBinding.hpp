// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/session/SessionFactory.hpp>
#include <contour/session/WindowBinding.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace contour::session
{

/// A window binding that is a factory and a label and nothing else: what the windows of a host that keeps
/// no state of its own per window are bound to (a tmux attach), and what a test binds a window to in place
/// of a daemon link.
class FactoryBinding final: public WindowBinding
{
  public:
    /// @param factory What the bound windows' sessions are made by. Borrowed: it must outlive the binding.
    /// @param hostLabel What hostLabel() reports.
    explicit FactoryBinding(SessionFactory& factory, std::string hostLabel = {}):
        _factory(factory), _hostLabel(std::move(hostLabel))
    {
    }

    [[nodiscard]] SessionFactory& sessions() noexcept override { return _factory; }
    [[nodiscard]] std::string_view hostLabel() const noexcept override { return _hostLabel; }
    /// @return None: a factory has no daemon whose release could drift.
    [[nodiscard]] window::DaemonDriftModel* daemonDrift() noexcept override { return nullptr; }
    /// Nothing to do: a factory shows nothing that outlives the window's sessions.
    void windowClosed(vtworkspace::WorkspaceId /*window*/) override {}
    /// Nothing to do: the factory serves whatever window comes next.
    void lastWindowReleased() override {}

  private:
    SessionFactory& _factory;
    std::string _hostLabel;
};

} // namespace contour::session
