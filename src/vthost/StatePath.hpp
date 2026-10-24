// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Derivation of Contour's per-user state directory: what outlives a daemon, as against the runtime
/// directory its sockets live in (@see SocketPath.hpp). It holds the daemon images
/// (@see DaemonImage.hpp) and, from phase 6 on, `sessions/<label>.snapshot`.

#include <core/Environment.hpp>
#include <core/Utf8Path.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef _WIN32
    #include <unistd.h>
#endif

namespace vthost
{

/// The operating system whose conventions a path is derived by.
///
/// A value rather than an `#if`, so every platform's rule is decided by portable code and tested on
/// every host. NativeHostPlatform is the one place the build's own platform enters.
enum class HostPlatform : uint8_t
{
    Linux = 0,
    MacOS,
    Windows,
    OtherPosix, ///< A POSIX system that is neither Linux nor macOS (the BSDs, for instance).
};

/// The platform this build runs on.
#ifdef _WIN32
inline constexpr HostPlatform NativeHostPlatform = HostPlatform::Windows;
#elifdef __APPLE__
inline constexpr HostPlatform NativeHostPlatform = HostPlatform::MacOS;
#elifdef __linux__
inline constexpr HostPlatform NativeHostPlatform = HostPlatform::Linux;
#else
inline constexpr HostPlatform NativeHostPlatform = HostPlatform::OtherPosix;
#endif

/// Whether @p path is absolute by @p platform's rule, whatever the host's is.
///
/// Judged from the text rather than by std::filesystem::path::is_absolute(), which applies the host's
/// rule: on Windows `/x` is not absolute, and on POSIX `C:\x` is a relative file name.
/// @param platform The platform whose rule applies.
/// @param path The path, as an environment variable spells it.
/// @return For POSIX, whether it starts with `/`; for Windows, whether it starts with a drive and a
///         separator (`X:\` or `X:/`) or with `\\` (a UNC or device path).
[[nodiscard]] constexpr bool isAbsoluteOn(HostPlatform platform, std::string_view path) noexcept
{
    if (platform != HostPlatform::Windows)
        return path.starts_with('/');
    auto const isSeparator = [](char ch) {
        return ch == '\\' || ch == '/';
    };
    auto const isLetter = [](char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
    };
    if (path.starts_with(R"(\\)"))
        return true;
    return path.size() >= 3 && isLetter(path[0]) && path[1] == ':' && isSeparator(path[2]);
}

/// The inputs the state directory is derived from.
///
/// Every field defaults, so a call site names only what it varies (@see MuxSocketPathInputs on why
/// that matters under -Wmissing-designated-field-initializers).
struct DaemonStateDirectoryInputs
{
    /// Whose conventions apply.
    HostPlatform platform = NativeHostPlatform;

    /// The value of `$XDG_STATE_HOME`, if set. Honoured on every POSIX system, macOS included.
    std::optional<std::string_view> xdgStateHome {};

    /// The value of `%LOCALAPPDATA%`, if set. Windows only.
    std::optional<std::string_view> localAppData {};

    /// The user's home directory (`$HOME`), or empty when unknown. POSIX only.
    std::string_view home {};

    /// What identifies this user in the fallback directory's name: the numeric uid on POSIX,
    /// `%USERNAME%` on Windows (as SocketPath.hpp's fallback names it).
    std::string_view user {};
};

/// Where a state directory came from.
///
/// The temporary fallback is told apart because it is not private: the temp directory is shared by
/// every local user, any of whom can pre-create `<temp>/contour-state-<user>` before this user does
/// and put into it what this user would then trust. Nothing is executed out of such a directory
/// (@see DaemonImageStoreConfig::stateOrigin), and whatever else is kept there must assume it may
/// have been planted.
enum class StateDirectoryOrigin : uint8_t
{
    Configured = 0,    ///< The platform's own per-user directory, under the user's home or profile.
    TemporaryFallback, ///< `<temp>/contour-state-<user>`: nothing above was usable.
};

namespace detail
{
    /// @return The platform's per-user state directory, or none when no input is usable.
    [[nodiscard]] inline std::optional<std::filesystem::path> configuredStateDirectory(
        DaemonStateDirectoryInputs const& inputs)
    {
        auto const fromUtf8 = [](std::string_view utf8) {
            return core::pathFromUtf8(utf8).value_or(std::filesystem::path {});
        };
        auto const usable = [&](std::optional<std::string_view> value) {
            return value && isAbsoluteOn(inputs.platform, *value);
        };

        if (inputs.platform == HostPlatform::Windows)
        {
            if (usable(inputs.localAppData))
                return fromUtf8(*inputs.localAppData) / "contour";
        }
        else
        {
            if (usable(inputs.xdgStateHome))
                return fromUtf8(*inputs.xdgStateHome) / "contour";
            if (usable(inputs.home))
                return fromUtf8(inputs.home) / ".local" / "state" / "contour";
        }
        return std::nullopt;
    }

