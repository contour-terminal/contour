// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>
#include <vtpty/Pty.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

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
