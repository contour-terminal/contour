// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/links/PathProbe.hpp>

#include <core/Utf8Path.hpp>
#include <core/async/DetachedTask.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/platform/FileSystem.hpp>

#include <algorithm>
#include <ranges>
#include <span>
#include <utility>

namespace vtbackend
{

namespace
{
    /// The cache key of @p candidate: its UTF-8 form. Not generic_string(), which on Windows narrows through
    /// the active code page and throws on a character it cannot map, here on the probe thread or under the
    /// terminal lock.
    [[nodiscard]] std::string keyOf(std::filesystem::path const& candidate)
    {
        return core::pathToUtf8(candidate);
    }

    /// Takes the executor as the ResumeOn that names it: a coroutine parameter must not be a reference.
    core::async::DetachedTask runProbe(core::async::ResumeOn on,
                                       std::shared_ptr<PathProbe> probe,
                                       PathQuery query,
                                       std::function<void(ResolvedPath)> done)
    {
        co_await on;
        done(probe->probe(query));
    }
} // namespace

PathProbe::PathProbe(core::platform::FileSystem const& fileSystem,
                     core::platform::IClock const& clock,
                     PathProbeLimits limits):
    _fileSystem { fileSystem },
    _clock { clock },
    _limits { limits },
    // LRUCache with capacity 0 has nothing to evict on its first insert, so the smallest usable cache is one
    // entry.
    _cache { std::max(limits.capacity, std::size_t { 1 }) }
{
}

std::optional<PathKind> PathProbe::remembered(std::string const& key) const
{
    auto const lock = std::scoped_lock { _mutex };
    auto const* entry = _cache.tryGet(key);
    if (!entry || _clock.now() - entry->checkedAt > _limits.maxAge)
        return std::nullopt;
    return entry->kind;
}

PathKind PathProbe::look(std::filesystem::path const& candidate) const
{
    // Outside the mutex: a hung mount stalls this thread only, and every other reader still gets the cache.
    auto const found = resolveLocalPath(_fileSystem, std::span { &candidate, 1 });
    return found ? found->kind : PathKind::Missing;
}

std::optional<ResolvedPath> PathProbe::cached(PathQuery const& query) const
{
    for (auto const index: std::views::iota(std::size_t { 0 }, query.candidates.size()))
    {
        auto const kind = remembered(keyOf(query.candidates[index]));
        if (!kind)
            return std::nullopt;
        if (*kind != PathKind::Missing)
            return ResolvedPath { .path = query.candidates[index], .kind = *kind, .candidateIndex = index };
    }
    return ResolvedPath { .path = {}, .kind = PathKind::Missing, .candidateIndex = 0 };
}

ResolvedPath PathProbe::probe(PathQuery const& query)
{
    for (auto const index: std::views::iota(std::size_t { 0 }, query.candidates.size()))
    {
        auto const& candidate = query.candidates[index];
        auto const key = keyOf(candidate);
        auto kind = remembered(key);
        if (!kind)
        {
            kind = look(candidate);
            auto const lock = std::scoped_lock { _mutex };
            auto const entry = Entry { .kind = *kind, .checkedAt = _clock.now() };
            if (auto* stored = _cache.tryGet(key))
                *stored = entry;
            else
                _cache.emplace(key, Entry { entry });
        }
        if (*kind != PathKind::Missing)
            return ResolvedPath { .path = candidate, .kind = *kind, .candidateIndex = index };
    }
    return ResolvedPath { .path = {}, .kind = PathKind::Missing, .candidateIndex = 0 };
}

void probeAsync(std::shared_ptr<PathProbe> probe,
                core::async::IExecutor& executor,
                PathQuery query,
                std::function<void(ResolvedPath)> done)
{
    (void) runProbe(core::async::ResumeOn { executor }, std::move(probe), std::move(query), std::move(done));
}

} // namespace vtbackend
