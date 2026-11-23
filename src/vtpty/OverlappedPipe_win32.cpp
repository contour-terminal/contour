// SPDX-License-Identifier: Apache-2.0
#include <vtpty/OverlappedPipe.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <string>
#include <utility>

namespace vtpty
{

namespace
{
    /// How a pipe of one direction is opened at either end.
    struct PipeDirectionRow
    {
        PipeDirection direction;
        DWORD ourAccess;   ///< This process's end, as CreateNamedPipeW() opens it.
        DWORD theirAccess; ///< The other end, as CreateFileW() opens it.
    };

    /// One row per direction, in enumerator order.
    constexpr auto PipeDirections = std::array {
        PipeDirectionRow { .direction = PipeDirection::Inbound,
                           .ourAccess = PIPE_ACCESS_INBOUND,
                           .theirAccess = GENERIC_WRITE | FILE_READ_ATTRIBUTES },
        // Duplex, though nothing comes back: an outbound end may not read its own attributes, and a writer
        // asks how much room the pipe has (ConPty's writableWithoutWaiting()).
        PipeDirectionRow { .direction = PipeDirection::Outbound,
                           .ourAccess = PIPE_ACCESS_DUPLEX,
                           .theirAccess = GENERIC_READ | FILE_WRITE_ATTRIBUTES },
    };

    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, PipeDirections.size()),
                                      [](std::size_t index) {
                                          return std::to_underlying(PipeDirections.at(index).direction)
                                                 == index;
                                      }),
                  "PipeDirections must list every direction in enumerator order");

    /// @return When this process started, in 100 ns ticks: what tells it apart from a later process given the
    ///         same id. A pipe outlives the process that named it once a live upgrade hands it on, and the
    ///         name must not then be asked for again.
    [[nodiscard]] std::uint64_t processStartTime() noexcept
    {
        static auto const started = [] {
            auto creation = FILETIME {};
            auto exit = FILETIME {};
            auto kernel = FILETIME {};
            auto user = FILETIME {};
            if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) == FALSE)
                return std::uint64_t { 0 };
            return (std::uint64_t { creation.dwHighDateTime } << 32) | creation.dwLowDateTime;
        }();
        return started;
    }
} // namespace

std::expected<OverlappedPipe, std::error_code> createOverlappedPipe(PipeDirection direction,
                                                                    std::wstring_view purpose,
                                                                    std::uint32_t bufferSize)
{
    static auto pipesCreated = std::atomic<unsigned> { 0 };
    auto const& row = PipeDirections.at(std::to_underlying(direction));
    auto const name = std::format(L"\\\\.\\pipe\\contour-{}-{}-{}-{}",
                                  purpose,
                                  GetCurrentProcessId(),
                                  processStartTime(),
                                  pipesCreated++);

    // One instance, ours, and only for a client on this machine. No security attributes: not inheritable.
    auto ours = UniqueHandle { CreateNamedPipeW(
        name.c_str(),
        row.ourAccess | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,
        bufferSize,
        bufferSize,
        0,
        nullptr) };
    if (!ours.valid())
        return std::unexpected(lastError());

    // Connected as it is opened: a client that connects before ConnectNamedPipe() is called leaves the
    // pipe connected, and the call is then not needed.
    auto theirs = UniqueHandle { CreateFileW(
        name.c_str(), row.theirAccess, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
    if (!theirs.valid())
        return std::unexpected(lastError());
    return OverlappedPipe { .ours = std::move(ours), .theirs = std::move(theirs) };
}

} // namespace vtpty
