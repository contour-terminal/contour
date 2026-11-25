// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Why Contour runs no extension host, said once: a host it could not start, or one it refused at the
/// handshake. Each reason is logged the first time it comes up, never again for that backend -- a reason
/// stays as long as the installation does, and every later attempt would only repeat it.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace vtextension::wasm::remote
{

/// Why no extension host serves.
enum class HostRefusal : uint8_t
{
    HelperNotFound,  ///< The host executable is missing, or cannot be executed.
    OutOfResources,  ///< The system is out of memory, processes or handles.
    Unsupported,     ///< The system lacks what starting a host takes.
    LaunchRefused,   ///< The system refused to start it for another reason.
    ProtocolVersion, ///< It speaks another protocol version than Contour.
    LockdownFailed,  ///< It failed to confine itself.
    Unsandboxed,     ///< It runs without a sandbox, which Contour requires.
    MemoryMode,      ///< It shares memory, where Contour copies it.
};

/// @param refusal Why no extension host serves.
/// @return The line logged for it, user-readable, naming no engine. A host that could not be started says
///         what the launcher says of its failure (launchErrors()), in one sentence for every such reason.
[[nodiscard]] std::string refusalLine(HostRefusal refusal);

/// Logs each reason the first time it comes up. Thread-safe; the reasons seen are kept wait-free.
class HostRefusalLog
{
  public:
    /// Writes one logged line.
    using Writer = std::function<void(std::string_view line)>;

    /// @param writer Where the lines go: errorLogWriter() for Contour's error log, a recorder in a test.
    explicit HostRefusalLog(Writer writer);

    /// Logs @p refusal's line, unless it was logged already.
    /// @param refusal Why no host serves.
    void refused(HostRefusal refusal);

  private:
    Writer _writer;
    std::atomic<uint32_t> _logged { 0 }; ///< A bit per reason logged, at its enumerator's value.
};

/// @return A writer to Contour's error log.
[[nodiscard]] HostRefusalLog::Writer errorLogWriter();

} // namespace vtextension::wasm::remote
