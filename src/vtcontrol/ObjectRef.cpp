// SPDX-License-Identifier: Apache-2.0
#include <vtcontrol/ObjectRef.hpp>

#include <charconv>
#include <format>
#include <ranges>

namespace vtcontrol
{

namespace
{
    constexpr auto BlockPrefix = std::string_view { "block:" };

    [[nodiscard]] std::optional<ObjectKind> kindFromName(std::string_view name) noexcept
    {
        return enumFromName<ObjectKind>(name);
    }
} // namespace

std::string toString(ObjectRef const& ref)
{
    return std::format("{}:{}", nameOf(ref.kind), toString(ref.uuid));
}

std::optional<ObjectRef> parseObjectRef(std::string_view text)
{
    auto const colon = text.find(':');
    if (colon == std::string_view::npos)
        return std::nullopt;
    auto const kind = kindFromName(text.substr(0, colon));
    auto const uuid = parseUuid(text.substr(colon + 1));
    if (!kind || !uuid)
        return std::nullopt;
    return ObjectRef { .kind = *kind, .uuid = *uuid };
}

std::string toString(BlockRef const& ref)
{
    return std::format("{}{}/{}", BlockPrefix, toString(ref.pane), ref.serial);
}

std::optional<BlockRef> parseBlockRef(std::string_view text)
{
    if (!text.starts_with(BlockPrefix))
        return std::nullopt;
    text.remove_prefix(BlockPrefix.size());
    auto const slash = text.find('/');
    if (slash == std::string_view::npos)
        return std::nullopt;
    auto const pane = parseUuid(text.substr(0, slash));
    auto const digits = text.substr(slash + 1);
    if (!pane || digits.empty())
        return std::nullopt;
    auto serial = uint64_t { 0 };
    auto const [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), serial);
    if (error != std::errc {} || end != digits.data() + digits.size())
        return std::nullopt;
    return BlockRef { .pane = *pane, .serial = serial };
}

} // namespace vtcontrol
