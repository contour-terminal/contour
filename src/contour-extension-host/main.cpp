// SPDX-License-Identifier: Apache-2.0
//
// contour-extension-host: started by Contour, never by hand, to run its extensions in a process of their own.
#include <string_view>
#include <vector>

#include <vtextension/wasm/host/HelperEngine.hpp>
#include <vtextension/wasm/host/HostProcess.hpp>

int main(int argc, char const* argv[])
{
    auto const arguments = std::vector<std::string_view>(argv + 1, argv + argc);
    return vtextension::wasm::host::runExtensionHost(arguments, &vtextension::wasm::host::makeHelperEngine);
}
