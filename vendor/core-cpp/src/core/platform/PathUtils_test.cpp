// SPDX-License-Identifier: Apache-2.0
#include <core/platform/PathUtils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>

using namespace core::platform;

TEST_CASE("normalizePath.forward_slashes_unchanged", "[platform]")
{
    CHECK(normalizePath(std::string("/usr/local/bin")) == "/usr/local/bin");
    CHECK(normalizePath(std::string("relative/path/file.txt")) == "relative/path/file.txt");
    CHECK(normalizePath(std::string("")).empty());
    CHECK(normalizePath(std::string("/")) == "/");
}

TEST_CASE("normalizePath.backslashes_converted", "[platform]")
{
    // On POSIX this is a no-op (backslashes are valid filename chars),
    // on Windows this converts backslashes to forward slashes.
#ifdef _WIN32
    CHECK(normalizePath(std::string("C:\\Users\\test\\file.txt")) == "C:/Users/test/file.txt");
    CHECK(normalizePath(std::string("\\\\server\\share\\path")) == "//server/share/path");
    CHECK(normalizePath(std::string("mixed/path\\with\\both")) == "mixed/path/with/both");
    CHECK(normalizePath(std::string("C:\\")) == "C:/");
#else
    // On POSIX, backslashes are preserved (they're valid in filenames)
    CHECK(normalizePath(std::string("path\\with\\backslashes")) == "path\\with\\backslashes");
#endif
}

TEST_CASE("normalizePath.UNC_paths", "[platform]")
{
#ifdef _WIN32
    CHECK(normalizePath(std::string("\\\\server\\share")) == "//server/share");
    CHECK(normalizePath(std::string("\\\\server\\share\\dir\\file")) == "//server/share/dir/file");
#endif
}

TEST_CASE("normalizePath.filesystem_path_overload", "[platform]")
{
    auto const p = std::filesystem::path("/some/path");
    CHECK(normalizePath(p) == "/some/path");
}

TEST_CASE("resolveDevicePath.null_device", "[platform]")
{
    // `/dev/null` is mapped to the platform-native null device on Windows and
    // returned unchanged on POSIX systems.
#ifdef _WIN32
    CHECK(resolveDevicePath("/dev/null") == "NUL");
#else
    CHECK(resolveDevicePath("/dev/null") == "/dev/null");
#endif
}

TEST_CASE("resolveDevicePath.regular_paths_unchanged", "[platform]")
{
    CHECK(resolveDevicePath("/tmp/foo.txt") == "/tmp/foo.txt");
    CHECK(resolveDevicePath("relative/path") == "relative/path");
    CHECK(resolveDevicePath("C:/Users/test/out.log") == "C:/Users/test/out.log");
    CHECK(resolveDevicePath("").empty());
}

TEST_CASE("resolveDevicePath.similar_but_not_null_device", "[platform]")
{
    // Only the exact POSIX null-device path is translated. Paths that merely start
    // with `/dev/` or contain `null` are regular filesystem paths.
    CHECK(resolveDevicePath("/dev/null/file") == "/dev/null/file");
    CHECK(resolveDevicePath("/dev/nullx") == "/dev/nullx");
    CHECK(resolveDevicePath("/dev/zero") == "/dev/zero");
    CHECK(resolveDevicePath("null") == "null");
}

TEST_CASE("canonicalCasePath.nonexistent_path_falls_back_to_normalize", "[platform]")
{
    // A path that does not exist on disk cannot be recased, so the helper must fall
    // back to a plain separator normalization rather than failing.
#ifdef _WIN32
    CHECK(canonicalCasePath(std::filesystem::path("C:\\does\\not\\exist_core_xyz"))
          == "C:/does/not/exist_core_xyz");
#else
    CHECK(canonicalCasePath(std::filesystem::path("/does/not/exist_core_xyz")) == "/does/not/exist_core_xyz");
#endif
}

