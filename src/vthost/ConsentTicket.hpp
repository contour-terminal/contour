// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The only key to recording a daemon-scoped extension's grant.

#include <string>
#include <string_view>
#include <utility>

namespace vthost
{

class NativeSession;

/// Proof that a window's connection was sent a daemon-scoped extension's question: minted only by a
/// NativeSession as it sends an ExtensionPermissionRequest to a client sharing
/// proto::Capability::DaemonExtensionConsent, and spent on that connection's reply
/// (SessionHost::answerDaemonExtension()). Nothing else can make one, so no control verb can record an
/// answer: a grant completes only as a confirmation in a Contour window.
class ConsentTicket
{
  public:
    ConsentTicket(ConsentTicket const&) = delete;
    ConsentTicket& operator=(ConsentTicket const&) = delete;
    ConsentTicket(ConsentTicket&&) noexcept = default;
    ConsentTicket& operator=(ConsentTicket&&) noexcept = default;
    ~ConsentTicket() = default;

    /// @return What the daemon's configuration calls the extension asked about.
    [[nodiscard]] std::string const& name() const noexcept { return _name; }

    /// @return The grant digest the question was asked for.
    [[nodiscard]] std::string const& grantDigest() const noexcept { return _grantDigest; }

  private:
    friend class NativeSession;

    ConsentTicket(std::string name, std::string grantDigest):
        _name { std::move(name) }, _grantDigest { std::move(grantDigest) }
    {
    }

    std::string _name;
    std::string _grantDigest;
};

} // namespace vthost
