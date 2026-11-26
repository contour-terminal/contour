// SPDX-License-Identifier: Apache-2.0
//
// The extension host's AppContainer on Windows: the launch carries the isolation Contour asks for, on every
// system; on Windows the profile is ensured once, the helper directory's grant is idempotent and seen where
// it is inherited, and a probe started in the container cannot reach the user's files, the network, or a
// child process, which the same probe uncontained reaches.
#include <core/Clock.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/wasm/remote/process/ExtensionHostSupervisor.hpp>
#include <vtextension/wasm/remote/process/ProcessHostConnector.hpp>
#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>
#include <vtextension/wasm/remote/process/testing/FakeProcessLauncher.hpp>
#include <vtextension/wasm/remote/process/testing/LaunchedHelper.hpp>

#ifdef _WIN32
    #include <algorithm>
    #include <array>
    #include <format>
    #include <fstream>
    #include <ranges>
    #include <thread>

    #include <vtextension/wasm/remote/process/AppContainer_win32.hpp>
    #include <vtextension/wasm/remote/process/LaunchRefusal_win32.hpp>
    #include <vtextension/wasm/remote/process/Process_win32.hpp>

    #define WIN32_LEAN_AND_MEAN
    #include <AclAPI.h>
    #include <WinSock2.h>
    #include <Windows.h>
    #include <sddl.h>
#endif

namespace process = vtextension::wasm::remote::process;
using process::ProcessIsolation;

TEST_CASE("AppContainer.launchCarriesTheIsolation", "[vtextension]")
{
    // What the connector is configured with reaches the launcher, on every launch: Contour's own default, the
    // container, and None, which a composition under wine names.
    struct Expectation
    {
        std::optional<ProcessIsolation> configured; ///< Nothing for the config's default.
        ProcessIsolation launched;
    };
    for (auto const expectation:
         { Expectation { .configured = std::nullopt, .launched = ProcessIsolation::AppContainer },
           Expectation { .configured = ProcessIsolation::None, .launched = ProcessIsolation::None },
           Expectation { .configured = ProcessIsolation::AppContainer,
                         .launched = ProcessIsolation::AppContainer } })
    {
        auto const fake = std::make_shared<process::testing::FakeProcessLauncher>();
        auto clock = core::ManualClock {};
        auto supervisor = process::ExtensionHostSupervisor::start(process::SupervisorConfig {
            .launcher = fake, .clock = &clock, .teardownGrace = std::chrono::milliseconds { 0 } });
        REQUIRE(supervisor.has_value());
        auto config = process::ProcessHostConfig { .helper = "contour-extension-host",
                                                   .environment = {},
                                                   .extraArguments = {},
                                                   .caps = {},
                                                   .standardError = std::nullopt };
        if (expectation.configured)
            config.isolation = *expectation.configured;
        {
            auto connector =
                process::ProcessHostConnector { (*supervisor)->mailbox(),
                                                std::move(config),
                                                [](vtextension::wasm::remote::LineSeverity /*severity*/,
                                                   std::string_view /*line*/) {} };
            REQUIRE(connector.connect().has_value());
            REQUIRE(connector.connect().has_value());
        }
        auto const launches = fake->launches();
        REQUIRE(launches.size() == 2);
        for (auto const& spec: launches)
            CHECK(spec.isolation == expectation.launched);
    }
}