#ifdef _WIN32
TEST_CASE("canonicalCasePath.corrects_case_and_uppercases_drive", "[platform]")
{
    namespace fs = std::filesystem;
    auto const dirName = std::string("CoreCanonCaseTEST_dir");
    auto const dir = fs::temp_directory_path() / dirName;

    std::error_code ec;
    fs::create_directory(dir, ec);
    REQUIRE_FALSE(ec);

    // Spell the existing directory entirely in lower case (including its drive letter)
    // to simulate a user-typed path that disagrees with the on-disk capitalization.
    auto wrongCase = canonicalCasePath(dir);
    std::ranges::transform(
        wrongCase, wrongCase.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto const result = canonicalCasePath(fs::path(wrongCase));
    auto const lastComponent = result.substr(result.rfind('/') + 1);

    REQUIRE(result.size() >= 2);
    CHECK(result[1] == ':');
    CHECK(std::isupper(static_cast<unsigned char>(result[0])) != 0); // Drive letter upper-cased.
    CHECK_FALSE(result.contains('\\'));                              // Forward slashes only.
    CHECK(lastComponent == dirName);                                 // Real on-disk component case.

    fs::remove(dir, ec);
}
#endif

// ============================================================================
// stripTrailingSeparator
// ============================================================================

TEST_CASE("stripTrailingSeparator.drops_the_artifact_left_by_lexically_normal", "[platform]")
{
    // A final "." or ".." makes lexically_normal() keep a trailing separator, which would
    // otherwise not compare equal to the same path spelled without one.
    CHECK(stripTrailingSeparator("/test/.") == "/test");
    CHECK(stripTrailingSeparator("/test/sub/..") == "/test");
    CHECK(stripTrailingSeparator("/test/") == "/test");
    CHECK(stripTrailingSeparator("/test") == "/test");
    // Relative paths keep their spelling, minus the separator.
    CHECK(stripTrailingSeparator("foo/") == "foo");
    CHECK(stripTrailingSeparator("foo/.") == "foo");
}

TEST_CASE("stripTrailingSeparator.never_strips_into_the_root", "[platform]")
{
    // The root is the one path whose trailing separator carries meaning: "/" is the root
    // directory, whereas "" is no path at all.
    CHECK(stripTrailingSeparator("/") == "/");
    CHECK(stripTrailingSeparator("/.") == "/");

#ifdef _WIN32
    // Likewise a drive root: "C:/" is the root of drive C, while "C:" names that drive's
    // current directory -- a different location entirely.
    CHECK(stripTrailingSeparator("C:/") == "C:/");
    CHECK(stripTrailingSeparator("C:/.") == "C:/");
    CHECK(stripTrailingSeparator("C:/test/") == "C:/test");
#endif
}

// ============================================================================
// isCaseOnlyRename
// ============================================================================

TEST_CASE("isCaseOnlyRename.detects_final_component_recase", "[platform]")
{
    // Same parent, final component differing only in lettercase.
    CHECK(isCaseOnlyRename("foo", "Foo"));
    CHECK(isCaseOnlyRename("Foo", "foo"));
    CHECK(isCaseOnlyRename("dir/a", "dir/A"));
    CHECK(isCaseOnlyRename("a/b/readme", "a/b/README"));
    // A trailing separator (e.g. on a directory operand) must not defeat detection.
    CHECK(isCaseOnlyRename("foo/", "Foo"));
    CHECK(isCaseOnlyRename("foo", "Foo/"));
    // A redundant ./ prefix normalizes away before comparison.
    CHECK(isCaseOnlyRename("./foo", "Foo"));
}

TEST_CASE("isCaseOnlyRename.rejects_non_recase", "[platform]")
{
    // Identical spelling is a no-op, not a recase.
    CHECK_FALSE(isCaseOnlyRename("foo", "foo"));
    // Genuinely different names.
    CHECK_FALSE(isCaseOnlyRename("foo", "bar"));
    CHECK_FALSE(isCaseOnlyRename("foo", "foobar"));
    // Same final component but different parent directories: a real move.
    CHECK_FALSE(isCaseOnlyRename("a/foo", "b/Foo"));
    CHECK_FALSE(isCaseOnlyRename("foo", "sub/Foo"));
    // Missing final component on either side.
    CHECK_FALSE(isCaseOnlyRename("", "Foo"));
    CHECK_FALSE(isCaseOnlyRename("foo", ""));
}

TEST_CASE("stripTrailingSeparator keeps a path the native narrow encoding cannot spell", "[platform]")
{
    // generic_string() narrows to the platform's native narrow encoding, which on Windows is the
    // ANSI code page: a path it cannot represent comes back mangled, or throws -- the very reason
    // normalizePath()'s own declaration rejects it. What this function returns keys
    // InMemoryFileSystem's whole file map, so two paths collapsing onto one spelling is a fixture
    // quietly answering with another file's content.
    //
    // The expected value is spelled as UTF-8 bytes rather than as a \u escape, so it does not
    // depend on the compiler's execution character set.
    auto const japanese = std::filesystem::path { std::u8string { u8"/tmp/\u65e5\u672c/" } };
    CHECK(stripTrailingSeparator(japanese) == "/tmp/\xe6\x97\xa5\xe6\x9c\xac");

    auto const windowsStyle = std::filesystem::path { std::u8string { u8"C:/tmp/\u65e5/" } };
    CHECK(stripTrailingSeparator(windowsStyle) == "C:/tmp/\xe6\x97\xa5");
}

TEST_CASE("isCaseOnlyRename compares paths the native narrow encoding cannot spell", "[platform]")
{
    // Both sides used to be rebuilt as std::filesystem::path out of a narrow string, which on
    // Windows is read back in the ANSI code page -- undoing the spelling and, where two distinct
    // characters narrow to the same replacement, making unrelated parents compare equal.
    auto const lower = std::filesystem::path { std::u8string { u8"/tmp/\u65e5/name" } };
    auto const upper = std::filesystem::path { std::u8string { u8"/tmp/\u65e5/NAME" } };
    CHECK(isCaseOnlyRename(lower, upper));

    auto const otherParent = std::filesystem::path { std::u8string { u8"/tmp/\u672c/NAME" } };
    CHECK_FALSE(isCaseOnlyRename(lower, otherParent));
}
