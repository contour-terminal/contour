// SPDX-License-Identifier: Apache-2.0
//
// The agent panes a daemon-world extension opens, as its host-function table reaches them (`pane-open`,
// `pane-close`, `panes`, `pane-reveal`): the policy's interface (IPaneService), and the host's (IPaneHost),
// which vthost -- a layer above -- adapts its own registry of owned panes to.
#pragma once

#include <vtpty/DirectoryHandle.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/DaemonWorld.hpp>
#include <vtextension/IHostServices.hpp>

namespace vtextension
{

/// Why a pane was not opened, closed or listed.
enum class PaneError : uint8_t
{
    NotDeclared,      ///< The manifest declares no pane template of that name.
    ArgumentRejected, ///< A placeholder has no value, a name is not the template's, or a value is not its
                      ///< kind.
    CwdOutsideRoot,   ///< The working directory does not belong to the template's root.
    CwdMissing,       ///< The working directory does not exist.
    CwdNotADirectory, ///< The working directory is no directory, or may not be opened.
    ProgramMissing,   ///< The template's program is not found on the PATH the pane would start it with.
    BadTitle,         ///< The title is too long, or holds a control or disguising code point.
    BadTag,           ///< The tag is not `[A-Za-z0-9][A-Za-z0-9._-]*` of at most MaxPaneTagBytes.
    NotOwned,         ///< The pane is not one the extension opened.
    TooMany,          ///< The extension has as many panes as it may.
    HostRefused,      ///< The host does not open panes now (a live upgrade runs), or cannot.
    NotReconciled,    ///< The host has not restored its sessions yet: the list would be wrong; retry later.
    Gone,             ///< The pane was closed: its tab is going, or never came.
    NotSupportedYet,  ///< The host does not serve this yet: a capability, never a fault.
};

/// What the host is asked to open: everything checked.
struct PaneSpawn
{
    std::vector<std::string> argv;                                   ///< The program and its arguments.
    std::vector<std::pair<std::string, std::string>> environment {}; ///< Laid over the inherited one.
    std::vector<std::string> removedEnvironment {};                  ///< Inherited names it does not see.
    std::shared_ptr<vtpty::DirectoryHandle const> directory {};      ///< Entered by its descriptor.
    std::string cwd {};                                              ///< The directory's resolved path.
    std::string title {};                                            ///< The tab's fixed name.
    std::string tag {};                                              ///< The extension's own label.
    std::optional<std::string> near {}; ///< The served tab whose workspace it opens in, if any.
};

/// The agent panes of one extension: its template, arguments, root and environment checked.
class IPaneService
{
  public:
    IPaneService() = default;
    IPaneService(IPaneService const&) = delete;
    IPaneService(IPaneService&&) = delete;
    IPaneService& operator=(IPaneService const&) = delete;
    IPaneService& operator=(IPaneService&&) = delete;
    virtual ~IPaneService() = default;

    /// `pane-open`: opens a pane running a declared template, without taking the focus.
    /// @param paneTemplate The template's name.
    /// @param args A value for each of its placeholders, by name.
    /// @param cwd The directory it starts in: absolute, inside the template's root.
    /// @param title The tab's name.
    /// @param tag The extension's own label for the pane.
    /// @param near A served tab of the extension's whose workspace it opens in; none for the focused one.
    /// @return The pane's id, before the pane exists; or why it was not opened.
    [[nodiscard]] virtual std::expected<std::string, PaneError> open(
        std::string_view paneTemplate,
        ConfigMap const& args,
        std::string_view cwd,
        std::string_view title,
        std::string_view tag,
        std::optional<std::string_view> near) = 0;

    /// `pane-close`: closes a pane the extension opened.
    /// @param pane Its id.
    /// @return Nothing; or why it was not closed.
    [[nodiscard]] virtual std::expected<void, PaneError> close(std::string_view pane) = 0;

    /// `panes`: the panes the extension opened, listed until their sessions ended.
    /// @return Each with its tag, status and program; or why they cannot be listed now.
    [[nodiscard]] virtual std::expected<std::vector<PaneInfo>, PaneError> list() = 0;

    /// `pane-reveal`: brings a pane the extension opened in front of the origin's window.
    /// @param pane Its id.
    /// @param origin The connection the invoked action came from.
    /// @return Nothing; or why it was not revealed.
    [[nodiscard]] virtual std::expected<void, PaneError> reveal(std::string_view pane, Origin origin) = 0;
};

/// The host side a checked spawn is forwarded to: what owns the panes, bound to one extension.
class IPaneHost
{
  public:
    IPaneHost() = default;
    IPaneHost(IPaneHost const&) = delete;
    IPaneHost(IPaneHost&&) = delete;
    IPaneHost& operator=(IPaneHost const&) = delete;
    IPaneHost& operator=(IPaneHost&&) = delete;
    virtual ~IPaneHost() = default;

