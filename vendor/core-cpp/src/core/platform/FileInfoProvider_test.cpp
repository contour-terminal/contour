// SPDX-License-Identifier: Apache-2.0
#include <core/platform/FileInfoProvider.hpp>
#include <core/platform/PathUtils.hpp>
#include <core/platform/testing/MockFileInfoProvider.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
    #include <core/platform/windows/WindowsFileInfoProvider.hpp>
#else
    #include <core/platform/posix/PosixFileInfoProvider.hpp>
#endif

using namespace core::platform;
namespace fs = std::filesystem;

namespace
{
// ---------------------------------------------------------------------------
// Helper: RAII temp directory with known files for filesystem tests. Unique per instance
// (ScopedTempDir), so two runs of this binary cannot delete each other's fixtures.
// ---------------------------------------------------------------------------
struct TempDir
{
    core::testing::ScopedTempDir dir { "core_ls_test" };
    fs::path path = dir.path();

    void createFile(std::string const& name, std::string const& content = "") const
    {
        std::ofstream ofs(path / name);
        ofs << content;
    }

    void createSubdir(std::string const& name) const { fs::create_directories(path / name); }
};
} // namespace

// ===========================================================================
// nativeFileInfoProvider
// ===========================================================================

TEST_CASE("nativeFileInfoProvider is this platform's own provider, and lists a directory", "[platform]")
{
    // The implementations are private (posix/, windows/); a composition root reaches them only
    // through the factory. Every POSIX system, and Emscripten, gets the lstat(2) one.
    auto const provider = nativeFileInfoProvider();
    REQUIRE(provider != nullptr);
#ifdef _WIN32
    CHECK(dynamic_cast<WindowsFileInfoProvider const*>(provider.get()) != nullptr);
#else
    CHECK(dynamic_cast<PosixFileInfoProvider const*>(provider.get()) != nullptr);
#endif

    TempDir const tmp;
    tmp.createFile("file.txt", "abc");
    tmp.createSubdir("dir");

    auto const entries = provider->listDirectory(tmp.path.string());
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].name == "dir");
    CHECK(entries[0].isDir);
    CHECK(entries[1].name == "file.txt");
    CHECK(!entries[1].isDir);
    CHECK(entries[1].size == 3);
}

// ===========================================================================
// MockFileInfoProvider tests
// ===========================================================================

TEST_CASE("MockFileInfoProvider.directory_lookup", "[platform][mock]")
{
    testing::MockFileInfoProvider mock;
    mock.setEntries(".",
                    { { .name = "a.txt", .size = 10, .mode = 0644, .mtime = 100, .isDir = false },
                      { .name = "b.md", .size = 20, .mode = 0644, .mtime = 200, .isDir = false } });

    auto entries = mock.listDirectory(".");
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].name == "a.txt");
    CHECK(entries[1].name == "b.md");
}

TEST_CASE("MockFileInfoProvider.single_file_lookup", "[platform][mock]")
{
    testing::MockFileInfoProvider mock;
    mock.setFileEntry("a.txt", { .name = "a.txt", .size = 10, .mode = 0644, .mtime = 100, .isDir = false });

    auto entries = mock.listDirectory("a.txt");
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].name == "a.txt");
    CHECK(entries[0].size == 10);
}

TEST_CASE("MockFileInfoProvider.glob_filtering", "[platform][mock]")
{
    testing::MockFileInfoProvider mock;
    mock.setEntries(".",
                    { { .name = "a.txt", .size = 10, .mode = 0644, .mtime = 100, .isDir = false },
                      { .name = "b.txt", .size = 20, .mode = 0644, .mtime = 200, .isDir = false },
                      { .name = "c.md", .size = 30, .mode = 0644, .mtime = 300, .isDir = false } });

    auto entries = mock.listDirectory("*.txt");
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].name == "a.txt");
    CHECK(entries[1].name == "b.txt");
}

