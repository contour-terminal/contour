// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// An IDaemonWorldHosting that runs no WebAssembly: it records what its host does with it, in order, so that
/// how a host makes and stops its daemon-scoped extensions is testable in a build without extensions too.

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

    void stop() noexcept override { _journal->emplace_back("stop"); }

    /// Hands the host's loop a job that records @p entry once it runs, as an extension's lane does.
    void post(std::string entry) const
    {
        _post([journal = _journal, entry = std::move(entry)] { journal->push_back(entry); });
    }

  private:
    std::shared_ptr<std::vector<std::string>> _journal;
    OwnerPost _post;
};

} // namespace vthost::testing