#ifdef _WIN32
namespace
{

/// Allocates a SID as userenv's are freed: with FreeSid().
[[nodiscard]] PSID fakeContainerSid() noexcept
{
    auto authority = SID_IDENTIFIER_AUTHORITY { SECURITY_APP_PACKAGE_AUTHORITY };
    PSID sid = nullptr;
    ::AllocateAndInitializeSid(&authority, 2, SECURITY_APP_PACKAGE_BASE_RID, 42, 0, 0, 0, 0, 0, 0, &sid);
    return sid;
}

/// What a fake API was asked.
struct ApiCalls
{
    std::size_t created = 0;
    std::size_t derived = 0;
};

/// @return An API whose profile is created with @p createResult (derived when it already exists), and whose
///         every directory lets everyone do anything (a null DACL), counting into @p calls.
[[nodiscard]] process::AppContainerApi fakeApi(std::shared_ptr<ApiCalls> const& calls, HRESULT createResult)
{
    return process::AppContainerApi {
        .createProfile =
            [calls,
             createResult](PCWSTR /*name*/, PCWSTR /*displayName*/, PCWSTR /*description*/, PSID* sid) {
                ++calls->created;
                if (SUCCEEDED(createResult))
                    *sid = fakeContainerSid();
                return createResult;
            },
        .deriveSid =
            [calls](PCWSTR /*name*/, PSID* sid) {
                ++calls->derived;
                *sid = fakeContainerSid();
                return S_OK;
            },
        .readDacl =
            [](PCWSTR /*path*/, PACL* dacl, PSECURITY_DESCRIPTOR* descriptor) {
                *dacl = nullptr;
                *descriptor = nullptr;
                return DWORD { ERROR_SUCCESS };
            },
        .writeDacl = [](PCWSTR /*path*/, PACL /*dacl*/) { return DWORD { ERROR_ACCESS_DENIED }; },
    };
}

/// A fresh directory under the temporary directory, removed when destroyed.
struct ScratchDirectory
{
    std::filesystem::path path;

    explicit ScratchDirectory(std::string_view name):
        path { std::filesystem::temp_directory_path()
               / std::format("contour-{}-{}", name, process::currentProcessId()) }
    {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ScratchDirectory(ScratchDirectory const&) = delete;
    ScratchDirectory(ScratchDirectory&&) = delete;
    ScratchDirectory& operator=(ScratchDirectory const&) = delete;
    ScratchDirectory& operator=(ScratchDirectory&&) = delete;
    ~ScratchDirectory()
    {
        auto error = std::error_code {};
        std::filesystem::remove_all(path, error);
    }
};

/// The SID of ALL APPLICATION PACKAGES, freed with LocalFree().
struct PackagesSid
{
    PSID sid = nullptr;

    PackagesSid()
    {
        ::ConvertStringSidToSidW(std::wstring { process::AllApplicationPackagesSid }.c_str(), &sid);
    }
    PackagesSid(PackagesSid const&) = delete;
    PackagesSid(PackagesSid&&) = delete;
    PackagesSid& operator=(PackagesSid const&) = delete;
    PackagesSid& operator=(PackagesSid&&) = delete;
    ~PackagesSid() { ::LocalFree(sid); }
};

/// Gives @p directory a DACL of its own, inheriting nothing: full control for this user, inherited by
/// everything beneath.
/// @return Whether it has one.
[[nodiscard]] bool protectDirectory(std::filesystem::path const& directory)
{
    auto currentUser = std::wstring { L"CURRENT_USER" };
    auto owner = EXPLICIT_ACCESS_W {};
    owner.grfAccessPermissions = GENERIC_ALL;
    owner.grfAccessMode = SET_ACCESS;
    owner.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    owner.Trustee.TrusteeForm = TRUSTEE_IS_NAME;
    owner.Trustee.TrusteeType = TRUSTEE_IS_USER;
    owner.Trustee.ptstrName = currentUser.data();
    PACL dacl = nullptr;
    if (::SetEntriesInAclW(1, &owner, nullptr, &dacl) != ERROR_SUCCESS)
        return false;
    auto path = directory.wstring();
    auto const result =
        ::SetNamedSecurityInfoW(path.data(),
                                SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                nullptr,
                                nullptr,
                                dacl,
                                nullptr);
    ::LocalFree(dacl);
    return result == ERROR_SUCCESS;
}

/// One entry for ALL APPLICATION PACKAGES in a DACL a test builds.
struct PackageEntry
{
    BYTE flags; ///< Its ACE flags.
    DWORD mask; ///< Its access mask.
};

/// A DACL built in memory from entries for ALL APPLICATION PACKAGES: what a directory's would read as.
class BuiltDacl
{
  public:
    explicit BuiltDacl(std::span<PackageEntry const> entries): _storage(1024)
    {
        auto* const acl = reinterpret_cast<PACL>(_storage.data());
        ::InitializeAcl(acl, static_cast<DWORD>(_storage.size()), ACL_REVISION);
        for (auto const& entry: entries)
            ::AddAccessAllowedAceEx(acl, ACL_REVISION, entry.flags, entry.mask, _packages.sid);
    }

