// SPDX-License-Identifier: Apache-2.0
//
// What the POSIX descriptors owe an owner-only creation: the mode is given to openat and mkdirat
// themselves, so no other user ever holds the object between its creation and a chmod; the umask can
// only take bits away. And what they read back: the access a handle reports comes from that handle's
// fstat, whatever has since been renamed over its name.
#include <core/fs/AccessScope.hpp>
#include <core/fs/Descriptor.hpp>
#include <core/fs/DescriptorTestSupport.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/posix/InheritableAcl.hpp>
#include <core/fs/posix/OwnerOnlyCreation.hpp>
#include <core/fs/posix/PosixSystem.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>

using core::fs::AccessScope;
using core::fs::CreationAccess;
using core::fs::DescriptorFlags;
using core::fs::FsErrorCode;
using core::fs::OpenFlags;
using core::fs::SymlinkPolicy;
using core::fs::test::DescriptorFixture;

namespace
{

/// Sets the process's umask for the case and restores it after.
class ScopedUmask
{
  public:
    explicit ScopedUmask(::mode_t mask) noexcept: _previous { ::umask(mask) } {}
    ScopedUmask(ScopedUmask const&) = delete;
    ScopedUmask(ScopedUmask&&) = delete;
    ScopedUmask& operator=(ScopedUmask const&) = delete;
    ScopedUmask& operator=(ScopedUmask&&) = delete;
    ~ScopedUmask() { ::umask(_previous); }

  private:
    ::mode_t _previous;
};

/// @return The permission bits of @p path, as the host sees them.
[[nodiscard]] ::mode_t modeOf(std::filesystem::path const& path)
{
    struct ::stat status {};
    REQUIRE(::lstat(path.c_str(), &status) == 0);
    return status.st_mode & 07777;
}

/// Runs @p command (an argument vector, found on PATH) with its output discarded, and waits for it.
/// @return Whether it ran and exited 0.
[[nodiscard]] bool ranSuccessfully(std::vector<std::string> const& command)
{
    auto arguments = std::vector<char*> {};
    for (auto const& argument: command)
        arguments.push_back(const_cast<char*>(argument.c_str()));
    arguments.push_back(nullptr);
    auto actions = ::posix_spawn_file_actions_t {};
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    ::posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    // An empty environment: the tools need none, and PATH is searched in this process's.
    auto noEnvironment = std::array<char*, 1> { nullptr };
    auto child = ::pid_t {};
    auto const spawned =
        ::posix_spawnp(&child, arguments.front(), &actions, nullptr, arguments.data(), noEnvironment.data());
    ::posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0)
        return false;
    auto status = 0;
    while (::waitpid(child, &status, 0) < 0)
        if (errno != EINTR)
            return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/// @return The access @p descriptor's own handle reports.
[[nodiscard]] std::optional<AccessScope> accessOf(DescriptorFixture const& fixture,
                                                  core::fs::IDescriptor& descriptor)
{
    auto const status = fixture.run(descriptor.stat());
    REQUIRE(status.has_value());
    return status->access;
}

} // namespace

TEST_CASE("posix: an OwnerOnly file is created 0600 whatever the umask", "[fs][native][access]")
{
    // Even a umask that takes nothing away leaves the group and others without a bit.
    auto const mask = ScopedUmask { 0 };
    auto fixture = DescriptorFixture::createNative();
    auto file =
        fixture.run(fixture.root().openAt(SymlinkPolicy::NoFollow,
                                          "secret",
                                          OpenFlags::Create | OpenFlags::Exclusive | OpenFlags::OwnerOnly,
                                          DescriptorFlags::Write));
    REQUIRE(file.has_value());
    CHECK(modeOf(fixture.hostDirectory() / "secret") == 0600);
    CHECK(accessOf(fixture, **file) == AccessScope::OwnerOnly);
}

TEST_CASE("posix: an OwnerOnly directory is created 0700 whatever the umask", "[fs][native][access]")
{
    auto const mask = ScopedUmask { 0 };
    auto fixture = DescriptorFixture::createNative();
    REQUIRE(fixture.run(fixture.root().createDirectoryAt("keys", CreationAccess::OwnerOnly)).has_value());
    CHECK(modeOf(fixture.hostDirectory() / "keys") == 0700);
}

