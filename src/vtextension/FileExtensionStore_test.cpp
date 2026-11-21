// SPDX-License-Identifier: Apache-2.0
//
// The file-backed extension store over an in-memory filesystem: the key grammar, the limits, and that a write
// is reported durable only once every step of it succeeded.
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/DaemonWorld.hpp>
#include <vtextension/ExtensionStore.hpp>
#include <vtextension/FileExtensionStore.hpp>
#include <vtextension/FileLocker.hpp>
#include <vtextension/testing/FailingFileSystem.hpp>
#include <vtextension/testing/FakeFileLocker.hpp>

using namespace std::string_view_literals;

using core::platform::testing::InMemoryFileSystem;
using vtextension::FileExtensionStore;
using vtextension::HostError;
using vtextension::LockError;
using vtextension::NativeFileLocker;
using vtextension::StoreError;
using vtextension::StoreLimits;
using vtextension::testing::FailingFileSystem;
using vtextension::testing::FakeFileLocker;
using vtextension::testing::FakeLocking;
using vtextension::testing::FileOperation;

namespace
{

constexpr auto StoreDirectory = "/state/extensions/orchestrator/store"sv;

/// @return The path of @p name in the store's directory.
std::filesystem::path inStore(std::string_view name)
{
    return std::filesystem::path { StoreDirectory } / name;
}

/// Opens the store at StoreDirectory over @p fileSystem, which must succeed. Each call has a locker of its
/// own, as another process would: two stores of a test that reach one directory do not exclude each other.
FileExtensionStore openStore(core::platform::FileSystem const& fileSystem, StoreLimits limits = {})
{
    auto store = FileExtensionStore::open(fileSystem, FakeFileLocker {}, StoreDirectory, limits);
    REQUIRE(store.has_value());
    return std::move(*store);
}

/// @return The names in the store's directory that start with `~`.
std::vector<std::string> temporariesIn(core::platform::FileSystem const& fileSystem)
{
    auto entries = fileSystem.listDirectory(StoreDirectory);
    REQUIRE(entries.has_value());
    auto names = std::vector<std::string> {};
    for (auto const& entry: *entries)
        if (auto name = entry.path.filename().string(); name.starts_with('~'))
            names.push_back(std::move(name));
    return names;
}

/// @return @p store's value of @p key, which must be readable.
std::optional<std::string> valueOf(FileExtensionStore const& store, std::string_view key)
{
    auto value = store.get(key);
    REQUIRE(value.has_value());
    return *value;
}

} // namespace

TEST_CASE("open creates the store directory with mode 0700", "[daemon][store]")
{
    auto const files = InMemoryFileSystem {};
    [[maybe_unused]] auto const store = openStore(files);
    CHECK(files.isDirectory(StoreDirectory));
    auto const mode = files.permissions(StoreDirectory);
    REQUIRE(mode.has_value());
    CHECK(*mode == std::filesystem::perms::owner_all);
}

TEST_CASE("an uncreatable directory is an open error", "[daemon][store]")
{
    auto files = InMemoryFileSystem {};
    auto const failing = FailingFileSystem { files, FileOperation::CreateDirectories };
    auto const store = FileExtensionStore::open(failing, FakeFileLocker {}, StoreDirectory, {});
    REQUIRE_FALSE(store.has_value());
    CHECK(store.error() == StoreError::Io);
}

TEST_CASE("a directory that cannot be made private is an open error", "[daemon][store]")
{
    auto files = InMemoryFileSystem {};
    auto const failing = FailingFileSystem { files, FileOperation::SetPermissions };
    auto const store = FileExtensionStore::open(failing, FakeFileLocker {}, StoreDirectory, {});
    REQUIRE_FALSE(store.has_value());
    CHECK(store.error() == StoreError::Io);
}

