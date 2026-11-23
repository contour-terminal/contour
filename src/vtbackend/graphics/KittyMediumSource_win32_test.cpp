// SPDX-License-Identifier: Apache-2.0
//
// The Windows kitty medium source against real files: every file is created below a fresh directory of its
// own under the system temp directory, which the policy names as its only root, and removed afterwards.
//
// The `[win32]` cases need no privilege, and CI fails when any of them is skipped. Those that need one --
// a symbolic link wants SeCreateSymbolicLinkPrivilege or Developer Mode -- are `[win32-privileged]`, and
// short (8.3) names, which a volume may have switched off, `[win32-shortnames]`: they SKIP, saying why,
// where the machine cannot set them up.

#include <vtbackend/graphics/KittyMediumSource.hpp>

#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32

    #include <core/Utf8Path.hpp>
    #include <core/testing/ScopedTempDir.hpp>

    #include <algorithm>
    #include <array>
    #include <cstddef>
    #include <cstdlib>
    #include <filesystem>
    #include <format>
    #include <fstream>
    #include <string>
    #include <system_error>
    #include <vector>

    #include <Windows.h>

using namespace vtbackend::kitty_graphics;

namespace
{
/// A fresh directory below the system temp directory, removed with everything in it on destruction.
class TestDirectory
{
  public:
    TestDirectory():
        _directory { "contour-media-test" },
        // Canonical, as the source compares final paths: the temp directory is often spelled in 8.3 names.
        _path { std::filesystem::canonical(_directory.path()) }
    {
    }

    [[nodiscard]] std::filesystem::path const& path() const noexcept { return _path; }

    /// Writes @p bytes to the file @p name (UTF-8) below this directory.
    /// @return The file's path.
    [[nodiscard]] std::filesystem::path write(std::string_view name, std::string_view bytes) const
    {
        auto const file = _path / core::pathFromTrustedUtf8(name);
        std::filesystem::create_directories(file.parent_path());
        auto stream = std::ofstream { file, std::ios::binary };
        stream << bytes;
        return file;
    }

  private:
    core::testing::ScopedTempDir _directory;
    std::filesystem::path _path;
};

/// The policy of a test: @p root the only temp root, a 16-byte size limit, paths compared as Windows does.
[[nodiscard]] MediaPolicy policyFor(TestDirectory const& root)
{
    return MediaPolicy { .transmission = FileTransmission::Enabled,
                         .temporaryRoots = { root.path() },
                         .temporaryFileOnlyRoots = {},
                         .allowedRoots = {},
                         .sizeLimit = 16,
                         .pathCase = PathCase::Insensitive };
}

/// @return The source of a test, reading as this process's own user.
[[nodiscard]] Win32MediumSource sourceOf(MediumReadInterleavings interleavings = {})
{
    auto readers = currentReaderSids();
    REQUIRE(readers.has_value());
    return Win32MediumSource { std::move(*readers), std::move(interleavings) };
}

/// @return @p path as a command names it: UTF-8.
[[nodiscard]] std::string requestOf(std::filesystem::path const& path)
{
    return core::pathToUtf8(path);
}

[[nodiscard]] std::vector<uint8_t> bytesOf(std::string_view text)
{
    return { text.begin(), text.end() };
}

/// Checks that @p result is the refusal @p refusal.
void checkRefused(std::expected<std::vector<uint8_t>, MediaRefusal> const& result, MediaRefusal refusal)
{
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == refusal);
}

/// Makes @p link a junction to the directory @p target, as `mklink /J` does: no privilege needed.
void createJunction(std::filesystem::path const& link, std::filesystem::path const& target)
{
    auto const command =
        std::format(L"cmd.exe /d /c mklink /J \"{}\" \"{}\" >NUL", link.native(), target.native());
    REQUIRE(_wsystem(command.c_str()) == 0);
    REQUIRE(std::filesystem::exists(link));
}

/// Adds @p attributes to those of @p path.
void addAttributes(std::filesystem::path const& path, DWORD attributes)
{
    auto const current = GetFileAttributesW(path.c_str());
    REQUIRE(current != INVALID_FILE_ATTRIBUTES);
    REQUIRE(SetFileAttributesW(path.c_str(), current | attributes) != FALSE);
}
} // namespace

