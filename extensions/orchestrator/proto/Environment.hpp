// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Commands.hpp>

namespace orchestrator::proto
{

/// How a withheld variable is named.
enum class NameMatch : uint8_t
{
    Exact,  ///< The variable of exactly this name.
    Prefix, ///< Every variable whose name starts with this.
};

/// A variable, or a family of them, the driver's children never inherit.
struct WithheldVariable
{
    std::string_view name; ///< The name, or the prefix.
    NameMatch match;       ///< How @ref name is compared.
};

/// The variables withheld from everything the driver starts:
/// - `CONTOUR_*`: nothing may find, and talk to, the daemon or pane the driver itself runs in;
/// - `GIT_*`: an inherited `GIT_DIR`, `GIT_WORK_TREE`, `GIT_INDEX_FILE`, `GIT_CONFIG_*` and the like would
///   point git elsewhere than the declared working directory, or merge with commandEnvironment();
///   `GIT_SSH_COMMAND`, `GIT_SSH` and `GIT_ASKPASS` are the exceptions and kept: they configure transport
///   authentication, not repository selection;
/// - `GH_REPO`, `GH_HOST` and the enterprise tokens: `gh` would talk about another repository or host than
///   the working directory's.
/// `GH_TOKEN`, `GITHUB_TOKEN`, `GH_CONFIG_DIR`, PATH, HOME, the locale, SSH_AUTH_SOCK, the proxy and the
/// SSL_CERT_* variables are kept.
/// @return The table, one row per variable or family.
[[nodiscard]] std::span<WithheldVariable const> withheldVariables() noexcept;

/// @param entry One environment entry, as `NAME=VALUE`.
/// @return Whether withheldVariables() names its variable and it is not one of the kept `GIT_*` exceptions.
[[nodiscard]] bool isWithheld(std::string_view entry) noexcept;

/// @param environment An environment, as `NAME=VALUE`.
/// @return @p environment without the entries isWithheld() names, in order.
[[nodiscard]] std::vector<std::string> withoutWithheld(std::span<std::string const> environment);

/// @param environment An environment, as `NAME=VALUE`.
/// @param name A variable's name.
/// @return Its value; std::nullopt when it is not set, or set to nothing.
[[nodiscard]] std::optional<std::string> lookup(std::span<std::string const> environment,
                                                std::string_view name);

/// @param base An environment, as `NAME=VALUE`.
/// @param layers The variables to add, each layer overriding what came before.
/// @return @p base with every variable of @p layers set, in order: one entry per name.
[[nodiscard]] std::vector<std::string> withVariables(
    std::vector<std::string> base, std::span<std::span<EnvironmentVariable const> const> layers);

} // namespace orchestrator::proto
