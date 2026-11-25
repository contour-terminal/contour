// SPDX-License-Identifier: Apache-2.0
#include <vtextension/ExtensionRegistry.hpp>

#include <core/Sha256.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <format>
#include <ranges>
#include <span>
#include <utility>

#include <vtextension/ExtensionHost.hpp>

namespace vtextension
{

namespace
{
    /// One world, and the name a module identity spells it with.
    struct WorldName
    {
        ExtensionWorld world;
        std::string_view name;
    };

    /// Every world, by name.
    constexpr auto WorldNames = std::array {
        WorldName { .world = ExtensionWorld::StatusLine, .name = "statusline" },
    };

    /// @return What a module identity calls @p world.
    [[nodiscard]] std::string_view nameOf(ExtensionWorld world) noexcept
    {
        auto const row = std::ranges::find(WorldNames, world, &WorldName::world);
        return row != WorldNames.end() ? row->name : std::string_view { "unknown" };
    }

    /// Absorbs @p field into @p hasher after its length, as eight little-endian bytes: no field's bytes can
    /// be read as its neighbour's.
    void absorbField(core::Sha256& hasher, std::string_view field)
    {
        auto length = std::array<std::byte, 8> {};
        auto remaining = static_cast<uint64_t>(field.size());
        for (auto& byte: length)
        {
            byte = static_cast<std::byte>(remaining & 0xFFU);
            remaining >>= 8U;
        }
        hasher.update(std::span<std::byte const> { length });
        hasher.update(field);
    }

    /// @return The digest, in hex, of what makes @p spec's configuration: what it may run, then its settings
    ///         sorted by name (settings of one name in the user's order).
    [[nodiscard]] std::string configDigestOf(ExtensionSpec const& spec)
    {
        auto settings = spec.config;
        std::ranges::stable_sort(settings, {}, &ConfigMap::value_type::first);
        auto hasher = core::Sha256 {};
        auto const runCommand = static_cast<char>(std::to_underlying(spec.runCommand));
        absorbField(hasher, std::string_view { &runCommand, 1 });
        for (auto const& [name, value]: settings)
        {
            absorbField(hasher, name);
            absorbField(hasher, value);
        }
        return core::toLowerHexString(hasher.finish());
    }
} // namespace

RegistryKey registryKeyOf(ExtensionSpec const& spec, ExtensionWorld world)
{
    return RegistryKey { .hostId = spec.hostId,
                         .world = world,
                         .configDigest = configDigestOf(spec),
                         .moduleDigest = core::toLowerHexString(core::Sha256::hash(spec.module)) };
}

wasm::ModuleIdentity moduleIdentityOf(RegistryKey const& key)
{
    // The host id last: the world's name and the digest hold no newline, so nothing of it can be taken for
    // them.
    return wasm::ModuleIdentity { .key = std::format(
                                      "{}\n{}\n{}", nameOf(key.world), key.configDigest, key.hostId) };
}

ExtensionRegistry::ExtensionRegistry(core::platform::IClock& clock, ExtensionLimits limits):
    _clock { clock }, _limits { limits }
{
}

bool ExtensionRegistry::isExpired(Entry const& entry) noexcept
{
    auto const* const live = std::get_if<std::weak_ptr<CompiledExtension>>(&entry.state);
    return live != nullptr && live->expired();
}

std::expected<std::shared_ptr<CompiledExtension>, ExtensionError> ExtensionRegistry::acquire(
    Engine& engine, RegistryKey const& key, std::vector<std::byte> bytes)
{
    auto loaded = std::promise<LoadResult> {};
    auto breaker = std::shared_ptr<HostBreaker> {};
    auto serial = uint64_t { 0 };
    {
        auto lock = std::unique_lock { _mutex };
        std::erase_if(_entries, isExpired);
        auto const entry = std::ranges::find_if(_entries, [&](Entry const& candidate) {
            return candidate.engine == &engine && candidate.key == key;
        });
        if (entry != _entries.end())
        {
            if (auto const* const loading = std::get_if<std::shared_future<LoadResult>>(&entry->state))
            {
                // Waited for outside the lock, which the load takes to publish its outcome.
                auto const pending = *loading;
                lock.unlock();
                return pending.get();
            }
            // Lost its last pane since the sweep above, on another thread: a new entry takes its place.
            if (auto live = std::get<std::weak_ptr<CompiledExtension>>(entry->state).lock())
                return live;
            _entries.erase(entry);
        }
        serial = _nextSerial++;
        breaker = std::make_shared<HostBreaker>(_clock, _limits.faultsBeforeDisable, _limits.faultWindow);
        _entries.push_back(Entry { .key = key,
                                   .engine = &engine,
                                   .serial = serial,
                                   .breaker = breaker,
                                   .state = loaded.get_future().share() });
    }

    auto result = LoadResult {};
    try
    {
        result = CompiledExtension::load(engine, moduleIdentityOf(key), std::move(bytes), std::move(breaker));
    }
    catch (...)
    {
        settle(serial, std::unexpected(ExtensionError::InvalidComponent));
        loaded.set_exception(std::current_exception());
        throw;
    }
    settle(serial, result);
    loaded.set_value(result);
    return result;
}

void ExtensionRegistry::settle(uint64_t serial, LoadResult const& result) noexcept
{
    auto const lock = std::scoped_lock { _mutex };
    auto const entry = std::ranges::find(_entries, serial, &Entry::serial);
    if (entry == _entries.end())
        return;
    if (result)
        entry->state = std::weak_ptr<CompiledExtension> { *result };
    else
        _entries.erase(entry); // the next acquire tries again
}

void ExtensionRegistry::resetBreakers() noexcept
{
    auto const lock = std::scoped_lock { _mutex };
    for (auto const& entry: _entries)
    {
        auto& breaker = *entry.breaker; // the breaker's reset(), not the pointer's
        breaker.reset();
    }
}

std::size_t ExtensionRegistry::liveEntries() const
{
    auto const lock = std::scoped_lock { _mutex };
    return static_cast<std::size_t>(
        std::ranges::count_if(_entries, [](Entry const& entry) { return !isExpired(entry); }));
}

} // namespace vtextension
