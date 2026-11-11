// SPDX-License-Identifier: Apache-2.0
#include <core/net/detail/SshAgentEndpoint.hpp>

#include <core/WideString.hpp>
#include <core/net/SshAgentEndpoint.hpp>
#include <core/net/windows/NamedPipe.hpp>

namespace core::net
{

std::string defaultSshAgentEndpoint([[maybe_unused]] core::Environment const& environment)
{
    // The Win32-OpenSSH agent's pipe; it has no environment variable.
    return R"(\\.\pipe\openssh-ssh-agent)";
}

namespace detail
{

    async::Task<std::expected<std::unique_ptr<ISocket>, NetError>> connectAgentEndpoint(EventLoop* loop,
                                                                                        std::string endpoint)
    {
        if (endpoint.empty())
            co_return std::unexpected(makeNetError(NetErrorCode::AddressError, 0, "no ssh-agent endpoint"));
        auto const wide = core::toUtf16(endpoint);
        if (!wide)
            co_return std::unexpected(makeNetError(NetErrorCode::AddressError, 0, "ssh-agent pipe name"));
        co_return connectNamedPipe(*loop, *wide);
    }

} // namespace detail

} // namespace core::net
