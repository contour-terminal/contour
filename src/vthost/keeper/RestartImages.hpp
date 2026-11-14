// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
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
    /// @param installedExecutable The installed executable, which a live upgrade of an install that runs in
    ///        place execs (a package upgrade replaced it where it lies); empty for any other install.
    StoreRestartImages(DaemonImageStore& store, std::filesystem::path installedExecutable);

    [[nodiscard]] std::filesystem::path pick(std::filesystem::path const& committed) override;

    /// Holds an image of the store by its lease; for an install that runs in place, its installed executable,
    /// which no lease guards.
    [[nodiscard]] std::expected<std::unique_ptr<HeldImage>, RestartImageRefusal> hold(
        std::filesystem::path const& executable) override;

  private:
    DaemonImageStore& _store;
    std::filesystem::path _installedExecutable;
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

    /// Holds nothing: with no store, no path a daemon names can be told to be an image.
    [[nodiscard]] std::expected<std::unique_ptr<HeldImage>, RestartImageRefusal> hold(
        std::filesystem::path const& /*executable*/) override
    {
        return std::unexpected { RestartImageRefusal::NotAnImage };
    }
};

} // namespace vthost::keeper