TEST_CASE("posix: a Default file under umask 022 is shared", "[fs][native][access]")
{
    auto const mask = ScopedUmask { 022 };
    auto fixture = DescriptorFixture::createNative();
    auto file = fixture.run(fixture.root().openAt(
        SymlinkPolicy::NoFollow, "plain", OpenFlags::Create | OpenFlags::Exclusive, DescriptorFlags::Write));
    REQUIRE(file.has_value());
    CHECK(modeOf(fixture.hostDirectory() / "plain") == 0644);
    CHECK(accessOf(fixture, **file) == AccessScope::Shared);
    REQUIRE(fixture.run(fixture.root().createDirectoryAt("dir", CreationAccess::Default)).has_value());
    CHECK(modeOf(fixture.hostDirectory() / "dir") == 0755);
}

TEST_CASE("posix: a group- or world-writable owner-only file is Shared", "[fs][native][access]")
{
    auto fixture = DescriptorFixture::createNative();
    auto file =
        fixture.run(fixture.root().openAt(SymlinkPolicy::NoFollow,
                                          "widened",
                                          OpenFlags::Create | OpenFlags::Exclusive | OpenFlags::OwnerOnly,
                                          DescriptorFlags::Write));
    REQUIRE(file.has_value());
    for (auto const mode: { ::mode_t { 0620 }, ::mode_t { 0602 }, ::mode_t { 0640 } })
    {
        INFO("mode " << mode);
        REQUIRE(::chmod((fixture.hostDirectory() / "widened").c_str(), mode) == 0);
        CHECK(accessOf(fixture, **file) == AccessScope::Shared);
    }
}

TEST_CASE("posix: access is read on the open handle, not the name", "[fs][native][access]")
{
    auto const mask = ScopedUmask { 022 };
    auto fixture = DescriptorFixture::createNative();
    auto& root = fixture.root();
    auto file = fixture.run(root.openAt(SymlinkPolicy::NoFollow,
                                        "key",
                                        OpenFlags::Create | OpenFlags::Exclusive | OpenFlags::OwnerOnly,
                                        DescriptorFlags::Read));
    REQUIRE(file.has_value());
    fixture.createFile("planted", "attacker");
    REQUIRE(modeOf(fixture.hostDirectory() / "planted") == 0644);
    std::filesystem::rename(fixture.hostDirectory() / "planted", fixture.hostDirectory() / "key");
    CHECK(accessOf(fixture, **file) == AccessScope::OwnerOnly);
    auto const byName = fixture.run(root.statAt(SymlinkPolicy::NoFollow, "key"));
    REQUIRE(byName.has_value());
    CHECK((!byName->access.has_value() || *byName->access == AccessScope::Shared));
}

TEST_CASE("posix: a foreign owner is ForeignOwner", "[fs][native][access]")
{
    auto fixture = DescriptorFixture::createNative();
    if (::geteuid() != 0)
    {
        // Without root nothing can be given away, but root owns `/` and lets everyone read it.
        auto slash = fixture.run(fixture.fileSystem().openPreopen("/", DescriptorFlags::Read));
        REQUIRE(slash.has_value());
        struct ::stat status {};
        REQUIRE(::stat("/", &status) == 0);
        if (status.st_uid == ::geteuid())
            SKIP("this process owns `/` (a user namespace mapped to it), so nothing here is another's");
        CHECK(accessOf(fixture, **slash) == AccessScope::ForeignOwner);
        return;
    }
    auto file =
        fixture.run(fixture.root().openAt(SymlinkPolicy::NoFollow,
                                          "theirs",
                                          OpenFlags::Create | OpenFlags::Exclusive | OpenFlags::OwnerOnly,
                                          DescriptorFlags::Read));
    REQUIRE(file.has_value());
    constexpr auto Nobody = ::uid_t { 65534 };
    REQUIRE(::lchown((fixture.hostDirectory() / "theirs").c_str(), Nobody, static_cast<::gid_t>(-1)) == 0);
    CHECK(accessOf(fixture, **file) == AccessScope::ForeignOwner);
}

