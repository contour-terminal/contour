// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Environment.hpp>
#include <core/platform/PlatformError.hpp>

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace core::platform
{

/// @brief Whether @p name can name a variable of an environment block.
///
/// @param name The name to ask about.
/// @return True unless it is empty or holds the '=' that ends a name or the NUL that ends an entry.
[[nodiscard]] constexpr bool isValidEnvironmentName(std::string_view name) noexcept
{
    return !name.empty() && !name.contains('=') && !name.contains('\0');
}

/// @brief A process environment a shell writes: variables it sets, and the ones it exports.
///
/// A @c core::Environment, so everything that only reads -- @c homeDirectory(), @c userName(),
/// @c configHome() and every reader a consumer writes -- takes the read seam and is handed this
/// same object; one test double serves both. What it adds is a shell's mutation: a variable is
/// set locally, which @c get() then answers, and reaches the process environment -- the one child
/// processes inherit -- only once it is exported.
///
/// The working directory is not here: it is not an environment variable, and has a seam of its
/// own, @c WorkingDirectory.
class ProcessEnvironment: public core::Environment
{
  public:
    /// Sets a variable locally. It is not exported, and so not inherited, until
    /// @c exportVariable().
    ///
    /// @param name  Variable name.
    /// @param value Variable value. An empty value sets the variable.
    /// @return Nothing, or @c PlatformError::InvalidArgument for a name @c isValidEnvironmentName()
    ///         refuses or a value that holds NUL.
    [[nodiscard]] virtual std::expected<void, PlatformError> set(std::string_view name,
                                                                 std::string_view value) = 0;

    /// Removes a variable, locally and from the process environment. Removing one that is not set
    /// succeeds.
    ///
    /// @param name Variable name to remove.
    /// @return Nothing, or @c PlatformError::InvalidArgument for a name @c isValidEnvironmentName()
    ///         refuses, or @c PlatformError::IoError where the operating system refused.
    [[nodiscard]] virtual std::expected<void, PlatformError> unset(std::string_view name) = 0;

    /// Exports a variable set with @c set() to the process environment, where child processes
    /// inherit it. A name that was never set is left as it is.
    ///
    /// @param name Variable name to export.
    /// @return Nothing, or why the process environment did not take it:
    ///         @c PlatformError::InvalidArgument for text it cannot hold, @c PlatformError::IoError
    ///         where the operating system refused.
    [[nodiscard]] virtual std::expected<void, PlatformError> exportVariable(std::string_view name) = 0;

    /// @return Every variable name defined, local and exported.
    [[nodiscard]] virtual std::vector<std::string> keys() const = 0;

    /// Sets a variable and exports it.
    ///
    /// @param name  Variable name.
    /// @param value Variable value.
    /// @return What @c set() answered, or else what @c exportVariable() did.
    [[nodiscard]] std::expected<void, PlatformError> setAndExport(std::string_view name,
                                                                  std::string_view value)
    {
        return set(name, value).and_then([&] { return exportVariable(name); });
    }
};

/// @brief Creates this operating system's own ProcessEnvironment, for a composition root.
///
/// It reads the process environment through @c core::LiveEnvironment and exports through
/// @c core::setProcessEnvironmentVariable(), so on Windows through the wide API in UTF-8, with
/// names matched case-insensitively. Under Emscripten it is the POSIX one, over the environment
/// Emscripten's libc keeps for the module (under node a fixed default set, not the host's), and
/// what it exports reaches only that environment, since a module starts no process. Both
/// implementations are private (`posix/`, `windows/`), so this is the way to reach them. Each call
/// makes a new one, and the variables one was told to set but not to export are its own.
///
/// @return The process environment, owned by the caller.
[[nodiscard]] std::unique_ptr<ProcessEnvironment> nativeProcessEnvironment();

} // namespace core::platform