TEST_CASE("round trip", "[daemon][store]")
{
    auto const files = InMemoryFileSystem {};
    auto store = openStore(files);
    CHECK(valueOf(store, "run/42") == std::nullopt);
    REQUIRE(store.set("run/42", "first"));
    CHECK(valueOf(store, "run/42") == "first");
    REQUIRE(store.set("run/42", "second"));
    CHECK(valueOf(store, "run/42") == "second");
    REQUIRE(store.set("run/empty", ""));
    CHECK(valueOf(store, "run/empty") == "");
    REQUIRE(store.remove("run/42"));
    CHECK(valueOf(store, "run/42") == std::nullopt);
    CHECK(temporariesIn(files).empty());
}

TEST_CASE("a new instance over the same directory sees the value", "[daemon][store]")
{
    auto const files = InMemoryFileSystem {};
    {
        auto store = openStore(files);
        REQUIRE(store.set("cfg/poll", "120"));
    }
    auto const reopened = openStore(files);
    CHECK(valueOf(reopened, "cfg/poll") == "120");
    auto const keys = reopened.keys("");
    REQUIRE(keys.has_value());
    CHECK(*keys == std::vector<std::string> { "cfg/poll" });
}

TEST_CASE("bad keys", "[daemon][store]")
{
    auto const files = InMemoryFileSystem {};
    auto store = openStore(files);
    auto const tooLong = std::string(129, 'k');
    for (auto const key: { ""sv,
                           ".."sv,
                           "a/../b"sv,
                           "a/./b"sv,
                           "/abs"sv,
                           "a//b"sv,
                           "x.tmp-1"sv,
                           "A"sv,
                           "a b"sv,
                           "~x"sv,
                           std::string_view { tooLong },
                           "a/"sv,
                           "."sv,
                           "a%b"sv,
                           "a."sv,
                           "a./b"sv,
                           "x/b.."sv,
                           "con"sv,
                           "con.txt"sv,
                           "a/nul"sv,
                           "prn"sv,
                           "aux.b.c"sv,
                           "com1"sv,
                           "com9/x"sv,
                           "lpt1"sv,
                           "lpt9.log"sv,
                           "#lock"sv })
    {
        INFO(std::format("key '{}'", key));
        CHECK(store.set(key, "v") == std::unexpected(StoreError::BadKey));
        CHECK(store.get(key) == std::unexpected(StoreError::BadKey));
        CHECK(store.remove(key) == std::unexpected(StoreError::BadKey));
    }
    auto const keys = store.keys("");
    REQUIRE(keys.has_value());
    CHECK(keys->empty());

    auto const longest = std::string(128, 'k');
    for (auto const key: { std::string_view { longest },
                           "a"sv,
                           "0.9/x_y-z"sv,
                           "a.b/.c/d"sv,
                           "console"sv,
                           "com10"sv,
                           "nul0.x"sv,
                           "lpt/con1"sv })
    {
        INFO(std::format("key '{}'", key));
        CHECK(store.set(key, "v").has_value());
        CHECK(valueOf(store, key) == "v");
    }
}

TEST_CASE("a value of exactly 64 KiB is kept, one byte more is TooLarge", "[daemon][store]")
{
    auto const files = InMemoryFileSystem {};
    auto store = openStore(files);
    auto const largest = std::string(std::size_t { 64 } << 10, 'v');
    REQUIRE(store.set("big", largest));
    CHECK(valueOf(store, "big") == largest);
    CHECK(store.set("big", largest + "v") == std::unexpected(StoreError::TooLarge));
    CHECK(valueOf(store, "big") == largest);
}

TEST_CASE("the 1025th key is refused, overwriting at 1024 is not", "[daemon][store]")
{
    auto const files = InMemoryFileSystem {};
    auto store = openStore(files);
    for (auto const index: std::views::iota(0, 1024))
        REQUIRE(store.set(std::format("k/{}", index), "v"));
    CHECK(store.set("k/1024", "v") == std::unexpected(StoreError::TooManyKeys));
    CHECK(valueOf(store, "k/1024") == std::nullopt);
    CHECK(store.set("k/7", "again").has_value());
    CHECK(valueOf(store, "k/7") == "again");
    REQUIRE(store.remove("k/0"));
    CHECK(store.set("k/1024", "v").has_value());
}

