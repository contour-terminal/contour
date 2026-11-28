// SPDX-License-Identifier: Apache-2.0
//
// The system trust of macOS: Security.framework decides, so a root the keychain distrusts is
// refused and one it would fetch is trusted. No file is read.
#include <core/Assert.hpp>
#include <core/Environment.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/Task.hpp>
#include <core/net/Tls.hpp>
#include <core/net/TrustStore.hpp>
#include <core/platform/FileSystem.hpp>

#include <expected>
#include <memory>
#include <string>

#include "DarwinChainVerifier.hpp"

namespace core::net
{

async::Task<std::expected<std::shared_ptr<ITlsContext>, std::string>> makeSystemTlsClientContext(
    platform::FileSystem const* /*files*/,
    Environment const* /*environment*/,
    async::IExecutor* home,
    async::IExecutor* blocking,
    TlsClientOptions options)
{
    Require(home != nullptr && blocking != nullptr);
    co_return makeTlsClientContext(detail::makeDarwinChainVerifier(*blocking), options);
}

} // namespace core::net
