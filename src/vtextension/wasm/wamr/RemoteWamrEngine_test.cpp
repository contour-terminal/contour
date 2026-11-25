// SPDX-License-Identifier: Apache-2.0
//
// The contract, through the protocol, on the WAMR engine an extension host process drives: the remote backend
// over hosts serving in this process. The whole of vtextension_remote_wamr_test, which links no other engine:
// vtextension_remote_test runs the same composition on Wasmtime's.
#include <memory>

#include <vtextension/wasm/BackendConfig.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/host/HostServer.hpp>
#include <vtextension/wasm/host/InProcessHost.hpp>
#include <vtextension/wasm/remote/RemoteBackend.hpp>
#include <vtextension/wasm/remote/RemoteKindState.hpp>
#include <vtextension/wasm/testing/BackendContract.hpp>
#include <vtextension/wasm/wamr/WamrEngine.hpp>

namespace
{

/// Hosts in this process on the WAMR engine, whose memory is copied to the parent.
struct RemoteWamrTraits
{
    [[nodiscard]] static std::unique_ptr<vtextension::wasm::IWasmBackend> make()
    {
        using namespace vtextension::wasm;
        // One kind for every backend made here, as a program composes one.
        static auto const kind = remote::makeRemoteKindState();
        return remote::makeRemoteBackend(
            kind,
            BackendConfig {
                .timeouts = {},
                .connector = std::make_shared<host::InProcessHostConnector>(
                    [] { return wamr::makeWamrEngine().value_or(nullptr); }, host::HostServerConfig {}),
                .lockdown = LockdownRequirement::AcceptUnsandboxed });
    }
};

} // namespace

CONTOUR_WASM_BACKEND_CONTRACT(RemoteWamrTraits)
