// SPDX-License-Identifier: Apache-2.0
//
// The Windows spelling, drive and attribute rules of the kitty file-medium policy: pure, so they run on every
// platform.

#include <vtbackend/graphics/KittyWin32Paths.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <optional>
#include <string>

using namespace std::string_view_literals;
using namespace vtbackend::kitty_graphics;
namespace attribute = vtbackend::kitty_graphics::win32::attribute;
using vtbackend::kitty_graphics::win32::EntryRole;

namespace
{
/// One row of a spelling table: what a rule answers for a text.
struct SpellingRow
{
    std::string_view text;
    std::optional<MediaRefusal> refusal; ///< nullopt: accepted.
};

void checkVerdict(std::expected<void, MediaRefusal> const& verdict, std::optional<MediaRefusal> refusal)
{
    if (refusal)
    {
        REQUIRE_FALSE(verdict.has_value());
        CHECK(verdict.error() == *refusal);
    }
    else
        CHECK(verdict.has_value());
}
} // namespace

TEST_CASE("KittyWin32Paths.checkPathSpelling", "[kitty][media]")
{
    auto const row = GENERATE(
        SpellingRow { "C:\\Users\\u\\AppData\\Local\\Temp\\tty-graphics-protocol-a.png", std::nullopt },
        SpellingRow { "c:/Users/u/AppData/Local/Temp/a.png", std::nullopt },
        SpellingRow { "C:\\Users\\JOHNDO~1\\AppData\\Local\\Temp\\a.png", std::nullopt },
        SpellingRow { "C:\\Users\\u\\.claude\\plugins\\data\\a.png", std::nullopt },
        SpellingRow { "C:\\Temp\\\\double\\a.png", std::nullopt },
        // Shares and the namespaces beside the drive letters.
        SpellingRow { "\\\\server\\share\\a.png", MediaRefusal::UnsupportedDrive },
        SpellingRow { "//server/share/a.png", MediaRefusal::UnsupportedDrive },
        SpellingRow { "\\\\?\\C:\\Temp\\a.png", MediaRefusal::UnsupportedDrive },
        SpellingRow { "\\\\?\\UNC\\server\\share\\a.png", MediaRefusal::UnsupportedDrive },
        SpellingRow { "\\\\.\\PhysicalDrive0", MediaRefusal::UnsupportedDrive },
        SpellingRow { "\\??\\C:\\Temp\\a.png", MediaRefusal::UnsupportedDrive },
        // Not drive-absolute: relative, drive-relative, rooted without a drive, or a WSL/MSYS POSIX path.
        SpellingRow { "Temp\\a.png", MediaRefusal::OutsideRoots },
        SpellingRow { "C:Temp\\a.png", MediaRefusal::OutsideRoots },
        SpellingRow { "\\Temp\\a.png", MediaRefusal::OutsideRoots },
        SpellingRow { "/tmp/tty-graphics-protocol-a.png", MediaRefusal::OutsideRoots },
        SpellingRow { "/mnt/c/Users/u/AppData/Local/Temp/a.png", MediaRefusal::OutsideRoots },
        SpellingRow { "1:\\Temp\\a.png", MediaRefusal::OutsideRoots },
        // A component Windows reads as something else.
        SpellingRow { "C:\\Temp\\a.png:secret", MediaRefusal::ReservedName },
        SpellingRow { "C:\\Temp\\..\\Windows\\a.png", MediaRefusal::ReservedName },
        SpellingRow { "C:\\Temp\\.\\a.png", MediaRefusal::ReservedName },
        SpellingRow { "C:\\Temp\\CON", MediaRefusal::ReservedName },
        SpellingRow { "C:\\Temp\\a*.png", MediaRefusal::ReservedName },
        SpellingRow { "C:\\Temp\\a.png.", MediaRefusal::ReservedName });
    INFO(row.text);
    checkVerdict(win32::checkPathSpelling(row.text), row.refusal);
}

TEST_CASE("KittyWin32Paths.checkComponent", "[kitty][media]")
{
    auto const row = GENERATE(SpellingRow { "a.png", std::nullopt },
                              SpellingRow { ".hidden", std::nullopt },
                              SpellingRow { "b\xC3\xBCro", std::nullopt },
                              SpellingRow { "console", std::nullopt },
                              SpellingRow { "com10", std::nullopt },
                              SpellingRow { "nul.png.bak", MediaRefusal::ReservedName },
                              SpellingRow { "", MediaRefusal::ReservedName },
                              SpellingRow { ".", MediaRefusal::ReservedName },
                              SpellingRow { "..", MediaRefusal::ReservedName },
                              SpellingRow { "...", MediaRefusal::ReservedName },
                              SpellingRow { "a.png ", MediaRefusal::ReservedName },
                              SpellingRow { "a:b", MediaRefusal::ReservedName },
                              SpellingRow { "a?", MediaRefusal::ReservedName },
                              SpellingRow { "a\"b", MediaRefusal::ReservedName },
                              SpellingRow { "a<b", MediaRefusal::ReservedName },
                              SpellingRow { "a>b", MediaRefusal::ReservedName },
                              SpellingRow { "a|b", MediaRefusal::ReservedName },
                              SpellingRow { "a\x01"
                                            "b",
                                            MediaRefusal::ReservedName },
                              SpellingRow { "CON", MediaRefusal::ReservedName },
                              SpellingRow { "con.txt", MediaRefusal::ReservedName },
                              SpellingRow { "Nul", MediaRefusal::ReservedName },
                              SpellingRow { "PRN", MediaRefusal::ReservedName },
                              SpellingRow { "aux.png", MediaRefusal::ReservedName },
                              SpellingRow { "COM1", MediaRefusal::ReservedName },
                              SpellingRow { "lpt9.log", MediaRefusal::ReservedName },
                              SpellingRow { "COM0", MediaRefusal::ReservedName },
                              SpellingRow { "COM\xC2\xB9", MediaRefusal::ReservedName },
                              SpellingRow { "CONIN$", MediaRefusal::ReservedName },
                              SpellingRow { "conout$.x", MediaRefusal::ReservedName },
                              SpellingRow { "CON .txt", MediaRefusal::ReservedName });
    INFO(row.text);
    checkVerdict(win32::checkComponent(row.text), row.refusal);
}

