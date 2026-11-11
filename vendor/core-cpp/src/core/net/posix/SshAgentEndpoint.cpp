// SPDX-License-Identifier: Apache-2.0
#include <core/net/detail/SshAgentEndpoint.hpp>

#include <core/net/Sockets.hpp>
#include <core/net/SshAgentEndpoint.hpp>

namespace core::net
{

std::string defaultSshAgentEndpoint(core::Environment const& environment)
{
    return environment.get("SSH_AUTH_SOCK").value_or(std::string {});
}

namespace detail
{

    async::Task<std::expected<std::unique_ptr<ISocket>, NetError>> connectAgentEndpoint(EventLoop* loop,
                                                                                        std::string endpoint)
    {
        if (endpoint.empty())
            co_return std::unexpected(makeNetError(NetErrorCode::AddressError, 0, "no ssh-agent endpoint"));
        co_return co_await connectUnix(loop, endpoint);
    }

} // namespace detail

} // namespace core::net
