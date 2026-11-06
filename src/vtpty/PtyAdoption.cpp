// SPDX-License-Identifier: Apache-2.0
#include <vtpty/PtyAdoption.hpp>

#ifdef _WIN32
    #include <vtpty/ConPty.hpp>

    #include <Windows.h>
#else
    #include <vtpty/UnixPty.hpp>

    #include <unistd.h>
#endif

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
    if (!child.processHandle.isNone())
        CloseHandle(child.processHandle.native());
#endif
}

} // namespace vtpty
