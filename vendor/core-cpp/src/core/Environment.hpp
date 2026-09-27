// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace core
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
/// Name resolution is the host's own: `GetEnvironmentVariableW()` on Windows, which matches
/// case-insensitively against the environment block the operating system itself synchronizes --
/// the name and the value converted from and to UTF-8, so a value outside the ANSI code page reads
/// intact, and a name that is not UTF-8 reads as unset -- and
/// a byte-wise scan of `environ` guarded by a process-wide mutex elsewhere. That mutex serializes
/// this class's readers against each other; it cannot protect them from a `setenv()` issued outside
/// it, which is why no first-party code may call `setenv()`: a program that must change its
/// environment on Windows (around CreateProcess(), say) uses the Win32 API for exactly this reason.
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
/// a collaborator in. Anything with a lifetime takes a @c core::Environment reference at
/// construction instead; a test can then supply its own answers, which this cannot.
/// @return A reference to a function-local-static @c CachingEnvironment over a @c LiveEnvironment.
[[nodiscard]] Environment& defaultEnvironment();

/// Sets a variable in the process's own environment, where @c LiveEnvironment reads it and child
/// processes inherit it.
///
/// The one writer of the process environment, in place of `setenv()`, which is thread-unsafe and
/// which this project's clang-tidy configuration rejects. On POSIX it never edits a block a reader
/// may be walking: it builds a new block with the change applied and publishes it in `environ` with
/// a single store, under the lock @c LiveEnvironment reads under. Nothing it publishes is ever
/// freed, because a reader outside that lock -- `getenv()` in another library, `execvp()` -- may
/// still hold a block published earlier. Each call so costs one block of pointers, which suits the
/// rare writes a process makes to its own environment: a variable it exports to the children it
/// starts, a test fixture's setting. A write that changes nothing (the variable already reads
/// @p value) publishes nothing. On Windows it is `SetEnvironmentVariableW()`, over the name and
/// value converted from UTF-8, which the operating system synchronizes; the CRT's own copy of the
/// environment, which its `getenv()` reads, does not see it.
///
/// Not for use between `fork()` and `exec()`: it takes a lock and allocates, and in the child of a
/// multi-threaded process the lock may be held by a thread that no longer exists. Build the
/// child's environment before forking, or pass it to `execve()`.
///
/// @param name Name of the variable to set: not empty, and without '=' or NUL.
/// @param value The value it should read as, without NUL. An empty value sets the variable.
/// @return Nothing, or why the variable could not be set: @c std::errc::invalid_argument for a
///         name or value no environment can hold -- on Windows also one that is not UTF-8 -- or
///         the operating system's error on Windows.
[[nodiscard]] std::expected<void, std::error_code> setProcessEnvironmentVariable(std::string_view name,
                                                                                 std::string_view value);

/// Removes a variable from the process's own environment, with the guarantees of
/// @c setProcessEnvironmentVariable(). Removing a variable that is not set succeeds, and publishes
/// nothing.
///
/// Not for use between `fork()` and `exec()`, for the same reason as the setter: it takes a lock
/// and allocates, and in the child of a multi-threaded process the lock may be held by a thread
/// that no longer exists. Build the child's environment before forking, or pass it to `execve()`.
///
/// @param name Name of the variable to remove: not empty, and without '=' or NUL.
/// @return Nothing, or why the variable could not be removed: @c std::errc::invalid_argument for a
///         name no environment can hold, or the operating system's error on Windows.
[[nodiscard]] std::expected<void, std::error_code> unsetProcessEnvironmentVariable(std::string_view name);

} // namespace core
