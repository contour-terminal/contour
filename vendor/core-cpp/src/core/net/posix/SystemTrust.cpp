// SPDX-License-Identifier: Apache-2.0
//
// The system trust of a POSIX system that is not Apple's: the anchors its bundle or hashed
// directory holds, found as OpenSSL finds them (`makeFileTrustStore`).
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
#include <utility>

namespace core::net
{

async::Task<std::expected<std::shared_ptr<ITlsContext>, std::string>> makeSystemTlsClientContext(
    platform::FileSystem const* files,
    Environment const* environment,
    async::IExecutor* home,
    async::IExecutor* blocking,
    TlsClientOptions options)
{
    Require(files != nullptr && environment != nullptr && home != nullptr && blocking != nullptr);
    auto const store = makeFileTrustStore(*files, *environment, *home, *blocking);
    auto anchors = co_await store->load();
    if (!anchors)
        co_return std::unexpected(std::move(anchors.error()));
    co_return makeTlsClientContext(std::move(*anchors), options);
}

} // namespace core::net
