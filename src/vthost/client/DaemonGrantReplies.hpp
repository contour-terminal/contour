// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where a client records its user's answers to a daemon's status-line extensions. Built only with
/// extensions (CONTOUR_EXTENSIONS), like vthost/DaemonExtensions.hpp.

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <vtextension/CommandPolicy.hpp>
#include <vtextension/ExtensionConsent.hpp>
#include <vthost/PermissionWire.hpp>
#include <vthost/proto/Pdu.hpp>

namespace vthost::client
{

/// @param request A daemon extension's question, as it arrived.
/// @return The same, as a consent prompt asks it: the manifest the daemon read, under the host id it
/// assigned; nothing for a question that asks for a control permission this build cannot name, which is
/// not asked, as its prompt could not say what it grants.
[[nodiscard]] std::optional<vtextension::ExtensionConsentRequest> consentRequestOf(
    proto::PermissionRequest const& request);

/// The vtextension::IGrantStore a client's consent prompt records a daemon's questions in: each answer is
/// sent to the daemon, which keeps it in its own grant store, and remembered here for as long as this lives,
/// so that a question answered in one pane is not asked again in another meanwhile.
///
/// Only the prompt reaches it -- no runner looks anything up here -- so it is confined to the thread the
/// prompt runs on, as the prompt is.
class DaemonGrantReplies final: public vtextension::IGrantStore
{
  public:
    /// Sends an answer to the daemon.
    using Reply = std::function<void(std::string_view hostId, std::string_view digest, GrantState answer)>;

    /// @param reply Sends each answer recorded to the daemon.
    explicit DaemonGrantReplies(Reply reply): _reply { std::move(reply) } {}

    [[nodiscard]] vtextension::GrantState lookup(std::string_view hostId,
                                                 std::string_view digest) const override;

    /// Sends the answer to the daemon, and remembers it. A prompt the user dismissed records nothing, so
    /// sends nothing: the question stays open on the daemon, which asks it of a client that attaches later,
    /// but not of this one again until it reattaches. (`Pending`, were it recorded, would close the question
    /// there, and forget what is remembered here.)
    void record(std::string_view hostId, std::string_view digest, vtextension::GrantState state) override;

  private:
    Reply _reply;
    std::map<std::pair<std::string, std::string>, vtextension::GrantState, std::less<>> _answers;
};

} // namespace vthost::client
