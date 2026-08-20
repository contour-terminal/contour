// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/ProgressState.hpp>

#include <crispy/Assert.hpp>
#include <crispy/Utils.hpp>

#include <algorithm>
#include <optional>
#include <utility>

namespace vtbackend
{

namespace
{
    /// Parses one decimal field of the sequence.
    ///
    /// Wraps @ref crispy::toInteger only to bound the field's LENGTH first: `toInteger` accumulates
    /// without overflow detection, so an absurdly long run of digits would wrap and could land back
    /// on a value the caller accepts. Both fields here are at most three digits.
    ///
    /// @param value The field's text.
    /// @return The parsed value, or nullopt when @p value is not a plain, short, non-negative number.
    [[nodiscard]] constexpr std::optional<unsigned> parseField(std::string_view value) noexcept
    {
        constexpr auto MaxDigits = size_t { 3 };
        if (value.size() > MaxDigits)
            return std::nullopt;
        return crispy::toInteger<10, unsigned>(value);
    }
} // namespace

std::expected<Progress, ProgressError> applyProgressSequence(Progress current, std::string_view payload)
{
    // `4;<state>[;<progress>]`. The leading "4" is what selected this handler, so it must be there,
    // and a bare `OSC 9;4 ST` is rejected rather than treated as a clear: the sequence says nothing,
    // and guessing would withdraw an indicator the application still wants shown.
    if (!payload.starts_with("4;"))
        return std::unexpected(ProgressError::MalformedPayload);
    payload.remove_prefix(2);

    auto const separator = payload.find(';');
    auto const stateField = payload.substr(0, separator);
    auto const progressField =
        separator != std::string_view::npos ? payload.substr(separator + 1) : std::string_view {};

    auto const stateNumber = parseField(stateField);
    if (!stateNumber || *stateNumber > std::to_underlying(ProgressState::Paused))
        return std::unexpected(ProgressError::UnknownState);
    auto const state = static_cast<ProgressState>(*stateNumber);

    // An empty field is "absent" rather than malformed, so `OSC 9;4;1;` reads as a missing value.
    auto supplied = std::optional<uint8_t> {};
    if (!progressField.empty())
    {
        auto const parsed = parseField(progressField);
        if (!parsed)
            return std::unexpected(ProgressError::MalformedProgress);
        // Clamped rather than rejected: an application overshooting 100 still means "finished", and
        // refusing the sequence would leave a stale bar on screen instead.
        supplied = static_cast<uint8_t>(std::min(*parsed, unsigned { MaxProgressPercentage }));
    }

    // What each state does to the percentage already in effect. The protocol is deliberately not
    // uniform about it -- @see applyProgressSequence's declaration for why.
    switch (state)
    {
        case ProgressState::Inactive: return Progress {};
        case ProgressState::Normal: return Progress { .state = state, .percentage = supplied.value_or(0) };
        case ProgressState::Error:
        case ProgressState::Paused:
            return Progress { .state = state, .percentage = supplied.value_or(current.percentage) };
        case ProgressState::Indeterminate:
            return Progress { .state = state, .percentage = current.percentage };
    }
    crispy::unreachable();
}

} // namespace vtbackend
