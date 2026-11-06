// SPDX-License-Identifier: Apache-2.0

// clang-format off
#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
#endif
// clang-format on

#include <vthost/DaemonImage.hpp>

#ifdef _WIN32

    #include <vtpty/Win32Handle.hpp>

    #include <core/Utf8Path.hpp>

    #include <chrono>
    #include <format>
    #include <ranges>
    #include <system_error>
    #include <thread>
    #include <utility>

    #include <vthost/Logging.hpp>

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

    /// @return Whether @p handle is open on the file at @p path: the same volume and file index. The path is
    ///         opened for its attributes alone and shared with everything, so the probe never conflicts with
    ///         a lease's share mode (it asks for no delete access); a claim, sharing nothing, makes it false.
    [[nodiscard]] bool isFileAt(HANDLE handle, std::filesystem::path const& path) noexcept
    {
        auto* const named = ::CreateFileW(path.c_str(),
                                          FILE_READ_ATTRIBUTES,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr,
                                          OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL,
                                          nullptr);
        if (named == INVALID_HANDLE_VALUE)
            return false;
        auto opened = BY_HANDLE_FILE_INFORMATION {};
        auto atPath = BY_HANDLE_FILE_INFORMATION {};
        auto const same = ::GetFileInformationByHandle(handle, &opened) != FALSE
                          && ::GetFileInformationByHandle(named, &atPath) != FALSE
                          && opened.dwVolumeSerialNumber == atPath.dwVolumeSerialNumber
                          && opened.nFileIndexHigh == atPath.nFileIndexHigh
                          && opened.nFileIndexLow == atPath.nFileIndexLow;
        ::CloseHandle(named);
        return same;
    }

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

        [[nodiscard]] vtpty::CrossingHandle nativeHandle() const noexcept override
        {
            return vtpty::CrossingHandle::of(_handle);
        }

      private:
        HANDLE _handle;
    };

    class NativeImageLeases final: public ImageLeases
    {
      public:
        /// @param inheritedLease The lease the predecessor handed over, which adoptInherited() takes over;
        ///        none for none. It stops being inherited right away, so that no program this process starts
        ///        carries it, whether or not anything adopts it.
        explicit NativeImageLeases(vtpty::CrossingHandle inheritedLease): _inheritedLease { inheritedLease }
        {
            if (_inheritedLease.isNone())
                return;
            if (auto const set =
                    vtpty::setInheritance(_inheritedLease.native(), vtpty::HandleInheritance::NotInherited);
                !set)
                errorLog()("the handed-over image lease stays inheritable: {}", set.error().message());
        }
        NativeImageLeases(NativeImageLeases const&) = delete;
        NativeImageLeases& operator=(NativeImageLeases const&) = delete;
        NativeImageLeases(NativeImageLeases&&) = delete;
        NativeImageLeases& operator=(NativeImageLeases&&) = delete;
        /// Lets go of a handed-over lease nothing adopted.
        ~NativeImageLeases() override
        {
            if (!_inheritedLease.isNone())
                ::CloseHandle(_inheritedLease.native());
        }

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

        /// The lease the predecessor handed over, once: a successor holds it as its own, so that the image it
        /// runs stays leased throughout. Crossed for this process alone: no program it starts inherits it.
        /// A handle that is no lease on @p leaseFile is kept, not adopted, as POSIX keeps such a descriptor.
        [[nodiscard]] std::unique_ptr<ImageLease> adoptInherited(
            std::filesystem::path const& leaseFile) override
        {
            if (_inheritedLease.isNone() || !isFileAt(_inheritedLease.native(), leaseFile))
                return nullptr;
            return std::make_unique<HandleLease>(
                std::exchange(_inheritedLease, vtpty::CrossingHandle {}).native());
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

        vtpty::CrossingHandle _inheritedLease; ///< What adoptInherited() takes over yet; none once it has.
    };
} // namespace

std::unique_ptr<ImageLeases> makeNativeImageLeases(vtpty::CrossingHandle inheritedLease)
{
    return std::make_unique<NativeImageLeases>(inheritedLease);
}

} // namespace vthost

#endif
