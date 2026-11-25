// SPDX-License-Identifier: Apache-2.0
//
// The default backend of the extension host's test binary: the remote backend, over hosts serving in this
// process on the engine the binary links. Compiled into vtextension_remote_test only, so that the suites
// written against the default backend run their real guests through the protocol, unchanged.
#include <cstdint>
#include <memory>
#include <utility>

#include <vtextension/wasm/BackendConfig.hpp>
#include <vtextension/wasm/DefaultBackend.hpp>
#include <vtextension/wasm/host/HostServer.hpp>
#include <vtextension/wasm/host/InProcessHost.hpp>
#include <vtextension/wasm/remote/RemoteBackend.hpp>
#include <vtextension/wasm/testing/HostedEngine.hpp>
#include <vtextension/wasm/testing/PreparedGeneration.hpp>

namespace vtextension::wasm
{

std::unique_ptr<IWasmBackend> makeDefaultBackend(BackendConfig config)
{
    // One kind for the binary, as a program composes one: every backend made here behaves as one.
    static auto const kind = remote::makeRemoteKindState();
    // No connector configured: this composition runs its hosts in this process.
    if (!config.connector)
        config.connector = std::make_shared<host::InProcessHostConnector>(
            [] { return testing::makeHostedEngine(); }, host::HostServerConfig {});
    return remote::makeRemoteBackend(kind, std::move(config));
}

} // namespace vtextension::wasm

namespace vtextension::wasm::testing
{

uint64_t preparedGeneration() noexcept
{
    return remote::preparedGeneration();
}

} // namespace vtextension::wasm::testing