TEST_CASE("posix: an inherited ACL never widens an OwnerOnly creation", "[fs][native][access]")
{
    auto const mask = ScopedUmask { 022 };
    auto fixture = DescriptorFixture::createNative();
    auto& root = fixture.root();
    REQUIRE(fixture.run(root.createDirectoryAt("inherits", CreationAccess::Default)).has_value());
    auto const command = core::fs::test::inheritableAclCommand(fixture.hostDirectory() / "inherits");
    INFO(command.front() << " " << command[command.size() - 2]);
    if (!ranSuccessfully(command))
        SKIP("no inheritable ACL here: the tool is missing, or the file system keeps no such ACL");
    // The ACL bites: what is created inside by default is reachable by others.
    auto plain = fixture.run(root.openAt(SymlinkPolicy::NoFollow,
                                         "inherits/plain",
                                         OpenFlags::Create | OpenFlags::Exclusive,
                                         DescriptorFlags::Write));
    REQUIRE(plain.has_value());
    if (accessOf(fixture, **plain) != AccessScope::Shared)
        SKIP("the ACL did not reach a new file, so nothing here could widen one");

    auto secret = fixture.run(root.openAt(SymlinkPolicy::NoFollow,
                                          "inherits/secret",
                                          OpenFlags::Create | OpenFlags::Exclusive | OpenFlags::OwnerOnly,
                                          DescriptorFlags::Write));
    REQUIRE(secret.has_value());
    CHECK(accessOf(fixture, **secret) == AccessScope::OwnerOnly);
    CHECK(modeOf(fixture.hostDirectory() / "inherits" / "secret") == 0600);
    REQUIRE(fixture.run(root.createDirectoryAt("inherits/keys", CreationAccess::OwnerOnly)).has_value());
    auto keys = fixture.run(
        root.openAt(SymlinkPolicy::NoFollow, "inherits/keys", OpenFlags::Directory, DescriptorFlags::Read));
    REQUIRE(keys.has_value());
    CHECK(accessOf(fixture, **keys) == AccessScope::OwnerOnly);
    CHECK(modeOf(fixture.hostDirectory() / "inherits" / "keys") == 0700);
}

namespace
{

using core::fs::detail::OwnerOnlyCreation;
using core::fs::detail::OwnerOnlyCreator;
using core::fs::test::FixedRandom;

/// The seed the creators of these cases draw their staging key from.
constexpr auto StagingSeed = std::uint64_t { 18 };

/// @return A creator as @p how says, its staging key drawn from @p seed.
[[nodiscard]] OwnerOnlyCreator creatorOf(
    OwnerOnlyCreation how,
    std::uint64_t seed = StagingSeed,
    core::fs::detail::StatHandle statHandle = &core::fs::detail::systemStatHandle)
{
    auto random = FixedRandom { seed };
    return OwnerOnlyCreator { random, how, std::move(statHandle) };
}

/// A stat of a file system that ignores the mode it was given, as vfat or exfat under `fmask` and
/// cifs under `noperm` do: everything reads 0755 (0644 for a file is as wide).
int modeIgnoringStat(int handle, struct ::stat* status)
{
    if (::fstat(handle, status) != 0)
        return -1;
    status->st_mode = (status->st_mode & ~::mode_t { 07777 }) | 0755;
    return 0;
}

/// A host directory and a handle on it, as a descriptor holds one.
struct HeldDirectory
{
    core::testing::ScopedTempDir directory { "core-cpp-fs-owner-only" };
    core::fs::detail::UniqueHandle handle { ::open(directory.path().c_str(),
                                                   O_RDONLY | O_DIRECTORY | O_CLOEXEC) };

