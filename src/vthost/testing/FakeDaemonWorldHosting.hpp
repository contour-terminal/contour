// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// An IDaemonWorldHosting that runs no WebAssembly: it records what its host does with it, in order, so that
/// how a host makes and stops its daemon-scoped extensions is testable in a build without extensions too.

#include <algorithm>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vthost/ExtensionHosting.hpp>

namespace vthost::testing
{

/// The daemon-scoped extensions of a host, as a test sees them.
class FakeDaemonWorldHosting final: public IDaemonWorldHosting
{
  public:
    /// @param journal Where every call is recorded, in order, shared with the test.
    /// @param post Where the extensions hand jobs to the host's loop.
    FakeDaemonWorldHosting(std::shared_ptr<std::vector<std::string>> journal, OwnerPost post):
        _journal { std::move(journal) }, _post { std::move(post) }
    {
    }

    void startOnDemand(std::string_view name) override { _journal->push_back(std::format("start {}", name)); }

    [[nodiscard]] std::vector<DaemonExtensionRequest> pendingRequests() const override { return pending; }

    [[nodiscard]] std::vector<DaemonExtensionListing> list() const override
    {
        auto listings = std::vector<DaemonExtensionListing> {};
        for (auto const& request: pending)
            listings.push_back(DaemonExtensionListing { .name = request.name,
                                                        .state = DaemonExtensionState::Pending,
                                                        .grantDigest = request.grantDigest,
                                                        .pending = request });
        listings.insert(listings.end(), others.begin(), others.end());
        return listings;
    }

    /// Records the answer, and closes the request it answers.
    void answer(std::string_view name, std::string_view grantDigest, GrantState answer) override
    {
        _journal->push_back(std::format("answer {} {} {}", name, grantDigest, std::to_underlying(answer)));
        std::erase_if(pending, [&](DaemonExtensionRequest const& request) {
            return request.name == name && request.grantDigest == grantDigest;
        });
    }

    void revoke(std::string_view name) override { _journal->push_back(std::format("revoke {}", name)); }

    void stop() noexcept override { _journal->emplace_back("stop"); }

    /// Hands the host's loop a job that records @p entry once it runs, as an extension's lane does.
    void post(std::string entry) const
    {
        _post([journal = _journal, entry = std::move(entry)] { journal->push_back(entry); });
    }

    /// The requests waiting for an answer, as a test set them.
    std::vector<DaemonExtensionRequest> pending;
    /// The extensions that wait for none, listed after the pending ones.
    std::vector<DaemonExtensionListing> others;

  private:
    std::shared_ptr<std::vector<std::string>> _journal;
    OwnerPost _post;
};

} // namespace vthost::testing