    [[nodiscard]] PACL acl() noexcept { return reinterpret_cast<PACL>(_storage.data()); }

  private:
    std::vector<std::byte> _storage;
    PackagesSid _packages;
};

/// @return The flags of each entry of @p directory's DACL that is its own (not inherited) and allows ALL
///         APPLICATION PACKAGES; nothing if it cannot be read.
[[nodiscard]] std::optional<std::vector<BYTE>> packageEntriesOf(std::filesystem::path const& directory)
{
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::GetNamedSecurityInfoW(directory.c_str(),
                                SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION,
                                nullptr,
                                nullptr,
                                &dacl,
                                nullptr,
                                &descriptor)
        != ERROR_SUCCESS)
        return std::nullopt;
    auto const packages = PackagesSid {};
    auto flags = std::vector<BYTE> {};
    for (auto const index:
         std::views::iota(DWORD { 0 }, dacl != nullptr ? DWORD { dacl->AceCount } : DWORD { 0 }))
    {
        void* ace = nullptr;
        if (!::GetAce(dacl, index, &ace))
            continue;
        auto const& header = *static_cast<ACE_HEADER const*>(ace);
        auto* const allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
        if (header.AceType == ACCESS_ALLOWED_ACE_TYPE && (header.AceFlags & INHERITED_ACE) == 0
            && ::EqualSid(&allowed->SidStart, packages.sid))
            flags.push_back(header.AceFlags);
    }
    ::LocalFree(descriptor);
    return flags;
}

/// @return Each entry of @p directory's DACL as `type flags mask SID`, one per line: what a failing case
///         shows.
[[nodiscard]] std::string daclText(std::filesystem::path const& directory)
{
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::GetNamedSecurityInfoW(directory.c_str(),
                                SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION,
                                nullptr,
                                nullptr,
                                &dacl,
                                nullptr,
                                &descriptor)
        != ERROR_SUCCESS)
        return "unreadable";
    if (dacl == nullptr)
    {
        ::LocalFree(descriptor);
        return "no DACL";
    }
    auto text = std::string {};
    for (auto const index: std::views::iota(DWORD { 0 }, DWORD { dacl->AceCount }))
    {
        void* ace = nullptr;
        if (!::GetAce(dacl, index, &ace))
            continue;
        auto const& header = *static_cast<ACE_HEADER const*>(ace);
        auto* const allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
        LPWSTR sid = nullptr;
        auto name = std::string { "?" };
        if (::ConvertSidToStringSidW(&allowed->SidStart, &sid))
        {
            auto const utf8 = std::filesystem::path { sid }.u8string();
            name.assign(utf8.begin(), utf8.end());
            ::LocalFree(sid);
        }
        text += std::format("type {} flags 0x{:02X} mask 0x{:08X} {}\n",
                            header.AceType,
                            header.AceFlags,
                            allowed->Mask,
                            name);
    }
    ::LocalFree(descriptor);
    return text;
}

/// @return The system's API, except that a DACL write counts into @p writes and fails: for a check that
///         none happens.
[[nodiscard]] process::AppContainerApi apiThatMustNotWrite(std::shared_ptr<std::size_t> const& writes)
{
    auto api = process::systemAppContainerApi();
    api.writeDacl = [writes](PCWSTR /*path*/, PACL /*dacl*/) {
        ++*writes;
        return DWORD { ERROR_ACCESS_DENIED };
    };
    return api;
}

