// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <vthost/DaemonConsentKey.hpp>
#include <vthost/testing/CountingRandom.hpp>

#ifndef _WIN32
    #include <unistd.h>
#endif

using vthost::ConsentContext;
using vthost::ConsentNonce;
using vthost::ConsentProofCheck;

namespace
{

/// A consent directory in memory: what was written, by directory and name, and which files a run holds.
class MemoryConsentKeyFiles final: public vthost::IConsentKeyFiles
{
  public:
    using Files = std::map<std::filesystem::path, std::map<std::string, std::string>>;
    using Held = std::set<std::pair<std::filesystem::path, std::string>>;

    [[nodiscard]] std::expected<std::unique_ptr<vthost::ConsentKeyHold>, std::string> keep(
        std::filesystem::path const& directory,
        std::string_view name,
        std::string_view content) const override
    {
        files[directory][std::string { name }] = std::string { content };
        return std::make_unique<MemoryHold>(held, std::pair { directory, std::string { name } });
    }

    [[nodiscard]] std::expected<std::string, std::string> read(std::filesystem::path const& directory,
                                                               std::string_view name) const override
    {
        auto const in = files.find(directory);
        if (in == files.end() || !in->second.contains(std::string { name }))
            return std::unexpected(std::string { "no such file" });
        return in->second.at(std::string { name });
    }

    [[nodiscard]] std::expected<std::vector<std::string>, std::string> list(
        std::filesystem::path const& directory) const override
    {
        auto names = std::vector<std::string> {};
        if (auto const in = files.find(directory); in != files.end())
            for (auto const& [name, content]: in->second)
                names.push_back(name);
        return names;
    }

    [[nodiscard]] std::expected<vthost::KeyFileRemoval, std::string> removeUnlessHeld(
        std::filesystem::path const& directory, std::string_view name) const override
    {
        if (held.contains(std::pair { directory, std::string { name } }))
            return vthost::KeyFileRemoval::Held;
        if (auto const in = files.find(directory); in != files.end())
            in->second.erase(std::string { name });
        return vthost::KeyFileRemoval::Removed;
    }

    mutable Files files;
    mutable Held held;

  private:
    /// Marks its file held until it goes.
    class MemoryHold final: public vthost::ConsentKeyHold
    {
      public:
        MemoryHold(Held& held, Held::value_type entry): _held { held }, _entry { std::move(entry) }
        {
            _held.insert(_entry);
        }
        MemoryHold(MemoryHold const&) = delete;
        MemoryHold(MemoryHold&&) = delete;
        MemoryHold& operator=(MemoryHold const&) = delete;
        MemoryHold& operator=(MemoryHold&&) = delete;
        ~MemoryHold() override { _held.erase(_entry); }

      private:
        Held& _held;
        Held::value_type _entry;
    };
};

constexpr auto Socket = std::string_view { "/run/user/1000/contour/default" };

} // namespace

TEST_CASE("DaemonConsentKey: each run of a daemon has a key file of its own", "[vthost][consent]")
{
    auto const one = vthost::daemonConsentKeyName(Socket, 1);
    CHECK(one.ends_with("-0000000000000001.key"));
    CHECK(one != vthost::daemonConsentKeyName(Socket, 2));
    CHECK(one != vthost::daemonConsentKeyName("/run/user/1000/contour/work", 1));
    CHECK(one == vthost::daemonConsentKeyName("/run/user/1000/contour/./default", 1));
    CHECK(vthost::daemonConsentKeyDirectory("/state") == std::filesystem::path { "/state/consent" });
}

TEST_CASE("DaemonConsentKey: the key of the run a hello names is read back", "[vthost][consent]")
{
    auto files = MemoryConsentKeyFiles {};
    auto random = vthost::testing::CountingRandom {};
    auto const made = vthost::createDaemonConsentKey(files, random, "/state/consent", Socket, 7);
    REQUIRE(made.has_value());
    CHECK(vthost::readDaemonConsentKey(files, "/state/consent", Socket, 7) == made->key);
    // Another run's key is not this one's.
    CHECK_FALSE(vthost::readDaemonConsentKey(files, "/state/consent", Socket, 8).has_value());
}

TEST_CASE("DaemonConsentKey: the run that holds the socket clears what earlier runs there left",
          "[vthost][consent]")
{
    auto files = MemoryConsentKeyFiles {};
    auto random = vthost::testing::CountingRandom {};
    // The earlier run at this socket is over: its hold went with it. The other socket's daemon still runs.
    REQUIRE(vthost::createDaemonConsentKey(files, random, "/state/consent", Socket, 1).has_value());
    auto const other = vthost::createDaemonConsentKey(files, random, "/state/consent", "/other/socket", 5);
    REQUIRE(other.has_value());
    files.files["/state/consent"][vthost::daemonConsentKeyName(Socket, 2) + ".tmp"] = "half";

    auto const third = vthost::createDaemonConsentKey(files, random, "/state/consent", Socket, 3);
    REQUIRE(third.has_value());
    auto const names = *files.list("/state/consent");
    CHECK(names
          == std::vector { vthost::daemonConsentKeyName("/other/socket", 5),
                           vthost::daemonConsentKeyName(Socket, 3) });
    // Another daemon's key stays its own.
    CHECK(vthost::readDaemonConsentKey(files, "/state/consent", "/other/socket", 5).has_value());
}

