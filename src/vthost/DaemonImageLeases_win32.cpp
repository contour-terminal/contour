// SPDX-License-Identifier: Apache-2.0

// clang-format off
#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
#endif
// clang-format on

#include <vthost/DaemonImage.hpp>

#ifdef _WIN32

    #include <core/Utf8Path.hpp>

    #include <chrono>
    #include <format>
    #include <ranges>
    #include <system_error>
    #include <thread>

namespace vthost
{

namespace
{
    /// How long hold() waits out a prune's claim, polling, since a share mode cannot be waited on as
    /// flock() is. A claim lasts as long as removing one image does; the bound only ends a wait on a
    /// claimer that hangs, at the same 30 s the declaration documents (@see ImageLeases::hold).
    constexpr auto HoldRetryDelay = std::chrono::milliseconds { 20 };
    constexpr auto HoldWaitLimit = std::chrono::seconds { 30 };
    constexpr auto HoldAttempts = static_cast<int>(HoldWaitLimit / HoldRetryDelay);

    /// A lease file held open; the handle's share mode is the lease.
    class HandleLease final: public ImageLease
    {
      public:
        explicit HandleLease(HANDLE handle) noexcept: _handle { handle } {}
        HandleLease(HandleLease const&) = delete;
        HandleLease& operator=(HandleLease const&) = delete;
        HandleLease(HandleLease&&) = delete;
        HandleLease& operator=(HandleLease&&) = delete;
        ~HandleLease() override { ::CloseHandle(_handle); }

      private:
        HANDLE _handle;
    };

    class NativeImageLeases final: public ImageLeases
    {
      public:
        [[nodiscard]] std::expected<std::unique_ptr<ImageLease>, StagingError> hold(
            std::filesystem::path const& leaseFile) override
        {
            for ([[maybe_unused]] auto const attempt: std::views::iota(0, HoldAttempts))
            {
                // Read and write shared, so leases coexist; delete not, so a probe or claim asking
                // for delete access fails while any lease is held. Not inheritable (no
                // SECURITY_ATTRIBUTES), so a child process never carries it.
                auto const handle = ::CreateFileW(leaseFile.c_str(),
                                                  GENERIC_READ,
                                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                  nullptr,
                                                  OPEN_ALWAYS,
                                                  FILE_ATTRIBUTE_NORMAL,
                                                  nullptr);
                if (handle != INVALID_HANDLE_VALUE)
                    return std::make_unique<HandleLease>(handle);
                auto const error = ::GetLastError();
                if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED)
                    return failure(leaseFile, error);
                // A claim holds the file (sharing violation), or deleted it and the delete is still
                // pending (access denied): try again once it is gone.
                std::this_thread::sleep_for(HoldRetryDelay);
            }
            return failure(leaseFile, ERROR_SHARING_VIOLATION);
        }

        [[nodiscard]] std::unique_ptr<ImageLease> claim(std::filesystem::path const& leaseFile) override
        {
            // Shared with nobody, and deleted on close: the lease file goes when the claim does.
            auto const handle = ::CreateFileW(leaseFile.c_str(),
                                              GENERIC_READ | DELETE,
                                              0,
                                              nullptr,
                                              OPEN_ALWAYS,
                                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE,
                                              nullptr);
            if (handle == INVALID_HANDLE_VALUE)
                return nullptr;
            return std::make_unique<HandleLease>(handle);
        }

        [[nodiscard]] LeaseState probe(std::filesystem::path const& leaseFile) override
        {
            auto const handle = ::CreateFileW(leaseFile.c_str(),
                                              DELETE,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                              nullptr,
                                              OPEN_EXISTING,
                                              FILE_ATTRIBUTE_NORMAL,
                                              nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                auto const error = ::GetLastError();
                return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? LeaseState::Free
                                                                                      : LeaseState::Held;
            }
            ::CloseHandle(handle);
            return LeaseState::Free;
        }

        /// Nothing to adopt: a lease is never handed across an exec on Windows, which has none
        /// (@see relocationFor).
        [[nodiscard]] std::unique_ptr<ImageLease> adoptInherited(std::filesystem::path const&) override
        {
            return nullptr;
        }

      private:
        [[nodiscard]] static std::unexpected<StagingError> failure(std::filesystem::path const& leaseFile,
                                                                   DWORD error)
        {
            return std::unexpected(StagingError {
                .code = StagingErrorCode::LeaseFailed,
                .context = std::format("{}: {}",
                                       core::pathToUtf8(leaseFile),
                                       std::system_category().message(static_cast<int>(error))) });
        }
    };
} // namespace

std::unique_ptr<ImageLeases> makeNativeImageLeases()
{
    return std::make_unique<NativeImageLeases>();
}

} // namespace vthost

#endif
