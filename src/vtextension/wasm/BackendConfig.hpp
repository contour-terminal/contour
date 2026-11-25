// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How the composition root configures the backend that runs extensions: where their host process comes
/// from, and how long it may take. Names no engine.

#include <chrono>
#include <filesystem>
#include <memory>

namespace vtextension::wasm
{

namespace remote
{
    class IHostConnector;
} // namespace remote

/// How long an extension host process may take over each step, before it counts as hung.
struct HostTimeouts
{
    std::chrono::milliseconds load { 10'000 };       ///< To load a module.
    std::chrono::milliseconds instantiate { 1'000 }; ///< To instantiate one.
    std::chrono::milliseconds handshake { 2'000 };   ///< To say Hello once started.
    std::chrono::milliseconds interruptGrace { 50 }; ///< To stop a call once interrupted.
};

/// How the backend that runs extensions is set up.
struct BackendConfig
{
    std::filesystem::path helper;                      ///< The extension host executable.
    HostTimeouts timeouts {};                          ///< How long a host may take; not enforced yet.
    std::shared_ptr<remote::IHostConnector> connector; ///< How hosts are started; null: in this process.
};

} // namespace vtextension::wasm
