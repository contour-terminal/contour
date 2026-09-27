# vendor/

Third-party code carried in this repository verbatim, so that a distribution build needs no
network.

## core-cpp

`vendor/core-cpp` is a verbatim copy of a tagged release of
[core-cpp](https://github.com/contour-terminal/core-cpp), the shared C++ foundation of the Contour
Terminal projects: its `base`, `log`, `cli`, `platform`, `async`, `net` and `testing` modules.
`vendor/core-cpp/MANIFEST` names the tag and the commit, and pins every file by its SHA-256.

- **Never edit a file under `vendor/core-cpp`.** The `core-cpp-vendored-copy` test (label `lint`)
  checks the copy against its manifest, so a local edit fails the test suite, and CI runs the same
  check in the "Check C++ style" job.
- **Fix it upstream.** A defect in this code is fixed in core-cpp, released as a new tag, and then
  re-vendored here.
- **Re-vendor with one command**, run from the repository root with the `CoreCppVendor.cmake` of a
  core-cpp checkout (never the copy's own; see core-cpp's `docs/vendoring.md`), and commit the
  result as one change:

  ```sh
  cmake -DMODE=sync -DREF=<tag> -DREPO=https://github.com/contour-terminal/core-cpp         -DDEST=vendor/core-cpp "-DMODULES=base;log;cli;platform;async;net;testing"         -P <core-cpp checkout>/cmake/CoreCppVendor.cmake
  ```

- **Verify the copy** without git or network:

  ```sh
  cmake -DMODE=check -DDEST=vendor/core-cpp -P vendor/core-cpp/cmake/CoreCppVendor.cmake
  ```
