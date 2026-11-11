// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <filesystem>
#include <memory>

#include <vthost/DaemonImage.hpp>
#include <vthost/keeper/Ports.hpp>

namespace vthost::keeper
{

/// Restarts from the newest image of a daemon image store, holding the lease of a newer one it picked.
class StoreRestartImages final: public RestartImages
{
  public:
    /// @param store The store of the keeper's own executable (not owned; outlives this).
    explicit StoreRestartImages(DaemonImageStore& store);

    [[nodiscard]] std::filesystem::path pick(std::filesystem::path const& committed) override;

  private:
    DaemonImageStore& _store;
    std::unique_ptr<ImageLease> _lease; ///< The newer image picked last; none while the committed one is.
};

/// Restarts from the committed image, always: where no image store is known (an executable that cannot
/// locate itself, the test harness).
class CommittedRestartImage final: public RestartImages
{
  public:
    [[nodiscard]] std::filesystem::path pick(std::filesystem::path const& committed) override
    {
        return committed;
    }
};

} // namespace vthost::keeper