TEST_CASE("a torn temporary is ignored", "[daemon][store]")
{
    auto files = InMemoryFileSystem {};
    auto store = openStore(files);
    REQUIRE(store.set("run/a", "kept"));
    REQUIRE(files.writeFile(inStore("~3"), "torn"));
    auto const keys = store.keys("");
    REQUIRE(keys.has_value());
    CHECK(*keys == std::vector<std::string> { "run/a" });
    CHECK(valueOf(store, "run/a") == "kept");
    REQUIRE(store.set("run/b", "written past it"));
    CHECK(valueOf(store, "run/b") == "written past it");
}

TEST_CASE("open removes a torn temporary", "[daemon][store]")
{
    auto files = InMemoryFileSystem {};
    REQUIRE(files.createDirectories(StoreDirectory));
    REQUIRE(files.writeFile(inStore("~0"), "torn"));
    REQUIRE(files.writeFile(inStore("run%a"), "kept"));
    auto const store = openStore(files);
    CHECK(temporariesIn(files).empty());
    CHECK(valueOf(store, "run/a") == "kept");
}

TEST_CASE("a failing sync is an error, never success", "[daemon][store]")
{
    auto files = InMemoryFileSystem {};
    auto store = openStore(files);
    REQUIRE(store.set("run/a", "previous"));

    for (auto const operation:
         { FileOperation::CreateNewFile, FileOperation::WriteFile, FileOperation::SyncFile })
    {
        INFO(std::format("failing operation {}", std::to_underlying(operation)));
        auto const failing = FailingFileSystem { files, operation };
        auto failingStore = openStore(failing);
        CHECK(failingStore.set("run/a", "next") == std::unexpected(StoreError::Io));
        CHECK(valueOf(store, "run/a") == "previous");
        CHECK(temporariesIn(files).empty());
    }
}

TEST_CASE("a failing replace is an error", "[daemon][store]")
{
    auto files = InMemoryFileSystem {};
    auto store = openStore(files);
    REQUIRE(store.set("run/a", "previous"));
    auto const failing = FailingFileSystem { files, FileOperation::ReplaceFile };
    auto failingStore = openStore(failing);
    CHECK(failingStore.set("run/a", "next") == std::unexpected(StoreError::Io));
    CHECK(failingStore.set("run/new", "next") == std::unexpected(StoreError::Io));
    CHECK(valueOf(store, "run/a") == "previous");
    CHECK(valueOf(store, "run/new") == std::nullopt);
    CHECK(temporariesIn(files).empty());
}

TEST_CASE("a failing directory sync is an error", "[daemon][store]")
{
    auto files = InMemoryFileSystem {};
    auto store = openStore(files);
    REQUIRE(store.set("run/a", "previous"));
    auto const failing = FailingFileSystem { files, FileOperation::SyncDirectory };
    auto failingStore = openStore(failing);
    CHECK(failingStore.set("run/b", "next") == std::unexpected(StoreError::Io));
    CHECK(failingStore.remove("run/a") == std::unexpected(StoreError::Io));
    CHECK(temporariesIn(files).empty());
}

TEST_CASE("a failing remove is an error", "[daemon][store]")
{
    auto files = InMemoryFileSystem {};
    auto store = openStore(files);
    REQUIRE(store.set("run/a", "kept"));
    auto const failing = FailingFileSystem { files, FileOperation::Remove };
    auto failingStore = openStore(failing);
    CHECK(failingStore.remove("run/a") == std::unexpected(StoreError::Io));
    CHECK(valueOf(store, "run/a") == "kept");
}

TEST_CASE("remove of an absent key succeeds", "[daemon][store]")
{
    auto const files = InMemoryFileSystem {};
    auto store = openStore(files);
    CHECK(store.remove("never/set").has_value());
}

TEST_CASE("keys filters by prefix", "[daemon][store]")
{
    auto const files = InMemoryFileSystem {};
    auto store = openStore(files);
    for (auto const key: { "run/b"sv, "cfg"sv, "run/a"sv })
        REQUIRE(store.set(key, "v"));
    auto const runs = store.keys("run/");
    REQUIRE(runs.has_value());
    CHECK(*runs == std::vector<std::string> { "run/a", "run/b" });
    auto const all = store.keys("");
    REQUIRE(all.has_value());
    CHECK(*all == std::vector<std::string> { "cfg", "run/a", "run/b" });
    auto const none = store.keys("zzz");
    REQUIRE(none.has_value());
    CHECK(none->empty());
}

