// SPDX-License-Identifier: Apache-2.0
#include <vtcontrol/Catalog.hpp>

#include <format>
#include <ranges>

namespace vtcontrol
{

std::expected<ControlGrants, ControlError> parseGrants(std::string_view commaList)
{
    if (commaList == "all")
        return ControlGrants::all();

    // Split yields nothing for an empty list, which would read as no grants rather than as a mistake.
    if (commaList.empty())
        return std::unexpected(ControlError {
            .code = ControlErrorCode::BadRequest, .message = "empty grant list", .candidates = {} });

    auto grants = ControlGrants::none();
    for (auto const name: std::views::split(commaList, ',') | std::views::transform([](auto const& part) {
                              return std::string_view { part.begin(), part.end() };
                          }))
    {
        auto const permission = enumFromName<Permission>(name);
        if (!permission)
            return std::unexpected(ControlError {
                .code = ControlErrorCode::BadRequest,
                .message = std::format("unknown grant '{}' (expected all, or a comma list of read, screen, "
                                       "input and write)",
                                       name),
                .candidates = {} });
        grants = grants.with(*permission);
    }
    return grants;
}

std::string toString(ControlGrants grants)
{
    auto text = std::string {};
    for (auto const& row: EnumTable<Permission>::Rows)
    {
        if (!grants.contains(row.value))
            continue;
        if (!text.empty())
            text += ',';
        text += row.name;
    }
    return text.empty() ? std::string { "none" } : text;
}

} // namespace vtcontrol
