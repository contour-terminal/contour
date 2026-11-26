// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The backend that runs extensions in a separate host: every runtime, module and instance it hands out
/// forwards to a host reached through an IHostConnector, over the protocol's channels.

#include <cstdint>
#include <memory>

#include <vtextension/wasm/BackendConfig.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension::wasm::remote
{

/// The state every remote backend of one kind shares: its runtime generation, its live runtimes and which
/// thread holds which lane (RemoteKindState.hpp).
class RemoteKindState;

/// @return A new kind's state, to share between every remote backend the composition root creates.
[[nodiscard]] std::shared_ptr<RemoteKindState> makeRemoteKindState();

/// Creates a backend running every module in a host the connector starts. Modules loaded under one identity
/// on one runtime share a host; a module of an empty identity gets a host of its own. A host that dies fails
/// every later call into its instances (`CallFailure::HostCrashed`), and is started anew when one of its
/// modules is next instantiated.
///
/// Instances' memory is reached by copy: a view is a copy of the range, read on first access, and what was
/// written to it goes back before the instance next runs.
///
/// Nothing it lets go waits for a host: a module or an instance dropped queues its release, and the backend
/// dropped lets every host it started go -- should a thread still use one of its runtimes then, its request
/// fails `HostUnavailable` rather than keep the dropping thread waiting.
/// @param kind The kind's state; every backend created with it behaves as one.
/// @param config How hosts are started; a null connector starts none, and every load fails `HostUnavailable`.
/// @return The backend.
[[nodiscard]] std::unique_ptr<IWasmBackend> makeRemoteBackend(std::shared_ptr<RemoteKindState> kind,
                                                              BackendConfig config);

/// Test-only.
/// @return The generation the calling thread's outermost remote guard was entered for; 0 if none.
[[nodiscard]] uint64_t preparedGeneration() noexcept;

} // namespace vtextension::wasm::remote