/// A TCP listener on 127.0.0.1, on a port the system picks, for as long as it lives, that accepts and closes
/// every connection on a thread of its own: a connection is refused only by whoever refuses it, never for a
/// full backlog.
class LoopbackListener
{
  public:
    LoopbackListener(): _started { startWinsock() }, _socket { ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) }
    {
        auto address = sockaddr_in {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        auto length = int { sizeof(address) };
        if (std::cmp_equal(_socket, INVALID_SOCKET)
            || ::bind(_socket, reinterpret_cast<sockaddr const*>(&address), sizeof(address)) != 0
            || ::listen(_socket, SOMAXCONN) != 0
            || ::getsockname(_socket, reinterpret_cast<sockaddr*>(&address), &length) != 0)
            return;
        _port = ::ntohs(address.sin_port);
        // Ends once the socket is closed, which fails the accept it waits in.
        _accepter = std::jthread { [socket = _socket] {
            while (true)
            {
                auto const accepted = ::accept(socket, nullptr, nullptr);
                if (std::cmp_equal(accepted, INVALID_SOCKET))
                    return;
                ::closesocket(accepted);
            }
        } };
    }
    LoopbackListener(LoopbackListener const&) = delete;
    LoopbackListener(LoopbackListener&&) = delete;
    LoopbackListener& operator=(LoopbackListener const&) = delete;
    LoopbackListener& operator=(LoopbackListener&&) = delete;
    ~LoopbackListener()
    {
        if (std::cmp_not_equal(_socket, INVALID_SOCKET))
            ::closesocket(_socket);
        if (_accepter.joinable())
            _accepter.join();
        if (_started == WinsockState::Started)
            ::WSACleanup();
    }

    /// @return The port it listens on; nothing if it does not.
    [[nodiscard]] std::optional<uint16_t> port() const noexcept { return _port; }

  private:
    /// Whether this listener started Winsock, and so must clean it up.
    enum class WinsockState : uint8_t
    {
        Absent,
        Started,
    };

    /// @return Whether Winsock was started.
    [[nodiscard]] static WinsockState startWinsock() noexcept
    {
        auto data = WSADATA {};
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0 ? WinsockState::Started : WinsockState::Absent;
    }

    WinsockState _started;
    SOCKET _socket;
    std::optional<uint16_t> _port;
    std::jthread _accepter; ///< Accepts until the socket is closed.
};

} // namespace

TEST_CASE("AppContainer.profileIsEnsuredOnce", "[vtextension][appcontainer]")
{
    SECTION("created once, for every launch after")
    {
        auto const calls = std::make_shared<ApiCalls>();
        auto provision = process::AppContainerProvision { fakeApi(calls, S_OK), [](std::string_view) {} };
        auto const first = provision.prepare(std::filesystem::temp_directory_path());
        auto const second = provision.prepare(std::filesystem::temp_directory_path());
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK(*first == *second);
        CHECK(calls->created == 1);
        CHECK(calls->derived == 0);
    }
    SECTION("already there: derived")
    {
        auto const calls = std::make_shared<ApiCalls>();
        auto const sid =
            process::ensureAppContainer(fakeApi(calls, HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)));
        CHECK(sid.has_value());
        CHECK(calls->created == 1);
        CHECK(calls->derived == 1);
    }
    SECTION("refused otherwise: the system lacks what it takes")
    {
        auto const calls = std::make_shared<ApiCalls>();
        auto const sid = process::ensureAppContainer(fakeApi(calls, E_ACCESSDENIED));
        REQUIRE_FALSE(sid.has_value());
        CHECK(sid.error()
              == process::LaunchRefusal { .error = process::LaunchError::Unsupported,
                                          .step = process::LaunchStep::Profile,
                                          .code = static_cast<DWORD>(E_ACCESSDENIED) });
        CHECK(calls->derived == 0);
    }
}