TEST_CASE("KittyMediumSource.win32.reads_a_regular_file", "[kitty][media][win32]")
{
    auto const root = TestDirectory {};
    auto const file = root.write("tty-graphics-protocol-a.rgba", "RGBA");
    auto const source = sourceOf();

    SECTION("t=f keeps it")
    {
        auto const bytes = source.read(requestOf(file), Medium::File, policyFor(root));
        REQUIRE(bytes.has_value());
        CHECK(*bytes == bytesOf("RGBA"));
        CHECK(std::filesystem::exists(file));
    }

    SECTION("t=t deletes it")
    {
        auto const bytes = source.read(requestOf(file), Medium::TemporaryFile, policyFor(root));
        REQUIRE(bytes.has_value());
        CHECK(*bytes == bytesOf("RGBA"));
        CHECK_FALSE(std::filesystem::exists(file));
    }

    SECTION("t=t deletes a read-only one too")
    {
        addAttributes(file, FILE_ATTRIBUTE_READONLY);
        REQUIRE(source.read(requestOf(file), Medium::TemporaryFile, policyFor(root)).has_value());
        CHECK_FALSE(std::filesystem::exists(file));
    }

    SECTION("t=t keeps a file without the marker")
    {
        auto const plain = root.write("a.rgba", "RGBA");
        REQUIRE(source.read(requestOf(plain), Medium::TemporaryFile, policyFor(root)).has_value());
        CHECK(std::filesystem::exists(plain));
    }

    SECTION("spelled with forward slashes and in another case")
    {
        auto spelling = requestOf(file);
        std::ranges::replace(spelling, '\\', '/');
        std::ranges::transform(spelling, spelling.begin(), [](char c) {
            return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        });
        auto const bytes = source.read(spelling, Medium::File, policyFor(root));
        REQUIRE(bytes.has_value());
        CHECK(*bytes == bytesOf("RGBA"));
    }

    SECTION("a name beyond ASCII, as UTF-8")
    {
        auto const named = root.write("tty-graphics-protocol-b\xC3\xBCro.rgba", "UTF8");
        auto const bytes = source.read(requestOf(named), Medium::File, policyFor(root));
        REQUIRE(bytes.has_value());
        CHECK(*bytes == bytesOf("UTF8"));
    }

    SECTION("in a directory below the root")
    {
        auto const nested = root.write("d\\e\\tty-graphics-protocol-n.rgba", "NEST");
        auto const bytes = source.read(requestOf(nested), Medium::TemporaryFile, policyFor(root));
        REQUIRE(bytes.has_value());
        CHECK_FALSE(std::filesystem::exists(nested));
    }
}

