// SPDX-License-Identifier: Apache-2.0
#include <contour/window/NodeDrag.hpp>
#include <contour/window/QtString.hpp>

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

namespace contour::window
{

namespace
{
    /// A row of the kind table: a node payload and its name in the JSON.
    struct NodeKindRow
    {
        DropPayload payload;         ///< The payload.
        std::string_view name;       ///< Its `kind`.
        std::size_t minimumSessions; ///< The fewest sessions it names.
        std::size_t maximumSessions; ///< The most sessions it names.
    };

    constexpr auto NodeKinds = std::array {
        NodeKindRow {
            .payload = DropPayload::Pane, .name = "pane", .minimumSessions = 1, .maximumSessions = 1 },
        NodeKindRow { .payload = DropPayload::Tab,
                      .name = "tab",
                      .minimumSessions = 2,
                      .maximumSessions = MaxDragSessions },
    };

    /// A row of the host table: a host kind and its name in the JSON.
    struct HostKindRow
    {
        HostKind kind;         ///< The kind.
        std::string_view name; ///< Its `host.kind`.
    };

    constexpr auto HostKinds = std::array {
        HostKindRow { .kind = HostKind::Process, .name = "process" },
        HostKindRow { .kind = HostKind::Daemon, .name = "daemon" },
    };

    /// The largest integer a JSON number carries exactly.
    constexpr auto MaxExactInteger = 9007199254740992.0;

    /// @return @p value as a whole number in [0, @p max], or std::nullopt.
    [[nodiscard]] std::optional<double> wholeIn(QJsonValue const& value, double max)
    {
        if (!value.isDouble())
            return std::nullopt;
        auto const number = value.toDouble();
        if (number < 0.0 || number > max || number != std::floor(number))
            return std::nullopt;
        return number;
    }

    /// @return Whether @p object has exactly the keys @p keys.
    [[nodiscard]] bool hasExactly(QJsonObject const& object, std::span<std::string_view const> keys)
    {
        return std::cmp_equal(object.size(), keys.size())
               && std::ranges::all_of(keys,
                                      [&](std::string_view key) { return object.contains(toQString(key)); });
    }

    constexpr auto DragKeys =
        std::array<std::string_view, 5> { "kind", "host", "windowId", "tabIndex", "sessions" };
    constexpr auto HostKeys = std::array<std::string_view, 2> { "kind", "id" };

    [[nodiscard]] std::optional<HostIdentity> parseHost(QJsonValue const& value)
    {
        if (!value.isObject())
            return std::nullopt;
        auto const object = value.toObject();
        if (!hasExactly(object, HostKeys))
            return std::nullopt;
        auto const kind = object.value("kind");
        auto const id = object.value("id");
        if (!kind.isString() || !id.isString() || id.toString().isEmpty())
            return std::nullopt;
        auto const row = std::ranges::find_if(HostKinds, [&](HostKindRow const& candidate) {
            return kind.toString() == toQString(candidate.name);
        });
        if (row == HostKinds.end())
            return std::nullopt;
        return HostIdentity { .kind = row->kind, .id = id.toString() };
    }
} // namespace

QString writeNodeDrag(NodeDrag const& drag)
{
    auto const kind = std::ranges::find(NodeKinds, drag.payload, &NodeKindRow::payload);
    auto const host = std::ranges::find(HostKinds, drag.host.kind, &HostKindRow::kind);
    auto sessions = QJsonArray {};
    for (auto const session: drag.sessions)
        sessions.append(static_cast<double>(session));
    auto const object = QJsonObject {
        { "kind", kind != NodeKinds.end() ? toQString(kind->name) : QString {} },
        { "host",
          QJsonObject { { "kind", host != HostKinds.end() ? toQString(host->name) : QString {} },
                        { "id", drag.host.id } } },
        { "windowId", static_cast<double>(drag.window.value) },
        { "tabIndex", drag.tabIndex },
        { "sessions", sessions },
    };
    return QString::fromUtf8(QJsonDocument { object }.toJson(QJsonDocument::Compact));
}

std::optional<NodeDrag> parseNodeDrag(QString const& json)
{
    auto const document = QJsonDocument::fromJson(json.toUtf8());
    if (!document.isObject())
        return std::nullopt;
    auto const object = document.object();
    if (!hasExactly(object, DragKeys))
        return std::nullopt;

    auto const kindValue = object.value("kind");
    if (!kindValue.isString())
        return std::nullopt;
    auto const kind = std::ranges::find_if(
        NodeKinds, [&](NodeKindRow const& row) { return kindValue.toString() == toQString(row.name); });
    auto const host = parseHost(object.value("host"));
    auto const window = wholeIn(object.value("windowId"), MaxExactInteger);
    auto const tabIndex = wholeIn(object.value("tabIndex"), std::numeric_limits<int>::max());
    auto const sessionsValue = object.value("sessions");
    if (kind == NodeKinds.end() || !host || !window || !tabIndex || !sessionsValue.isArray())
        return std::nullopt;

    // Counted before it is read: a forged list may be of any length.
    auto const array = sessionsValue.toArray();
    auto const count = static_cast<std::size_t>(array.size());
    if (count < kind->minimumSessions || count > kind->maximumSessions)
        return std::nullopt;
    auto sessions = std::vector<uint64_t> {};
    sessions.reserve(count);
    for (auto const& entry: array)
    {
        auto const session = wholeIn(entry, MaxExactInteger);
        if (!session)
            return std::nullopt;
        sessions.push_back(static_cast<uint64_t>(*session));
    }
    // A session named twice is a forged list: a tab names each of its panes once.
    auto sorted = sessions;
    std::ranges::sort(sorted);
    if (std::ranges::adjacent_find(sorted) != sorted.end())
        return std::nullopt;

    return NodeDrag { .payload = kind->payload,
                      .host = *host,
                      .window = vtworkspace::WindowId { static_cast<uint64_t>(*window) },
                      .tabIndex = static_cast<int>(*tabIndex),
                      .sessions = std::move(sessions) };
}

} // namespace contour::window