TEST_CASE("DaemonConsentKey: a run's clean-up never removes the key of a run that still lives",
          "[vthost][consent]")
{
    // Two daemons that both came to hold the same socket (core-cpp's bind is check, unlink, bind): the
    // second's clean-up must not take the first one's key, nor the first's later restart the second's.
    auto files = MemoryConsentKeyFiles {};
    auto random = vthost::testing::CountingRandom {};
    auto first = vthost::createDaemonConsentKey(files, random, "/state/consent", Socket, 1);
    REQUIRE(first.has_value());
    auto const second = vthost::createDaemonConsentKey(files, random, "/state/consent", Socket, 2);
    REQUIRE(second.has_value());
    CHECK(vthost::readDaemonConsentKey(files, "/state/consent", Socket, 1) == first->key);
    CHECK(vthost::readDaemonConsentKey(files, "/state/consent", Socket, 2) == second->key);

    // Once the first run ended, the next one clears its file, and still leaves the second's.
    first->hold.reset();
    REQUIRE(vthost::createDaemonConsentKey(files, random, "/state/consent", Socket, 3).has_value());
    CHECK_FALSE(vthost::readDaemonConsentKey(files, "/state/consent", Socket, 1).has_value());
    CHECK(vthost::readDaemonConsentKey(files, "/state/consent", Socket, 2) == second->key);
}

TEST_CASE("DaemonConsentKey: a proof holds for its key, its run, its socket and its nonce only",
          "[vthost][consent]")
{
    auto random = vthost::testing::CountingRandom {};
    auto key = vthost::ConsentKey {};
    random.fill(key);
    auto nonce = ConsentNonce {};
    random.fill(nonce);
    auto otherNonce = ConsentNonce {};
    random.fill(otherNonce);
    auto otherKey = vthost::ConsentKey {};
    random.fill(otherKey);
    auto const context = ConsentContext { .daemonInstance = 7, .socket = std::string { Socket } };

    auto const proof = vthost::consentProofOf(key, context, nonce);
    CHECK(vthost::checkConsentProof(key, context, nonce, proof) == ConsentProofCheck::Proven);
    // Replayed against another connection's nonce, made with another key, for another run or another socket.
    CHECK(vthost::checkConsentProof(key, context, otherNonce, proof) == ConsentProofCheck::Refused);
    CHECK(vthost::checkConsentProof(otherKey, context, nonce, proof) == ConsentProofCheck::Refused);
    CHECK(vthost::checkConsentProof(
              key, ConsentContext { .daemonInstance = 8, .socket = context.socket }, nonce, proof)
          == ConsentProofCheck::Refused);
    CHECK(vthost::checkConsentProof(
              key, ConsentContext { .daemonInstance = 7, .socket = "/elsewhere" }, nonce, proof)
          == ConsentProofCheck::Refused);
    auto forged = proof;
    forged.back() ^= std::byte { 1 };
    CHECK(vthost::checkConsentProof(key, context, nonce, forged) == ConsentProofCheck::Refused);
}

