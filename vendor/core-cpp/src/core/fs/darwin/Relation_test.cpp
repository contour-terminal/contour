// SPDX-License-Identifier: Apache-2.0
//
// The native relation across macOS's own volumes. The home directory lies on the Data volume,
// mounted at /System/Volumes/Data, but a walk up from it meets /Users, a firmlink whose `..` is the
// system volume's root: it leaves the Data volume below that volume's root, and never meets it. So
// the Data volume's root, which does contain the home directory, must never be answered Unrelated,
// and nor must /System/Volumes and /System, which hold the Data volume's mount point and lie on the
// system volume, another device. Two directories beneath the home directory, whose walks both pass
// /Users, are still decided, and so is /private/tmp, beside /Users on the Data volume.
#include <core/Environment.hpp>
#include <core/fs/Descriptor.hpp>
#include <core/fs/DescriptorTestSupport.hpp>
#include <core/fs/FsError.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

#include <unistd.h>

using core::fs::DescriptorFlags;
using core::fs::Relation;
using core::fs::test::DescriptorFixture;

namespace
{

/// Removes a directory tree when it goes, whatever the case did.
class Cleanup
{
  public:
    explicit Cleanup(std::filesystem::path path): _path { std::move(path) } {}
    Cleanup(Cleanup const&) = delete;
    Cleanup& operator=(Cleanup const&) = delete;
    Cleanup(Cleanup&&) = delete;
    Cleanup& operator=(Cleanup&&) = delete;
    ~Cleanup()
    {
        auto error = std::error_code {};
        std::filesystem::remove_all(_path, error);
    }

  private:
    std::filesystem::path _path;
};

} // namespace

TEST_CASE("relation across a firmlink", "[fs][native][relation]")
{
    // Every macOS from 10.15 on has the Data volume, and every runner a home directory: a case that
    // skipped here would pass unseen, so both are required.
    auto const home = core::LiveEnvironment {}.get("HOME");
    auto const data = std::filesystem::path { "/System/Volumes/Data" };
    REQUIRE((home && !home->empty()));
    REQUIRE(std::filesystem::is_directory(data));
    auto fixture = DescriptorFixture::createNative();
    auto& system = fixture.fileSystem();
    auto const homeDirectory = std::filesystem::path { *home };
    auto const below = homeDirectory / ("core-cpp-relation-" + std::to_string(::getpid()));
    std::filesystem::create_directories(below / "a");
    std::filesystem::create_directories(below / "b");
    auto const cleanup = Cleanup { below };

    auto const open = [&](std::filesystem::path const& path) {
        auto opened = fixture.run(system.openPreopen(path, DescriptorFlags::Read));
        REQUIRE(opened.has_value());
        return std::move(*opened);
    };
    auto const dataRoot = open(data);
    auto const homeHandle = open(homeDirectory);
    auto const a = open(below / "a");
    auto const b = open(below / "b");

    auto const fromData = fixture.run(system.relation(*dataRoot, *homeHandle));
    INFO((fromData ? std::string { "decided" } : fromData.error().toString()));
    CHECK_FALSE((fromData && *fromData == Relation::Unrelated));
    auto const fromHome = fixture.run(system.relation(*homeHandle, *dataRoot));
    CHECK_FALSE((fromHome && *fromHome == Relation::Unrelated));

    auto const aB = fixture.run(system.relation(*a, *b));
    INFO((aB ? std::string { "decided" } : aB.error().toString()));
    CHECK((aB && *aB == Relation::Unrelated));
    auto const homeA = fixture.run(system.relation(*homeHandle, *a));
    CHECK((homeA && *homeA == Relation::FirstContainsSecond));

    // The mount point's path, on the system volume: skipped by the walk from a, so never Unrelated.
    for (auto const* ancestor: { "/System/Volumes", "/System" })
    {
        INFO(ancestor);
        auto const grant = open(ancestor);
        auto const fromGrant = fixture.run(system.relation(*grant, *a));
        INFO((fromGrant ? std::string { "decided" } : fromGrant.error().toString()));
        CHECK_FALSE((fromGrant && *fromGrant == Relation::Unrelated));
        auto const fromKey = fixture.run(system.relation(*a, *grant));
        CHECK_FALSE((fromKey && *fromKey == Relation::Unrelated));
    }
    // A directory beside the firmlink, on the Data volume: none of what was skipped, so decided.
    auto const temporary = open("/private/tmp");
    auto const tmpA = fixture.run(system.relation(*temporary, *a));
    INFO((tmpA ? std::string { "decided" } : tmpA.error().toString()));
    CHECK((tmpA && *tmpA == Relation::Unrelated));
    auto const aTmp = fixture.run(system.relation(*a, *temporary));
    CHECK((aTmp && *aTmp == Relation::Unrelated));
}

TEST_CASE("relation with a disk image mounted beneath a grant", "[fs][native][relation]")
{
    // A walk up from either directory never sees a mount beneath the other, so relation() scans
    // the mount table for one. A disk image of its own file system beneath the grant shows nothing of
    // the key's: the answer stays Unrelated, and the scan is not a refusal of every grant that has a
    // mount beneath it. (No bind mount exists here to show the key's file system beneath it; the
    // Linux namespace rows and the FreeBSD nullfs rows prove that refusal.)
    auto const temporary =
        std::filesystem::path { "/private/tmp" } / ("core-cpp-mount-" + std::to_string(::getpid()));
    std::filesystem::create_directories(temporary / "grant" / "volume");
    std::filesystem::create_directories(temporary / "key");
    auto const cleanup = Cleanup { temporary };
    auto const image = temporary / "image.dmg";
    auto const mountPoint = temporary / "grant" / "volume";
    auto const create =
        "hdiutil create -quiet -size 2m -fs HFS+ -volname core-cpp -layout NONE '" + image.string() + "'";
    auto const attach = "hdiutil attach -quiet -nobrowse -noautoopen -mountpoint '" + mountPoint.string()
                        + "' '" + image.string() + "'";
    if (std::system(create.c_str()) != 0 || std::system(attach.c_str()) != 0)
        SKIP("hdiutil cannot make or attach a disk image here");
    struct Detach
    {
        explicit Detach(std::string text): command { std::move(text) } {}
        std::string command;
        Detach(Detach const&) = delete;
        Detach& operator=(Detach const&) = delete;
        Detach(Detach&&) = delete;
        Detach& operator=(Detach&&) = delete;
        ~Detach() { std::ignore = std::system(command.c_str()); }
    } const detach { "hdiutil detach -quiet -force '" + mountPoint.string() + "'" };

    auto fixture = DescriptorFixture::createNative();
    auto& system = fixture.fileSystem();
    auto const open = [&](std::filesystem::path const& path) {
        auto opened = fixture.run(system.openPreopen(path, DescriptorFlags::Read));
        REQUIRE(opened.has_value());
        return std::move(*opened);
    };
    auto const grant = open(temporary / "grant");
    auto const key = open(temporary / "key");
    auto const fromGrant = fixture.run(system.relation(*grant, *key));
    INFO((fromGrant ? std::string { "decided" } : fromGrant.error().toString()));
    CHECK((fromGrant && *fromGrant == Relation::Unrelated));
    auto const fromKey = fixture.run(system.relation(*key, *grant));
    CHECK((fromKey && *fromKey == Relation::Unrelated));
}
