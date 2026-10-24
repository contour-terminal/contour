// SPDX-License-Identifier: Apache-2.0
#include <vtcontrol/Events.hpp>

#include <format>
#include <optional>
#include <ranges>

#include <vtcontrol/Json.hpp>

namespace vtcontrol
{

namespace
{
    template <std::size_t Index>
    [[nodiscard]] constexpr auto nameAt() noexcept
    {
        return std::get<Index>(EventCatalog).name;
    }

    template <std::size_t... Index>
    [[nodiscard]] std::string_view eventNameImpl(EventPayload const& payload,
                                                 std::index_sequence<Index...> /*unused*/)
    {
        auto name = std::string_view {};
        (void) ((payload.index() == Index ? (name = nameAt<Index>(), true) : false) || ...);
        return name;
    }

    template <std::size_t... Index>
    [[nodiscard]] std::expected<EventPayload, ControlError> decodePayload(
        std::string_view type, std::string_view json, std::index_sequence<Index...> /*unused*/)
    {
        auto result = std::optional<std::expected<EventPayload, ControlError>> {};
        auto const tryRow = [&]<std::size_t I>() {
            if (result || nameAt<I>() != type)
                return;
            using Payload = std::variant_alternative_t<I, EventPayload>;
            auto decoded = decodeJson<Payload>(json);
            if (decoded)
                result = EventPayload { std::in_place_index<I>, std::move(*decoded) };
            else
                result = std::unexpected(std::move(decoded.error()));
        };
        (tryRow.template operator()<Index>(), ...);
        if (result)
            return std::move(*result);
        return std::unexpected(ControlError { .code = ControlErrorCode::BadRequest,
                                              .message = std::format("unknown event type '{}'", type),
                                              .candidates = {} });
    }

    constexpr auto PayloadIndices = std::make_index_sequence<std::variant_size_v<EventPayload>> {};
} // namespace

std::optional<Permission> eventPermission(std::string_view name) noexcept
{
    auto permission = std::optional<Permission> {};
    std::apply(
        [&](auto const&... row) {
            (void) (((row.name == name) ? (permission = row.permission, true) : false) || ...);
        },
        EventCatalog);
    return permission;
}

std::string_view eventName(EventPayload const& payload)
{
    return eventNameImpl(payload, PayloadIndices);
}

std::string encodeEvent(Event const& event)
{
    auto const payload = std::visit([](auto const& value) { return encodeJson(value); }, event.payload);
    return std::format(R"({{"type":{},"window":{},"tab":{},"pane":{},"payload":{}}})",
                       quoteJsonString(eventName(event.payload)),
                       quoteJsonString(event.origin.window),
                       quoteJsonString(event.origin.tab),
                       quoteJsonString(event.origin.pane),
                       payload);
}

std::expected<Event, ControlError> decodeEvent(std::string_view text)
{
    auto envelope = decodeJson<EventEnvelope>(text);
    if (!envelope)
        return std::unexpected(std::move(envelope.error()));
    auto payload = decodePayload(envelope->type, envelope->payload.text, PayloadIndices);
    if (!payload)
        return std::unexpected(std::move(payload.error()));
    return Event { .origin = { .window = std::move(envelope->window),
                               .tab = std::move(envelope->tab),
                               .pane = std::move(envelope->pane) },
                   .payload = std::move(*payload) };
}

} // namespace vtcontrol
