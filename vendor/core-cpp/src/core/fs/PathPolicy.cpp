// SPDX-License-Identifier: Apache-2.0
#include <core/fs/PathPolicy.hpp>

#include <array>
#include <string_view>

namespace core::fs
{

namespace
{

    using namespace std::string_view_literals;

    // A name's stem (what precedes its first dot, trailing spaces dropped) that Windows opens as a
    // device wherever it appears. The superscript digits are U+00B9, U+00B2 and U+00B3, which
    // Windows reads as COM1-3 and LPT1-3.
    constexpr auto WindowsReservedNames = std::to_array<std::string_view>({
        "CON",         "PRN",         "AUX",    "NUL",         "COM0",        "COM1",        "COM2",
        "COM3",        "COM4",        "COM5",   "COM6",        "COM7",        "COM8",        "COM9",
        "LPT0",        "LPT1",        "LPT2",   "LPT3",        "LPT4",        "LPT5",        "LPT6",
        "LPT7",        "LPT8",        "LPT9",   "COM\xc2\xb9", "COM\xc2\xb2", "COM\xc2\xb3", "LPT\xc2\xb9",
        "LPT\xc2\xb2", "LPT\xc2\xb3", "CONIN$", "CONOUT$",
    });

    // The characters Win32 refuses in a name: the control characters 1-31 and <>:"|?*. The separators
    // are not here; they separate.
    constexpr auto WindowsForbiddenCharacters =
        "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f"
        "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f"
        "<>:\"|?*"sv;

    constexpr auto PosixPolicy = PathPolicy {
        .separators = "/",
        .targetSeparators = "/",
        .reservedNames = {},
        .volumeSeparators = {},
        .forbiddenCharacters = {},
        .encoding = PathEncoding::Utf8Bytes,
        .caseRule = CaseRule::Sensitive,
        .trailing = TrailingDotsAndSpaces::Allowed,
    };

    constexpr auto WindowsPolicy = PathPolicy {
        .separators = "/\\",
        .targetSeparators = "/\\",
        .reservedNames = WindowsReservedNames,
        .volumeSeparators = ":",
        .forbiddenCharacters = WindowsForbiddenCharacters,
        .encoding = PathEncoding::Utf16,
        .caseRule = CaseRule::Insensitive,
        .trailing = TrailingDotsAndSpaces::Refused,
    };

} // namespace

PathPolicy const& posixPathPolicy() noexcept
{
    return PosixPolicy;
}

PathPolicy const& windowsPathPolicy() noexcept
{
    return WindowsPolicy;
}

bool isRootedLinkTarget(std::string_view target, PathPolicy const& policy) noexcept
{
    if (target.empty())
        return false;
    if (policy.targetSeparators.contains(target.front()))
        return true;
    auto const firstComponent = target.substr(0, target.find_first_of(policy.targetSeparators));
    return firstComponent.find_first_of(policy.volumeSeparators) != std::string_view::npos;
}

} // namespace core::fs
