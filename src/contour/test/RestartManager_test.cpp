// SPDX-License-Identifier: Apache-2.0
//
// What Windows Installer relies on to replace a running Contour: Restart Manager finds it, closes it without
// asking, and starts it again. Driven with the real API against a real contour.exe on the interactive
// desktop, so it opens a window briefly and is skipped where there is no visible window station.

#include <core/Utf8Path.hpp>
#include <core/WideString.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

// RestartManager.h and TlHelp32.h use the types Windows.h declares and include nothing themselves, so it
// has to come first, against the alphabetical order.
// clang-format off
#include <Windows.h>
#include <RestartManager.h>
#include <TlHelp32.h>
// clang-format on

namespace
{

using namespace std::chrono_literals;

/// How a poll for something that is yet to happen ended.
enum class Awaited : uint8_t
{
    TimedOut = 0,
    Happened,
};

/// Polls @p condition until it holds or @p timeout passes.
template <typename Condition>
[[nodiscard]] Awaited awaitCondition(std::chrono::milliseconds timeout, Condition const& condition)
{
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (condition())
            return Awaited::Happened;
        std::this_thread::sleep_for(100ms);
    }
    return condition() ? Awaited::Happened : Awaited::TimedOut;
}

/// A directory of its own, removed with the object.
class TemporaryDirectory
{
  public:
    TemporaryDirectory():
        _path { std::filesystem::temp_directory_path()
                / ("contour-restart-manager-" + std::to_string(GetCurrentProcessId())) }
    {
        std::filesystem::create_directories(_path);
    }

    ~TemporaryDirectory()
    {
        auto ignored = std::error_code {};
        std::filesystem::remove_all(_path, ignored);
    }

    TemporaryDirectory(TemporaryDirectory const&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory const&) = delete;
    TemporaryDirectory(TemporaryDirectory&&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;

    [[nodiscard]] std::filesystem::path const& path() const noexcept { return _path; }

  private:
    std::filesystem::path _path;
};

/// A Windows handle, closed with the object.
class OwnedHandle
{
  public:
    OwnedHandle() = default;
    explicit OwnedHandle(HANDLE handle) noexcept: _handle { handle } {}

    ~OwnedHandle()
    {
        if (_handle != nullptr)
            CloseHandle(_handle);
    }

    OwnedHandle(OwnedHandle const&) = delete;
    OwnedHandle& operator=(OwnedHandle const&) = delete;
    OwnedHandle(OwnedHandle&&) = delete;
    OwnedHandle& operator=(OwnedHandle&&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return _handle; }

  private:
    HANDLE _handle = nullptr;
};

/// Whether the process' window station shows windows on a screen: a service's or a headless runner's does
/// not.
[[nodiscard]] bool windowStationIsVisible()
{
    auto flags = USEROBJECTFLAGS {};
    auto length = DWORD {};
    if (!GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS, &flags, sizeof(flags), &length))
        return false;
    return (flags.dwFlags & WSF_VISIBLE) != 0;
}

/// @return The ids of the running processes whose image is @p image.
[[nodiscard]] std::vector<DWORD> processesOfImage(std::filesystem::path const& image)
{
    auto ids = std::vector<DWORD> {};
    auto const snapshot = OwnedHandle { CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) };
    if (snapshot.get() == INVALID_HANDLE_VALUE)
        return ids;
    auto entry = PROCESSENTRY32W {};
    entry.dwSize = sizeof(entry);
    for (auto more = Process32FirstW(snapshot.get(), &entry); more;
         more = Process32NextW(snapshot.get(), &entry))
    {
        auto const process =
            OwnedHandle { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID) };
        if (process.get() == nullptr)
            continue;
        auto path = std::wstring(MAX_PATH * 4, L'\0');
        auto size = static_cast<DWORD>(path.size());
        if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &size))
            continue;
        path.resize(size);
        auto equivalent = std::error_code {};
        if (std::filesystem::equivalent(std::filesystem::path { path }, image, equivalent))
            ids.push_back(entry.th32ProcessID);
    }
    return ids;
}

/// Whether @p processId has a visible top-level window.
[[nodiscard]] bool hasVisibleWindow(DWORD processId)
{
    struct Search
    {
        DWORD processId = 0;
        Awaited found = Awaited::TimedOut; // set by the EnumWindows callback
    };
    auto search = Search { .processId = processId };
    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL {
            auto& found = *reinterpret_cast<Search*>(parameter);
            auto owner = DWORD {};
            GetWindowThreadProcessId(window, &owner);
            if (owner == found.processId && IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr)
                found.found = Awaited::Happened;
            return found.found == Awaited::Happened ? FALSE : TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found == Awaited::Happened;
}

/// Ends the process @p processId, whatever it is doing, and waits until it is gone.
void terminateProcessAndWait(DWORD processId)
{
    if (auto const process = OwnedHandle { OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, processId) };
        process.get())
    {
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), 10'000);
    }
}

