// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Image leases kept in memory, and the store configurations the image tests stage with.

#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <vthost/DaemonImage.hpp>

namespace vthost::testing
{

/// ImageLeases that record each held lease in a multiset of lease-file paths.
///
/// A lease's destructor erases its one entry; probe() and claim() answer from what is left, so a
/// claim succeeds only while no hold is recorded. The fake
/// must outlive the leases it hands out.
class FakeImageLeases final: public ImageLeases
{
  public:
    /// Runs at the start of every hold(), before the hold is recorded: what a test races a prune
    /// through, the window between a launcher asking for a lease and getting it.
    std::function<void(std::filesystem::path const&)> beforeHold;

    /// Runs at the start of every claim(), before it is decided: what a test races a hold through,
    /// the window between a prune listing an image and claiming it.
    std::function<void(std::filesystem::path const&)> beforeClaim;

    /// What every hold() fails with, when set.
    std::optional<StagingError> holdError;

    [[nodiscard]] std::expected<std::unique_ptr<ImageLease>, StagingError> hold(
        std::filesystem::path const& leaseFile) override
    {
        if (beforeHold)
            beforeHold(leaseFile);
        if (holdError)
            return std::unexpected(*holdError);
        return record(leaseFile);
    }

    [[nodiscard]] std::unique_ptr<ImageLease> claim(std::filesystem::path const& leaseFile) override
    {
        if (beforeClaim)
            beforeClaim(leaseFile);
        if (probe(leaseFile) == LeaseState::Held)
            return nullptr;
        return std::make_unique<ImageLease>();
    }

    [[nodiscard]] LeaseState probe(std::filesystem::path const& leaseFile) override
    {
        return holds(leaseFile) == 0 ? LeaseState::Free : LeaseState::Held;
    }

    [[nodiscard]] std::unique_ptr<ImageLease> adoptInherited(std::filesystem::path const& leaseFile) override
    {
        auto const entry = _inherited.find(leaseFile.lexically_normal());
        if (entry == _inherited.end())
            return nullptr;
        _inherited.erase(entry);
        ++adoptions;
        return record(leaseFile);
    }

    /// Makes this process one that inherited a lease on @p leaseFile across an exec, for
    /// adoptInherited() to find. Not counted as held until adopted.
    void inherit(std::filesystem::path const& leaseFile) { _inherited.insert(leaseFile.lexically_normal()); }

    /// @return How many leases on @p leaseFile are held.
    [[nodiscard]] std::size_t holds(std::filesystem::path const& leaseFile) const
    {
        return _held.count(leaseFile.lexically_normal());
    }

    /// @return How many leases are held, on any lease file.
    [[nodiscard]] std::size_t heldCount() const noexcept { return _held.size(); }

    /// How many times a lease was made to survive an exec (ImageLease::keepAcrossExec).
    std::size_t handOvers = 0;

    /// How many inherited leases adoptInherited() took over.
    std::size_t adoptions = 0;

  private:
    using Held = std::multiset<std::filesystem::path>;

    /// One recorded hold, erased with the lease.
    class Lease final: public ImageLease
    {
      public:
        Lease(FakeImageLeases& leases, Held::iterator entry): _leases { leases }, _entry { entry } {}
        Lease(Lease const&) = delete;
        Lease& operator=(Lease const&) = delete;
        Lease(Lease&&) = delete;
        Lease& operator=(Lease&&) = delete;
        ~Lease() override { _leases._held.erase(_entry); }

        [[nodiscard]] std::error_code keepAcrossExec() override
        {
            ++_leases.handOvers;
            return {};
        }

      private:
        FakeImageLeases& _leases;
        Held::iterator _entry;
    };

    /// @return A new recorded hold of @p leaseFile.
    [[nodiscard]] std::unique_ptr<ImageLease> record(std::filesystem::path const& leaseFile)
    {
        return std::make_unique<Lease>(*this, _held.insert(leaseFile.lexically_normal()));
    }

    Held _held;
    Held _inherited;
};

/// SymbolicLinks that record each copy instead of creating it.
///
/// InMemoryFileSystem holds links but offers no way to create one through FileSystem, so a test
/// checks what was asked for rather than what the file system holds.
class FakeSymbolicLinks final: public SymbolicLinks
{
  public:
    /// One recorded copy: the link that was read and where it was to be recreated.
    struct Copy
    {
        std::filesystem::path link;
        std::filesystem::path to;
        bool operator==(Copy const&) const = default;
    };

    [[nodiscard]] std::expected<void, std::string> copy(std::filesystem::path const& link,
                                                        std::filesystem::path const& to) const override
    {
        copies.push_back(Copy { .link = link, .to = to });
        return {};
    }

    /// Every copy asked for, in order.
    mutable std::vector<Copy> copies;
};

/// @return A store configuration for an AppImage on Linux: the Linux layout that stages.
/// @param imagesRoot Where the images live.
/// @param releaseVersion The release that names them.
[[nodiscard]] inline DaemonImageStoreConfig appImageStoreConfig(std::filesystem::path imagesRoot,
                                                                std::string releaseVersion)
{
    return DaemonImageStoreConfig { .imagesRoot = std::move(imagesRoot),
                                    .releaseVersion = std::move(releaseVersion),
                                    .inputs = { .platform = HostPlatform::Linux,
                                                .executable = "/tmp/.mount_x/usr/bin/contour",
                                                .appImage = "/home/u/Contour.AppImage" },
                                    .processId = 42 };
}

/// @return A store configuration for a packaged Linux install, `/usr/bin/contour`, which runs in place.
/// @param imagesRoot Where the images would live.
/// @param releaseVersion The release that would name them.
[[nodiscard]] inline DaemonImageStoreConfig linuxPackageStoreConfig(std::filesystem::path imagesRoot,
                                                                    std::string releaseVersion)
{
    return DaemonImageStoreConfig { .imagesRoot = std::move(imagesRoot),
                                    .releaseVersion = std::move(releaseVersion),
                                    .inputs = { .platform = HostPlatform::Linux,
                                                .executable = "/usr/bin/contour" },
                                    .processId = 42 };
}

} // namespace vthost::testing
