// SPDX-License-Identifier: Apache-2.0
//
// The native relation on FreeBSD with mounts beneath a grant. A walk up from either directory never
// sees a mount beneath the other, so relation() scans the mount table: a nullfs mount beneath the
// grant that shows the key's file system puts the key inside the grant though neither walk meets the
// other, and is Unsupported; a tmpfs beneath it shows nothing of the key's, and the answer stands.
// Mounting needs root, which FreeBSD's CI VM has; elsewhere the case skips.
#include <core/fs/Descriptor.hpp>
#include <core/fs/DescriptorTestSupport.hpp>
#include <core/fs/FsError.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using core::fs::DescriptorFlags;
using core::fs::FsErrorCode;
using core::fs::Relation;
using core::fs::test::DescriptorFixture;

namespace
{

/// Unmounts what a case mounted when it goes, last first, whatever the case did.
class Mounts
{
  public:
    Mounts() = default;
    Mounts(Mounts const&) = delete;
    Mounts& operator=(Mounts const&) = delete;
    Mounts(Mounts&&) = delete;
    Mounts& operator=(Mounts&&) = delete;
    ~Mounts()
    {
        for (auto const& point: _points | std::views::reverse)
            std::ignore = std::system(("umount -f '" + point + "' 2>/dev/null").c_str());
    }

    /// Runs `mount -t @p type @p source @p point`.
    /// @return Whether it mounted.
    [[nodiscard]] bool mount(char const* type, std::string const& source, std::string const& point)
    {
        if (std::system(("mount -t " + std::string { type } + " '" + source + "' '" + point + "' 2>/dev/null")
                            .c_str())
            != 0)
            return false;
        _points.push_back(point);
        return true;
    }

  private:
    std::vector<std::string> _points;
};

} // namespace

TEST_CASE("relation with mounts beneath a grant on FreeBSD", "[fs][native][relation]")
{
    auto fixture = DescriptorFixture::createNative();
    auto const& host = fixture.hostDirectory();
    for (auto const* directory: { "g/inner", "k/secret", "h/tm", "c" })
        std::filesystem::create_directories(host / directory);
    auto mounts = Mounts {};
    if (!mounts.mount("nullfs", (host / "k").string(), (host / "g" / "inner").string())
        || !mounts.mount("tmpfs", "tmpfs", (host / "h" / "tm").string()))
        SKIP("no nullfs and tmpfs mounts here (they need root)");

    auto& system = fixture.fileSystem();
    auto const open = [&](std::filesystem::path const& path) {
        auto opened = fixture.run(system.openPreopen(path, DescriptorFlags::Read));
        REQUIRE(opened.has_value());
        return std::move(*opened);
    };
    auto const grant = open(host / "g");
    auto const key = open(host / "k" / "secret");
    for (auto const& answer:
         { fixture.run(system.relation(*grant, *key)), fixture.run(system.relation(*key, *grant)) })
    {
        REQUIRE_FALSE(answer.has_value());
        CHECK(answer.error().code == FsErrorCode::Unsupported);
    }
    auto const beside = open(host / "h");
    auto const other = open(host / "c");
    auto const unrelated = fixture.run(system.relation(*beside, *other));
    INFO((unrelated ? std::string { "decided" } : unrelated.error().toString()));
    CHECK((unrelated && *unrelated == Relation::Unrelated));
}
