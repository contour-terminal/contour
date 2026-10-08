// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ScopeRequestEncoding.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <string>
#include <string_view>
#include <variant>

using vtpty::ScopeError;
using vtpty::ScopeProperty;
using vtpty::ScopeProtocol;

namespace
{

/// @return A request for pid 4242 speaking @p protocol, with a pidfd of 7 when @p pidfd says so.
vtpty::ScopeRequest requestFor(ScopeProtocol protocol, int pidfd)
{
    return vtpty::ScopeRequest { .unitName = "contour-session-1-4242-1.scope",
                                 .slice = "app.slice",
                                 .description = "Contour session",
                                 .protocol = protocol,
                                 .pid = 4242,
                                 .pidfd = pidfd,
                                 .memoryLimit = std::nullopt };
}

/// @return The property named @p name, or null.
ScopeProperty const* find(std::vector<ScopeProperty> const& properties, std::string_view name)
{
    auto const found = std::ranges::find(properties, name, &ScopeProperty::name);
    return found != properties.end() ? &*found : nullptr;
}

} // namespace

TEST_CASE("ScopeRequestEncoding.current protocol", "[placement][encoding]")
{
    auto const properties = vtpty::scopeProperties(requestFor(ScopeProtocol::Current, 7));

    auto const* const oomPolicy = find(properties, "OOMPolicy");
    REQUIRE(oomPolicy != nullptr);
    CHECK(std::get<std::string>(oomPolicy->value) == "continue");

    auto const* const pidfds = find(properties, "PIDFDs");
    REQUIRE(pidfds != nullptr);
    CHECK(std::get<vtpty::PidFdList>(pidfds->value).pidfd == 7);
    CHECK(find(properties, "PIDs") == nullptr);

    CHECK(std::get<std::string>(find(properties, "Slice")->value) == "app.slice");
    CHECK(std::get<std::string>(find(properties, "CollectMode")->value) == "inactive-or-failed");
    CHECK(find(properties, "MemoryMax") == nullptr);
}

TEST_CASE("ScopeRequestEncoding.current protocol without a pidfd names the pid", "[placement][encoding]")
{
    auto const properties = vtpty::scopeProperties(requestFor(ScopeProtocol::Current, -1));
    CHECK(find(properties, "PIDFDs") == nullptr);
    REQUIRE(find(properties, "PIDs") != nullptr);
    CHECK(std::get<vtpty::PidList>(find(properties, "PIDs")->value).pid == 4242);
    CHECK(find(properties, "OOMPolicy") != nullptr);
}

TEST_CASE("ScopeRequestEncoding.legacy protocol sends only what systemd before 253 knows",
          "[placement][encoding]")
{
    // OOMPolicy on scopes and PIDFDs both arrived in systemd 253; an older one refuses the whole
    // request over either. Its scopes did not react to an out-of-memory kill anyway.
    auto const properties = vtpty::scopeProperties(requestFor(ScopeProtocol::Legacy, 7));
    CHECK(find(properties, "OOMPolicy") == nullptr);
    CHECK(find(properties, "PIDFDs") == nullptr);
    REQUIRE(find(properties, "PIDs") != nullptr);
    CHECK(std::get<vtpty::PidList>(find(properties, "PIDs")->value).pid == 4242);
}

TEST_CASE("ScopeRequestEncoding.memory limit", "[placement][encoding]")
{
    auto request = requestFor(ScopeProtocol::Current, 7);
    request.memoryLimit = vtpty::MemoryLimit { .maxBytes = 64, .swapMaxBytes = 0 };
    auto const properties = vtpty::scopeProperties(request);
    REQUIRE(find(properties, "MemoryMax") != nullptr);
    CHECK(std::get<std::uint64_t>(find(properties, "MemoryMax")->value) == 64);
    CHECK(std::get<std::uint64_t>(find(properties, "MemorySwapMax")->value) == 0);
}

TEST_CASE("ScopeRequestEncoding.classify", "[placement][encoding]")
{
    struct Row
    {
        int returnCode;
        std::string_view errorName;
        ScopeError expected;
    };
    auto const rows = std::array {
        Row { .returnCode = -ETIMEDOUT, .errorName = "", .expected = ScopeError::TimedOut },
        Row { .returnCode = -ECONNRESET, .errorName = "", .expected = ScopeError::Disconnected },
        Row { .returnCode = -ENOTCONN, .errorName = "", .expected = ScopeError::Disconnected },
        // A bus that cannot pass file descriptors: PIDFDs cannot work, PIDs can.
        Row { .returnCode = -EOPNOTSUPP, .errorName = "", .expected = ScopeError::Unsupported },
        Row { .returnCode = -EIO,
              .errorName = "org.freedesktop.DBus.Error.ServiceUnknown",
              .expected = ScopeError::Unavailable },
        Row { .returnCode = -EIO,
              .errorName = "org.freedesktop.DBus.Error.NameHasNoOwner",
              .expected = ScopeError::Unavailable },
        // How systemd answers a property it does not know.
        Row { .returnCode = -EIO,
              .errorName = "org.freedesktop.DBus.Error.PropertyReadOnly",
              .expected = ScopeError::Unsupported },
        // Not a protocol question -- an invalid unit name, a kernel thread -- so no fallback either.
        Row { .returnCode = -EIO,
              .errorName = "org.freedesktop.DBus.Error.InvalidArgs",
              .expected = ScopeError::Refused },
        Row { .returnCode = -EIO,
              .errorName = "org.freedesktop.systemd1.UnitExists",
              .expected = ScopeError::Refused },
    };
    for (auto const& row: rows)
    {
        INFO("return code " << row.returnCode << ", error " << row.errorName);
        CHECK(vtpty::classifyScopeFailure(row.returnCode, row.errorName) == row.expected);
    }
}