TEST_CASE("AppContainer.packageGrantIsIdempotent", "[vtextension][appcontainer]")
{
    auto const directory = ScratchDirectory { "package-grant" };
    REQUIRE(protectDirectory(directory.path));
    auto const before = packageEntriesOf(directory.path);
    REQUIRE(before.has_value());
    CHECK(before->empty());

    SECTION("granted once, to the directory and its files; then never written again")
    {
        CHECK(
            process::ensurePackagesCanExecute(directory.path, process::systemAppContainerApi()).has_value());
        INFO(daclText(directory.path));
        auto const after = packageEntriesOf(directory.path);
        REQUIRE(after.has_value());
        // One entry, inherited by the files directly in it only: nothing beneath is reached, not even as an
        // inherit-only copy on a subdirectory.
        CHECK(*after == std::vector<BYTE> { OBJECT_INHERIT_ACE | NO_PROPAGATE_INHERIT_ACE });

        // The grant is seen: a second call writes nothing, as its write would fail.
        auto const writes = std::make_shared<std::size_t>(0);
        CHECK(process::ensurePackagesCanExecute(directory.path, apiThatMustNotWrite(writes)).has_value());
        CHECK(*writes == 0);
    }
    SECTION("a directory this user may not change: not granted, and said with its fix")
    {
        auto const writes = std::make_shared<std::size_t>(0);
        auto const api = apiThatMustNotWrite(writes);
        auto const granted = process::ensurePackagesCanExecute(directory.path, api);
        REQUIRE_FALSE(granted.has_value());
        CHECK(granted.error()
              == process::LaunchRefusal { .error = process::LaunchError::NotGranted,
                                          .step = process::LaunchStep::WriteGrant,
                                          .code = ERROR_ACCESS_DENIED });
        CHECK(*writes == 1);

        auto lines = std::vector<std::string> {};
        auto const calls = std::make_shared<ApiCalls>();
        auto provisionApi = fakeApi(calls, S_OK);
        provisionApi.readDacl = api.readDacl;
        // Refused until an administrator grants it, as far as this test is concerned; then the system's own.
        auto const refused = std::make_shared<bool>(true);
        provisionApi.writeDacl = [writes, refused, system = process::systemAppContainerApi()](PCWSTR path,
                                                                                              PACL dacl) {
            ++*writes;
            if (*refused)
                return DWORD { ERROR_ACCESS_DENIED };
            return system.writeDacl(path, dacl);
        };
        auto provision =
            process::AppContainerProvision { std::move(provisionApi),
                                             [&lines](std::string_view line) { lines.emplace_back(line); } };
        auto const prepared = provision.prepare(directory.path);
        REQUIRE_FALSE(prepared.has_value());
        REQUIRE(lines.size() == 1);
        INFO(lines.front());
        CHECK(lines.front().contains(directory.path.filename().string()));
        CHECK(lines.front().contains("installer"));
        CHECK(lines.front().contains("S-1-15-2-1"));

        // The next launch asks again, and is refused alike without saying it a second time.
        auto const again = provision.prepare(directory.path);
        REQUIRE_FALSE(again.has_value());
        CHECK(again.error().error == process::LaunchError::NotGranted);
        CHECK(lines.size() == 1);
        CHECK(*writes == 3);

        // Once it may be granted, the next launch is: nothing is remembered against it.
        *refused = false;
        CHECK(provision.prepare(directory.path).has_value());
        CHECK(*writes == 4);
        CHECK(lines.size() == 1);
        // And granted, it is not asked again.
        CHECK(provision.prepare(directory.path).has_value());
        CHECK(*writes == 4);
    }
}

TEST_CASE("AppContainer.programFilesGrantIsSeen", "[vtextension][appcontainer]")
{
    // What a directory under `Program Files` inherits for ALL APPLICATION PACKAGES -- `(I)(RX)` for itself
    // and
    // `(I)(OI)(CI)(IO)(GR,GE)` for what it holds -- is a grant already: nothing is written, which an
    // installation this user does not own could not take. Built in memory and read through the API, as not
    // every system computes inheritance on creation (wine does not). Either entry alone is not enough.
    struct Row
    {
        std::string_view name;
        std::vector<PackageEntry> entries;
        std::size_t writes; ///< 0: seen as granted; 1: a grant is written.
    };
    constexpr auto Itself = PackageEntry { .flags = INHERITED_ACE, .mask = FILE_GENERIC_READ | FILE_EXECUTE };
    constexpr auto ItsFiles =
        PackageEntry { .flags = INHERITED_ACE | OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE,
                       .mask = GENERIC_READ | GENERIC_EXECUTE };
    auto const rows = std::array {
        Row { .name = "both", .entries = { Itself, ItsFiles }, .writes = 0 },
        Row { .name = "itself only", .entries = { Itself }, .writes = 1 },
        Row { .name = "its files only", .entries = { ItsFiles }, .writes = 1 },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto dacl = std::make_shared<BuiltDacl>(row.entries);
        auto const writes = std::make_shared<std::size_t>(0);
        auto api = apiThatMustNotWrite(writes);
        api.readDacl = [dacl](PCWSTR /*path*/, PACL* read, PSECURITY_DESCRIPTOR* descriptor) {
            *read = dacl->acl();
            *descriptor = nullptr;
            return DWORD { ERROR_SUCCESS };
        };
        CHECK(process::ensurePackagesCanExecute(std::filesystem::temp_directory_path(), api).has_value()
              == (row.writes == 0));
        CHECK(*writes == row.writes);
    }
}

