// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace crispy
{

/// Reads variables from a process environment.
///
/// An interface per the project's dependency-injection principle: the environment is an ambient
/// global that `setenv()`/`putenv()` may mutate. Code that depends on it takes this by reference
/// rather than calling `getenv()` itself, so a test can supply its own answers instead of mutating
/// the environment of the test binary that is running it.
///
/// Implementations are thread safe.
class Environment
{
  public:
    Environment() = default;
    virtual ~Environment() = default;

    Environment(Environment const&) = delete;
    Environment& operator=(Environment const&) = delete;
    Environment(Environment&&) = delete;
    Environment& operator=(Environment&&) = delete;

    /// Reads a variable as the raw bytes the environment holds.
    ///
    /// Raw bytes rather than text decoded through a locale's 8-bit codec: what these name is most
    /// often a filesystem path, and a path is a byte string that has to survive intact. A codec
    /// that cannot represent some byte yields U+FFFD in its place, and the path then simply does
    /// not exist.
    ///
    /// @param name Name of the variable to look up.
    /// @return Its value, or std::nullopt if it is not set.
    [[nodiscard]] virtual std::optional<std::string> get(std::string_view name) const = 0;
};

/// The process's own environment, read afresh on every lookup.
///
/// The one production reader. It holds no state, so constructing one costs nothing and any
/// composition root that wants the environment as it stands right now -- `${VAR}` expansion when a
/// configuration file is loaded or reloaded, for instance -- simply makes its own rather than
/// reaching for a global.
///
/// Name resolution is the host's own: `GetEnvironmentVariableA()` on Windows, which matches
/// case-insensitively against the environment block the operating system itself synchronizes, and
/// a byte-wise scan of `environ` guarded by a process-wide mutex elsewhere. That mutex serializes
/// this class's readers against each other; it cannot protect them from a `setenv()` issued outside
/// it, which is why no first-party code may call `setenv()` -- see vtpty's InheritingEnvBlock, which
/// mutates the environment around CreateProcess() and uses the Win32 API to do it for exactly this
/// reason.
class LiveEnvironment final: public Environment
{
  public:
    /// @param name Name of the variable to look up.
    /// @return Its value as the environment holds it now, or std::nullopt if it is not set.
    [[nodiscard]] std::optional<std::string> get(std::string_view name) const override;
};

/// Remembers what another environment answered, so each name is read at most once.
///
/// A decorator rather than a second reader, so the caching is a decision a caller makes about a
/// source rather than a second flavour every call site has to choose between. What it buys is the
/// "frozen for the process's lifetime" semantics the tree's readers of HOME, PATH and USER have
/// always had: those name facts about the process that its own configuration is derived from, and a
/// value that changed halfway through would leave that configuration internally inconsistent.
///
/// Thread safe: every lookup, hit or miss, runs under this object's own mutex.
class CachingEnvironment final: public Environment
{
  public:
    /// @param source The environment to read a name from the first time it is asked for. It must
    ///               outlive this object.
    explicit CachingEnvironment(Environment const& source) noexcept;

    /// @param name Name of the variable to look up.
    /// @return What @c source answered the first time this name was asked for.
    [[nodiscard]] std::optional<std::string> get(std::string_view name) const override;

  private:
    Environment const& _source;
    mutable std::mutex _mutex;
    /// Holds the misses too -- a name that is unset is a fact worth remembering, and re-reading it
    /// would be the one case the cache never covered.
    mutable std::map<std::string, std::optional<std::string>, std::less<>> _cache;
};

/// The process-wide environment, cached on first read.
///
/// Composition-root scaffolding, for the free and static functions that have no constructor to take
/// a collaborator in -- `vtpty::Process::homeDirectory()`, `crispy::App`'s log-filter read, the
/// developer knobs. Anything with a lifetime takes a @c crispy::Environment reference at
/// construction instead; a test can then supply its own answers, which this cannot.
/// @return A reference to a function-local-static @c CachingEnvironment over a @c LiveEnvironment.
[[nodiscard]] Environment& defaultEnvironment();

} // namespace crispy
