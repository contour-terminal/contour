// SPDX-License-Identifier: Apache-2.0
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <vthost/DurableFiles.hpp>

using namespace vthost;
namespace fs = std::filesystem;

namespace
{

[[nodiscard]] std::string contentOf(fs::path const& path)
{
    auto stream = std::ifstream { path, std::ios::binary };
    return std::string { std::istreambuf_iterator<char> { stream }, {} };
}

} // namespace

TEST_CASE("NativeDurableFiles syncs a file, replaces another with it, and syncs their directory",
          "[vthost][snapshot][durable]")
{
    auto const dir = core::testing::ScopedTempDir { "contour-durable" };
    auto const temporary = dir.path() / "snapshot.tmp";
    auto const target = dir.path() / "snapshot";
    std::ofstream { target } << "old";
    std::ofstream { temporary } << "new";
    auto const& files = NativeDurableFiles::instance();

    REQUIRE(files.syncFile(temporary));
    REQUIRE(files.replace(temporary, target));
    REQUIRE(files.syncDirectory(dir.path()));
    CHECK(contentOf(target) == "new");
    CHECK_FALSE(fs::exists(temporary));
}

TEST_CASE("NativeDurableFiles says why it cannot", "[vthost][snapshot][durable]")
{
    auto const dir = core::testing::ScopedTempDir { "contour-durable" };
    auto const missing = dir.path() / "missing";
    auto const& files = NativeDurableFiles::instance();

    auto const synced = files.syncFile(missing);
    REQUIRE_FALSE(synced);
    CHECK(synced.error().contains("missing"));
    auto const replaced = files.replace(missing, dir.path() / "target");
    REQUIRE_FALSE(replaced);
    CHECK(replaced.error().contains("missing"));
}

TEST_CASE("NativeDurableFiles creates a file exclusively", "[vthost][snapshot][durable]")
{
    auto const dir = core::testing::ScopedTempDir { "contour-durable" };
    auto const file = dir.path() / "snapshot.tmp";
    auto const& files = NativeDurableFiles::instance();

    REQUIRE(files.createExclusive(file));
    CHECK(fs::is_regular_file(file));
    CHECK(fs::file_size(file) == 0);
    CHECK_FALSE(files.createExclusive(file));
#ifndef _WIN32
    // A link planted where the file goes is refused, not followed.
    auto const elsewhere = dir.path() / "elsewhere";
    auto const planted = dir.path() / "planted.tmp";
    fs::create_symlink(elsewhere, planted);
    CHECK_FALSE(files.createExclusive(planted));
    CHECK_FALSE(fs::exists(elsewhere));
#endif
}