TEST_CASE("KittyWin32Paths.checkDriveType", "[kitty][media]")
{
    // GetDriveTypeW: DRIVE_UNKNOWN, NO_ROOT_DIR, REMOVABLE, FIXED, REMOTE, CDROM, RAMDISK.
    struct DriveRow
    {
        uint32_t type;
        std::optional<MediaRefusal> refusal;
    };
    auto const row = GENERATE(DriveRow { 0, MediaRefusal::UnsupportedDrive },
                              DriveRow { 1, MediaRefusal::UnsupportedDrive },
                              DriveRow { 2, MediaRefusal::UnsupportedDrive },
                              DriveRow { win32::drive_type::Fixed, std::nullopt },
                              DriveRow { 4, MediaRefusal::UnsupportedDrive },
                              DriveRow { 5, MediaRefusal::UnsupportedDrive },
                              DriveRow { win32::drive_type::RamDisk, std::nullopt });
    INFO(row.type);
    checkVerdict(win32::checkDriveType(row.type), row.refusal);
}

TEST_CASE("KittyWin32Paths.drivePathOfFinalPath", "[kitty][media]")
{
    auto const accepted = win32::drivePathOfFinalPath(L"\\\\?\\C:\\Users\\u\\AppData\\Local\\Temp\\a.png");
    REQUIRE(accepted.has_value());
    CHECK(*accepted == L"C:\\Users\\u\\AppData\\Local\\Temp\\a.png");

    auto const refused = GENERATE(L"\\\\?\\UNC\\server\\share\\a.png"sv,
                                  L"\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}\\a.png"sv,
                                  L"C:\\Temp\\a.png"sv,
                                  L""sv);
    auto const verdict = win32::drivePathOfFinalPath(refused);
    REQUIRE_FALSE(verdict.has_value());
    CHECK(verdict.error() == MediaRefusal::UnsupportedDrive);
}

TEST_CASE("KittyWin32Paths.checkAttributes", "[kitty][media]")
{
    struct AttributeRow
    {
        uint32_t attributes;
        EntryRole role;
        std::optional<MediaRefusal> refusal;
    };
    constexpr auto Archive = uint32_t { 0b0010'0000 }; // FILE_ATTRIBUTE_ARCHIVE: no rule's concern.
    constexpr auto ReadOnly = uint32_t { 0b0000'0001 };
    auto const row = GENERATE_COPY(
        AttributeRow { Archive, EntryRole::File, std::nullopt },
        AttributeRow { ReadOnly, EntryRole::File, std::nullopt },
        AttributeRow { attribute::Directory, EntryRole::Directory, std::nullopt },
        AttributeRow { attribute::Directory | ReadOnly, EntryRole::Directory, std::nullopt },
        AttributeRow { Archive, EntryRole::Directory, MediaRefusal::NotRegularFile },
        AttributeRow { attribute::Directory, EntryRole::File, MediaRefusal::NotRegularFile },
        AttributeRow { attribute::Device, EntryRole::File, MediaRefusal::NotRegularFile },
        AttributeRow { attribute::ReparsePoint, EntryRole::File, MediaRefusal::SymbolicLink },
        AttributeRow { attribute::ReparsePoint | attribute::Directory,
                       EntryRole::Directory,
                       MediaRefusal::SymbolicLink },
        AttributeRow { attribute::Hidden, EntryRole::File, MediaRefusal::HiddenComponent },
        AttributeRow {
            attribute::System | attribute::Directory, EntryRole::Directory, MediaRefusal::HiddenComponent },
        AttributeRow { attribute::Offline, EntryRole::File, MediaRefusal::Unreadable },
        AttributeRow {
            attribute::RecallOnOpen | attribute::Directory, EntryRole::Directory, MediaRefusal::Unreadable },
        AttributeRow { attribute::RecallOnDataAccess, EntryRole::File, MediaRefusal::Unreadable },
        // The first rule that matches decides: a hidden junction is refused as a link.
        AttributeRow {
            attribute::ReparsePoint | attribute::Hidden, EntryRole::File, MediaRefusal::SymbolicLink });
    INFO(row.attributes);
    checkVerdict(win32::checkAttributes(row.attributes, row.role), row.refusal);
}