    /// @return The names in it, sorted.
    [[nodiscard]] std::vector<std::string> names() const
    {
        auto listed = std::vector<std::string> {};
        for (auto const& entry: std::filesystem::directory_iterator { directory.path() })
            listed.push_back(entry.path().filename().string());
        std::ranges::sort(listed);
        return listed;
    }
};

constexpr auto CreateFlags = O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW | O_CLOEXEC;

[[nodiscard]] AccessScope scopeOf(int handle)
{
    struct ::stat status {};
    REQUIRE(::fstat(handle, &status) == 0);
    auto const facts = core::fs::detail::accessFactsOf(handle, status);
    REQUIRE(facts.has_value());
    return core::fs::classifyAccess(*facts);
}

} // namespace

TEST_CASE("posix: an owner-only creation, direct or staged, leaves the object and nothing else",
          "[fs][native][access]")
{
    auto const how = GENERATE(OwnerOnlyCreation::Direct, OwnerOnlyCreation::Staged);
    auto const mask = ScopedUmask { 0 };
    auto const held = HeldDirectory {};
    auto const creator = creatorOf(how);
    auto file = creator.createFile(held.handle.get(), "secret", CreateFlags);
    REQUIRE(file.has_value());
    REQUIRE(creator.createDirectory(held.handle.get(), "keys").has_value());
    CHECK(held.names() == std::vector<std::string> { "keys", "secret" });
    CHECK(modeOf(held.directory.path() / "secret") == 0600);
    CHECK(modeOf(held.directory.path() / "keys") == 0700);
    CHECK(scopeOf(file->get()) == AccessScope::OwnerOnly);
    // The handle is the file under its final name, its only link.
    struct ::stat byHandle {};
    struct ::stat byName {};
    REQUIRE(::fstat(file->get(), &byHandle) == 0);
    REQUIRE(::lstat((held.directory.path() / "secret").c_str(), &byName) == 0);
    CHECK(byHandle.st_ino == byName.st_ino);
    CHECK(byHandle.st_nlink == 1);
}

TEST_CASE("posix: an owner-only creation over an existing name is Exist and leaves nothing behind",
          "[fs][native][access]")
{
    auto const how = GENERATE(OwnerOnlyCreation::Direct, OwnerOnlyCreation::Staged);
    auto const held = HeldDirectory {};
    std::ofstream { held.directory.path() / "taken" } << "theirs";
    std::filesystem::create_directory(held.directory.path() / "takendir");
    auto const creator = creatorOf(how);
    auto const file = creator.createFile(held.handle.get(), "taken", CreateFlags);
    REQUIRE_FALSE(file.has_value());
    CHECK(file.error().code == FsErrorCode::Exist);
    auto const directory = creator.createDirectory(held.handle.get(), "takendir");
    REQUIRE_FALSE(directory.has_value());
    CHECK(directory.error().code == FsErrorCode::Exist);
    CHECK(held.names() == std::vector<std::string> { "taken", "takendir" });
    auto content = std::string {};
    std::getline(std::ifstream { held.directory.path() / "taken" }, content);
    CHECK(content == "theirs");
}

TEST_CASE("posix: a staging name already taken is passed over, never used", "[fs][native][access]")
{
    auto const held = HeldDirectory {};
    // A twin with the same key names what the creator will try first: only a holder of the key can.
    auto const planted = creatorOf(OwnerOnlyCreation::Staged).nextStagingName();
    std::filesystem::create_directory(held.directory.path() / planted);
    std::ofstream { held.directory.path() / planted / ".object" } << "decoy";
    auto const file =
        creatorOf(OwnerOnlyCreation::Staged).createFile(held.handle.get(), "secret", CreateFlags);
    REQUIRE(file.has_value());
    CHECK(held.names() == std::vector<std::string> { planted, "secret" });
    CHECK(std::filesystem::file_size(held.directory.path() / planted / ".object") == 5);
}