namespace
{

/// @return A launcher of this system's processes that records in @p lines what it would log.
[[nodiscard]] std::shared_ptr<process::IProcessLauncher> recordingLauncher(std::vector<std::string>& lines)
{
    return process::makeWin32ProcessLauncher(process::systemAppContainerApi(),
                                             [&lines](std::string_view line) { lines.emplace_back(line); });
}

/// What the probe reported, by key.
using ProbeReport = std::vector<std::string>;

/// Starts the probe in @p isolation, asked to open @p profileFile and connect to 127.0.0.1:@p port, and reads
/// its report; fails the case, saying how the launch failed or how the probe ended, if there is none.
/// @return The report.
[[nodiscard]] ProbeReport probeReportIn(ProcessIsolation isolation,
                                        std::filesystem::path const& profileFile,
                                        uint16_t port)
{
    auto standardError = process::testing::CapturedStandardError::create();
    REQUIRE(standardError.has_value());
    auto refusals = std::vector<std::string> {};
    auto probe = process::testing::launchHelper(
        recordingLauncher(refusals),
        process::testing::HelperLaunch {
            .executable = VTEXTENSION_PROBE_PATH,
            .leading = { "--contained" },
            .trailing =
                [&](std::span<vtextension::wasm::remote::transport::NativeHandle const> /*handles*/) {
                    auto const utf8 = profileFile.u8string();
                    return std::vector<std::string> { std::format("--profile-file={}",
                                                                  std::string { utf8.begin(), utf8.end() }),
                                                      std::format("--port={}", port) };
                },
            .environment = process::defaultHelperEnvironment(),
            .standardError = standardError->handle(),
            .isolation = isolation });
    INFO("launch: " << (probe ? std::string { "started" }
                              : std::string { process::launchErrorText(probe.error()) }));
    for (auto const& line: refusals)
        UNSCOPED_INFO(line);
    REQUIRE(probe.has_value());
    auto report = process::testing::readReport(probe->control);
    if (!report)
        FAIL("no report: " << process::testing::endOf(
                 *probe->launcher, *probe->process, [&] { return standardError->text(); }));
    for (auto const& line: *report)
        UNSCOPED_INFO(line);
    probe->closeControl();
    process::testing::checkEndedCleanly(
        *probe->launcher, *probe->process, [&] { return standardError->text(); });
    return *std::move(report);
}

/// @return The value of @p key in @p report; empty if it has none.
[[nodiscard]] std::string valueOf(ProbeReport const& report, std::string_view key)
{
    auto const prefix = std::string { key } + '=';
    auto const line =
        std::ranges::find_if(report, [&](auto const& entry) { return entry.starts_with(prefix); });
    return line == report.end() ? std::string {} : line->substr(prefix.size());
}

/// What a loopback connection from an AppContainer without a network capability may fail with. Windows
/// documents only that the Windows Filtering Platform's default block filter drops it, not the error a
/// caller sees: WSAEACCES (10013) where the connect is refused at authorization, WSAECONNREFUSED (10061),
/// which is what UWP apps are widely reported to see (0x8007274D), WSAETIMEDOUT (10060) where the packet is
/// dropped silently, and WSAENETUNREACH (10051) or WSAEHOSTUNREACH (10065) where no route is offered. Each is
/// the container's refusal: the listener accepts every connection, and the uncontained run proves it.
constexpr auto ContainedConnectRefusals = std::array<std::string_view, 5> { "refused:connect:10013",
                                                                            "refused:connect:10061",
                                                                            "refused:connect:10060",
                                                                            "refused:connect:10051",
                                                                            "refused:connect:10065" };

} // namespace

