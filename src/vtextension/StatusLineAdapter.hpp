// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/screen/StatusLineExtensions.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <vtextension/Event.hpp>
#include <vtextension/ExtensionHost.hpp>

namespace vtextension
{

/// @param event An event as the terminal reports it.
/// @return The same event as the statusline world carries it.
[[nodiscard]] Event toEvent(vtbackend::StatusLineEvent event);

/// @param kinds Event kinds, as the extensions subscribe to them.
/// @return Those of @p kinds a terminal reports; the always-delivered ones are not among them.
[[nodiscard]] vtbackend::StatusLineEventKinds toStatusLineEventKinds(EventKinds kinds) noexcept;

/// @param sink The terminal's sink, which it attaches itself to and revokes before it is destroyed; must
///             outlive every runner.
/// @return What RunnerServices::onSnapshotPublished is to be: tells the terminal through @p sink, so a
///         frame published while the terminal is torn down reaches nobody rather than a dead terminal.
[[nodiscard]] std::function<void()> publisherFor(vtbackend::StatusLinePublishSink& sink);

/// A pane's extensions as its terminal sees them: the vtbackend::IStatusLineExtensions over an
/// ExtensionHost. Adds nothing but the translation; every call is the host's, and as non-blocking.
class StatusLineAdapter final: public vtbackend::IStatusLineExtensions
{
  public:
    /// @param host The pane's extensions; must outlive the adapter.
    explicit StatusLineAdapter(ExtensionHost& host) noexcept: _host { host } {}

    [[nodiscard]] vtbackend::StatusLineEventKinds subscriptions() const noexcept override;
    void post(vtbackend::StatusLineEvent event) noexcept override;
    void resize(vtbackend::PageSize statusLineSize) noexcept override;
    [[nodiscard]] std::shared_ptr<vtbackend::StatusLineExtensionSnapshot const> snapshot(
        std::string_view name) const noexcept override;
    [[nodiscard]] std::optional<std::string> takeOverName() const noexcept override;

  private:
    ExtensionHost& _host;
};

} // namespace vtextension
