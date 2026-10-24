// SPDX-License-Identifier: Apache-2.0
#include <vtcontrol/Describe.hpp>

#include <format>
#include <optional>
#include <string_view>
#include <tuple>
#include <vector>

#include <vtcontrol/Catalog.hpp>
#include <vtcontrol/Events.hpp>
#include <vtcontrol/Json.hpp>

/// A `RawJson` holds any value, as glaze's own `raw_json` does.
template <>
struct glz::detail::to_json_schema<vtcontrol::RawJson>
{
    template <auto Opts>
    static void op(auto& schema, auto& /*defs*/) noexcept
    {
        schema.type =
            std::vector<std::string_view> { "number", "string", "boolean", "object", "array", "null" };
    }
};

namespace vtcontrol
{

namespace
{
    template <typename Row>
    struct RowPayload;

    template <typename P>
    struct RowPayload<EventRow<P>>
    {
        using Type = P;
    };

    /// @return The JSON Schema of `T`, generated from the struct, or `{}` if glaze cannot.
    template <typename T>
    [[nodiscard]] RawJson schemaOf()
    {
        auto schema = glz::write_json_schema<T>();
        return RawJson { .text = schema ? std::move(*schema) : std::string { "{}" } };
    }

    [[nodiscard]] RawJson rawJson(std::string_view text)
    {
        return RawJson { .text = std::string { text } };
    }

    template <typename Row>
    [[nodiscard]] ApiDescription describeMethod(Row const& row)
    {
        return ApiDescription { .name = std::string { row.name },
                                .summary = std::string { row.summary },
                                .inputSchema = schemaOf<typename Row::Request>(),
                                .outputSchema = schemaOf<typename Row::Reply>(),
                                .requestExample = rawJson(row.requestExample),
                                .responseExample = rawJson(row.responseExample) };
    }

    template <typename Row>
    [[nodiscard]] ApiDescription describeEvent(Row const& row)
    {
        using Payload = RowPayload<Row>::Type;
        return ApiDescription { .name = std::string { row.name },
                                .summary = std::string { row.summary },
                                .inputSchema = rawJson("{}"),
                                .outputSchema = schemaOf<Payload>(),
                                .requestExample = rawJson("{}"),
                                .responseExample = rawJson(encodeJson(Payload {})) };
    }

    template <typename Catalog, typename Describe>
    [[nodiscard]] std::optional<ApiDescription> find(Catalog const& catalog,
                                                     std::string_view name,
                                                     Describe const& describe)
    {
        auto found = std::optional<ApiDescription> {};
        std::apply(
            [&](auto const&... row) {
                (void) (((row.name == name) ? (found = describe(row), true) : false) || ...);
            },
            catalog);
        return found;
    }

    template <typename Catalog>
    [[nodiscard]] std::vector<ApiEntry> entries(Catalog const& catalog)
    {
        auto result = std::vector<ApiEntry> {};
        std::apply(
            [&](auto const&... row) {
                (result.push_back(
                     ApiEntry { .name = std::string { row.name }, .summary = std::string { row.summary } }),
                 ...);
            },
            catalog);
        return result;
    }
} // namespace

ApiListReply listApi()
{
    return ApiListReply { .methods = entries(Methods), .events = entries(EventCatalog) };
}

std::expected<ApiDescription, ControlError> describeApi(std::string_view name)
{
    if (auto method = find(Methods, name, [](auto const& row) { return describeMethod(row); }))
        return std::move(*method);
    if (auto event = find(EventCatalog, name, [](auto const& row) { return describeEvent(row); }))
        return std::move(*event);
    return std::unexpected(ControlError { .code = ControlErrorCode::MethodNotFound,
                                          .message = std::format("no method or event '{}'", name),
                                          .candidates = {} });
}

} // namespace vtcontrol
