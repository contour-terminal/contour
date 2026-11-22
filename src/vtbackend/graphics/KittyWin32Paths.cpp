// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/graphics/KittyWin32Paths.hpp>

#include <algorithm>
#include <array>

namespace vtbackend::kitty_graphics::win32
{

namespace
{
    /// The characters no component may hold: the stream separator, the wildcards and the quote, which Win32
    /// reads as syntax rather than as part of a name.
    constexpr auto ReservedCharacters = std::string_view { ":*?\"<>|" };

    /// The prefix of the NT object namespace, which Win32 passes through unnormalized: refused as a whole,
    /// not as a path without a drive. (`\\?\` and `\\.\` begin with two separators, as a share does.)
    constexpr auto NtNamespacePrefix = std::string_view { "\\??\\" };

    /// The device names Win32 maps whatever directory they appear in, with or without an extension; the
    /// superscript digits (`COM¹`) are devices too.
    constexpr auto DeviceNames = std::array<std::string_view, 32> {
        "con",         "prn",         "aux",         "nul",         "conin$", "conout$",     "com0",
        "com1",        "com2",        "com3",        "com4",        "com5",   "com6",        "com7",
        "com8",        "com9",        "lpt0",        "lpt1",        "lpt2",   "lpt3",        "lpt4",
        "lpt5",        "lpt6",        "lpt7",        "lpt8",        "lpt9",   "com\xC2\xB9", "com\xC2\xB2",
        "com\xC2\xB3", "lpt\xC2\xB9", "lpt\xC2\xB2", "lpt\xC2\xB3",
    };

    /// The drive types read; any other is refused (@see checkDriveType).
    constexpr auto ReadableDriveTypes = std::array<uint32_t, 2> { drive_type::Fixed, drive_type::RamDisk };

    /// The prefix of every final path Windows reports.
    constexpr auto FinalPathPrefix = std::wstring_view { L"\\\\?\\" };

    /// Which roles an attribute rule refuses.
    enum class RuleScope : uint8_t
    {
        Both = 0, ///< Directories on the way and the file alike.
        File = 1, ///< The file only.
    };

    /// One attribute rule: an entry carrying any of @c mask is refused as @c refusal.
    struct AttributeRule
    {
        uint32_t mask;
        RuleScope scope;
        MediaRefusal refusal;
    };

    /// The attribute rules, in the order they are asked.
    constexpr auto AttributeRules = std::array {
        AttributeRule { .mask = attribute::ReparsePoint,
                        .scope = RuleScope::Both,
                        .refusal = MediaRefusal::SymbolicLink },
        AttributeRule { .mask = attribute::Hidden | attribute::System,
                        .scope = RuleScope::Both,
                        .refusal = MediaRefusal::HiddenComponent },
        AttributeRule { .mask = attribute::Offline | attribute::RecallOnOpen | attribute::RecallOnDataAccess,
                        .scope = RuleScope::Both,
                        .refusal = MediaRefusal::Unreadable },
        AttributeRule { .mask = attribute::Directory | attribute::Device,
                        .scope = RuleScope::File,
                        .refusal = MediaRefusal::NotRegularFile },
    };

    [[nodiscard]] constexpr bool isSeparator(char c) noexcept
    {
        return c == '\\' || c == '/';
    }

    [[nodiscard]] constexpr char asciiLower(char c) noexcept
    {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }

    /// @return Whether @p text starts with a drive and a separator: `X:\` or `X:/`.
    template <typename Char>
    [[nodiscard]] constexpr bool isDriveAbsolute(std::basic_string_view<Char> text) noexcept
    {
        auto const letter = text.size() >= 3 ? text[0] : Char {};
        return text.size() >= 3
               && ((letter >= Char { 'a' } && letter <= Char { 'z' })
                   || (letter >= Char { 'A' } && letter <= Char { 'Z' }))
               && text[1] == Char { ':' } && (text[2] == Char { '\\' } || text[2] == Char { '/' });
    }

    /// @return Whether @p component, its extension and any spaces before it disregarded, is a device name.
    [[nodiscard]] bool isDeviceName(std::string_view component) noexcept
    {
        auto stem = component.substr(0, component.find('.'));
        while (stem.ends_with(' '))
            stem.remove_suffix(1);
        return std::ranges::any_of(DeviceNames, [stem](std::string_view device) {
            return std::ranges::equal(stem, device, [](char a, char b) { return asciiLower(a) == b; });
        });
    }
} // namespace

std::expected<void, MediaRefusal> checkComponent(std::string_view component) noexcept
{
    auto const isReservedCharacter = [](char c) {
        return static_cast<unsigned char>(c) < 0x20 || ReservedCharacters.contains(c);
    };
    if (component.empty() || component == "." || component == ".." || component.ends_with('.')
        || component.ends_with(' ') || std::ranges::any_of(component, isReservedCharacter)
        || isDeviceName(component))
        return std::unexpected(MediaRefusal::ReservedName);
    return {};
}

std::expected<void, MediaRefusal> checkPathSpelling(std::string_view path) noexcept
{
    if (path.size() >= 2 && isSeparator(path[0]) && isSeparator(path[1]))
        return std::unexpected(MediaRefusal::UnsupportedDrive);
    if (path.starts_with(NtNamespacePrefix))
        return std::unexpected(MediaRefusal::UnsupportedDrive);
    if (!isDriveAbsolute(path))
        return std::unexpected(MediaRefusal::OutsideRoots);
    auto rest = path.substr(3);
    while (!rest.empty())
    {
        auto const end = rest.find_first_of("\\/");
        if (auto const component = rest.substr(0, end); !component.empty())
            if (auto const accepted = checkComponent(component); !accepted)
                return accepted;
        if (end == std::string_view::npos)
            break;
        rest.remove_prefix(end + 1);
    }
    return {};
}

std::expected<void, MediaRefusal> checkDriveType(uint32_t driveType) noexcept
{
    if (std::ranges::contains(ReadableDriveTypes, driveType))
        return {};
    return std::unexpected(MediaRefusal::UnsupportedDrive);
}

std::expected<std::wstring, MediaRefusal> drivePathOfFinalPath(std::wstring_view finalPath)
{
    if (!finalPath.starts_with(FinalPathPrefix))
        return std::unexpected(MediaRefusal::UnsupportedDrive);
    finalPath.remove_prefix(FinalPathPrefix.size());
    if (!isDriveAbsolute(finalPath))
        return std::unexpected(MediaRefusal::UnsupportedDrive);
    return std::wstring { finalPath };
}

std::expected<void, MediaRefusal> checkAttributes(uint32_t attributes, EntryRole role) noexcept
{
    for (auto const& rule: AttributeRules)
        if ((attributes & rule.mask) != 0 && (rule.scope == RuleScope::Both || role == EntryRole::File))
            return std::unexpected(rule.refusal);
    if (role == EntryRole::Directory && (attributes & attribute::Directory) == 0)
        return std::unexpected(MediaRefusal::NotRegularFile);
    return {};
}

} // namespace vtbackend::kitty_graphics::win32
