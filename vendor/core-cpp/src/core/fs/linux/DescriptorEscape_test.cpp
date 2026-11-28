// SPDX-License-Identifier: Apache-2.0
//
// Linux's magic links: /proc/self/cwd, /proc/self/root and /proc/self/fd/N name objects anywhere,
// whatever their text says. Beneath a preopen of /proc/self no path may go through one: openat2
// refuses them (RESOLVE_NO_MAGICLINKS, Loop), the walk reads their absolute text (NotPermitted).
#include <core/fs/Descriptor.hpp>
#include <core/fs/DescriptorTestSupport.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/NativeFileSystem.hpp>
#include <core/fs/detail/OwnedHandle.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <string>

#include <fcntl.h>

using core::fs::BeneathStrategy;
using core::fs::DescriptorFlags;
using core::fs::FsErrorCode;
using core::fs::OpenFlags;
using core::fs::SymlinkPolicy;
using core::fs::test::DescriptorFixture;
using core::fs::test::Outcome;
using core::fs::test::outcomeOf;

TEST_CASE("no path goes through a magic link of /proc", "[fs][escape]")
{
    auto const strategy = GENERATE(BeneathStrategy::Best, BeneathStrategy::ComponentWalk);
    CAPTURE(strategy == BeneathStrategy::Best ? "Best" : "ComponentWalk");
    auto fixture = DescriptorFixture::createNative({ .strategy = strategy });
    auto proc = fixture.run(
        fixture.fileSystem().openPreopen(std::filesystem::path { "/proc/self" }, DescriptorFlags::Read));
    if (!proc)
        SKIP("/proc/self is not mounted here");
    auto& self = **proc;
    auto const refused = [](Outcome const& outcome) {
        return outcome == FsErrorCode::NotPermitted || outcome == FsErrorCode::Loop;
    };

    // A descriptor this process surely has, open on the file system's root: a magic link that,
    // followed, would reach every directory there is.
    auto const slash = core::fs::detail::UniqueHandle { ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC) };
    REQUIRE(slash.get() >= 0);
    auto const throughDescriptor = "fd/" + std::to_string(slash.get()) + "/etc";

    for (auto const& path:
         { std::string { "cwd/." }, std::string { "root/etc" }, throughDescriptor, std::string { "cwd/" } })
    {
        CAPTURE(path);
        CHECK(refused(outcomeOf(fixture.run(self.statAt(SymlinkPolicy::NoFollow, path)))));
        CHECK(refused(outcomeOf(fixture.run(
            self.openAt(SymlinkPolicy::NoFollow, path, OpenFlags::None, DescriptorFlags::Read)))));
    }
    for (auto const* path: { "cwd", "root", "exe" })
    {
        CAPTURE(path);
        CHECK(refused(outcomeOf(fixture.run(self.statAt(SymlinkPolicy::Follow, path)))));
        CHECK(fixture.run(self.statAt(SymlinkPolicy::NoFollow, path)).has_value());
    }
}
