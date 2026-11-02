// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/UserVar.hpp>

#include <vtbackend/vt/Base64Validation.hpp>

#include <core/Base64.hpp>

#include <algorithm>

namespace vtbackend
{

std::string_view toString(UserVarRejection rejection) noexcept
{
    switch (rejection)
    {
        case UserVarRejection::MissingSeparator: return "missing '=' after the name";
        case UserVarRejection::InvalidName: return "invalid name";
        case UserVarRejection::InvalidValue: return "value is not valid base64";
        case UserVarRejection::ValueTooLong: return "value is too long";
    }
    return "unknown";
}

std::expected<UserVarAssignment, UserVarRejection> decodeUserVar(std::string_view argument,
                                                                 UserVarLimits const& limits)
{
    auto const separator = argument.find('=');
    if (separator == std::string_view::npos)
        return std::unexpected(UserVarRejection::MissingSeparator);

    auto const name = argument.substr(0, separator);
    // Printable ASCII only: no space, no control (C0, DEL or C1 as UTF-8) and nothing a locale could
    // redefine.
    constexpr auto IsNameByte = [](char ch) {
        return ch >= '\x21' && ch <= '\x7E';
    };
    if (name.empty() || name.size() > limits.maxNameBytes || !std::ranges::all_of(name, IsNameByte))
        return std::unexpected(UserVarRejection::InvalidName);

    auto const encoded = argument.substr(separator + 1);
    auto const digits = base64DigitCount(encoded);
    if (!digits)
        return std::unexpected(UserVarRejection::InvalidValue);

    // The decoded size is known from the digits alone, so an oversize value is refused undecoded.
    if (*digits * 3 / 4 > limits.maxValueBytes)
        return std::unexpected(UserVarRejection::ValueTooLong);

    return UserVarAssignment { .name = std::string { name }, .value = core::base64::decode(encoded) };
}

} // namespace vtbackend