TEST_CASE("MockFileInfoProvider.glob_no_match", "[platform][mock]")
{
    testing::MockFileInfoProvider mock;
    mock.setEntries(".", { { .name = "a.txt", .size = 10, .mode = 0644, .mtime = 100, .isDir = false } });

    auto entries = mock.listDirectory("*.xyz");
    CHECK(entries.empty());
}

TEST_CASE("MockFileInfoProvider.glob_with_directory_prefix", "[platform][mock]")
{
    testing::MockFileInfoProvider mock;
    mock.setEntries("subdir",
                    { { .name = "x.txt", .size = 5, .mode = 0644, .mtime = 100, .isDir = false },
                      { .name = "y.md", .size = 15, .mode = 0644, .mtime = 200, .isDir = false } });

    auto entries = mock.listDirectory("subdir/*.txt");
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].name == "x.txt");
}

TEST_CASE("MockFileInfoProvider.nonexistent_returns_empty", "[platform][mock]")
{
    testing::MockFileInfoProvider const mock;
    auto entries = mock.listDirectory("/no/such/path");
    CHECK(entries.empty());
}

// ===========================================================================
// PosixFileInfoProvider tests (real filesystem: every POSIX system, and Emscripten's)
// ===========================================================================

#ifndef _WIN32
namespace
{
/// Tests whether @p reported is what a listing may report for a symlink in @p directory whose
/// target was written as @p written: that text, verbatim. Emscripten's readlink() (3.1.56 at
/// least) answers with the target resolved against the link's directory instead, so there the
/// resolved path is what the provider can report.
[[nodiscard]] bool isReportedTarget(std::string const& reported,
                                    std::string const& written,
                                    [[maybe_unused]] fs::path const& directory)
{
    #ifdef __EMSCRIPTEN__
    if (reported == (directory / written).generic_string())
        return true;
    #endif
    return reported == written;
}
} // namespace

TEST_CASE("PosixFileInfoProvider.listDirectory_directory", "[platform][posix]")
{
    TempDir const tmp;
    tmp.createFile("alpha.txt", "hello");
    tmp.createFile("beta.md", "world");
    tmp.createSubdir("gamma");

    auto const provider = PosixFileInfoProvider {};
    auto entries = provider.listDirectory(tmp.path.string());

    REQUIRE(entries.size() == 3);
    // Sorted by name
    CHECK(entries[0].name == "alpha.txt");
    CHECK(entries[1].name == "beta.md");
    CHECK(entries[2].name == "gamma");
    CHECK(entries[2].isDir == true);
}

TEST_CASE("PosixFileInfoProvider.listDirectory_single_file", "[platform][posix]")
{
    TempDir const tmp;
    tmp.createFile("target.txt", "content");

    auto const provider = PosixFileInfoProvider {};
    auto entries = provider.listDirectory((tmp.path / "target.txt").string());

    REQUIRE(entries.size() == 1);
    CHECK(entries[0].name == "target.txt");
    CHECK(entries[0].size == 7); // "content" is 7 bytes
    CHECK(entries[0].isDir == false);
}

TEST_CASE("PosixFileInfoProvider.listDirectory_single_directory_entry", "[platform][posix]")
{
    TempDir const tmp;
    tmp.createSubdir("mydir");

    auto const provider = PosixFileInfoProvider {};
    // When path points to a directory, it lists contents (even if empty).
    auto entries = provider.listDirectory((tmp.path / "mydir").string());
    CHECK(entries.empty()); // empty directory
}

TEST_CASE("PosixFileInfoProvider.listDirectory_glob_pattern", "[platform][posix]")
{
    TempDir const tmp;
    tmp.createFile("a.txt", "aaa");
    tmp.createFile("b.txt", "bbb");
    tmp.createFile("c.md", "ccc");
    tmp.createSubdir("d_dir");

    auto const provider = PosixFileInfoProvider {};
    auto entries = provider.listDirectory((tmp.path / "*.txt").string());

    REQUIRE(entries.size() == 2);
    CHECK(entries[0].name == "a.txt");
    CHECK(entries[1].name == "b.txt");
}