TEST_CASE("KittyMediumSource.win32.refusals", "[kitty][media][win32]")
{
    auto const root = TestDirectory {};
    auto const source = sourceOf();
    auto const policy = policyFor(root);

    SECTION("disabled touches nothing")
    {
        auto const file = root.write("tty-graphics-protocol-a", "RGBA");
        auto disabled = policy;
        disabled.transmission = FileTransmission::Disabled;
        checkRefused(source.read(requestOf(file), Medium::TemporaryFile, disabled), MediaRefusal::Disabled);
        CHECK(std::filesystem::exists(file));
    }

    SECTION("a hard link")
    {
        auto const file = root.write("tty-graphics-protocol-a", "RGBA");
        auto const link = root.path() / "tty-graphics-protocol-b";
        REQUIRE(CreateHardLinkW(link.c_str(), file.c_str(), nullptr) != FALSE);
        checkRefused(source.read(requestOf(file), Medium::File, policy), MediaRefusal::MultipleLinks);
    }

    SECTION("a file over the size limit")
    {
        auto const file = root.write("tty-graphics-protocol-a", std::string(17, 'x'));
        checkRefused(source.read(requestOf(file), Medium::TemporaryFile, policy), MediaRefusal::TooLarge);
        CHECK(std::filesystem::exists(file));
    }

    SECTION("a file that does not exist")
    {
        checkRefused(source.read(requestOf(root.path() / "tty-graphics-protocol-gone"), Medium::File, policy),
                     MediaRefusal::Unreadable);
    }

    SECTION("a directory")
    {
        std::filesystem::create_directory(root.path() / "tty-graphics-protocol-dir");
        checkRefused(source.read(requestOf(root.path() / "tty-graphics-protocol-dir"), Medium::File, policy),
                     MediaRefusal::NotRegularFile);
    }

    SECTION("a dot-named directory below the root")
    {
        auto const file = root.write(".hidden\\tty-graphics-protocol-a", "RGBA");
        checkRefused(source.read(requestOf(file), Medium::File, policy), MediaRefusal::HiddenComponent);
    }

    SECTION("a hidden or system file, or one in such a directory")
    {
        auto const attributes = std::array<DWORD, 2> { FILE_ATTRIBUTE_HIDDEN, FILE_ATTRIBUTE_SYSTEM };
        for (auto const attribute: attributes)
        {
            auto const file = root.write(std::format("f{}\\tty-graphics-protocol-a", attribute), "RGBA");
            addAttributes(file, attribute);
            checkRefused(source.read(requestOf(file), Medium::TemporaryFile, policy),
                         MediaRefusal::HiddenComponent);
            CHECK(std::filesystem::exists(file));

            auto const inside = root.write(std::format("d{}\\tty-graphics-protocol-a", attribute), "RGBA");
            addAttributes(inside.parent_path(), attribute);
            checkRefused(source.read(requestOf(inside), Medium::File, policy), MediaRefusal::HiddenComponent);
        }
    }

    SECTION("a file another process holds open for writing")
    {
        auto const file = root.write("tty-graphics-protocol-a", "RGBA");
        auto writer = std::ofstream { file, std::ios::binary | std::ios::app };
        REQUIRE(writer.good());
        checkRefused(source.read(requestOf(file), Medium::File, policy), MediaRefusal::Unreadable);
    }

    SECTION("a file owned by another user")
    {
        // LocalSystem (S-1-5-18) alone reads here: the file the test made belongs to the test's user.
        auto const file = root.write("tty-graphics-protocol-a", "RGBA");
        auto const system =
            Win32MediumSource::Sid { std::byte { 1 },  std::byte { 1 }, std::byte { 0 }, std::byte { 0 },
                                     std::byte { 0 },  std::byte { 0 }, std::byte { 0 }, std::byte { 5 },
                                     std::byte { 18 }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 } };
        auto const other = Win32MediumSource { { system } };
        checkRefused(other.read(requestOf(file), Medium::File, policy), MediaRefusal::WrongOwner);
    }
}

TEST_CASE("KittyMediumSource.win32.spellings", "[kitty][media][win32]")
{
    // Refused as spelled, before anything is opened.
    auto const root = TestDirectory {};
    auto const source = sourceOf();
    auto const policy = policyFor(root);
    auto const file = root.write("tty-graphics-protocol-a", "RGBA");
    auto const rooted = requestOf(root.path());

    struct Row
    {
        std::string path;
        MediaRefusal refusal;
    };
    auto const rows = std::array {
        // An alternate data stream of a file the policy would read.
        Row { rooted + "\\tty-graphics-protocol-a:hidden", MediaRefusal::ReservedName },
        Row { rooted + "\\tty-graphics-protocol-a::$DATA", MediaRefusal::ReservedName },
        Row { rooted + "\\CON", MediaRefusal::ReservedName },
        Row { rooted + "\\nul.png", MediaRefusal::ReservedName },
        Row { rooted + "\\tty-graphics-protocol-a.", MediaRefusal::ReservedName },
        Row { rooted + "\\tty-graphics-protocol-a ", MediaRefusal::ReservedName },
        Row { rooted + "\\d\\..\\tty-graphics-protocol-a", MediaRefusal::ReservedName },
        Row { "\\\\?\\" + rooted + "\\tty-graphics-protocol-a", MediaRefusal::OutsideRoots },
        Row { "\\\\localhost\\C$\\Windows\\win.ini", MediaRefusal::OutsideRoots },
        Row { "/tmp/tty-graphics-protocol-a", MediaRefusal::OutsideRoots },
        Row { "tty-graphics-protocol-a", MediaRefusal::OutsideRoots },
    };
    for (auto const& row: rows)
    {
        INFO(row.path);
        checkRefused(source.read(row.path, Medium::File, policy), row.refusal);
    }
    CHECK(std::filesystem::exists(file));
}