    /// Opens a pane for a spawn the policy checked.
    /// @param spawn What to run, and where.
    /// @return The pane's id; or why the host refused.
    [[nodiscard]] virtual std::expected<std::string, PaneError> open(PaneSpawn spawn) = 0;

    /// @copydoc IPaneService::close
    [[nodiscard]] virtual std::expected<void, PaneError> close(std::string_view pane) = 0;

    /// @copydoc IPaneService::list
    [[nodiscard]] virtual std::expected<std::vector<PaneInfo>, PaneError> list() = 0;

    /// @copydoc IPaneService::reveal
    [[nodiscard]] virtual std::expected<void, PaneError> reveal(std::string_view pane, Origin origin) = 0;
};

/// The service of a host that opens no panes: every call answers `not-supported-yet`, which a guest treats as
/// a capability it does not have, never as a fault.
class UnservedPanes final: public IPaneService
{
  public:
    [[nodiscard]] std::expected<std::string, PaneError> open(
        std::string_view /*paneTemplate*/,
        ConfigMap const& /*args*/,
        std::string_view /*cwd*/,
        std::string_view /*title*/,
        std::string_view /*tag*/,
        std::optional<std::string_view> /*near*/) override
    {
        return std::unexpected(PaneError::NotSupportedYet);
    }

    [[nodiscard]] std::expected<void, PaneError> close(std::string_view /*pane*/) override
    {
        return std::unexpected(PaneError::NotSupportedYet);
    }

    [[nodiscard]] std::expected<std::vector<PaneInfo>, PaneError> list() override
    {
        return std::unexpected(PaneError::NotSupportedYet);
    }

    [[nodiscard]] std::expected<void, PaneError> reveal(std::string_view /*pane*/, Origin /*origin*/) override
    {
        return std::unexpected(PaneError::NotSupportedYet);
    }
};

/// One row of the table that turns a PaneError into what the guest sees.
struct PaneErrorRow
{
    PaneError error;     ///< The service's reason.
    HostError hostError; ///< The WIT host-error it reaches the guest as.
};

/// One row per error, indexed by value. Only a missing working directory and a closed pane are `gone`; a
/// refusal the host may lift later (an upgrade running, sessions not restored yet) is `limit`. A working
/// directory that is no directory answers what a declared process's does (CwdRefusal); too many panes, and a
/// program not found, each have a word of their own, which the user can act on.
inline constexpr auto PaneErrorToHostError = std::array {
    PaneErrorRow { .error = PaneError::NotDeclared, .hostError = HostError::BadArgument },
    PaneErrorRow { .error = PaneError::ArgumentRejected, .hostError = HostError::BadArgument },
    PaneErrorRow { .error = PaneError::CwdOutsideRoot, .hostError = HostError::BadArgument },
    PaneErrorRow { .error = PaneError::CwdMissing, .hostError = HostError::Gone },
    PaneErrorRow { .error = PaneError::CwdNotADirectory, .hostError = HostError::NotADirectory },
    PaneErrorRow { .error = PaneError::ProgramMissing, .hostError = HostError::ProgramMissing },
    PaneErrorRow { .error = PaneError::BadTitle, .hostError = HostError::BadArgument },
    PaneErrorRow { .error = PaneError::BadTag, .hostError = HostError::BadArgument },
    PaneErrorRow { .error = PaneError::NotOwned, .hostError = HostError::NotOwned },
    PaneErrorRow { .error = PaneError::TooMany, .hostError = HostError::TooMany },
    PaneErrorRow { .error = PaneError::HostRefused, .hostError = HostError::Limit },
    PaneErrorRow { .error = PaneError::NotReconciled, .hostError = HostError::Limit },
    PaneErrorRow { .error = PaneError::Gone, .hostError = HostError::Gone },
    PaneErrorRow { .error = PaneError::NotSupportedYet, .hostError = HostError::NotSupportedYet },
};

static_assert(PaneErrorToHostError.size() == std::to_underlying(PaneError::NotSupportedYet) + 1U,
              "every PaneError has a row");
static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, PaneErrorToHostError.size()),
                                  [](std::size_t index) {
                                      return std::to_underlying(PaneErrorToHostError.at(index).error)
                                             == index;
                                  }),
              "PaneErrorToHostError is indexed by the enumerator's value");

/// @param error Why the service refused.
/// @return The host-error the guest is answered with, by PaneErrorToHostError.
[[nodiscard]] constexpr HostError hostErrorOf(PaneError error) noexcept
{
    return PaneErrorToHostError.at(std::to_underlying(error)).hostError;
}

} // namespace vtextension