TEST_CASE("PosixFileInfoProvider.listDirectory_glob_question_mark", "[platform][posix]")
{
    TempDir const tmp;
    tmp.createFile("ab.txt");
    tmp.createFile("cd.txt");
    tmp.createFile("abc.txt");

    auto const provider = PosixFileInfoProvider {};
    auto entries = provider.listDirectory((tmp.path / "??.txt").string());

    REQUIRE(entries.size() == 2);
    CHECK(entries[0].name == "ab.txt");
    CHECK(entries[1].name == "cd.txt");
}

TEST_CASE("PosixFileInfoProvider.listDirectory_glob_no_match", "[platform][posix]")
{
    TempDir const tmp;
    tmp.createFile("a.txt");

    auto const provider = PosixFileInfoProvider {};
    auto entries = provider.listDirectory((tmp.path / "*.xyz").string());
    CHECK(entries.empty());
}

TEST_CASE("PosixFileInfoProvider.listDirectory_lists_dangling_symlink", "[platform][posix]")
{
    // A dangling symlink fails to resolve via status() (which follows). It must still be
    // listed via the symlink_status() fallback — mirroring how the Windows provider keeps
    // App Execution Aliases (winget) visible despite their reparse points being unfollowable.
    TempDir const tmp;
    tmp.createFile("real.txt", "data");
    fs::create_symlink(tmp.path / "missing_target", tmp.path / "broken.lnk");

    auto const provider = PosixFileInfoProvider {};
    auto entries = provider.listDirectory(tmp.path.string());

    REQUIRE(entries.size() == 2);
    CHECK(entries[0].name == "broken.lnk");
    CHECK(entries[0].isDir == false);
    CHECK(entries[1].name == "real.txt");
}

TEST_CASE("PosixFileInfoProvider.populates_stat_metadata", "[platform][posix]")
{
    // The lstat-based provider must fill the disk-usage / identity fields
    // (blocks, dev, ino) for real files, not just apparent size.
    TempDir const tmp;
    tmp.createFile("data.bin", std::string(8192, 'x')); // 8 KiB => several 512B blocks

    auto const provider = PosixFileInfoProvider {};
    auto entries = provider.listDirectory((tmp.path / "data.bin").string());

    REQUIRE(entries.size() == 1);
    auto const& e = entries[0];
    CHECK(e.size == 8192);
    CHECK(e.blocks > 0); // allocated blocks reported (st_blocks)
    CHECK(e.dev != 0);   // device id reported (st_dev)
    CHECK(e.ino != 0);   // inode reported (st_ino)
    CHECK(e.isSymlink == false);
    CHECK(e.isDir == false);
}

TEST_CASE("PosixFileInfoProvider.marks_symlink", "[platform][posix]")
{
    // A symlink must be flagged isSymlink and never reported as a directory,
    // even when it points at one (it is not followed).
    TempDir const tmp;
    tmp.createSubdir("realdir");
    fs::create_symlink(tmp.path / "realdir", tmp.path / "link_to_dir");

    auto const provider = PosixFileInfoProvider {};
    auto entries = provider.listDirectory(tmp.path.string());

    REQUIRE(entries.size() == 2);
    // Sorted by name: "link_to_dir" < "realdir"
    CHECK(entries[0].name == "link_to_dir");
    CHECK(entries[0].isSymlink == true);
    CHECK(entries[0].isDir == false); // not followed -> not a directory
    CHECK(entries[1].name == "realdir");
    CHECK(entries[1].isSymlink == false);
    CHECK(entries[1].isDir == true);
}

TEST_CASE("PosixFileInfoProvider.populates_symlink_target", "[platform][posix]")
{
    // A symlink entry must expose its target path verbatim (read via readlink),
    // while a regular file leaves symlinkTarget empty.
    TempDir const tmp;
    tmp.createFile("real.txt", "data");
    fs::create_symlink("real.txt", tmp.path / "link.txt"); // relative target, kept verbatim

    auto const provider = PosixFileInfoProvider {};
    auto entries = provider.listDirectory(tmp.path.string());

    REQUIRE(entries.size() == 2);
    // Sorted by name: "link.txt" < "real.txt"
    CHECK(entries[0].name == "link.txt");
    CHECK(entries[0].isSymlink == true);
    CHECK(isReportedTarget(entries[0].symlinkTarget, "real.txt", tmp.path));
    CHECK(entries[1].name == "real.txt");
    CHECK(entries[1].isSymlink == false);
    CHECK(entries[1].symlinkTarget.empty());
}

