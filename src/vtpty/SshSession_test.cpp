// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SshSession.hpp>
#include <vtpty/test/NonAsciiSample.hpp>

#include <crispy/BufferObject.hpp>

#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

#ifndef _WIN32
    #include <sys/socket.h>

    #include <unistd.h>

    #include <arpa/inet.h>
    #include <netinet/in.h>
#endif

namespace
{

/// Writes @p contents to @p path, as it is.
void writeFile(std::filesystem::path const& path, std::string_view contents)
{
    auto out = std::ofstream { path, std::ios::binary };
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

/// @return A host on the loopback interface's port 1, where nothing listens, logged into as @p username.
[[nodiscard]] vtpty::SshHostConfig refusingHost(std::string username)
{
    auto host = vtpty::SshHostConfig {};
    host.hostname = "127.0.0.1";
    host.port = 1;
    host.username = std::move(username);
    return host;
}

/// How long a reader in these cases waits on its own: far beyond WakeBound, so that a reader that returns
/// within that bound can only have been woken, however slowly a loaded machine runs it.
constexpr auto ReaderTimeout = std::chrono::seconds { 60 };

/// How long a case waits for a woken reader to return. A bound, not a latency budget: only a wake that is
/// lost altogether takes this long.
constexpr auto WakeBound = std::chrono::seconds { 30 };

} // namespace

TEST_CASE("SSH reader wakeup interrupts the socket wait", "[ssh][threading]")
{
    auto session = vtpty::SshSession { {}, {} };
    auto waiting = std::promise<void> {};
    auto entered = waiting.get_future();
    auto reader = std::async(std::launch::async, [&] {
        waiting.set_value();
        return session.waitForSocket(ReaderTimeout);
    });
    entered.wait();
    session.wakeupReader();
    CHECK(reader.wait_for(WakeBound) == std::future_status::ready);
    CHECK(reader.get() > 0);
}

TEST_CASE("SSH reader wakeup interrupts the pre-login wait", "[ssh][threading]")
{
    auto session = vtpty::SshSession { {}, {} };
    auto reader = std::async(std::launch::async, [&] {
        auto buffers = crispy::BufferObjectPool<char> { 4096 };
        auto storage = buffers.allocateBufferObject();
        auto const result = session.read(*storage, ReaderTimeout, 4096);
        return std::pair { result.has_value(), errno };
    });
    session.wakeupReader();
    CHECK(reader.wait_for(WakeBound) == std::future_status::ready);
    auto const [hasResult, error] = reader.get();
    CHECK_FALSE(hasResult);
    CHECK(error == EINTR);
}

TEST_CASE("SSH reader wakeups coalesce and are consumed once", "[ssh]")
{
    auto session = vtpty::SshSession { {}, {} };
    auto buffers = crispy::BufferObjectPool<char> { 4096 };
    auto storage = buffers.allocateBufferObject();
    session.wakeupReader();
    session.wakeupReader();
    CHECK(session.waitForSocket(std::chrono::milliseconds { 0 }) > 0);
    CHECK_FALSE(session.read(*storage, std::chrono::milliseconds { 0 }, 4096).has_value());
    CHECK(errno == EINTR);
    CHECK(session.waitForSocket(std::chrono::milliseconds { 0 }) == 0);
    CHECK_FALSE(session.read(*storage, std::chrono::milliseconds { 0 }, 4096).has_value());
    CHECK(errno == EAGAIN);
}

TEST_CASE("closing an SSH session wakes the pre-login reader", "[ssh][threading]")
{
    auto session = vtpty::SshSession { {}, {} };
    auto reader = std::async(std::launch::async, [&] {
        auto buffers = crispy::BufferObjectPool<char> { 4096 };
        auto storage = buffers.allocateBufferObject();
        return session.read(*storage, ReaderTimeout, 4096).has_value();
    });
    session.close();
    CHECK(reader.wait_for(WakeBound) == std::future_status::ready);
    CHECK_FALSE(reader.get());
}

TEST_CASE("closing an SSH session wakes a reader that waits without a timeout", "[ssh][threading]")
{
    // A reader parked in read() with no timeout, as the terminal's reader thread is when the tab is closed.
    auto session = vtpty::SshSession { refusingHost("user"), {} };

    SECTION("after a failed connection whose message has been read")
    {
        // Nothing listens on port 1, so the connection is refused.
        std::ignore = session.start();
        auto buffers = crispy::BufferObjectPool<char> { 4096 };
        auto storage = buffers.allocateBufferObject();
        while (session.read(*storage, std::chrono::milliseconds { 0 }, 4096).has_value())
        {
            // Drain the injected text.
        }
    }

    SECTION("before the session has left its initial state")
    {
    }

    auto waiting = std::promise<void> {};
    auto entered = waiting.get_future();
    auto reader = std::async(std::launch::async, [&] {
        auto buffers = crispy::BufferObjectPool<char> { 4096 };
        auto storage = buffers.allocateBufferObject();
        waiting.set_value();
        return session.read(*storage, std::nullopt, 4096).has_value();
    });
    entered.wait();
    session.close();

    auto const status = reader.wait_for(std::chrono::seconds { 5 });
    if (status != std::future_status::ready)
        session.wakeupReader(); // Should close() stop waking it: fail the check below, do not hang.
    CHECK(status == std::future_status::ready);
    CHECK_FALSE(reader.get());
}

#ifndef _WIN32
TEST_CASE("an SSH host that never speaks fails within ssh.connect_timeout", "[ssh]")
{
    // A listener that never accepts: the kernel completes the TCP handshake from the backlog, and then
    // nothing ever arrives -- the SSH handshake stalls, as on a host that hangs before its banner.
    auto const listener = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listener >= 0);
    auto const closeListener = core::Finally([listener]() { ::close(listener); });
    auto address = sockaddr_in {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(listener, reinterpret_cast<sockaddr const*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(listener, 1) == 0);
    auto length = static_cast<socklen_t>(sizeof(address));
    REQUIRE(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0);

    auto host = refusingHost("user");
    host.port = ntohs(address.sin_port);
    host.connectTimeout = std::chrono::seconds { 1 };

    auto const started = std::chrono::steady_clock::now();
    auto result = vtpty::StartResult {};
    {
        auto session = vtpty::SshSession { host, {} };
        result = session.start();
        auto const failedAfter = std::chrono::steady_clock::now() - started;
        // libssh2 counts its timeout in whole seconds, so it can run up to a second over.
        CHECK(failedAfter < std::chrono::seconds { 5 });
    }
    // The destructor tears a timed-out session down without waiting on the host.
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds { 6 });
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().detail.contains("ssh.connect_timeout"));
    CHECK(result.error().detail.contains("the SSH handshake"));
}
#endif

