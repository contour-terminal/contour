// SPDX-License-Identifier: Apache-2.0
//
// Test-only: an IServedTabService that keeps an extension's tabs in memory, as the host's mirror would.
#pragma once

#include <algorithm>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/DaemonWorld.hpp>
#include <vtextension/ServedTabService.hpp>

namespace vtextension::testing
{

/// The tabs of one extension, in memory: an open makes one (or answers the one it made), a write records the
/// bytes, a close forgets it. Single-threaded, as the runner's lane calls it.
class FakeServedTabService final: public IServedTabService
{
  public:
    [[nodiscard]] std::expected<std::string, HostError> tabOpen(std::string_view scheme,
                                                                Origin /*origin*/) override
    {
        ++opens;
        if (openTabs.empty())
            openTabs.push_back(
                TabInfo { .tab = "tab-1", .scheme = std::string(scheme), .cols = 80, .rows = 24 });
        return openTabs.front().tab;
    }

    [[nodiscard]] std::expected<void, HostError> tabClose(std::string_view tab) override
    {
        ++closes;
        if (std::erase_if(openTabs, [&](TabInfo const& info) { return info.tab == tab; }) == 0)
            return std::unexpected(HostError::NotOwned);
        return {};
    }

    [[nodiscard]] std::expected<void, HostError> tabReveal(std::string_view tab, Origin /*origin*/) override
    {
        return owns(tab) ? std::expected<void, HostError> {} : std::unexpected(HostError::NotOwned);
    }

    [[nodiscard]] std::expected<void, HostError> urlOpen(std::string_view url, Origin /*origin*/) override
    {
        opened.emplace_back(url);
        return {};
    }

    [[nodiscard]] std::expected<std::vector<TabInfo>, HostError> tabs() override { return openTabs; }

    [[nodiscard]] std::expected<ByteList, HostError> tabRead(std::string_view tab) override
    {
        if (!owns(tab))
            return std::unexpected(HostError::NotOwned);
        return ByteList {};
    }

    [[nodiscard]] std::expected<void, HostError> tabWrite(std::string_view tab,
                                                          std::span<uint8_t const> bytes) override
    {
        if (!owns(tab))
            return std::unexpected(HostError::NotOwned);
        written.emplace_back(bytes.begin(), bytes.end());
        return {};
    }

    [[nodiscard]] std::expected<std::vector<ActionInfo>, HostError> actionsTake() override
    {
        return std::exchange(actions, {});
    }

    std::vector<TabInfo> openTabs;    ///< The open tabs.
    std::vector<std::string> written; ///< Every tab-write's bytes, in order.
    std::vector<ActionInfo> actions;  ///< What the next actions-take answers.
    std::vector<std::string> opened;  ///< Every url-open's link, in order.
    int opens = 0;                    ///< How many tab-opens came.
    int closes = 0;                   ///< How many tab-closes came.

  private:
    [[nodiscard]] bool owns(std::string_view tab) const
    {
        return std::ranges::contains(openTabs, tab, &TabInfo::tab);
    }
};

} // namespace vtextension::testing
