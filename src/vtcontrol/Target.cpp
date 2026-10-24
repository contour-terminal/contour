// SPDX-License-Identifier: Apache-2.0
#include <vtcontrol/Target.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <ranges>
#include <vector>

namespace vtcontrol
{

namespace
{
    constexpr std::size_t MinimumSuffixDigits = 4;

    [[nodiscard]] bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept
    {
        return std::ranges::equal(a, b, [](char x, char y) {
            return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
        });
    }

    [[nodiscard]] bool isHexSuffix(std::string_view spec) noexcept
    {
        return spec.size() >= MinimumSuffixDigits && std::ranges::all_of(spec, [](char ch) {
                   return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
               });
    }

    [[nodiscard]] std::expected<ObjectRef, ControlError> settle(std::vector<ObjectRef> const& matches,
                                                                std::string_view spec,
                                                                ObjectKind kind)
    {
        if (matches.size() == 1)
            return matches.front();
        if (matches.empty())
            return std::unexpected(
                ControlError { .code = ControlErrorCode::NotFound,
                               .message = std::format("no {} matches '{}'", nameOf(kind), spec),
                               .candidates = {} });
        auto candidates = std::vector<std::string> {};
        for (auto const& match: matches)
            candidates.push_back(toString(match));
        return std::unexpected(
            ControlError { .code = ControlErrorCode::Ambiguous,
                           .message = std::format("'{}' matches {} {}s", spec, matches.size(), nameOf(kind)),
                           .candidates = std::move(candidates) });
    }

    /// @return The refs of the candidates `matches` accepts.
    [[nodiscard]] std::vector<ObjectRef> collect(std::span<TargetCandidate const> candidates, auto&& matches)
    {
        auto refs = std::vector<ObjectRef> {};
        for (auto const& candidate: candidates)
            if (matches(candidate))
                refs.push_back(candidate.ref);
        return refs;
    }
} // namespace

std::expected<ObjectRef, ControlError> resolveTarget(ObjectKind kind,
                                                     std::string_view spec,
                                                     std::span<TargetCandidate const> candidates)
{
    // 1. A full id of this kind. An id of another kind is never reinterpreted.
    if (auto const ref = parseObjectRef(spec))
    {
        if (ref->kind != kind)
            return std::unexpected(
                ControlError { .code = ControlErrorCode::NotFound,
                               .message = std::format("'{}' is not a {} id", spec, nameOf(kind)),
                               .candidates = {} });
        auto const found = collect(candidates, [&](TargetCandidate const& c) { return c.ref == *ref; });
        return settle(found, spec, kind);
    }

    // 2. A label, ignoring case.
    if (auto const byLabel = collect(
            candidates,
            [&](TargetCandidate const& c) { return !c.label.empty() && equalsIgnoreCase(c.label, spec); });
        !byLabel.empty())
        return settle(byLabel, spec, kind);

    // 3. A 1-based position, for tabs and windows.
    if (kind != ObjectKind::Pane)
    {
        auto position = uint32_t { 0 };
        auto const [end, error] = std::from_chars(spec.data(), spec.data() + spec.size(), position);
        if (!spec.empty() && error == std::errc {} && end == spec.data() + spec.size())
            if (auto const byPosition =
                    collect(candidates, [&](TargetCandidate const& c) { return c.position == position; });
                !byPosition.empty())
                return settle(byPosition, spec, kind);
    }

    // 4. An id suffix of at least four hex digits.
    if (isHexSuffix(spec))
    {
        auto const bySuffix = collect(candidates, [&](TargetCandidate const& c) {
            auto const text = toString(c.ref.uuid);
            return text.size() >= spec.size()
                   && equalsIgnoreCase(std::string_view { text }.substr(text.size() - spec.size()), spec);
        });
        return settle(bySuffix, spec, kind);
    }

    return settle({}, spec, kind);
}

} // namespace vtcontrol
