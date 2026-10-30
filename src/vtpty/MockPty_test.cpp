// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>
#include <vtpty/Pty.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <optional>
#include <string>
#include <thread>

namespace
{

constexpr auto TestPageSize =
    vtpty::PageSize { .lines = vtpty::LineCount(25), .columns = vtpty::ColumnCount(80) };

/// What a caller holding only the interface is told.
[[nodiscard]] std::optional<std::string> foregroundOf(vtpty::Pty const& pty)
{
    return pty.foregroundProcessName();
}

} // namespace

TEST_CASE("MockPty.foregroundProcessName", "[vtpty][mockpty]")
{
    auto pty = vtpty::MockPty { TestPageSize };
    CHECK_FALSE(foregroundOf(pty).has_value());

    pty.setForegroundProcessName("vim");
    CHECK(foregroundOf(pty) == std::optional<std::string> { "vim" });

    pty.setForegroundProcessName(std::nullopt);
    CHECK_FALSE(foregroundOf(pty).has_value());
}

TEST_CASE("Pty.inputEcho.isUnknownUnlessTheDeviceCanTell", "[vtpty][mockpty][inputecho]")
{
    auto pty = vtpty::MockPty { TestPageSize };
    auto const& device = static_cast<vtpty::Pty const&>(pty);
    CHECK(device.inputEcho() == vtpty::InputEcho::Unknown);

    pty.setInputEcho(vtpty::InputEcho::Hidden);
    CHECK(device.inputEcho() == vtpty::InputEcho::Hidden);
}

TEST_CASE("MockPty.closeFromTheReaderWhileTheOwnerAsks", "[vtpty][mockpty][threading]")
{
    // A session's terminal loop closes its device when a read ends it, while the GUI thread tearing the
    // session down asks whether it is closed yet -- the settings preview runs on a MockPty in the shipped
    // application, so this is the device's real threading, not only a test's.
    auto pty = vtpty::MockPty { TestPageSize };
    auto started = std::atomic<bool> { false };
    auto reader = std::thread { [&] {
        started.store(true);
        pty.close();
    } };
    while (!started.load())
        std::this_thread::yield();
    auto sawClosed = false;
    while (!sawClosed)
        sawClosed = pty.isClosed();
    reader.join();
    CHECK(pty.isClosed());
}
