// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <deque>

#include <orchestrator/core/Engine.hpp>

namespace orchestrator::proto
{

/// What the driver does next.
enum class NextStep : uint8_t
{
    Idle,      ///< Nothing: wait for the world.
    Feed,      ///< Feed the input take() returns.
    ListPanes, ///< List the panes now, reading the request time first, and push what was listed.
};

/// The driver's order of work: the inputs its effects produced, fed in the order they arose, and the pane
/// listing a Wake asks for.
///
/// A listing is taken only once every queued input was fed, not right after the Wake's own effects: an input
/// queued behind the Wake, such as the PaneOpenedFor of a tab opened while the Wake waited, would otherwise
/// change its run only after the listing was asked for, and the listing, which predates the tab, would tell
/// the run its tab is gone.
class StepQueue
{
  public:
    /// Queues an input behind the others.
    /// @param input What happened.
    void push(Input input);

    /// Notes that every effect of @p fed was carried out: a Wake asks for a listing.
    /// @param fed The input just fed.
    void carriedOut(Input const& fed);

    /// @return What to do next: feed while inputs are queued, then list the panes if a Wake asked for it.
    [[nodiscard]] NextStep next() const noexcept;

    /// Takes the next input; next() is NextStep::Feed.
    /// @return The oldest queued input.
    [[nodiscard]] Input take();

    /// Notes that the listing next() asked for was taken, whether or not it could be read.
    void listingTaken() noexcept { _listing = Listing::NotWanted; }

  private:
    /// Whether a Wake asked for a listing not yet taken.
    enum class Listing : uint8_t
    {
        NotWanted, ///< None is due.
        Wanted,    ///< A Wake was fed since the last listing.
    };

    std::deque<Input> _inputs;
    Listing _listing = Listing::NotWanted;
};

} // namespace orchestrator::proto
