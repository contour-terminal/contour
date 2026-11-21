// SPDX-License-Identifier: Apache-2.0
//
// The tabs a daemon-world extension serves and the actions its user invoked, as its host-function table
// reaches them (`tab-open` … `tab-write`, `actions-take`): an interface, so that the table is tested with a
// fake, and so that the host that owns the tabs -- vthost, a layer above -- adapts its own to it.
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/DaemonWorld.hpp>
#include <vtextension/IHostServices.hpp>

namespace vtextension
{

/// Who a served-tab service serves: what its host names the extension's tabs by, from its manifest.
struct ServedTabOwner
{
    std::string_view extension;             ///< What the configuration calls it.
    std::string_view title;                 ///< Its manifest's `title`, else its name: its tabs' name.
    std::optional<std::string_view> scheme; ///< Its manifest's `serve` line; none when it serves no tab.
    std::string_view moduleDigest;          ///< The hex SHA-256 of its module, which the chrome names.
};

/// The served tabs and invoked actions of one extension.
class IServedTabService
{
  public:
    IServedTabService() = default;
    IServedTabService(IServedTabService const&) = delete;
    IServedTabService(IServedTabService&&) = delete;
    IServedTabService& operator=(IServedTabService const&) = delete;
    IServedTabService& operator=(IServedTabService&&) = delete;
    virtual ~IServedTabService() = default;

    /// `tab-open`: opens the extension's tab where the origin's window is, or reveals the one it has there.
    /// @param scheme What the tab is served as: the manifest's `serve` line.
    /// @param origin The connection the invoked action came from.
    /// @return The tab's id; or why it was not opened.
    [[nodiscard]] virtual std::expected<std::string, HostError> tabOpen(std::string_view scheme,
                                                                        Origin origin) = 0;

    /// `tab-close`: closes one of the extension's tabs.
    /// @param tab Its id.
    /// @return Nothing; or why it was not closed.
    [[nodiscard]] virtual std::expected<void, HostError> tabClose(std::string_view tab) = 0;

    /// `tab-reveal`: brings one of the extension's tabs in front of the origin's window.
    /// @param tab Its id.
    /// @param origin The connection the invoked action came from.
    /// @return Nothing; or why it was not revealed.
    [[nodiscard]] virtual std::expected<void, HostError> tabReveal(std::string_view tab, Origin origin) = 0;

    /// `tabs`: the extension's tabs.
    /// @return Each with its scheme and size; or why they cannot be listed.
    [[nodiscard]] virtual std::expected<std::vector<TabInfo>, HostError> tabs() = 0;

    /// `tab-read`: the input the tab's terminal wrote since the last read.
    /// @param tab Its id.
    /// @return The bytes; or why there are none.
    [[nodiscard]] virtual std::expected<ByteList, HostError> tabRead(std::string_view tab) = 0;

    /// `tab-write`: draws into the tab.
    /// @param tab Its id.
    /// @param bytes What its terminal receives as its program's output.
    /// @return Nothing; or why it was not written.
    [[nodiscard]] virtual std::expected<void, HostError> tabWrite(std::string_view tab,
                                                                  std::span<uint8_t const> bytes) = 0;

    /// `actions-take`: the actions the user invoked since the last take, oldest first.
    /// @return Each with the connection it came from; or why they cannot be taken.
    [[nodiscard]] virtual std::expected<std::vector<ActionInfo>, HostError> actionsTake() = 0;
};

/// The service of a host that serves no tabs: every call answers `not-supported-yet`, which a guest treats as
/// a capability it does not have, never as a fault.
class UnservedTabs final: public IServedTabService
{
  public:
    [[nodiscard]] std::expected<std::string, HostError> tabOpen(std::string_view /*scheme*/,
                                                                Origin /*origin*/) override
    {
        return std::unexpected(HostError::NotSupportedYet);
    }

    [[nodiscard]] std::expected<void, HostError> tabClose(std::string_view /*tab*/) override
    {
        return std::unexpected(HostError::NotSupportedYet);
    }

    [[nodiscard]] std::expected<void, HostError> tabReveal(std::string_view /*tab*/,
                                                           Origin /*origin*/) override
    {
        return std::unexpected(HostError::NotSupportedYet);
    }

    [[nodiscard]] std::expected<std::vector<TabInfo>, HostError> tabs() override
    {
        return std::unexpected(HostError::NotSupportedYet);
    }

    [[nodiscard]] std::expected<ByteList, HostError> tabRead(std::string_view /*tab*/) override
    {
        return std::unexpected(HostError::NotSupportedYet);
    }

    [[nodiscard]] std::expected<void, HostError> tabWrite(std::string_view /*tab*/,
                                                          std::span<uint8_t const> /*bytes*/) override
    {
        return std::unexpected(HostError::NotSupportedYet);
    }

    [[nodiscard]] std::expected<std::vector<ActionInfo>, HostError> actionsTake() override
    {
        return std::unexpected(HostError::NotSupportedYet);
    }
};

} // namespace vtextension
