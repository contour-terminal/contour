// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/links/LinkTypes.hpp>
#include <vtbackend/links/PathResolution.hpp>

#include <crispy/LRUCache.hpp>

#include <core/platform/Clock.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace core::async
{
class IExecutor;
}
namespace core::platform
{
class FileSystem;
}

namespace vtbackend
{

/// How much a PathProbe remembers, and for how long (§5.2).
struct PathProbeLimits
{
    std::size_t capacity = 8192; ///< Candidates remembered, least recently used first out; 0 is clamped to 1.
    std::chrono::seconds maxAge { 10 }; ///< After this an answer is looked up again, so new files appear.
};

/// Where a printed path may be: pathCandidates()' places, first to last.
struct PathQuery
{
    std::vector<std::filesystem::path> candidates;

    bool operator==(PathQuery const&) const = default;
};

/// Existence checks for detected paths, through an injected filesystem, with a bounded cache (§5.2).
///
/// Thread-safe. probe() blocks on the filesystem: detection and a click run it on the dedicated probe thread,
/// so that a hung mount stops path underlines and nothing else; hint mode, which the user starts, runs it at
/// once on the thread that holds the terminal lock (§12). cached() never touches the filesystem and may run
/// under the terminal lock.
///
/// The injected FileSystem and IClock are called from several threads at once (two probes may look up the
/// same candidate), so their const members must tolerate concurrent calls.
class PathProbe
{
  public:
    /// @param fileSystem Where to look; must outlive the probe.
    /// @param clock What ages answers; must outlive the probe.
    /// @param limits Capacity and age.
    PathProbe(core::platform::FileSystem const& fileSystem,
              core::platform::IClock const& clock,
              PathProbeLimits limits);

    /// What is known about @p query without looking.
    /// @return The first existing candidate, or a ResolvedPath of kind Missing when every candidate is known
    ///         not to exist; nullopt when a candidate before the answer is unknown or its answer is too old.
    ///         An empty query has no candidate that exists, so it answers Missing at once.
    [[nodiscard]] std::optional<ResolvedPath> cached(PathQuery const& query) const;

    /// Answers @p query, looking at the filesystem for whatever the cache does not know. Blocks.
    /// @return The first existing candidate; kind Missing when none exists. An empty query answers Missing
    ///         without looking.
    [[nodiscard]] ResolvedPath probe(PathQuery const& query);

  private:
    struct Entry
    {
        PathKind kind = PathKind::Missing;
        core::platform::SteadyTimePoint checkedAt {};
    };

    [[nodiscard]] std::optional<PathKind> remembered(std::string const& key) const;
    [[nodiscard]] PathKind look(std::filesystem::path const& candidate) const;

    core::platform::FileSystem const& _fileSystem;
    core::platform::IClock const& _clock;
    PathProbeLimits _limits;
    mutable std::mutex _mutex;
    mutable crispy::LRUCache<std::string, Entry> _cache;
};

/// Runs @p probe on @p executor and hands the answer to @p done there.
///
/// The job is not run on the calling thread. @p done runs on @p executor, must not throw (an exception
/// escaping it terminates the process), and is called at most once. If @p executor is destroyed with the job
/// still queued, the job is freed without calling @p done. A ThreadPoolExecutor drains its queue on
/// destruction, so @p done may run while that executor is being destroyed; the injected FileSystem and IClock
/// must therefore outlive it too. The shared_ptr keeps the probe alive until @p done has returned.
///
/// @param probe Kept alive by the job until @p done returns.
/// @param executor The probe thread.
/// @param query What to look for.
/// @param done Called on @p executor with the answer.
void probeAsync(std::shared_ptr<PathProbe> probe,
                core::async::IExecutor& executor,
                PathQuery query,
                std::function<void(ResolvedPath)> done);

} // namespace vtbackend