TEST_CASE("KittyMediumSource.win32.a_junction_below_the_root_is_never_followed", "[kitty][media][win32]")
{
    auto const root = TestDirectory {};
    auto const outside = TestDirectory {};
    auto const secret = outside.write("tty-graphics-protocol-a", "KEY");
    auto const source = sourceOf();

    SECTION("to a directory outside the root")
    {
        createJunction(root.path() / "j", outside.path());
        checkRefused(source.read(requestOf(root.path() / "j" / "tty-graphics-protocol-a"),
                                 Medium::TemporaryFile,
                                 policyFor(root)),
                     MediaRefusal::SymbolicLink);
        CHECK(std::filesystem::exists(secret));
    }

    SECTION("even back into the root")
    {
        auto const file = root.write("tty-graphics-protocol-a", "RGBA");
        createJunction(root.path() / "alias", root.path());
        checkRefused(source.read(requestOf(root.path() / "alias" / "tty-graphics-protocol-a"),
                                 Medium::File,
                                 policyFor(root)),
                     MediaRefusal::SymbolicLink);
    }

    SECTION("named as the file")
    {
        createJunction(root.path() / "tty-graphics-protocol-j", outside.path());
        checkRefused(
            source.read(requestOf(root.path() / "tty-graphics-protocol-j"), Medium::File, policyFor(root)),
            MediaRefusal::SymbolicLink);
    }
}

TEST_CASE("KittyMediumSource.win32.swaps_after_the_path_check", "[kitty][media][win32]")
{
    // The path policy judged one file; whatever is put in its place before the open is refused, not read.
    auto const root = TestDirectory {};
    auto const outside = TestDirectory {};
    auto const secret = outside.write("dir\\tty-graphics-protocol-a", "KEY");
    auto const policy = policyFor(root);

    SECTION("the file swapped for a junction")
    {
        auto const file = root.write("tty-graphics-protocol-a", "RGBA");
        auto const source = sourceOf({ .pathAccepted = [&](std::filesystem::path const&) {
            std::filesystem::remove(file);
            createJunction(file, secret.parent_path());
        } });
        checkRefused(source.read(requestOf(file), Medium::TemporaryFile, policy), MediaRefusal::SymbolicLink);
        CHECK(std::filesystem::exists(secret));
    }

    SECTION("a directory above it swapped for a junction")
    {
        auto const file = root.write("dir\\tty-graphics-protocol-a", "RGBA");
        auto const source = sourceOf({ .pathAccepted = [&](std::filesystem::path const&) {
            std::filesystem::rename(root.path() / "dir", root.path() / "moved");
            createJunction(root.path() / "dir", secret.parent_path());
        } });
        checkRefused(source.read(requestOf(file), Medium::TemporaryFile, policy), MediaRefusal::SymbolicLink);
        CHECK(std::filesystem::exists(secret));
    }
}

TEST_CASE("KittyMediumSource.win32.nothing_on_the_way_moves_before_the_delete", "[kitty][media][win32]")
{
    // POSIX checks that the directory is still where it was before it unlinks; Windows holds every directory
    // on the way, and the file, without sharing their deletion: none of them can be moved meanwhile, and
    // the file read is deleted through the handle that read it.
    auto const root = TestDirectory {};
    auto const outside = TestDirectory {};
    auto const file = root.write("dir\\tty-graphics-protocol-a", "RGBA");
    auto moved = std::error_code {};
    auto renamed = std::error_code {};
    auto const source = sourceOf({ .beforeDelete = [&](std::filesystem::path const&) {
        std::filesystem::rename(root.path() / "dir", outside.path() / "dir", moved);
        std::filesystem::rename(file, root.path() / "dir" / "elsewhere", renamed);
    } });

    REQUIRE(source.read(requestOf(file), Medium::TemporaryFile, policyFor(root)).has_value());
    CHECK(moved);
    CHECK(renamed);
    CHECK_FALSE(std::filesystem::exists(file));
    CHECK_FALSE(std::filesystem::exists(outside.path() / "dir"));
}

