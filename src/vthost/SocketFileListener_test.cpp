// SPDX-License-Identifier: Apache-2.0
#include <core/Utf8Path.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <ranges>
#include <utility>

#include <vthost/SocketFileListener.hpp>

using vthost::SocketFileListener;
using vthost::SocketFileRemoval;

namespace
{

/// Whether a socket file still existed when its listener closed.
enum class FileAtClose : uint8_t
{
    Gone = 0,
    Present,
};

/// A listener that accepts nothing, counts its closes, notes whether its socket file still existed as it
/// closed, and answers a made-up handle.
class CountingListener final: public core::net::IListener
{
  public:
    CountingListener(int& closes,
                     core::platform::NativeHandle handle,
                     std::filesystem::path file = {},
                     std::optional<FileAtClose>* atClose = nullptr):
        _closes { closes }, _handle { handle }, _file { std::move(file) }, _atClose { atClose }
    {
    }

    [[nodiscard]] core::async::Task<core::net::AcceptResult> accept() override
    {
        co_return std::unexpected { core::net::NetError {} };
    }
    [[nodiscard]] std::uint16_t boundPort() const noexcept override { return 0; }
    [[nodiscard]] core::platform::NativeHandle nativeHandle() const noexcept override { return _handle; }

  protected:
    void doClose() noexcept override
    {
        ++_closes;
        if (_atClose != nullptr)
            *_atClose = std::filesystem::exists(_file) ? FileAtClose::Present : FileAtClose::Gone;
    }

  private:
    int& _closes;
    core::platform::NativeHandle _handle;
    std::filesystem::path _file;
    std::optional<FileAtClose>* _atClose;
};

/// A socket file stand-in at `<temp>/control`, and a listener on it.
struct Endpoint
{
    core::testing::ScopedTempDir temp { "contour-socket-file" };
    std::filesystem::path path = [this] {
        auto file = temp.path() / "control";
        std::ofstream { file } << "";
        return file;
    }();
    int closes = 0;
    std::optional<FileAtClose> atClose; ///< Whether the socket file existed as the inner listener closed.

    [[nodiscard]] std::optional<SocketFileListener> listen(SocketFileRemoval removal)
    {
        return std::optional<SocketFileListener> { std::in_place,
                                                   std::make_unique<CountingListener>(
                                                       closes, core::platform::InvalidHandle, path, &atClose),
                                                   path,
                                                   removal };
    }
};

} // namespace

TEST_CASE("an inherited listener removes its socket file as it closes", "[vthost][listener]")
{
    auto e = Endpoint {};
    auto listener = e.listen(SocketFileRemoval::OnClose);
    listener->close();
    CHECK(e.closes == 1);
    CHECK_FALSE(std::filesystem::exists(e.path));
    // Removed before the listener closed: while it is open, no other daemon can have bound the path anew.
    CHECK(e.atClose == FileAtClose::Gone);
    listener.reset(); // closed once only
    CHECK(e.closes == 1);
}

TEST_CASE("an inherited listener destroyed unclosed removes its socket file too", "[vthost][listener]")
{
    auto e = Endpoint {};
    e.listen(SocketFileRemoval::OnClose).reset();
    CHECK(e.closes == 1);
    CHECK_FALSE(std::filesystem::exists(e.path));
}

TEST_CASE("a socket file bound anew at the path survives the old listener's close", "[vthost][listener]")
{
    auto e = Endpoint {};
    auto listener = e.listen(SocketFileRemoval::OnClose);
    // Another daemon took the path for a stale one and bound its own socket there. Moved aside rather than
    // removed: a bound socket keeps its file's inode taken, which a plain file removed would free for the new
    // one to reuse -- and the same inode on the same device reads as the same file.
    std::filesystem::rename(e.path, e.temp.path() / "unlinked");
    std::ofstream { e.path } << "another daemon's";
    listener->close();
    CHECK(e.closes == 1);
#ifndef _WIN32
    CHECK(std::filesystem::exists(e.path));
#endif
}

TEST_CASE("a daemon behind a keeper leaves the socket files to it; one behind none removes them",
          "[vthost][listener]")
{
    STATIC_CHECK(vthost::socketFileRemovalFor(vthost::KeeperPresence::Behind)
                 == SocketFileRemoval::LeftToKeeper);
    STATIC_CHECK(vthost::socketFileRemovalFor(vthost::KeeperPresence::None) == SocketFileRemoval::OnClose);
}

TEST_CASE("a listener behind a keeper leaves its socket file to the keeper", "[vthost][listener]")
{
    auto e = Endpoint {};
    auto listener = e.listen(SocketFileRemoval::LeftToKeeper);
    listener->close();
    CHECK(e.closes == 1);
    CHECK(e.atClose == FileAtClose::Present);
    CHECK(std::filesystem::exists(e.path));
    listener.reset();
    CHECK(std::filesystem::exists(e.path));
}

TEST_CASE("an inherited listener hands over the handle it accepts on", "[vthost][listener]")
{
    auto e = Endpoint {};
    auto const handle = core::platform::NativeHandle {
#ifdef _WIN32
        reinterpret_cast<core::platform::NativeHandle>(static_cast<std::intptr_t>(4711))
#else
        4711
#endif
    };
    auto const listener = SocketFileListener { std::make_unique<CountingListener>(e.closes, handle),
                                               e.path,
                                               SocketFileRemoval::OnClose };
    CHECK(listener.nativeHandle() == handle);
}

#ifndef _WIN32
TEST_CASE("a listener bound behind a keeper leaves its socket file, and another's, to the keeper",
          "[vthost][listener]")
{
    auto const temp = core::testing::ScopedTempDir { "contour-socket-file" };
    auto const path = temp.path() / "sockets" / "control";
    auto const backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };

    auto listener = vthost::listenUnixLeavingFile(loop, path);
    REQUIRE(listener.has_value());
    CHECK(std::filesystem::is_socket(path));
    // Nothing is left of the name it was bound at.
    CHECK(std::ranges::distance(std::filesystem::directory_iterator { path.parent_path() }) == 1);

    // A live daemon at the path refuses a second one.
    auto const second = vthost::listenUnixLeavingFile(loop, path);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code == core::net::NetErrorCode::AddressInUse);

    // The daemon stopped, and another bound the path: the first one's close leaves the other's file.
    std::filesystem::remove(path);
    auto other = core::net::listenUnix(loop, core::pathToUtf8(path));
    REQUIRE(other.has_value());
    auto const others = vthost::socketFileIdentityOf(path);
    (*listener)->close();
    listener->reset();
    CHECK(vthost::socketFileIdentityOf(path) == others);
}

TEST_CASE("a listener bound behind a keeper replaces a stale socket file and leaves its own as it closes",
          "[vthost][listener]")
{
    auto const temp = core::testing::ScopedTempDir { "contour-socket-file" };
    auto const path = temp.path() / "sockets" / "control";
    auto const backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    {
        // A crashed daemon's file: bound, then its descriptor closed, its name left.
        auto stale = core::net::listenUnix(loop, core::pathToUtf8(path));
        REQUIRE(stale.has_value());
        std::filesystem::rename(path, temp.path() / "kept");
        stale->reset();
        std::filesystem::rename(temp.path() / "kept", path);
    }
    auto listener = vthost::listenUnixLeavingFile(loop, path);
    REQUIRE(listener.has_value());
    (*listener)->close();
    CHECK(std::filesystem::is_socket(path)); // the keeper removes it
}
#endif
