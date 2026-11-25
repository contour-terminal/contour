// SPDX-License-Identifier: Apache-2.0
//
// The default backend of the extension host process's test binary: the remote backend, over real helper
// processes the binary's one supervisor starts. Compiled into vtextension_helper_test only, so that the
// contract and the suites written against the default backend run their real guests through a process
// boundary, unchanged.
#include <cstdint>
#include <memory>
#include <utility>

#include <vtextension/wasm/BackendConfig.hpp>
#include <vtextension/wasm/DefaultBackend.hpp>
#include <vtextension/wasm/remote/RemoteBackend.hpp>
#include <vtextension/wasm/remote/process/testing/HelperComposition.hpp>
#include <vtextension/wasm/testing/PreparedGeneration.hpp>

namespace vtextension::wasm
{

std::unique_ptr<IWasmBackend> makeDefaultBackend(BackendConfig config)
{
    // One kind and one supervisor for the binary, as a program composes them.
    auto const& composition = remote::process::testing::binaryComposition();
    // No connector configured: this composition starts the real helper, which no sandbox confines yet.
    if (!config.connector)
    {
        config.connector = composition.connector(remote::process::testing::HelperBackendConfig {
            .helper = VTEXTENSION_HELPER_PATH,
            .extraArguments = {},
            .lockdown = LockdownRequirement::AcceptUnsandboxed,
            .timeouts = config.timeouts });
        config.lockdown = LockdownRequirement::AcceptUnsandboxed;
    }
    return remote::makeRemoteBackend(composition.kind(), std::move(config));
}

} // namespace vtextension::wasm

namespace vtextension::wasm::testing
{

uint64_t preparedGeneration() noexcept
{
    return remote::preparedGeneration();
}

} // namespace vtextension::wasm::testing