TEST_CASE("a store error reaches the guest as its host-error", "[daemon][store]")
{
    CHECK(hostErrorOf(StoreError::BadKey) == HostError::BadArgument);
    CHECK(hostErrorOf(StoreError::TooLarge) == HostError::Limit);
    CHECK(hostErrorOf(StoreError::TooManyKeys) == HostError::Limit);
    CHECK(hostErrorOf(StoreError::Io) == HostError::Gone);
    CHECK(hostErrorOf(StoreError::Busy) == HostError::Gone);
}

TEST_CASE("a second open of the same store is Busy until the first is gone", "[daemon][store]")
{
    auto const files = InMemoryFileSystem {};
    auto const locker = FakeFileLocker {};
    {
        auto first = FileExtensionStore::open(files, locker, StoreDirectory, {});
        REQUIRE(first.has_value());
        CHECK(locker.held().size() == 1);
        auto const second = FileExtensionStore::open(files, locker, StoreDirectory, {});
        CHECK(second == std::unexpected(StoreError::Busy));
        // Moving the store moves its lock with it.
        auto const moved = std::move(*first);
        CHECK(locker.held().size() == 1);
    }
    CHECK(locker.held().empty());
    CHECK(FileExtensionStore::open(files, locker, StoreDirectory, {}).has_value());
}

TEST_CASE("a lock that cannot be taken is an open error", "[daemon][store]")
{
    auto const files = InMemoryFileSystem {};
    auto const store =
        FileExtensionStore::open(files, FakeFileLocker { FakeLocking::FailsWithIo }, StoreDirectory, {});
    CHECK(store == std::unexpected(StoreError::Io));
}

TEST_CASE("a symlinked store directory is refused", "[daemon][store]")
{
    auto files = InMemoryFileSystem {};
    REQUIRE(files.createDirectories("/elsewhere"));
    REQUIRE(files.createDirectories(std::filesystem::path { StoreDirectory }.parent_path()));
    files.addSymlink(StoreDirectory, "/elsewhere");
    auto const store = FileExtensionStore::open(files, FakeFileLocker {}, StoreDirectory, {});
    CHECK(store == std::unexpected(StoreError::Io));
    auto const mode = files.permissions("/elsewhere");
    REQUIRE(mode.has_value());
    CHECK(*mode != std::filesystem::perms::owner_all);
}

TEST_CASE("a symlink under a key's name is not a value", "[daemon][store]")
{
    auto files = InMemoryFileSystem {};
    auto store = openStore(files);
    REQUIRE(files.writeFile("/elsewhere/secret", "not the store's"));
    files.addSymlink(inStore("run%a"), "/elsewhere/secret");
    REQUIRE(store.set("run/b", "v"));
    auto const keys = store.keys("");
    REQUIRE(keys.has_value());
    CHECK(*keys == std::vector<std::string> { "run/b" });
    CHECK(valueOf(store, "run/a") == std::nullopt);
}

TEST_CASE("the native locker refuses a second lock until the first is released", "[daemon][store]")
{
    auto const directory = std::filesystem::temp_directory_path()
                           / std::format("vtextension_test-lock-{}",
                                         std::chrono::steady_clock::now().time_since_epoch().count());
    REQUIRE(std::filesystem::create_directories(directory));
    auto const lockFile = directory / "#lock";
    auto const locker = NativeFileLocker {};
    {
        auto const first = locker.tryLockExclusive(lockFile);
        REQUIRE(first.has_value());
        CHECK(locker.tryLockExclusive(lockFile).error() == LockError::Held);
    }
    CHECK(locker.tryLockExclusive(lockFile).has_value());
#ifndef _WIN32
    // A link planted as the lock file is refused, never followed.
    auto const link = directory / "link";
    std::filesystem::create_symlink(lockFile, link);
    CHECK(locker.tryLockExclusive(link).error() == LockError::Io);
#endif
    std::filesystem::remove_all(directory);
}