TEST_CASE("posix: staging names cannot be told in advance", "[fs][native][access]")
{
    // Every name a process id and an attempt number could spell is taken, as by a user who saw one.
    auto const held = HeldDirectory {};
    for (auto const attempt: std::views::iota(0, 64))
        std::filesystem::create_directory(
            held.directory.path()
            / (".core-cpp-owner-only." + std::to_string(::getpid()) + "." + std::to_string(attempt)));
    auto const creator = creatorOf(OwnerOnlyCreation::Staged);
    CHECK(creator.createFile(held.handle.get(), "secret", CreateFlags).has_value());
    CHECK(creator.createDirectory(held.handle.get(), "keys").has_value());
    // Each creation draws a name of its own, and another key draws others.
    auto const first = creatorOf(OwnerOnlyCreation::Staged);
    auto const other = creatorOf(OwnerOnlyCreation::Staged, StagingSeed + 1);
    auto const name = first.nextStagingName();
    CHECK(name != first.nextStagingName());
    CHECK(name != other.nextStagingName());
}

TEST_CASE("posix: a creation the file system will not hold owner-only leaves nothing behind",
          "[fs][native][access]")
{
    // A mode-ignoring file system (vfat or exfat under fmask, cifs under noperm) reads the new
    // object back wider than it was made: the creation is refused and what it made removed.
    auto const how = GENERATE(OwnerOnlyCreation::Direct, OwnerOnlyCreation::Staged);
    auto const held = HeldDirectory {};
    auto const creator = creatorOf(how, StagingSeed, &modeIgnoringStat);
    auto const file = creator.createFile(held.handle.get(), "secret", CreateFlags);
    REQUIRE_FALSE(file.has_value());
    CHECK(file.error().code == FsErrorCode::NotPermitted);
    auto const directory = creator.createDirectory(held.handle.get(), "keys");
    REQUIRE_FALSE(directory.has_value());
    CHECK(directory.error().code == FsErrorCode::NotPermitted);
    CHECK(held.names().empty());
}

TEST_CASE("posix: a placeholder the file system will not hold owner-only is removed", "[fs][native][access]")
{
    // A staged directory reads back its staging directory, itself, then the placeholder under the
    // final name: only the third is wider than it was made.
    auto const held = HeldDirectory {};
    auto reads = 0;
    auto const creator =
        creatorOf(OwnerOnlyCreation::Staged, StagingSeed, [&reads](int handle, struct ::stat* status) {
            ++reads;
            return reads == 3 ? modeIgnoringStat(handle, status) : ::fstat(handle, status);
        });
    auto const directory = creator.createDirectory(held.handle.get(), "keys");
    REQUIRE_FALSE(directory.has_value());
    CHECK(directory.error().code == FsErrorCode::NotPermitted);
    CHECK(reads == 3);
    CHECK(held.names().empty());
}

TEST_CASE("posix: a staged creation under an inheritable ACL is owner-only", "[fs][native][access]")
{
    auto const held = HeldDirectory {};
    auto const command = core::fs::test::inheritableAclCommand(held.directory.path());
    if (!ranSuccessfully(command))
        SKIP("no inheritable ACL here: the tool is missing, or the file system keeps no such ACL");
    auto const creator = creatorOf(OwnerOnlyCreation::Staged);
    auto const file = creator.createFile(held.handle.get(), "secret", CreateFlags);
    REQUIRE(file.has_value());
    CHECK(scopeOf(file->get()) == AccessScope::OwnerOnly);
    REQUIRE(creator.createDirectory(held.handle.get(), "keys").has_value());
    CHECK(modeOf(held.directory.path() / "keys") == 0700);
    CHECK(held.names() == std::vector<std::string> { "keys", "secret" });
}

TEST_CASE("posix: who may reach an object is judged through a handle only", "[fs][native][access]")
{
    // Linux decides from the handle whether the mode bounds access at all (an NFS or SMB mount, an
    // NFSv4 ACL); by name there is no handle to ask, so nothing is judged.
    auto const directory = core::testing::ScopedTempDir { "core-cpp-fs-facts" };
    auto const path = directory.path() / "plain";
    std::ofstream { path } << "x";
    struct ::stat status {};
    REQUIRE(::stat(path.c_str(), &status) == 0);
    CHECK_FALSE(core::fs::detail::accessFactsOf(core::platform::InvalidHandle, status).has_value());
}
