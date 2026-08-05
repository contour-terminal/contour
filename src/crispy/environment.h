// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
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
/// The implementations differ in *when* they observe the environment — see @c snapshot_environment
/// and @c live_environment. Both are thread safe.
class environment
{
  public:
    environment() = default;
    virtual ~environment() = default;

    environment(environment const&) = delete;
    environment& operator=(environment const&) = delete;
    environment(environment&&) = delete;
    environment& operator=(environment&&) = delete;

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

/// Environment served from an immutable copy taken when this object is constructed.
///
/// Thread safe by construction: the copy is never written to, so concurrent readers need no lock
/// and cannot race a `getenv()` that another thread's `setenv()` is rewriting. The trade is
/// visibility — a variable changed after construction is not seen. Where that matters, use
/// @c live_environment.
class snapshot_environment final: public environment
{
  public:
    /// Copies the process environment.
    snapshot_environment();

    /// @param name Name of the variable to look up.
    /// @return Its value as of construction, or std::nullopt if it was not set.
    [[nodiscard]] std::optional<std::string> get(std::string_view name) const override;

  private:
    /// Orders variable names the way the host's own `getenv()` resolves them: byte-wise on POSIX,
    /// case-insensitively on Windows.
    ///
    /// Windows stores whatever casing the creating process used but matches names
    /// case-insensitively, so a byte-wise map would answer nullopt for a `LOCALAPPDATA` lookup
    /// against a block that spells it `LocalAppData` -- a regression against the `getenv()` this
    /// snapshot replaces.
    struct name_less
    {
        using is_transparent = void;

        /// @param a Left-hand name.
        /// @param b Right-hand name.
        /// @return Whether @p a orders before @p b.
        [[nodiscard]] bool operator()(std::string_view a, std::string_view b) const noexcept;
    };

    std::map<std::string, std::string, name_less> _entries;
};

/// Environment read afresh on every lookup, as the process holds it now.
///
/// For the callers where a variable may change during the process's lifetime and the current value
/// is the one that counts -- `${VAR}` expansion when a configuration file is loaded or reloaded,
/// for instance, which has to see the environment as it stands at that moment.
///
/// Thread safe: `GetEnvironmentVariableA()` on Windows, which reads the environment block the
/// operating system itself synchronizes, and a `getenv()` guarded by a process-wide mutex
/// elsewhere. That mutex serializes this class's own readers against each other; it cannot protect
/// them from a `setenv()` issued outside it, which is why no first-party code may call `setenv()`
/// -- see vtpty's InheritingEnvBlock, which mutates the environment around CreateProcess() and
/// uses the Win32 API to do it for exactly this reason.
class live_environment final: public environment
{
  public:
    /// @param name Name of the variable to look up.
    /// @return Its value as the environment holds it now, or std::nullopt if it is not set.
    [[nodiscard]] std::optional<std::string> get(std::string_view name) const override;
};

/// Process-wide @c snapshot_environment, for callers that legitimately want a default source
/// without threading an injection through every constructor (a default argument, typically).
/// Prefer explicit injection wherever a test needs to control what is read.
/// @return A reference to a function-local-static @c snapshot_environment.
[[nodiscard]] environment& defaultEnvironment();

/// Process-wide @c live_environment. @see defaultEnvironment for when a default is acceptable.
/// @return A reference to a function-local-static @c live_environment.
[[nodiscard]] environment& defaultLiveEnvironment() noexcept;

} // namespace crispy