TEST_CASE("Sandbox.windowsHelperRunsContained", "[vtextension][appcontainer]")
{
    // A file of the user's, and a loopback port that listens: both open to the probe uncontained, both shut
    // to it contained, as is starting a process -- refused by the container's child process policy
    // (ERROR_ACCESS_DENIED), not only by the job's process count (ERROR_NOT_ENOUGH_QUOTA, 1816).
    auto const directory = ScratchDirectory { "contained-probe" };
    auto const profileFile = directory.path / "secret.txt";
    std::ofstream { profileFile } << "not the extension host's\n";
    auto const listener = LoopbackListener {};
    REQUIRE(listener.port().has_value());

    {
        INFO("uncontained");
        auto const report = probeReportIn(ProcessIsolation::None, profileFile, *listener.port());
        CHECK(valueOf(report, "container") == "no");
        CHECK(valueOf(report, "profile-file") == "ok");
        CHECK(valueOf(report, "connect") == "ok");
        // Under wine the job's process count is not held: what it says is only recorded.
        if (!process::testing::runsOnWine())
            CHECK(valueOf(report, "spawn") == "refused:CreateProcessW:1816");
    }

    if (process::testing::runsOnWine())
    {
        // What wine made of the contained launch, for the record; nothing it does proves a container.
        auto refusals = std::vector<std::string> {};
        auto const probe = process::testing::launchHelper(
            recordingLauncher(refusals),
            process::testing::HelperLaunch { .executable = VTEXTENSION_PROBE_PATH,
                                             .leading = { "--contained" },
                                             .environment = process::defaultHelperEnvironment(),
                                             .isolation = ProcessIsolation::AppContainer });
        auto const outcome = probe ? std::string { "it started" }
                                   : std::format("it failed: {}{}{}",
                                                 process::launchErrorText(probe.error()),
                                                 refusals.empty() ? "" : " -- ",
                                                 refusals.empty() ? std::string {} : refusals.front());
        SKIP(std::format("wine stubs AppContainers; the contained launch was asked for and {}", outcome));
    }
    INFO("contained");
    auto const report = probeReportIn(ProcessIsolation::AppContainer, profileFile, *listener.port());
    CHECK(valueOf(report, "container") == "yes");
    CHECK(valueOf(report, "profile-file") == "refused:CreateFileW:5");
    auto const connect = valueOf(report, "connect");
    INFO("connect=" << connect);
    CHECK(std::ranges::contains(ContainedConnectRefusals, std::string_view { connect }));
    CHECK(valueOf(report, "spawn") == "refused:CreateProcessW:5");
}
TEST_CASE("LaunchRefusal.lineNamesTheStepAndTheCode", "[vtextension]")
{
    auto const win32 =
        process::launchRefusalLine(process::LaunchRefusal { .error = process::LaunchError::Io,
                                                            .step = process::LaunchStep::CreateProcess,
                                                            .code = ERROR_INVALID_PARAMETER });
    INFO(win32);
    CHECK(win32.contains("Windows refused to create its process (error 87"));
    CHECK(win32.contains(process::launchErrorText(process::LaunchError::Io)));
    CHECK_FALSE(win32.contains('\n'));

    auto const hresult =
        process::launchRefusalLine(process::LaunchRefusal { .error = process::LaunchError::Unsupported,
                                                            .step = process::LaunchStep::Profile,
                                                            .code = static_cast<DWORD>(E_ACCESSDENIED) });
    INFO(hresult);
    CHECK(hresult.contains("app container profile (error 0x80070005"));
}

TEST_CASE("LaunchRefusal.refusalALaunchErrorLosesIsSaidOnce", "[vtextension]")
{
    // An argument that is not UTF-8 cannot be spelled for CreateProcessW(): `Io`, whose text cannot say so.
    auto lines = std::vector<std::string> {};
    auto const launcher = recordingLauncher(lines);
    auto const spec = process::LaunchSpec {
        .executable = VTEXTENSION_PROBE_PATH,
        .arguments = [](auto /*handles*/) { return std::vector<std::string> { std::string { "\xFF" } }; },
    };
    for (auto const attempt: { 1, 2 })
    {
        INFO("attempt " << attempt);
        auto const launched = launcher->launch(spec);
        REQUIRE_FALSE(launched.has_value());
        CHECK(launched.error() == process::LaunchError::Io);
        REQUIRE(lines.size() == 1);
        INFO(lines.front());
        CHECK(lines.front().contains("spell its command line"));
        CHECK(lines.front().contains("(error 1113"));
    }

    // One its text says all of is not logged.
    auto const missing = launcher->launch(process::LaunchSpec {
        .executable =
            std::filesystem::path { VTEXTENSION_PROBE_PATH }.replace_filename("no-such-program.exe") });
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == process::LaunchError::NotFound);
    CHECK(lines.size() == 1);
}
#endif
