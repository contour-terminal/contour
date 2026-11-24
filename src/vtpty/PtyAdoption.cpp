// SPDX-License-Identifier: Apache-2.0
#include <vtpty/PtyAdoption.hpp>

#ifdef _WIN32
    #include <vtpty/ConPty.hpp>

    #include <Windows.h>
#else
    #include <vtpty/UnixPty.hpp>

    #include <unistd.h>
#endif

#include <cstdint>
#include <utility>

namespace vtpty
{

namespace
{
#ifdef _WIN32
    using Adopted = ConPty;
#else
    using Adopted = UnixPty;
#endif

#ifdef _WIN32
    /// Whether a handle names a Job Object: its accounting information can be queried.
    /// @param handle A handle of this process, which may name no job at all.
    /// @return Whether @p handle names a Job Object.
    [[nodiscard]] bool namesJob(HANDLE handle) noexcept
    {
        auto accounting = JOBOBJECT_BASIC_ACCOUNTING_INFORMATION {};
        return QueryInformationJobObject(
                   handle, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)
               != FALSE;
    }

    /// Whether a handle names a given process: its process id is the one it crossed as.
    /// @param handle A handle of this process, which may name no process at all.
    /// @param pid The process id the handle crossed as.
    /// @return Whether @p handle names the process @p pid.
    [[nodiscard]] bool namesProcess(HANDLE handle, std::int64_t pid) noexcept
    {
        return pid > 0 && std::cmp_equal(GetProcessId(handle), pid);
    }
#endif
} // namespace

std::expected<void, AdoptError> canAdoptPty(InheritedPty const& pty)
{
    return Adopted::canAdopt(pty);
}

std::expected<std::unique_ptr<Pty>, AdoptError> adoptPty(InheritedPty const& pty)
{
    return Adopted::adopt(pty).transform(
        [](std::unique_ptr<Adopted> adopted) { return std::unique_ptr<Pty> { std::move(adopted) }; });
}

PseudoconsoleAdoptability pseudoconsoleAdoptability()
{
#ifdef _WIN32
    return ConPty::canAdoptAny() ? PseudoconsoleAdoptability::Able : PseudoconsoleAdoptability::Unable;
#else
    return PseudoconsoleAdoptability::NotApplicable;
#endif
}

void closeInherited(InheritedPty const& pty) noexcept
{
    for (auto const handle: pty.handles())
    {
#ifdef _WIN32
        CloseHandle(handle.native());
#else
        // The standard streams are never a PTY's, whatever a corrupt handover image names.
        if (handle.native() > STDERR_FILENO)
            ::close(handle.native());
#endif
    }
}

void releaseUnadopted([[maybe_unused]] AdoptedChild const& child) noexcept
{
#ifdef _WIN32
    // Closed once shown to be the process they crossed as: a value that names something else by now (this
    // process reuses handle numbers) is left alone.
    if (!child.processHandle.isNone() && namesProcess(child.processHandle.native(), child.pid))
        CloseHandle(child.processHandle.native());
    // The job's last handle, once its predecessor's is gone: closing it ends the tree no one serves any more.
    // Closed only once shown to name a job, for the same reason: which job is not shown, so a reused value
    // that names another job would still be closed, which only a corrupt handover image could bring about.
    if (!child.job.isNone() && namesJob(child.job.native()))
        CloseHandle(child.job.native());
#endif
}

} // namespace vtpty
