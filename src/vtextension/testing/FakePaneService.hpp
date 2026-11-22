// SPDX-License-Identifier: Apache-2.0
//
// Test-only: an IPaneService that records what it is asked and answers what the test set.
#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/DaemonWorld.hpp>
#include <vtextension/PaneService.hpp>

namespace vtextension::testing
{

/// What one open() was asked.
struct PaneOpenCall
{
    std::string paneTemplate;
    ConfigMap args;
    std::string cwd;
    std::string title;
    std::string tag;
    std::optional<std::string> nearTab;
};

/// Records every call, and answers each with what the test set; `not-supported-yet` until it sets otherwise.
class FakePaneService final: public IPaneService
{
  public:
    [[nodiscard]] std::expected<std::string, PaneError> open(std::string_view paneTemplate,
                                                             ConfigMap const& args,
                                                             std::string_view cwd,
                                                             std::string_view title,
                                                             std::string_view tag,
                                                             std::optional<std::string_view> nearTab) override
    {
        opens.push_back(PaneOpenCall {
            .paneTemplate = std::string { paneTemplate },
            .args = args,
            .cwd = std::string { cwd },
            .title = std::string { title },
            .tag = std::string { tag },
            .nearTab = nearTab.transform([](std::string_view tab) { return std::string { tab }; }),
        });
        return openAnswer;
    }

    [[nodiscard]] std::expected<void, PaneError> close(std::string_view pane) override
    {
        closes.emplace_back(pane);
        return closeAnswer;
    }

    [[nodiscard]] std::expected<std::vector<PaneInfo>, PaneError> list() override { return listAnswer; }

    [[nodiscard]] std::expected<void, PaneError> reveal(std::string_view pane, Origin origin) override
    {
        reveals.emplace_back(std::string { pane }, origin);
        return revealAnswer;
    }

    [[nodiscard]] std::expected<void, PaneError> send(std::string_view pane, std::string_view text) override
    {
        sends.emplace_back(std::string { pane }, std::string { text });
        return sendAnswer;
    }

    [[nodiscard]] std::expected<void, PaneError> press(std::string_view pane, PaneKeyName key) override
    {
        presses.emplace_back(std::string { pane }, key);
        return sendAnswer;
    }

    std::vector<PaneOpenCall> opens;                        ///< Every open(), in order.
    std::vector<std::string> closes;                        ///< Every close(), in order.
    std::vector<std::pair<std::string, Origin>> reveals;    ///< Every reveal(), in order.
    std::vector<std::pair<std::string, std::string>> sends; ///< Every send(), in order.
    std::vector<std::pair<std::string, PaneKeyName>> presses; ///< Every press(), in order.
    std::expected<std::string, PaneError> openAnswer { std::unexpect, PaneError::NotSupportedYet };
    std::expected<void, PaneError> closeAnswer { std::unexpect, PaneError::NotSupportedYet };
    std::expected<std::vector<PaneInfo>, PaneError> listAnswer { std::unexpect, PaneError::NotSupportedYet };
    std::expected<void, PaneError> revealAnswer { std::unexpect, PaneError::NotSupportedYet };
    std::expected<void, PaneError> sendAnswer { std::unexpect, PaneError::NotSupportedYet };
};

} // namespace vtextension::testing
