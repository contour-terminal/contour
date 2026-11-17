# The orchestrator's WebAssembly guest

The orchestrator as a daemon-world extension: a `wasm32-wasip1` reactor that implements the
`terminal-extensions:daemon@0.1.0` world (`wit/daemon/` in Contour's tree) and runs in Contour's daemon
with no window attached.

## Layout

- `HostCalls.hpp` — `IGuestHost`, the world's imports as the driver uses them, in the core's vocabulary.
- `Driver.{hpp,cpp}` — the engine (`core/Engine.hpp`) driven through `IGuestHost`: `load` pulls the store,
  the processes and the panes into a `StartContext`; `update` serves the hints. It keeps the engine's
  driver contract (one reply per `RunCommand`, `ReadFile` and `OpenPane`; the time read on arrival on every
  input; the panes listed after a `Wake`) and releases a finished process's result only once the step
  that consumed it carried out its store writes. Built natively too, as `orchestrator_guest_driver`, and
  tested against a fake host in `Driver_test.cpp` (`orchestrator_test "[guest]"`).
- `WitHost.cpp`, `Exports.cpp` — the WebAssembly-only adapter: `IGuestHost` over the generated bindings,
  and the world's exports `load` and `update`, which keep one `Driver`.
- `bindings/` — generated, committed, never edited, and left out of the format check.
- `CMakeLists.txt` — the module's own project, built by Contour's `contour_add_wasm_cxx_guest()`
  (`cmake/ContourWasiCxx.cmake`) when `CONTOUR_EXTENSIONS_ORCHESTRATOR_GUEST` is on, into
  `<build>/wasm/orchestrator.wasm`.

The module's `terminal-extension-manifest` custom section is generated at build time: the host tool
`orchestrator_manifest` (`tools/ManifestMain.cpp`) prints `manifestText()`, and
`cmake/EmbedManifest.cmake` turns that into module-level assembly. The test
`orchestrator_manifest_matches_corpus` keeps the tool's output byte for byte equal to
`contract/manifest-v2/valid/orchestrator.manifest`.

## Regenerating the bindings

With [wit-bindgen](https://github.com/bytecodealliance/wit-bindgen) 0.63.0
(`cargo install wit-bindgen-cli --version 0.63.0 --locked`), from the repository root:

```sh
wit-bindgen c wit/daemon --world daemon --no-object-file --out-dir extensions/orchestrator/guest/bindings
```

`--no-object-file` leaves out the component-type object, which a core module does not carry;
`bindings/daemon.c` still names its force-link symbol, which `Exports.cpp` defines empty. `WitHost.cpp`
asserts the generated records' sizes against the Canonical ABI layouts the host is tested with, so
bindings generated from a changed WIT fail to build until the adapter follows.