#ifndef _WIN32
namespace
{
/// A scratch directory, removed as it goes.
struct ScratchDirectory
{
    ScratchDirectory():
        path { std::filesystem::temp_directory_path()
               / std::format("contour-consent-key-test-{}", ::getpid()) }
    {
        auto error = std::error_code {};
        std::filesystem::remove_all(path, error);
        std::filesystem::create_directories(path);
    }
    ScratchDirectory(ScratchDirectory const&) = delete;
    ScratchDirectory& operator=(ScratchDirectory const&) = delete;
    ScratchDirectory(ScratchDirectory&&) = delete;
    ScratchDirectory& operator=(ScratchDirectory&&) = delete;
    ~ScratchDirectory()
    {
        auto error = std::error_code {};
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};
} // namespace

TEST_CASE("DaemonConsentKey: on disk, the file is 0600 in a 0700 directory, and nothing else is read",
          "[vthost][consent]")
{
    namespace fs = std::filesystem;
    auto const scratch = ScratchDirectory {};
    auto const directory = vthost::daemonConsentKeyDirectory(scratch.path);
    auto const& files = vthost::nativeConsentKeyFiles();
    auto random = vthost::testing::CountingRandom {};

    auto const made = vthost::createDaemonConsentKey(files, random, directory, Socket, 1);
    REQUIRE(made.has_value());
    auto const file = directory / vthost::daemonConsentKeyName(Socket, 1);
    CHECK((fs::status(file).permissions() & fs::perms::all)
          == (fs::perms::owner_read | fs::perms::owner_write));
    CHECK((fs::status(directory).permissions() & fs::perms::all) == fs::perms::owner_all);
    CHECK(vthost::readDaemonConsentKey(files, directory, Socket, 1) == made->key);

    SECTION("a key file others can read is refused")
    {
        fs::permissions(file, fs::perms::group_read | fs::perms::others_read, fs::perm_options::add);
        CHECK_FALSE(vthost::readDaemonConsentKey(files, directory, Socket, 1).has_value());
    }
    SECTION("a link in a key file's place is not followed")
    {
        fs::rename(file, directory / "real.key");
        fs::create_symlink(directory / "real.key", file);
        CHECK_FALSE(vthost::readDaemonConsentKey(files, directory, Socket, 1).has_value());
    }
    SECTION("a directory others can enter is refused for reading")
    {
        fs::permissions(directory, fs::perms::group_exec, fs::perm_options::add);
        CHECK_FALSE(vthost::readDaemonConsentKey(files, directory, Socket, 1).has_value());
    }
}

TEST_CASE("DaemonConsentKey: on disk, a live run's key file is locked against every other run's clean-up",
          "[vthost][consent]")
{
    namespace fs = std::filesystem;
    auto const scratch = ScratchDirectory {};
    auto const directory = vthost::daemonConsentKeyDirectory(scratch.path);
    auto const& files = vthost::nativeConsentKeyFiles();
    auto random = vthost::testing::CountingRandom {};

    auto first = vthost::createDaemonConsentKey(files, random, directory, Socket, 1);
    REQUIRE(first.has_value());
    // A second daemon at the same socket -- the bind race -- cleans up as it starts: the first one's key
    // stays.
    auto second = vthost::createDaemonConsentKey(files, random, directory, Socket, 2);
    REQUIRE(second.has_value());
    CHECK(vthost::readDaemonConsentKey(files, directory, Socket, 1) == first->key);
    CHECK(vthost::readDaemonConsentKey(files, directory, Socket, 2) == second->key);
    auto const firstName = vthost::daemonConsentKeyName(Socket, 1);
    CHECK(files.removeUnlessHeld(directory, firstName) == vthost::KeyFileRemoval::Held);
    CHECK(fs::exists(directory / firstName));

    // The first run ends; the next one's clean-up takes its file, and still not the second's.
    first->hold.reset();
    auto const third = vthost::createDaemonConsentKey(files, random, directory, Socket, 3);
    REQUIRE(third.has_value());
    CHECK_FALSE(fs::exists(directory / firstName));
    CHECK(vthost::readDaemonConsentKey(files, directory, Socket, 2) == second->key);

    // What no run holds -- a crashed run's temporary file, a link -- goes.
    auto const leftover = vthost::daemonConsentKeyName(Socket, 4) + ".tmp";
    {
        auto out = std::ofstream(directory / leftover);
        out << "half";
    }
    fs::create_symlink(scratch.path / "nowhere", directory / vthost::daemonConsentKeyName(Socket, 5));
    CHECK(files.removeUnlessHeld(directory, leftover) == vthost::KeyFileRemoval::Removed);
    CHECK(files.removeUnlessHeld(directory, vthost::daemonConsentKeyName(Socket, 5))
          == vthost::KeyFileRemoval::Removed);
    CHECK(files.removeUnlessHeld(directory, "gone.key") == vthost::KeyFileRemoval::Removed);
    auto const names = files.list(directory);
    REQUIRE(names.has_value());
    CHECK(names->size() == 2U); // the second's and the third's keys
}

TEST_CASE("DaemonConsentKey: a consent directory planted as a link is refused, by the daemon and a window",
          "[vthost][consent]")
{
    namespace fs = std::filesystem;
    auto const scratch = ScratchDirectory {};
    // Someone points the consent directory at a place of their choosing.
    auto const elsewhere = scratch.path / "elsewhere";
    fs::create_directories(elsewhere);
    fs::permissions(elsewhere, fs::perms::owner_all, fs::perm_options::replace);
    auto const directory = vthost::daemonConsentKeyDirectory(scratch.path);
    fs::create_directory_symlink(elsewhere, directory);
    auto const& files = vthost::nativeConsentKeyFiles();
    auto random = vthost::testing::CountingRandom {};

    auto const made = vthost::createDaemonConsentKey(files, random, directory, Socket, 1);
    REQUIRE_FALSE(made.has_value());
    CHECK(fs::is_empty(elsewhere));

    // A key planted behind the link is not read either.
    auto const planted = elsewhere / vthost::daemonConsentKeyName(Socket, 1);
    {
        auto out = std::ofstream(planted);
        out << std::string(64, 'a');
    }
    fs::permissions(planted, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    CHECK_FALSE(vthost::readDaemonConsentKey(files, directory, Socket, 1).has_value());
}
#endif