    /// Calls @p derive with the inputs @p env holds for this build's platform.
    ///
    /// A continuation rather than a returned DaemonStateDirectoryInputs, because the inputs view the
    /// strings read here rather than owning them.
    template <typename Derive>
    [[nodiscard]] auto withStateDirectoryInputs(core::Environment const& env, Derive derive)
    {
        auto const xdgStateHome = env.get("XDG_STATE_HOME");
        auto const localAppData = env.get("LOCALAPPDATA");
        auto const home = env.get("HOME").value_or(std::string {});
#ifndef _WIN32
        auto const user = std::to_string(::getuid());
#else
        auto const user = env.get("USERNAME").value_or("user");
#endif
        auto const asView = [](std::string const& value) {
            return std::string_view { value };
        };
        return derive(DaemonStateDirectoryInputs { .platform = NativeHostPlatform,
                                                   .xdgStateHome = xdgStateHome.transform(asView),
                                                   .localAppData = localAppData.transform(asView),
                                                   .home = home,
                                                   .user = user });
    }
} // namespace detail

/// Contour's per-user state directory.
///
/// In order:
///  1. POSIX: `<$XDG_STATE_HOME>/contour`, if it is set, non-empty and absolute (the XDG Base
///     Directory specification tells a relative value to be ignored);
///  2. POSIX: `<$HOME>/.local/state/contour`, if the home directory is absolute;
///  3. Windows: `<%LOCALAPPDATA%>\contour`, if it is absolute;
///  4. `<temp>/contour-state-<user>` when nothing above is usable. Per user, so two users never
///     share it; still not private, so stateDirectoryOrigin() names it for what it is.
///
/// Pure: the directory is neither created nor checked here.
/// @param inputs What the directory is derived from.
/// @return The state directory.
[[nodiscard]] inline std::filesystem::path daemonStateDirectory(DaemonStateDirectoryInputs const& inputs)
{
    if (auto configured = detail::configuredStateDirectory(inputs))
        return std::move(*configured);
    // The error_code overload: with neither TMPDIR nor /tmp usable the no-throw answer is an empty
    // base, and the directory is then relative -- a TemporaryFallback all the same, run from by nothing.
    auto ec = std::error_code {};
    return std::filesystem::temp_directory_path(ec)
           / core::pathFromUtf8("contour-state-" + std::string { inputs.user }).value_or("contour-state");
}

/// Where daemonStateDirectory() of the same @p inputs comes from.
///
/// A query of its own rather than a second return value, so the directory's callers stay as they are.
/// @param inputs What the directory is derived from.
/// @return TemporaryFallback when the directory is the shared temp directory's; Configured otherwise.
[[nodiscard]] inline StateDirectoryOrigin stateDirectoryOrigin(DaemonStateDirectoryInputs const& inputs)
{
    return detail::configuredStateDirectory(inputs) ? StateDirectoryOrigin::Configured
                                                    : StateDirectoryOrigin::TemporaryFallback;
}

/// Production entry point: derives the state directory from a process environment, by this build's
/// platform's conventions.
/// @param env The environment to read `$XDG_STATE_HOME`, `$HOME`, `%LOCALAPPDATA%` and (on Windows)
///            `%USERNAME%` from.
/// @return The state directory.
[[nodiscard]] inline std::filesystem::path daemonStateDirectory(core::Environment const& env)
{
    return detail::withStateDirectoryInputs(
        env, [](DaemonStateDirectoryInputs const& inputs) { return daemonStateDirectory(inputs); });
}

/// Production entry point of stateDirectoryOrigin().
/// @param env The environment daemonStateDirectory(env) reads.
/// @return Where daemonStateDirectory(env) comes from.
[[nodiscard]] inline StateDirectoryOrigin stateDirectoryOrigin(core::Environment const& env)
{
    return detail::withStateDirectoryInputs(
        env, [](DaemonStateDirectoryInputs const& inputs) { return stateDirectoryOrigin(inputs); });
}

} // namespace vthost
