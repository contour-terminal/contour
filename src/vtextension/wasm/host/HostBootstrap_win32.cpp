// SPDX-License-Identifier: Apache-2.0
//
// What the extension host does to itself on Windows: it asks for no fault report. The kill-on-close job
// Contour starts it in ends it with its parent, caps its memory and its processes, and ends it on an
// unhandled exception rather than show a dialog.
#include <cstdint>
#include <expected>

#include <vtextension/wasm/host/HostBootstrap.hpp>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <werapi.h>

namespace vtextension::wasm::host
{

std::expected<void, BootstrapError> bindToParent([[maybe_unused]] int64_t parentProcessId) noexcept
{
    return {};
}

remote::proto::LockdownState hardenProcess() noexcept
{
    return SUCCEEDED(::WerSetFlags(WER_FAULT_REPORTING_NO_UI)) ? remote::proto::LockdownState::Unsandboxed
                                                               : remote::proto::LockdownState::Failed;
}

remote::proto::LockdownState capResources([[maybe_unused]] uint64_t dataBytes,
                                          [[maybe_unused]] CapPolicy policy) noexcept
{
    return remote::proto::LockdownState::Unsandboxed;
}

} // namespace vtextension::wasm::host