TEST_CASE("which budget governs each state, and what it is called", "[ssh]")
{
    using State = vtpty::SshSession::State;
    struct Row
    {
        State state;
        std::string_view stage;
        vtpty::SshStepBudget budget = vtpty::SshStepBudget::Connect;
    };
    auto const table = std::array {
        Row { State::Initial, "connecting", vtpty::SshStepBudget::None },
        Row { State::Started, "the TCP connect" },
        Row { State::Connect, "the TCP connect" },
        Row { State::Handshake, "the SSH handshake" },
        Row { State::VerifyHostkey, "host key verification" },
        Row { State::VerifyHostkeyWaitForInput, "host key verification", vtpty::SshStepBudget::None },
        Row { State::AuthenticateAgent, "authentication", vtpty::SshStepBudget::Agent },
        Row { State::AuthenticatePrivateKeyStart, "authentication" },
        Row { State::AuthenticatePrivateKeyRequest, "authentication" },
        Row { State::AuthenticatePrivateKeyWaitForInput, "authentication", vtpty::SshStepBudget::None },
        Row { State::AuthenticatePrivateKey, "authentication" },
        Row { State::AuthenticatePasswordStart, "authentication" },
        Row { State::AuthenticatePasswordWaitForInput, "authentication", vtpty::SshStepBudget::None },
        Row { State::AuthenticatePassword, "authentication" },
        Row { State::InstallTerminfoOpenChannel, "the terminfo install" },
        Row { State::InstallTerminfoExec, "the terminfo install" },
        Row { State::InstallTerminfoRead, "the terminfo install" },
        Row { State::OpenChannel, "opening the session channel" },
        Row { State::RequestAuthAgent, "the agent forwarding request" },
        Row { State::RequestPty, "the PTY request" },
        Row { State::SetEnv, "sending the environment" },
        Row { State::StartShell, "starting the shell" },
        Row { State::Operational, "connecting", vtpty::SshStepBudget::None },
        Row { State::ResizeScreen, "connecting", vtpty::SshStepBudget::None },
        Row { State::Failure, "connecting", vtpty::SshStepBudget::None },
        Row { State::Closed, "connecting", vtpty::SshStepBudget::None },
    };
    for (auto const& row: table)
    {
        INFO(std::format("{}", row.state));
        CHECK(vtpty::connectStageName(row.state) == row.stage);
        CHECK(vtpty::sshStepBudgetFor(row.state) == row.budget);
    }
}

