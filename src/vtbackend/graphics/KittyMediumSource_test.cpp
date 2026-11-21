// SPDX-License-Identifier: Apache-2.0
//
// The POSIX kitty medium source against real files: every file is created below a fresh directory of its
// own under the system temp directory, which the policy names as its only root, and removed afterwards.

#include <vtbackend/graphics/KittyMediumSource.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#ifndef _WIN32

    #include <sys/stat.h>

    #include <filesystem>
    #include <fstream>
    #include <string>

    #include <unistd.h>

using namespace std::string_literals;
using namespace vtbackend::kitty_graphics;

namespace
{
/// A fresh directory below the system temp directory, removed with everything in it on destruction.
class TestDirectory
{
  public:
    TestDirectory()
    {
        auto pattern = (std::filesystem::temp_directory_path() / "contour-media-test-XXXXXX").string();
        REQUIRE(::mkdtemp(pattern.data()) != nullptr);
        // Canonical, as the source compares canonical paths: the temp directory may be a link (macOS /var).
        _path = std::filesystem::canonical(pattern);
    }
    ~TestDirectory()
    {
        auto error = std::error_code {};
        std::filesystem::remove_all(_path, error);
    }
    TestDirectory(TestDirectory const&) = delete;
    TestDirectory(TestDirectory&&) = delete;
    TestDirectory& operator=(TestDirectory const&) = delete;
    TestDirectory& operator=(TestDirectory&&) = delete;

    [[nodiscard]] std::filesystem::path const& path() const noexcept { return _path; }

    /// Writes @p bytes to the file @p name below this directory.
    /// @return The file's path.
    [[nodiscard]] std::filesystem::path write(std::filesystem::path const& name, std::string_view bytes) const
    {
        auto const file = _path / name;
        std::filesystem::create_directories(file.parent_path());
        auto stream = std::ofstream { file, std::ios::binary };
        stream << bytes;
        return file;
    }