TEST_CASE("PosixFileInfoProvider.populates_dangling_symlink_target", "[platform][posix]")
{
    // readlink succeeds for a dangling symlink: the target string is still reported
    // even though it does not resolve to an existing file.
    TempDir const tmp;
    fs::create_symlink("missing_target", tmp.path / "broken.lnk");

    auto const provider = PosixFileInfoProvider {};
    auto entries = provider.listDirectory(tmp.path.string());

    REQUIRE(entries.size() == 1);
    CHECK(entries[0].name == "broken.lnk");
    CHECK(entries[0].isSymlink == true);
    CHECK(isReportedTarget(entries[0].symlinkTarget, "missing_target", tmp.path));
}

TEST_CASE("PosixFileInfoProvider.siblings_share_device", "[platform][posix]")
{
    // Two entries in the same directory live on the same filesystem, so their
    // st_dev must match — the invariant cross-device detection relies on.
    TempDir const tmp;
    tmp.createFile("a.txt", "a");
    tmp.createFile("b.txt", "b");

    auto const provider = PosixFileInfoProvider {};
    auto entries = provider.listDirectory(tmp.path.string());

    REQUIRE(entries.size() == 2);
    CHECK(entries[0].dev == entries[1].dev);
    CHECK(entries[0].ino != entries[1].ino); // distinct files -> distinct inodes
}

TEST_CASE("PosixFileInfoProvider.populates_absolute_path", "[platform][posix]")
{
    // FileEntry::name is only a basename, so anything that needs to address the entry rather
    // than display it depends on this field being absolute.
    TempDir const tmp;
    tmp.createFile("a.txt", "a");

    auto const provider = PosixFileInfoProvider {};
    auto const entries = provider.listDirectory(tmp.path.string());

    REQUIRE(entries.size() == 1);
    CHECK(entries[0].path == normalizePath(tmp.path / "a.txt"));
}

TEST_CASE("PosixFileInfoProvider.absolute_path_from_relative_listing", "[platform][posix]")
{
    // Listing a relative directory must still yield absolute entry paths.
    TempDir const tmp;
    tmp.createFile("rel.txt", "x");

    auto const previous = fs::current_path();
    fs::current_path(tmp.path);
    auto const provider = PosixFileInfoProvider {};
    auto const entries = provider.listDirectory(".");
    auto const globbed = provider.listDirectory("*.txt");
    auto const single = provider.listDirectory("rel.txt");
    fs::current_path(previous);

    auto const expected = normalizePath(fs::canonical(tmp.path) / "rel.txt");

    REQUIRE(entries.size() == 1);
    CHECK(normalizePath(fs::canonical(entries[0].path)) == expected);

    REQUIRE(globbed.size() == 1);
    CHECK(normalizePath(fs::canonical(globbed[0].path)) == expected);

    REQUIRE(single.size() == 1);
    CHECK(normalizePath(fs::canonical(single[0].path)) == expected);
}

TEST_CASE("PosixFileInfoProvider.listDirectory_nonexistent", "[platform][posix]")
{
    auto const provider = PosixFileInfoProvider {};
    auto entries = provider.listDirectory("/nonexistent/path/that/does/not/exist");
    CHECK(entries.empty());
}

TEST_CASE("PosixFileInfoProvider.listDirectory_glob_nonexistent_pattern", "[platform][posix]")
{
    auto const provider = PosixFileInfoProvider {};
    auto entries = provider.listDirectory("*.surely_nonexistent_ext_xyz");
    // If CWD has no such files, should be empty.
    CHECK(entries.empty());
}

#endif // !_WIN32