TEST_CASE("SSH injected text is read no further than the buffer's free space", "[ssh]")
{
    // The user name is echoed in the "Starting SSH session" text start() injects, so a long one makes
    // that text far longer than the buffer it is read into. Nothing need listen on the port: the text
    // is injected before the connection is attempted.
    auto const username = std::string(600, 'u');
    auto session = vtpty::SshSession { refusingHost(username), {} };
    std::ignore = session.start();

    // The pool rounds a buffer up, so ask the one it hands out how much it holds.
    auto buffers = crispy::BufferObjectPool<char> { 16 };
    auto storage = buffers.allocateBufferObject();
    auto const freeSpace = storage->bytesAvailable();
    REQUIRE(freeSpace < username.size());

    auto text = std::string {};
    while (auto const result = session.read(*storage, std::chrono::milliseconds { 0 }, 4096))
    {
        REQUIRE(result->data.size() <= freeSpace);
        text += result->data;
    }
    CHECK(text.contains(username));
}

TEST_CASE("readSshKeyFiles reads a key pair under a directory beyond ASCII", "[ssh]")
{
    // Read through the paths as they are, for libssh2 to take in memory: its own fopen() reads a narrow
    // name, which Windows spells in the ANSI code page.
    auto const temp = core::testing::ScopedTempDir { "contour-ssh-keys" };
    auto const directory = temp.path() / vtpty::testing::NonAsciiName;
    std::filesystem::create_directory(directory);
    writeFile(directory / "id_ed25519", "private\r\nkey\n");
    writeFile(directory / "id_ed25519.pub", "ssh-ed25519 public\n");

    auto config = vtpty::SshHostConfig {};
    config.privateKeyFile = directory / "id_ed25519";

    SECTION("the private key alone")
    {
        auto const keys = vtpty::readSshKeyFiles(config);
        REQUIRE(keys.has_value());
        CHECK(keys->privateKey == "private\r\nkey\n");
        CHECK(keys->publicKey.empty());
    }

    SECTION("with its public key")
    {
        config.publicKeyFile = directory / "id_ed25519.pub";
        auto const keys = vtpty::readSshKeyFiles(config);
        REQUIRE(keys.has_value());
        CHECK(keys->privateKey == "private\r\nkey\n");
        CHECK(keys->publicKey == "ssh-ed25519 public\n");
    }

    SECTION("a key file that is not there")
    {
        config.publicKeyFile = directory / "missing.pub";
        auto const keys = vtpty::readSshKeyFiles(config);
        REQUIRE_FALSE(keys.has_value());
        CHECK(keys.error() == std::errc::no_such_file_or_directory);
    }
}

TEST_CASE("SshHostConfig::toString names each key file under its own label", "[ssh]")
{
    auto config = vtpty::SshHostConfig {};
    config.hostname = "example.invalid";
    config.username = "user";
    config.privateKeyFile = "id_ed25519";
    config.publicKeyFile = "id_ed25519.pub";
    config.knownHostsFile = "known_hosts";
    CHECK(config.toString()
          == "user@example.invalid:22, private key: id_ed25519, public key: id_ed25519.pub, "
             "known hosts: known_hosts, ForwardAgent: No");
}

TEST_CASE("SshHostConfig spells its files in UTF-8", "[ssh]")
{
    // path::string() spelled them in the ANSI code page on Windows, and threw for a name beyond it.
    auto const spelled = std::format("/home/{}/.ssh", vtpty::testing::NonAsciiNameUtf8);
    auto config = vtpty::SshHostConfig {};
    config.hostname = "example.invalid";
    config.privateKeyFile = core::pathFromTrustedUtf8(spelled + "/id_ed25519");
    config.knownHostsFile = core::pathFromTrustedUtf8(spelled + "/known_hosts");

    CHECK(config.toString().contains(std::format("private key: {}/id_ed25519", spelled)));
    CHECK(config.toString().contains(std::format("known hosts: {}/known_hosts", spelled)));
    CHECK(config.toConfigString().contains(std::format("IdentityFile {}/id_ed25519\n", spelled)));
    CHECK(config.toConfigString().contains(std::format("KnownHostsFile {}/known_hosts\n", spelled)));
}

TEST_CASE("loadSshConfig reads an IdentityFile beyond ASCII as UTF-8", "[ssh]")
{
    // The file is UTF-8. Assigned to a path as a narrow string, Windows read it in the ANSI code page.
    auto const temp = core::testing::ScopedTempDir { "contour-ssh-config" };
    auto const identity = std::format("/home/{}/.ssh/id_ed25519", vtpty::testing::NonAsciiNameUtf8);
    writeFile(temp / "config",
              std::format("Host work\n    HostName example.invalid\n    IdentityFile {}\n", identity));

    auto const configs = vtpty::loadSshConfig(temp / "config");
    REQUIRE(configs.has_value());
    REQUIRE(configs->contains("work"));
    CHECK(configs->at("work").privateKeyFile == core::pathFromTrustedUtf8(identity));
}
