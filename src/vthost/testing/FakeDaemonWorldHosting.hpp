// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// An IDaemonWorldHosting that runs no WebAssembly: it records what its host does with it, in order, so that
/// how a host makes and stops its daemon-scoped extensions is testable in a build without extensions too.

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
    /// @param events What the host hears from them.
    FakeDaemonWorldHosting(std::shared_ptr<std::vector<std::string>> journal,
                           OwnerPost post,
                           DaemonWorldEvents events = {}):
        _journal { std::move(journal) }, _post { std::move(post) }, _events { std::move(events) }
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
                                                        .pending = request,
                                                        .problem = {} });
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

    [[nodiscard]] std::optional<DaemonExtensionStatus> status(std::string_view name) const override
    {
        auto const found = statuses.find(name);
        if (found == statuses.end())
            return std::nullopt;
        return found->second;
    }

    [[nodiscard]] vtbackend::IDaemonExtensionSummaries const& summaries() const noexcept override
    {
        return _summaries;
    }

    [[nodiscard]] std::vector<ExtensionActionRows> actionRows() const override { return rows; }

    void hint(std::string_view name, ServedHint hint) override
    {
        // Spelled by ServedHint's value, so a hint added there is a row here.
        static constexpr auto HintNames = std::array<std::string_view, 3> { "tabs", "actions", "panes" };
        static_assert(HintNames.size() == std::to_underlying(ServedHint::Panes) + 1U);
        _journal->push_back(std::format("hint {} {}", name, HintNames.at(std::to_underlying(hint))));
    }

    void restart(std::string_view name) override { _journal->push_back(std::format("restart {}", name)); }

    void stop() noexcept override { _journal->emplace_back("stop"); }

    /// Sets what actionRows() answers, and tells the host, as a phase change on an extension's lane does.
    void changePresence(std::vector<ExtensionActionRows> changed)
    {
        rows = std::move(changed);
        if (_events.presenceChanged)
            _events.presenceChanged();
    }

    /// Publishes @p text as @p name's one-line summary, and tells the host, as an extension's lane does.
    void publishSummary(std::string const& name, std::string text)
    {
        {
            auto const lock = std::scoped_lock { _summaries.mutex };
            _summaries.texts.insert_or_assign(name, std::move(text));
        }
        if (_events.summaryChanged)
            _events.summaryChanged();
    }

    /// Hands the host's loop a job that records @p entry once it runs, as an extension's lane does.
    void post(std::string entry) const
    {
        _post([journal = _journal, entry = std::move(entry)] { journal->push_back(entry); });
    }

    /// The requests waiting for an answer, as a test set them.
    std::vector<DaemonExtensionRequest> pending;
    /// The extensions that wait for none, listed after the pending ones.
    std::vector<DaemonExtensionListing> others;
    /// What actionRows() answers.
    std::vector<ExtensionActionRows> rows;
    /// What status() answers, by name; a name not here does not compose.
    std::map<std::string, DaemonExtensionStatus, std::less<>> statuses;

  private:
    /// The summaries publishSummary() set.
    class Summaries final: public vtbackend::IDaemonExtensionSummaries
    {
      public:
        [[nodiscard]] std::string summaryOf(std::string_view name) const override
        {
            auto const lock = std::scoped_lock { mutex };
            auto const found = texts.find(name);
            return found != texts.end() ? found->second : std::string {};
        }

        mutable std::mutex mutex;
        std::map<std::string, std::string, std::less<>> texts; ///< Guarded by @ref mutex.
    };

    std::shared_ptr<std::vector<std::string>> _journal;
    OwnerPost _post;
    DaemonWorldEvents _events;
    Summaries _summaries;
};

} // namespace vthost::testing
