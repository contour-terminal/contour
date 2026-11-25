// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/Clock.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <vtextension/CompiledExtension.hpp>
#include <vtextension/Engine.hpp>
#include <vtextension/ExtensionError.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/HostBreaker.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension
{

struct ExtensionSpec;

/// The world an extension speaks: which host functions and exports its module is checked against.
enum class ExtensionWorld : uint8_t
{
    StatusLine, ///< The statusline world.
};

/// What names one loaded extension in a process: two panes whose keys are equal share one
/// CompiledExtension, and so one host and one breaker.
struct RegistryKey
{
    std::string hostId;       ///< The installed extension's host id, which grants are keyed on.
    ExtensionWorld world {};  ///< The world it speaks.
    std::string configDigest; ///< SHA-256, in hex, of its effective configuration.
    std::string moduleDigest; ///< SHA-256, in hex, of its module's bytes.

    bool operator==(RegistryKey const&) const = default;
};

/// @param spec One configured extension, its module bytes still in it.
/// @param world The world it speaks.
/// @return Its key: its host id, @p world, and SHA-256 digests over its effective configuration (what it
///         may run, then its settings sorted by name, each field length-prefixed) and over its module.
[[nodiscard]] RegistryKey registryKeyOf(ExtensionSpec const& spec, ExtensionWorld world);

/// @param key An extension's key.
/// @return Which host its module runs in: named by everything of @p key but the module, so that a changed
///         module of the same extension reaches the same host.
[[nodiscard]] wasm::ModuleIdentity moduleIdentityOf(RegistryKey const& key);

/// The extensions loaded in one hosting process, each once, however many panes run it: what makes a
/// configuration reload that changes nothing of an extension reuse its module and its host.
///
/// It keeps no extension alive: an entry is a weak reference, and the last pane dropping its
/// CompiledExtension releases it (on that pane's owner thread). Each entry carries the HostBreaker of its
/// extension, which every instance of it reports its host's deaths to.
///
/// Thread-safe: modules are acquired on the build thread, and breakers reset on the owner's.
class ExtensionRegistry
{
  public:
    /// @param clock What the breakers measure their fault window by; must outlive every entry's module.
    /// @param limits Where each breaker's `faultsBeforeDisable` and `faultWindow` come from.
    ExtensionRegistry(core::platform::IClock& clock, ExtensionLimits limits);

    /// The live entry for @p key on @p engine, or loads one (once, however many ask at once). An entry loaded
    /// on another engine is none of @p engine's.
    ///
    /// Called on a thread prepared for @p engine's backend. A load runs outside the registry's lock; a
    /// caller asking for an entry still loading waits for that load and gets its outcome.
    /// @param engine What the module is loaded on.
    /// @param key What names the extension.
    /// @param bytes The module's bytes; read only if it is to be loaded.
    /// @return The loaded extension, or why it could not be loaded (CompiledExtension::load()).
    [[nodiscard]] std::expected<std::shared_ptr<CompiledExtension>, ExtensionError> acquire(
        Engine& engine, RegistryKey const& key, std::vector<std::byte> bytes);

    /// Closes every breaker, of reused and replaced entries alike: the configuration was reloaded, which
    /// lifts what a host that kept dying was disabled for.
    void resetBreakers() noexcept;

    /// @return How many entries are loading or alive.
    [[nodiscard]] std::size_t liveEntries() const;

    /// Test hook.
    /// @return How many acquire() calls are waiting for a load another call runs.
    [[nodiscard]] std::size_t waitingAcquires() const noexcept { return _waiting.load(); }

  private:
    using LoadResult = std::expected<std::shared_ptr<CompiledExtension>, ExtensionError>;

    /// One extension on one engine: being loaded, or loaded and referenced by panes.
    struct Entry
    {
        RegistryKey key;
        Engine const* engine;                 ///< Compared only: the entry's module keeps it alive.
        uint64_t serial;                      ///< Tells the entry apart from a later one of the same key.
        std::shared_ptr<HostBreaker> breaker; ///< The extension's, shared with its module.
        /// What callers wait for while it loads; then what the panes hold.
        std::variant<std::shared_future<LoadResult>, std::weak_ptr<CompiledExtension>> state;
    };

    /// @return Whether @p entry is loaded and no pane holds it any more.
    [[nodiscard]] static bool isExpired(Entry const& entry) noexcept;

    /// Publishes how the load of the entry @p serial went: alive on success, gone otherwise. Locks.
    void settle(uint64_t serial, LoadResult const& result) noexcept;

    core::platform::IClock& _clock;
    ExtensionLimits _limits;
    mutable std::mutex _mutex;
    std::vector<Entry> _entries;           ///< Guarded by _mutex.
    uint64_t _nextSerial = 0;              ///< Guarded by _mutex.
    std::atomic<std::size_t> _waiting = 0; ///< How many acquire() calls wait for another's load.
};

} // namespace vtextension
