// SPDX-License-Identifier: Apache-2.0
//
// Configure-time probe (cmake/ContourThirdParties.cmake): exits 0 only if this WAMR stops a guest
// call that loops forever when another thread calls wasm_runtime_terminate(). That needs a WAMR built
// with WAMR_BUILD_THREAD_MGR=1; without it the interpreter never polls for termination, and the
// probe gives up after a few seconds instead of hanging the configure.
//
// It also rejects (exit code 8) a WAMR whose wasm_runtime_init() installs a SIGSEGV handler: one
// built with hardware bounds checks, which takes over the process's SIGSEGV and records process-wide
// signal state from every thread that prepares to run guests; and (exit code 9) one whose init
// installs a SIGUSR1 handler: one built with wakeup of blocking operations, which takes over SIGUSR1
// for the whole process. POSIX only; Windows has no sigaction.
//
// It rejects (exit code 10) a WAMR that loads a module using a feature extensions may not use, and
// (exit code 11) one that refuses a feature they may use, naming the feature on standard output:
// src/vtextension/wasm/EngineFeatures.hpp is the table, and the modules are the ones the backend
// contract checks (src/vtextension/wasm/testing/FeatureModules.hpp).
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <thread>
#include <vector>

#include <wasm_export.h>

#include <vtextension/wasm/testing/FeatureModules.hpp>

#ifndef _WIN32
    #include <signal.h>
#endif

namespace
{

// Whether WAMR loads a copy of the module (WAMR may rewrite the bytes it loads).
bool loads(std::span<std::uint8_t const> module)
{
    auto bytes = std::vector<std::uint8_t>(module.begin(), module.end());
    auto error = std::array<char, 128> {};
    auto* const loaded = wasm_runtime_load(bytes.data(),
                                           static_cast<std::uint32_t>(bytes.size()),
                                           error.data(),
                                           static_cast<std::uint32_t>(error.size()));
    if (!loaded)
        return false;
    wasm_runtime_unload(loaded);
    return true;
}

} // namespace

int main()
{
    // (module (func (export "spin") (loop (br 0))))
    auto bytes = std::to_array<std::uint8_t>({
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,                   // magic, version
        0x01, 0x04, 0x01, 0x60, 0x00, 0x00,                               // type: () -> ()
        0x03, 0x02, 0x01, 0x00,                                           // func 0: type 0
        0x07, 0x08, 0x01, 0x04, 's',  'p',  'i',  'n',  0x00, 0x00,       // export "spin" = func 0
        0x0a, 0x09, 0x01, 0x07, 0x00, 0x03, 0x40, 0x0c, 0x00, 0x0b, 0x0b, // loop { br 0 }
    });
    auto error = std::array<char, 128> {};

#ifndef _WIN32
    struct sigaction before {};
    sigaction(SIGSEGV, nullptr, &before);
    struct sigaction usr1Before {};
    sigaction(SIGUSR1, nullptr, &usr1Before);
#endif
    if (!wasm_runtime_init())
        return 2;
#ifndef _WIN32
    struct sigaction after {};
    sigaction(SIGSEGV, nullptr, &after);
    if (after.sa_handler != before.sa_handler || after.sa_flags != before.sa_flags)
        return 8;
    struct sigaction usr1After {};
    sigaction(SIGUSR1, nullptr, &usr1After);
    if (usr1After.sa_handler != usr1Before.sa_handler || usr1After.sa_flags != usr1Before.sa_flags)
        return 9;
#endif
    // The first feature module WAMR gets wrong is named on standard output, for the configure's reason.
    for (auto const& [feature, module]: vtextension::wasm::testing::FeatureModules)
    {
        auto const& row = vtextension::wasm::featureRow(feature);
        if (loads(module) != (row.state == vtextension::wasm::FeatureState::On))
        {
            std::printf("%.*s", static_cast<int>(row.name.size()), row.name.data());
            return row.state == vtextension::wasm::FeatureState::On ? 11 : 10;
        }
    }
    auto* const module = wasm_runtime_load(bytes.data(),
                                           static_cast<std::uint32_t>(bytes.size()),
                                           error.data(),
                                           static_cast<std::uint32_t>(error.size()));
    if (!module)
        return 3;
    auto* const instance = wasm_runtime_instantiate(
        module, 16 * 1024, 0, error.data(), static_cast<std::uint32_t>(error.size()));
    if (!instance)
        return 4;
    auto* const execEnv = wasm_runtime_create_exec_env(instance, 16 * 1024);
    auto* const spin = wasm_runtime_lookup_function(instance, "spin");
    if (!execEnv || !spin)
        return 5;

    auto giveUp = std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::_Exit(6);
    });
    giveUp.detach();
    auto watchdog = std::thread([instance] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        wasm_runtime_terminate(instance);
    });

    auto const returned = wasm_runtime_call_wasm(execEnv, spin, 0, nullptr);
    watchdog.join();
    return returned ? 7 : 0;
}
