// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the served-session tests share: a fake extension that opens a served tab and draws a fixed pattern.

#include <vtbackend/core/Primitives.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtcontrol/Uuid.hpp>
#include <vthost/ConnectionId.hpp>
#include <vthost/RepeatedText.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

namespace vthost::testing
{

/// A thread-safe source of served-tab UUIDs, apart from the host's own: theirs start with 0x5e.
class ServedUuids final: public vtcontrol::UuidSource
{
  public:
    [[nodiscard]] vtcontrol::Uuid next() override
    {
        auto const lock = std::lock_guard { _mutex };
        auto uuid = vtcontrol::Uuid {};
        uuid.bytes[0] = 0x5e;
        uuid.bytes[6] = 0x70;
        uuid.bytes[8] = 0x80;
        uuid.bytes[15] = ++_counter;
        return uuid;
    }

  private:
    std::mutex _mutex;
    uint8_t _counter = 0;
};

/// A window on a connection of its own: it selects one workspace, follows the daemon, can be asked to switch,
/// and records what it was asked and told.
class FakeWindow final: public SessionStreamEvents
{
  public:
    explicit FakeWindow(uint64_t index):
        _id { ConnectionId { .endpoint = "native", .index = index, .peer = {} } }
    {
    }

    [[nodiscard]] bool mirrorsScreens() const noexcept override { return false; }
    [[nodiscard]] uint64_t selectedWorkspace() const noexcept override { return selected; }
    [[nodiscard]] ConnectionId connectionId() const override { return _id; }
    [[nodiscard]] bool followsDaemon() const noexcept override { return true; }
    [[nodiscard]] bool canSwitch() const noexcept override { return true; }
    void requestSwitch(proto::SwitchRequest const& request) override { switches.push_back(request); }
    void revealPane(vtworkspace::SessionId session) override { reveals.push_back(session); }
    [[nodiscard]] LinkOpening linkOpening() const noexcept override { return links; }
    void openUrl(std::string_view url) override { opened.emplace_back(url); }

    uint64_t selected = 0;                       ///< What it selects.
    LinkOpening links = LinkOpening::Able;       ///< Whether its client opens links.
    std::vector<std::string> opened;             ///< Every link it was told to open.
    std::vector<proto::SwitchRequest> switches;  ///< Every switch it was asked for.
    std::vector<vtworkspace::SessionId> reveals; ///< Every pane it was told to reveal.

  private:
    ConnectionId _id;
};

/// What the fake extension draws into its tab: the screen cleared, then a word at the top left.
inline constexpr auto ServedPattern = std::string_view { "\x1b[2J\x1b[Hserved-stub" };

/// What a served tab's screen shows once ServedPattern was drawn.
inline constexpr auto ServedText = std::string_view { "served-stub" };

/// The size the fake extension's device starts at.
inline constexpr auto ServedSize =
    vtbackend::PageSize { vtbackend::LineCount(24), vtbackend::ColumnCount(80) };

/// Opens a served tab in @p host's workspace, as an extension does, and draws ServedPattern into it.
/// @param host The host.
/// @return The tab's session; nullopt when the host refused the spawn.
[[nodiscard]] inline std::optional<vtworkspace::SessionId> openServedTab(SessionHost& host)
{
    auto const device = host.served().makeDevice(ServedSize);
    auto const* const tab = host.createTabIn(
        host.workspaceId(),
        { .served =
              ServedSpawn { .extension = "stub", .scheme = "stub", .moduleDigest = "", .device = device } });
    if (tab == nullptr)
        return std::nullopt;
    device->feed(ServedPattern);
    return tab->rootPane()->session();
}

/// @return @p page split into its lines, each without its trailing blanks.
/// @param page A screen's main page as text (Screen::renderMainPageText()).
[[nodiscard]] inline std::vector<std::string> pageLines(std::string_view page)
{
    auto lines = std::vector<std::string> {};
    for (auto const line: std::views::split(page, '\n'))
    {
        auto text = std::string(std::string_view { line });
        text.erase(text.find_last_not_of(' ') + 1);
        lines.push_back(std::move(text));
    }
    return lines;
}

/// A host frame's box as a test expects it (vthost::stoppedFrame(), vthost::restartingFrame()), centred on a
/// page of @p size.
struct ExpectedStoppedBox
{
    std::size_t top = 0;            ///< The page line its top border is on.
    std::vector<std::string> lines; ///< Its lines, borders included, without trailing blanks.
};

/// @param size The page.
/// @param shown Each line inside, as shown, with the columns it takes.
/// @param inner The columns inside the box, between its padding.
/// @return Where and how the box is drawn.
[[nodiscard]] inline ExpectedStoppedBox expectedStoppedBox(
    vtbackend::PageSize size,
    std::vector<std::pair<std::string_view, std::size_t>> const& shown,
    std::size_t inner)
{
    auto const left = std::string((unbox<std::size_t>(size.columns) - inner - 4) / 2, ' ');
    auto const rule = repeatedText("\u2500", inner + 2);
    auto box =
        ExpectedStoppedBox { .top = (unbox<std::size_t>(size.lines) - shown.size() - 2) / 2, .lines = {} };
    box.lines.push_back(left + "\u256D" + rule + "\u256E");
    for (auto const& [text, columns]: shown)
        box.lines.push_back(left + "\u2502 " + std::string(text) + std::string(inner - columns, ' ')
                            + " \u2502");
    box.lines.push_back(left + "\u2570" + rule + "\u256F");
    return box;
}

/// @return Whether @p page shows @p box where it belongs.
[[nodiscard]] inline bool showsBox(std::string_view page, ExpectedStoppedBox const& box)
{
    auto const lines = pageLines(page);
    return lines.size() >= box.top + box.lines.size()
           && std::ranges::equal(lines | std::views::drop(box.top) | std::views::take(box.lines.size()),
                                 box.lines);
}

} // namespace vthost::testing
