// SPDX-License-Identifier: Apache-2.0
//
// The extension host's sandbox on Linux. Before any thread: no_new_privs, then Landlock, which every thread
// started later inherits (and, where the ABI has TSYNC, every thread already running too). Once every thread
// was started and prepared: the system call filter, installed on every thread at once.
#include <sys/prctl.h>

#include <array>
#include <cerrno>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <vtextension/wasm/BuildInstrumentation.hpp>
#include <vtextension/wasm/host/HostProcess.hpp>
#include <vtextension/wasm/host/Sandbox.hpp>
#include <vtextension/wasm/host/linux/Landlock.hpp>
#include <vtextension/wasm/host/linux/SeccompFilter.hpp>
#include <vtextension/wasm/host/linux/SystemCallRows.hpp>

namespace vtextension::wasm::host
{

namespace
{
    /// A directory an instrumented build's host may still reach once it gave up the file system.
    struct InstrumentationDirectory
    {
        BuildInstrumentation build; ///< The build that needs it.
        char const* path;           ///< The directory.
        uint64_t rights;            ///< What it may do beneath it.
    };

    /// Every directory an instrumented build keeps: a sanitizer reads its process's maps for a report (it
    /// names modules and offsets, unsymbolized), and a coverage build writes its counters into the build
    /// tree. `/proc/self` resolves to `/proc/<pid>` as it is opened, so the rule covers the host's own
    /// entries and no other process's.
    constexpr auto InstrumentationDirectories = std::array {
        InstrumentationDirectory { .build = BuildInstrumentation::Sanitizers,
                                   .path = "/proc/self",
                                   .rights = LandlockReadFile | LandlockReadDirectory },
#ifdef CONTOUR_COVERAGE_OBJECT_ROOT
        InstrumentationDirectory { .build = BuildInstrumentation::Coverage,
                                   .path = CONTOUR_COVERAGE_OBJECT_ROOT,
                                   .rights = LandlockReadFile | LandlockWriteFile | LandlockReadDirectory
                                             | LandlockMakeRegular | LandlockTruncate },
#endif
    };

    /// The directories of InstrumentationDirectories this build keeps, opened: closed with it.
    class KeptDirectories
    {
      public:
        KeptDirectories() = default;
        KeptDirectories(KeptDirectories const&) = delete;
        KeptDirectories(KeptDirectories&&) = delete;
        KeptDirectories& operator=(KeptDirectories const&) = delete;
        KeptDirectories& operator=(KeptDirectories&&) = delete;
        ~KeptDirectories()
        {
            for (auto const& rule: _rules)
                ::close(rule.parent);
        }

        /// Opens each directory this build keeps.
        /// @return Nothing once all are open; the errno of the first that is not.
        [[nodiscard]] std::expected<void, int> open()
        {
            for (auto const& directory: InstrumentationDirectories)
            {
                if (directory.build != ThisBuildInstrumentation)
                    continue;
                auto const descriptor = ::open(directory.path, O_PATH | O_DIRECTORY | O_CLOEXEC);
                if (descriptor < 0)
                    return std::unexpected(errno);
                _rules.push_back(PathBeneathRule { .parent = descriptor, .allowedAccess = directory.rights });
            }
            return {};
        }

        /// @return What Landlock is to grant beneath them.
        [[nodiscard]] std::span<PathBeneathRule const> rules() const noexcept { return _rules; }

      private:
        std::vector<PathBeneathRule> _rules;
    };

    /// What the host is told when Landlock is unavailable: it runs confined by its system call filter alone.
    constexpr auto LandlockUnavailable = std::string_view {
        "the file system restriction is unavailable on this system; system calls are restricted"
    };

    class LinuxSandbox final: public ISandbox
    {
      public:
        LinuxSandbox(SandboxDescriptors descriptors,
                     LandlockSystem landlock,
                     std::function<void(std::string_view)> tell):
            _descriptors { descriptors }, _landlock { landlock }, _tell { std::move(tell) }
        {
        }

        [[nodiscard]] std::expected<void, ConfinementFailure> restrictBeforeThreads() override
        {
            // Every step runs; the first that failed is the one stated.
            auto first = std::optional<ConfinementFailure> {};
            if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
                first = ConfinementFailure { .step = remote::proto::LockdownStep::NoNewPrivileges,
                                             .code = errno };

            auto kept = KeptDirectories {};
            auto const restricted =
                kept.open()
                    .transform_error([](int error) {
                        return ConfinementFailure { .step = remote::proto::LockdownStep::FileSystem,
                                                    .code = error };
                    })
                    .and_then([&] { return restrictFileSystem(kept.rules(), _landlock); });
            if (restricted == LandlockOutcome::Unavailable)
                _tell(LandlockUnavailable);
            if (!restricted && !first)
                first = restricted.error();
            if (first)
                return std::unexpected(*first);
            return {};
        }

        [[nodiscard]] std::expected<void, ConfinementFailure> lockDown() override
        {
            auto const rows = buildRows(ThisAuditArch);
            return assembleFilter(rows, _descriptors, ::getpid(), ThisAuditArch)
                .and_then([](std::vector<sock_filter> const& program) { return installFilter(program); });
        }

        [[nodiscard]] remote::proto::LockdownState lockdownState() const noexcept override
        {
            return remote::proto::LockdownState::LockedDown;
        }

      private:
        SandboxDescriptors _descriptors;
        LandlockSystem _landlock;
        std::function<void(std::string_view)> _tell;
    };
} // namespace

std::unique_ptr<ISandbox> makeSystemSandbox(SandboxDescriptors descriptors)
{
    return std::make_unique<LinuxSandbox>(descriptors, systemLandlock(), &tellOnStandardError);
}

} // namespace vtextension::wasm::host