/// A Restart Manager session, ended with the object.
class RestartManagerSession
{
  public:
    RestartManagerSession()
    {
        auto key = std::wstring(CCH_RM_SESSION_KEY + 1, L'\0');
        _status = RmStartSession(&_handle, 0, key.data());
    }

    ~RestartManagerSession()
    {
        if (_status == ERROR_SUCCESS)
            RmEndSession(_handle);
    }

    RestartManagerSession(RestartManagerSession const&) = delete;
    RestartManagerSession& operator=(RestartManagerSession const&) = delete;
    RestartManagerSession(RestartManagerSession&&) = delete;
    RestartManagerSession& operator=(RestartManagerSession&&) = delete;

    [[nodiscard]] DWORD status() const noexcept { return _status; }
    [[nodiscard]] DWORD handle() const noexcept { return _handle; }

  private:
    DWORD _handle = 0;
    DWORD _status = ERROR_INVALID_HANDLE;
};

} // namespace

TEST_CASE("Restart Manager closes Contour without asking and reopens it", "[win32][restartmanager]")
{
    if (!windowStationIsVisible())
        SKIP("no visible window station: Contour's window cannot open here");

    auto const binary = core::pathFromTrustedUtf8(CONTOUR_BINARY);
    auto const state = TemporaryDirectory {};
    auto const configFile = state.path() / "contour.yml";
    std::ofstream { configFile } << "\n";

    // The developer's own configuration and state stay out of it.
    SetEnvironmentVariableW(L"LOCALAPPDATA", state.path().c_str());
    SetEnvironmentVariableW(L"QT_QPA_PLATFORM", L"windows");
    SetEnvironmentVariableW(L"QT_QUICK_BACKEND", L"software");

    auto commandLine =
        L"\"" + binary.wstring() + L"\" --config \"" + configFile.wstring() + L"\" --daemon-mode in_process";
    // Contour processes that were here before this run (a developer's own session): never counted, never
    // ended.
    auto const before = processesOfImage(binary);
    auto const isNew = [&before](DWORD id) {
        return !std::ranges::contains(before, id);
    };

    auto startup = STARTUPINFOW {};
    startup.cb = sizeof(startup);
    auto information = PROCESS_INFORMATION {};
    REQUIRE(CreateProcessW(
        nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &information));
    auto const process = OwnedHandle { information.hProcess };
    auto const thread = OwnedHandle { information.hThread };
    auto const firstId = information.dwProcessId;

    // Whatever happens below, nothing of this run is left behind.
    struct Cleanup
    {
        std::filesystem::path binary;
        std::vector<DWORD> before;
        ~Cleanup()
        {
            // Each is waited for, so the state directory can be removed once it is gone.
            for (auto const id: processesOfImage(binary))
                if (!std::ranges::contains(before, id))
                    terminateProcessAndWait(id);
        }
    } const cleanup { binary, before };

    REQUIRE(awaitCondition(30s, [&] { return hasVisibleWindow(firstId); }) == Awaited::Happened);

    auto const session = RestartManagerSession {};
    REQUIRE(session.status() == ERROR_SUCCESS);

    auto unique = RM_UNIQUE_PROCESS {};
    unique.dwProcessId = firstId;
    auto created = FILETIME {};
    auto ignoredTime = FILETIME {};
    REQUIRE(GetProcessTimes(process.get(), &created, &ignoredTime, &ignoredTime, &ignoredTime));
    unique.ProcessStartTime = created;
    REQUIRE(RmRegisterResources(session.handle(), 0, nullptr, 1, &unique, 0, nullptr) == ERROR_SUCCESS);

    auto needed = UINT {};
    auto available = UINT { 0 };
    auto reasons = DWORD {};
    REQUIRE(RmGetList(session.handle(), &needed, &available, nullptr, &reasons) == ERROR_MORE_DATA);
    auto affected = std::vector<RM_PROCESS_INFO>(needed);
    available = needed;
    REQUIRE(RmGetList(session.handle(), &needed, &available, affected.data(), &reasons) == ERROR_SUCCESS);
    REQUIRE(available == 1);
    // The detached policy lets the GUI open without a new console on supported Windows versions.
    // Restart Manager still classifies the executable by its console subsystem.
    CHECK(affected.front().ApplicationType == RmConsole);
    CHECK(affected.front().bRestartable == TRUE);
    CHECK(core::toUtf8(affected.front().strAppName) == "Contour Terminal Emulator");

    // Closed without a question: the process ends on its own, not by being terminated.
    REQUIRE(RmShutdown(session.handle(), 0, nullptr) == ERROR_SUCCESS);
    CHECK(WaitForSingleObject(process.get(), 10'000) == WAIT_OBJECT_0);

    REQUIRE(RmRestart(session.handle(), 0, nullptr) == ERROR_SUCCESS);
    auto const restarted = [&] {
        return std::ranges::any_of(processesOfImage(binary),
                                   [&](DWORD id) { return id != firstId && isNew(id); });
    };
    CHECK(awaitCondition(15s, restarted) == Awaited::Happened);
}
