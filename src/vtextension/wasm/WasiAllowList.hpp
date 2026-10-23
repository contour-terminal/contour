// SPDX-License-Identifier: Apache-2.0
//
// The WASI preview 1 functions the extension host lets a guest import: the one list the world check
// applies and the backend contract holds every backend to providing.
#pragma once

#include <array>
#include <string_view>

namespace vtextension::wasm
{

/// The module name of WASI preview 1, the one import module besides the world's own.
inline constexpr auto WasiModule = std::string_view { "wasi_snapshot_preview1" };

/// The WASI preview 1 functions a guest may import. Each one returns at once and reaches nothing
/// beyond the instance: with no preopened directory, no environment and standard streams bound
/// to the null device, there is nothing for it to reach. Every other WASI function is refused,
/// in particular `poll_oneoff`, whose clock wait the watchdog cannot interrupt (a guest times
/// itself with `request-frame`, it never sleeps), and the `sock_*` and `path_*` families.
inline constexpr auto AllowedWasiFunctions = std::to_array<std::string_view>({
    "args_get",            // the (empty) arguments
    "args_sizes_get",      //
    "environ_get",         // the (empty) environment; Rust's std reads it at start-up
    "environ_sizes_get",   //
    "clock_time_get",      // reads a clock; never waits
    "clock_res_get",       //
    "fd_write",            // to the null device
    "fd_close",            //
    "fd_fdstat_get",       // what a standard stream is; Rust's std asks before it prints
    "fd_prestat_get",      // finds no preopened directory
    "fd_prestat_dir_name", //
    "fd_seek",             // on the null device
    "random_get",          // fills guest memory; Rust's HashMap seeds itself with it
    "proc_exit",           // ends the call as a trap
    "sched_yield",         // returns at once in a single-threaded instance
});

} // namespace vtextension::wasm
