// SPDX-License-Identifier: Apache-2.0
//
// The default backend where extensions run out of process (CONTOUR_EXTENSION_HOST): the remote backend, over
// extension host processes of the helper installed beside this program. Compiled into
// vtextension_out_of_process only, which links no engine: the engine runs in the helper.
#include <vtextension/wasm/DefaultBackend.hpp>

#include <vtpty/ExecutablePath.hpp>

#include <core/platform/NativeFileSystem.hpp>

#include <expected>
#include <filesystem>
#include <memory>
#include <utility>

#include <vtextension/wasm/remote/HostRefusals.hpp>
#include <vtextension/wasm/remote/RemoteBackend.hpp>
#include <vtextension/wasm/remote/process/HelperLocation.hpp>
#include <vtextension/wasm/remote/process/HostComposition.hpp>

namespace vtextension::wasm
{

namespace
{
    /// @return The extension host installed beside this program; or why there is none.
    [[nodiscard]] std::expected<std::filesystem::path, remote::process::HelperLocationError> installedHelper()
    {
        auto const self = vtpty::currentExecutablePath();
        if (!self)
            return std::unexpected(remote::process::HelperLocationError::NotFound);
        auto const fileSystem = core::platform::NativeFileSystem {};
        return remote::process::locateExtensionHost(
            *self, [&](std::filesystem::path const& path) { return fileSystem.isExecutableFile(path); });
    }
} // namespace

std::unique_ptr<IWasmBackend> makeDefaultBackend(BackendConfig config)
{
    // A connector of the caller's starts the hosts: no helper to look for, and no composition to share.
    if (config.connector)
        return remote::makeRemoteBackend(remote::makeRemoteKindState(), std::move(config));
    return remote::process::makeProcessBackend(
        std::move(config), installedHelper(), remote::errorLogWriter());
}

} // namespace vtextension::wasm
