// SPDX-License-Identifier: Apache-2.0
//
// The system trust of Windows: CryptoAPI's chain engine and SSL policy decide, so a certificate in
// the Disallowed store is refused and a root Automatic Root Update would fetch is trusted. No file
// is read.
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

#include "WindowsChainVerifier.hpp"

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
    co_return makeTlsClientContext(detail::makeWindowsChainVerifier(*blocking), options);
}

} // namespace core::net
