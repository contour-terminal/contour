// SPDX-License-Identifier: Apache-2.0
//
// The openat2 opener (Linux 5.6): the directory before a path's final name is opened in one call
// with RESOLVE_BENEATH, which refuses `..` above the starting directory, an absolute path and a
// link that leaves it (EXDEV), and RESOLVE_NO_MAGICLINKS, which refuses /proc's magic links
// (ELOOP). The final name is still the walk's, acted on without following.
#include <core/fs/linux/Openat2.hpp>

#include <core/fs/FsError.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/posix/PosixSystem.hpp>
#include <core/platform/Types.hpp>

#include <sys/syscall.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <ranges>
#include <string>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace core::fs::detail
{

namespace
{

    /// What every directory is opened with: a path handle, so a directory the process may search but
    /// not read is still resolved through.
    constexpr auto DirectoryHow = OpenHow {
        .flags = static_cast<std::uint64_t>(O_PATH | O_DIRECTORY | O_CLOEXEC),
        .mode = 0,
        .resolve =
            static_cast<std::uint64_t>(Resolve::Beneath) | static_cast<std::uint64_t>(Resolve::NoMagicLinks),
    };

    class KernelOpenat2 final: public IOpenat2System
    {
      public:
        [[nodiscard]] int openat2(int directory, char const* path, OpenHow const& how) const noexcept override
        {
            return static_cast<int>(::syscall(Openat2Syscall, directory, path, &how, sizeof how));
        }
    };

    class Openat2Opener final: public IBeneathOpener
    {
      public:
        explicit Openat2Opener(std::shared_ptr<IOpenat2System const> system): _system { std::move(system) } {}

        [[nodiscard]] FsResult<UniqueHandle> openDirectoryBeneath(platform::NativeHandle root,
                                                                  std::string const& path) const override
        {
            for ([[maybe_unused]] auto const attempt: std::views::iota(std::size_t { 0 }, Openat2Retries + 1))
            {
                auto const opened =
                    retryOnInterrupt([&] { return _system->openat2(root, path.c_str(), DirectoryHow); });
                if (opened >= 0)
                    return UniqueHandle { opened };
                switch (errno)
                {
                    case EAGAIN: continue;
                    // The path, or a link on it, leaves the root.
                    case EXDEV:
                        return std::unexpected { FsError {
                            .code = FsErrorCode::NotPermitted, .systemCode = EXDEV, .context = "openat2" } };
                    // The call was there at the probe and is refused now (a seccomp filter installed
                    // since, a flag the kernel will not take): this call walks instead.
                    case ENOSYS:
                    case EPERM:
                    case EINVAL: return UniqueHandle { platform::InvalidHandle };
                    default: return errnoError("openat2");
                }
            }
            // The kernel kept racing a rename: this call walks instead.
            return UniqueHandle { platform::InvalidHandle };
        }

      private:
        std::shared_ptr<IOpenat2System const> _system;
    };

} // namespace

std::shared_ptr<IOpenat2System const> systemOpenat2()
{
    return std::make_shared<KernelOpenat2 const>();
}

std::unique_ptr<IBeneathOpener const> makeOpenat2Opener(std::shared_ptr<IOpenat2System const> system)
{
    auto const probed = system->openat2(-1, ".", DirectoryHow);
    if (probed >= 0)
        closeNativeHandle(probed);
    else if (errno != EBADF)
        return nullptr;
    return std::make_unique<Openat2Opener const>(std::move(system));
}

} // namespace core::fs::detail
