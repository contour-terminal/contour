// SPDX-License-Identifier: Apache-2.0
//
// The policy between a daemon-world extension and the agent panes it opens: only a pane template its manifest
// declares, with arguments of their declared kinds, in a directory of the template's
// root held open, with a title and tag of their own grammar, and an environment that selects no other
// repository but keeps -- and adds to -- the user's own git settings.
#pragma once

#include <vtpty/DirectoryHandle.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/Manifest.hpp>
#include <vtextension/PaneService.hpp>
#include <vtextension/RootResolution.hpp>
#include <vtextension/SelfPrograms.hpp>

namespace vtextension
{

/// The longest title a pane may have, in code points: its tab's fixed name.
inline constexpr std::size_t MaxPaneTitleLength = 64;

/// The longest tag a pane may have, in bytes: `[A-Za-z0-9][A-Za-z0-9._-]*`.
inline constexpr std::size_t MaxPaneTagBytes = 128;

/// An environment family git reads as numbered pairs (`GIT_CONFIG_COUNT`, `GIT_CONFIG_KEY_<n>`,
/// `GIT_CONFIG_VALUE_<n>`): a pane keeps the inherited pairs and appends its template's after them.
struct IndexedEnvironmentFamily
{
    std::string_view count;       ///< The variable holding how many pairs there are.
    std::string_view keyPrefix;   ///< The prefix of each pair's key variable, its index appended.
    std::string_view valuePrefix; ///< The prefix of each pair's value variable, its index appended.
};

/// The families a pane appends to rather than replaces: the user's own `git -c` settings survive the
/// template's (`push.default=upstream`, which a fork's checkout needs to push).
inline constexpr auto AppendedPaneEnvironment = std::array {
    IndexedEnvironmentFamily {
        .count = "GIT_CONFIG_COUNT", .keyPrefix = "GIT_CONFIG_KEY_", .valuePrefix = "GIT_CONFIG_VALUE_" },
};

/// The inherited names a pane does not see: what InheritanceRules withholds from a declared command (git's
/// and gh's repository selection, Contour's control socket), less every name of an AppendedPaneEnvironment
/// family, which the pane keeps.
/// @param inherited The daemon's environment, as `NAME=value` entries.
/// @return The names to withhold, in @p inherited's order.
[[nodiscard]] std::vector<std::string> paneWithheldNames(std::span<std::string const> inherited);

/// The environment a pane's template adds: its pairs of an AppendedPaneEnvironment family renumbered after
/// the inherited ones, the count raised by theirs; every other declared name as given. Without an inherited
/// count (or with one that is no number) the template's pairs are used as declared.
/// @param declared The template's `pane-env` values, in declaration order.
/// @param inherited The daemon's environment, as `NAME=value` entries.
/// @return The variables to set, in declaration order.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> paneEnvironment(
    std::span<EnvironmentEntry const> declared, std::span<std::string const> inherited);

/// What a DeclaredPanes is: one extension's declarations and the daemon's facts about it.
struct DeclaredPanesConfig
{
    Manifest const& manifest;                      ///< The extension's manifest; outlives the service.
    std::vector<ResolvedRoot> roots;               ///< Its roots, resolved; distinct.
    std::vector<std::string> inheritedEnvironment; ///< The daemon's environment, as `NAME=value`.
    /// The programs run from a given executable (programTableOf()): a template whose program is one runs that
    /// executable, not what the pane's PATH finds.
    std::vector<ProgramPath> programs = {};
};

/// One extension's agent panes. open() checks, in this order: the template is declared; the title and the
/// tag; every placeholder's value (DeclaredArguments); the working directory, opened in the template's root
/// and handed on open. Nothing reaches the host before every check passed. close(), list() and reveal() are
/// the host's: it knows which panes are the extension's. The user's grant is not asked here: the service is
/// made only for an extension the user granted (DaemonExtensionRuntime), and goes with it when that grant
/// does.
class DeclaredPanes final: public IPaneService
{
    /// Lets only create() construct one, through std::make_unique.
    struct Key
    {
        explicit Key() = default;
    };

  public:
    /// Makes the service, once its roots are fit for it: no two declared directories equal, and every root a
    /// template or a `path:` argument names resolved.
    /// @param config The extension's declarations.
    /// @param directories Opens working directories and resolves path arguments; outlives the service.
    /// @param host Opens the checked panes; outlives the service.
    /// @return The service; `EqualRoots` or `Unresolved` otherwise.
    [[nodiscard]] static std::expected<std::unique_ptr<DeclaredPanes>, RootError> create(
        DeclaredPanesConfig config, vtpty::IDirectoryOpener& directories, IPaneHost& host);

    /// For create() only.
    DeclaredPanes(Key key, DeclaredPanesConfig config, vtpty::IDirectoryOpener& directories, IPaneHost& host);

    [[nodiscard]] std::expected<std::string, PaneError> open(
        std::string_view paneTemplate,
        ConfigMap const& args,
        std::string_view cwd,
        std::string_view title,
        std::string_view tag,
        std::optional<std::string_view> nearTab) override;
    [[nodiscard]] std::expected<void, PaneError> close(std::string_view pane) override;
    [[nodiscard]] std::expected<std::vector<PaneInfo>, PaneError> list() override;
    [[nodiscard]] std::expected<void, PaneError> reveal(std::string_view pane, Origin origin) override;

  private:
    DeclaredPanesConfig _config;
    vtpty::IDirectoryOpener& _directories;
    IPaneHost& _host;
    std::vector<std::string> _withheld; ///< paneWithheldNames() of the inherited environment.
};

} // namespace vtextension
