// SPDX-License-Identifier: Apache-2.0
//
// The WAMR backend of the WebAssembly runtime interface: the fast interpreter, with WASI preview 1.
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension::wasm::wamr
{

/// Opens a null device.
/// @return Its raw handle (a file descriptor, or a HANDLE on Windows); nothing if it cannot be opened.
using NullDeviceOpener = std::function<std::optional<int64_t>()>;

/// @return The opener of the system's null device: `/dev/null`, or `NUL` on Windows.
[[nodiscard]] NullDeviceOpener systemNullDeviceOpener();

/// An open null device, closed as it is destroyed.
class OpenNullDevice
{
  public:
    /// @param handle The raw handle an opener returned; owned from here on.
    explicit OpenNullDevice(int64_t handle) noexcept: _handle { handle } {}
    ~OpenNullDevice();

    OpenNullDevice(OpenNullDevice const&) = delete;
    OpenNullDevice(OpenNullDevice&&) = delete;
    OpenNullDevice& operator=(OpenNullDevice const&) = delete;
    OpenNullDevice& operator=(OpenNullDevice&&) = delete;

    /// @return The raw handle.
    [[nodiscard]] int64_t handle() const noexcept { return _handle; }

  private:
    int64_t _handle;
};

/// The null device a backend's runtimes bind their guests' WASI standard streams to, which WAMR needs for
/// every module that imports WASI: opened by each runtime as it is created, or opened once and shared.
class NullDevice
{
  public:
    /// Each runtime opens a device of its own as it is created: what a backend in Contour's own process does.
    /// @param open Opens each.
    /// @return The policy.
    [[nodiscard]] static NullDevice openPerRuntime(NullDeviceOpener open = systemNullDeviceOpener());

    /// One device, opened now, which every runtime shares: what a host that may open no file once it locked
    /// itself down needs.
    /// @param open Opens it, once.
    /// @return The device; `RuntimeUnavailable` if it could not be opened.
    [[nodiscard]] static std::expected<NullDevice, ExtensionError> openShared(
        NullDeviceOpener const& open = systemNullDeviceOpener());

    /// @return The device a runtime created now binds to: the shared one, or one opened for it;
    ///         `RuntimeUnavailable` if none could be opened.
    [[nodiscard]] std::expected<std::shared_ptr<OpenNullDevice const>, ExtensionError> forRuntime() const;

  private:
    NullDevice(NullDeviceOpener open, std::shared_ptr<OpenNullDevice const> shared) noexcept:
        _open { std::move(open) }, _shared { std::move(shared) }
    {
    }

    NullDeviceOpener _open;                        ///< Opens a runtime's own; empty when one is shared.
    std::shared_ptr<OpenNullDevice const> _shared; ///< The one every runtime shares; null when each opens.
};

/// How a WAMR backend is set up.
struct WamrConfig
{
    /// The null device its runtimes bind guests' standard streams to.
    NullDevice nullDevice = NullDevice::openPerRuntime();
};

/// Creates a WAMR backend.
///
/// WAMR's runtime, its reference-counted initialization and its per-thread environment are process-wide,
/// so the backend is a facade over that one state (@see ThreadEnv, acquireRuntime): any number of WAMR
/// backends may exist in a process, and all of them behave as one. Refused memory growth is reported
/// (MemoryGrowthReporting::Reported).
/// @param config How it is set up.
/// @return The backend.
[[nodiscard]] std::unique_ptr<IWasmBackend> makeWamrBackend(WamrConfig config = {});

} // namespace vtextension::wasm::wamr