  private:
    std::filesystem::path _path;
};

/// The policy of a test: @p root the only temp root, a 16-byte size limit.
[[nodiscard]] MediaPolicy policyFor(TestDirectory const& root)
{
    return MediaPolicy { .transmission = FileTransmission::Enabled,
                         .temporaryRoots = { root.path() },
                         .temporaryFileOnlyRoots = {},
                         .allowedRoots = {},
                         .userId = static_cast<uint32_t>(::getuid()),
                         .sizeLimit = 16 };
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
} // namespace

TEST_CASE("KittyMediumSource.posix.reads_a_regular_file", "[kitty][media]")
{
    auto const root = TestDirectory {};
    auto const file = root.write("tty-graphics-protocol-a.rgba", "RGBA");
    auto const source = PosixMediumSource {};

    SECTION("t=f keeps it")
    {
        auto const bytes = source.read(file.native(), Medium::File, policyFor(root));
        REQUIRE(bytes.has_value());
        CHECK(*bytes == bytesOf("RGBA"));
        CHECK(std::filesystem::exists(file));
    }

    SECTION("t=t deletes it")
    {
        auto const bytes = source.read(file.native(), Medium::TemporaryFile, policyFor(root));
        REQUIRE(bytes.has_value());
        CHECK(*bytes == bytesOf("RGBA"));
        CHECK_FALSE(std::filesystem::exists(file));
    }

    SECTION("a path through a link inside the root is refused, even back into the root")
    {
        std::filesystem::create_directory_symlink(root.path(), root.path() / "alias");
        checkRefused(source.read((root.path() / "alias" / "tty-graphics-protocol-a.rgba").native(),
                                 Medium::File,
                                 policyFor(root)),
                     MediaRefusal::SymbolicLink);
    }
}

TEST_CASE("KittyMediumSource.posix.refusals", "[kitty][media]")
{
    auto const root = TestDirectory {};
    auto const source = PosixMediumSource {};
    auto const policy = policyFor(root);

    SECTION("disabled touches nothing")
    {
        auto const file = root.write("tty-graphics-protocol-a", "RGBA");
        auto disabled = policy;
        disabled.transmission = FileTransmission::Disabled;
        checkRefused(source.read(file.native(), Medium::TemporaryFile, disabled), MediaRefusal::Disabled);
        CHECK(std::filesystem::exists(file));
    }

    SECTION("t=s")
    {
        checkRefused(source.read("tty-graphics-protocol-shm", Medium::SharedMemory, policy),
                     MediaRefusal::UnsupportedMedium);
    }

    SECTION("a symbolic link inside the root to a file outside it")
    {
        auto const outside = TestDirectory {};
        auto const target = outside.write("secret", "KEY");
        std::filesystem::create_symlink(target, root.path() / "tty-graphics-protocol-link");
        checkRefused(source.read((root.path() / "tty-graphics-protocol-link").native(), Medium::File, policy),
                     MediaRefusal::SymbolicLink);
        CHECK(std::filesystem::exists(target));
    }

    SECTION("a FIFO, without blocking")
    {
        auto const fifo = root.path() / "tty-graphics-protocol-fifo";
        REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
        checkRefused(source.read(fifo.native(), Medium::File, policy), MediaRefusal::NotRegularFile);
    }

    SECTION("a hard link")
    {
        auto const file = root.write("tty-graphics-protocol-a", "RGBA");
        REQUIRE(::link(file.c_str(), (root.path() / "tty-graphics-protocol-b").c_str()) == 0);
        checkRefused(source.read(file.native(), Medium::File, policy), MediaRefusal::MultipleLinks);
    }

    SECTION("a file over the size limit")
    {
        auto const file = root.write("tty-graphics-protocol-a", std::string(17, 'x'));
        checkRefused(source.read(file.native(), Medium::TemporaryFile, policy), MediaRefusal::TooLarge);
        CHECK(std::filesystem::exists(file));
    }

    SECTION("a file that does not exist")
    {
        checkRefused(source.read((root.path() / "tty-graphics-protocol-gone").native(), Medium::File, policy),
                     MediaRefusal::Unreadable);
    }

    SECTION("a hidden directory below the root")
    {
        auto const file = root.write(".hidden/tty-graphics-protocol-a", "RGBA");
        checkRefused(source.read(file.native(), Medium::File, policy), MediaRefusal::HiddenComponent);
    }

    SECTION("a directory")
    {
        std::filesystem::create_directory(root.path() / "tty-graphics-protocol-dir");
        checkRefused(source.read((root.path() / "tty-graphics-protocol-dir").native(), Medium::File, policy),
                     MediaRefusal::NotRegularFile);
    }
}

TEST_CASE("KittyMediumSource.posix.swaps_after_the_path_check", "[kitty][media]")
{
    // The path policy judged one file; whatever is put in its place before the open is refused, not read.
    auto const root = TestDirectory {};
    auto const outside = TestDirectory {};
    auto const secret = outside.write("dir/tty-graphics-protocol-a", "KEY");
    auto const policy = policyFor(root);

    SECTION("the file swapped for a symbolic link (O_NOFOLLOW)")
    {
        auto const file = root.write("tty-graphics-protocol-a", "RGBA");
        auto const source =
            PosixMediumSource { { .pathAccepted = [&](std::filesystem::path const& canonical) {
                std::filesystem::remove(canonical);
                std::filesystem::create_symlink(secret, canonical);
            } } };
        checkRefused(source.read(file.native(), Medium::TemporaryFile, policy), MediaRefusal::SymbolicLink);
        CHECK(std::filesystem::exists(secret));
    }

    SECTION("a directory above it swapped for a symbolic link")
    {
        auto const file = root.write("dir/tty-graphics-protocol-a", "RGBA");
        auto const source = PosixMediumSource { { .pathAccepted = [&](std::filesystem::path const&) {
            std::filesystem::rename(root.path() / "dir", root.path() / "moved");
            std::filesystem::create_directory_symlink(secret.parent_path(), root.path() / "dir");
        } } };
        checkRefused(source.read(file.native(), Medium::TemporaryFile, policy), MediaRefusal::SymbolicLink);
        CHECK(std::filesystem::exists(secret));
    }
}

TEST_CASE("KittyMediumSource.posix.a_link_below_the_root_is_never_followed", "[kitty][media]")
{
    // Following it would look its target up -- an automount, a network filesystem -- under the terminal lock.
    // Refused as a link, whatever it points at, existing or not: its target is never resolved.
    auto const root = TestDirectory {};
    auto const source = PosixMediumSource {};
    auto const target = GENERATE(std::filesystem::path { "/net/example.invalid/tty-graphics-protocol-a" },
                                 std::filesystem::path { "/nonexistent-contour-media-test/dir" });
    INFO(target);

    SECTION("as the file")
    {
        std::filesystem::create_symlink(target, root.path() / "tty-graphics-protocol-n");
        checkRefused(
            source.read((root.path() / "tty-graphics-protocol-n").native(), Medium::File, policyFor(root)),
            MediaRefusal::SymbolicLink);
    }

    SECTION("as a directory on the way")
    {
        std::filesystem::create_directory_symlink(target, root.path() / "n");
        checkRefused(source.read((root.path() / "n" / "tty-graphics-protocol-a").native(),
                                 Medium::File,
                                 policyFor(root)),
                     MediaRefusal::SymbolicLink);
    }
}

TEST_CASE("KittyMediumSource.posix.a_swap_before_the_unlink_deletes_nothing_outside", "[kitty][media]")
{
    // The file that was read moves out of the roots, and a link takes its directory's place: the very inode
    // read is reachable through the old path, but deleting through that link would delete outside the roots.
    auto const root = TestDirectory {};
    auto const outside = TestDirectory {};
    auto const file = root.write("dir/tty-graphics-protocol-a", "RGBA");
    auto const moved = outside.path() / "dir";
    auto const source = PosixMediumSource { { .beforeUnlink = [&](std::filesystem::path const&) {
        std::filesystem::rename(root.path() / "dir", moved);
        std::filesystem::create_directory_symlink(moved, root.path() / "dir");
    } } };

    auto const bytes = source.read(file.native(), Medium::TemporaryFile, policyFor(root));
    REQUIRE(bytes.has_value());
    CHECK(std::filesystem::exists(moved / "tty-graphics-protocol-a"));
}

TEST_CASE("KittyMediumSource.posix.nothing_outside_the_roots_is_resolved", "[kitty][media]")
{
    // Checked as requested before anything resolves it: a lookup there could reach an automount or a network
    // filesystem, stalling the terminal or beaconing to whoever named it.
    auto const root = TestDirectory {};
    auto const source = PosixMediumSource {};
    auto const policy = policyFor(root);

    auto const path = GENERATE("/net/example.invalid/tty-graphics-protocol-a"s,
                               "tty-graphics-protocol-relative"s,
                               "/nonexistent-contour-media-test/tty-graphics-protocol-a"s);
    INFO(path);
    checkRefused(source.read(path, Medium::File, policy), MediaRefusal::OutsideRoots);

    // `..` leaves the root before anything is resolved.
    checkRefused(
        source.read((root.path() / ".." / "tty-graphics-protocol-gone").native(), Medium::File, policy),
        MediaRefusal::OutsideRoots);
}

TEST_CASE("KittyMediumSource.posix.a_root_spelled_through_a_link", "[kitty][media]")
{
    // As macOS spells `/tmp`: the root's own link is followed, and the real path is what the policy judges.
    auto const root = TestDirectory {};
    auto const elsewhere = TestDirectory {};
    auto const spelling = elsewhere.path() / "spelled-root";
    std::filesystem::create_directory_symlink(root.path(), spelling);
    auto const file = root.write("tty-graphics-protocol-a", "RGBA");
    auto const requested = (spelling / "tty-graphics-protocol-a").native();
    auto const source = PosixMediumSource {};
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

TEST_CASE("KittyMediumSource.posix.a_root_relinked_into_a_system_tree_opens_nothing_below_it",
          "[kitty][media]")
{
    // A root's spelling re-pointed into /dev: the real path is judged before anything below the root is
    // opened, as opening a device node has side effects.
    auto const elsewhere = TestDirectory {};
    auto const spelling = elsewhere.path() / "spelled-root";
    std::filesystem::create_directory_symlink("/dev", spelling);
    auto opened = std::vector<std::filesystem::path> {};
    auto const source = PosixMediumSource { { .opening = [&](std::filesystem::path const& path) {
        opened.push_back(path);
    } } };
    auto policy = policyFor(elsewhere);
    policy.temporaryRoots = { spelling };

    checkRefused(source.read((spelling / "null").native(), Medium::File, policy), MediaRefusal::SystemPath);
    CHECK(opened.empty());
}

#endif