TEST_CASE("KittyMediumSource.win32.a_root_spelled_through_a_junction", "[kitty][media][win32]")
{
    // As `%TEMP%` may be spelled: the root's own link is followed, and the real path is what the policy
    // judges.
    auto const root = TestDirectory {};
    auto const elsewhere = TestDirectory {};
    auto const spelling = elsewhere.path() / "spelled-root";
    createJunction(spelling, root.path());
    auto const file = root.write("tty-graphics-protocol-a", "RGBA");
    auto const requested = requestOf(spelling / "tty-graphics-protocol-a");
    auto const source = sourceOf();
    auto policy = policyFor(root);

    SECTION("read through the spelling, the canonical root beside it")
    {
        policy.temporaryRoots = { spelling, root.path() };
        auto const bytes = source.read(requested, Medium::File, policy);
        REQUIRE(bytes.has_value());
        CHECK(*bytes == bytesOf("RGBA"));
    }

    SECTION("the spelling alone does not admit where it leads")
    {
        policy.temporaryRoots = { spelling };
        checkRefused(source.read(requested, Medium::File, policy), MediaRefusal::OutsideRoots);
    }

    SECTION("t=t through the spelling deletes the file")
    {
        policy.temporaryRoots = { spelling, root.path() };
        REQUIRE(source.read(requested, Medium::TemporaryFile, policy).has_value());
        CHECK_FALSE(std::filesystem::exists(file));
    }
}

TEST_CASE("KittyMediumSource.win32.a_symbolic_link_below_the_root_is_never_followed",
          "[kitty][media][win32-privileged]")
{
    auto const root = TestDirectory {};
    auto const outside = TestDirectory {};
    auto const secret = outside.write("tty-graphics-protocol-a", "KEY");
    auto const source = sourceOf();

    auto error = std::error_code {};
    std::filesystem::create_symlink(secret, root.path() / "tty-graphics-protocol-link", error);
    if (error)
        SKIP("creating a symbolic link needs SeCreateSymbolicLinkPrivilege or Developer Mode: "
             << error.message());
    std::filesystem::create_directory_symlink(outside.path(), root.path() / "dlink");

    checkRefused(
        source.read(requestOf(root.path() / "tty-graphics-protocol-link"), Medium::File, policyFor(root)),
        MediaRefusal::SymbolicLink);
    checkRefused(source.read(requestOf(root.path() / "dlink" / "tty-graphics-protocol-a"),
                             Medium::TemporaryFile,
                             policyFor(root)),
                 MediaRefusal::SymbolicLink);
    CHECK(std::filesystem::exists(secret));
}

TEST_CASE("KittyMediumSource.win32.a_short_name_alias_is_refused", "[kitty][media][win32-shortnames]")
{
    // An 8.3 alias names the same file by another spelling than the one the policy judged.
    auto const root = TestDirectory {};
    auto const file = root.write("tty-graphics-protocol-long-name.rgba", "RGBA");
    auto shortName = std::wstring(MAX_PATH, L'\0');
    auto const length =
        GetShortPathNameW(file.c_str(), shortName.data(), static_cast<DWORD>(shortName.size()));
    REQUIRE(length > 0);
    shortName.resize(length);
    auto const alias = std::filesystem::path { shortName };
    if (alias.filename() == file.filename())
        SKIP("the volume of the temp directory makes no short (8.3) names");

    auto const policy = policyFor(root);
    checkRefused(sourceOf().read(requestOf(root.path() / alias.filename()), Medium::TemporaryFile, policy),
                 MediaRefusal::Unreadable);
    CHECK(std::filesystem::exists(file));
}

#endif
