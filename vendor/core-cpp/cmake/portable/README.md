# Portable CMake modules

Files in this directory are **verbatim copies** from another repository. Do not
edit them here. When the upstream copy changes, re-sync the whole file and update
the provenance below in the same commit. A fix goes upstream first.

| File | Upstream | Commit |
| --- | --- | --- |
| `CompileCache.cmake` | [fastcached](https://github.com/LASTRADA-Software/fastcached) `cmake/portable/CompileCache.cmake` | `f6ec49f3446b8bc121eba82c64cde2de759e774a` (2026-09-20) |

A commit here is the one whose blob this file equals — the last upstream commit that
touched it, which is not necessarily `origin/master`. When `f6ec49f3` was taken,
fastcached's `origin/master` was `0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21` and
carried the identical blob.

`../FetchTransferBound.cmake` is a verbatim copy from the same commit
(`cmake/FetchTransferBound.cmake`). It exports process-wide environment, so only
`../CoreCppTopLevel.cmake` includes it, before any dependency is resolved.
`../CPM.cmake` reads its bound when it is defined.

To re-sync, read the upstream file as a blob so no line-ending conversion touches
it, and check that it contains no CR byte:

```sh
git -C <fastcached> fetch origin
git -C <fastcached> -c core.autocrlf=false -c core.eol=lf \
    show origin/master:cmake/portable/CompileCache.cmake > cmake/portable/CompileCache.cmake
```

Two gates watch this file, and they answer different questions. The nightly
`downstream.yml` workflow diffs both it and `../FetchTransferBound.cmake` against
fastcached `master` and **fails** on drift: *is this copy byte-identical to upstream's
current one?*
`scripts/check-upstream-drift.py` walks `.agent/reference/provenance.md` and
**reports** without failing: *has upstream touched this file since the commit the row
pins?* The first is the one that must stay green; the second is what names the commits
to read when it is not.

## `CompileCache.cmake`

`cmake/CoreCppTopLevel.cmake` includes it first, before anything else it does. The
top-level `CMakeLists.txt` includes that file only when core-cpp is the top-level
project, after `project()` and before `core_cpp_resolve_dependencies()`. That order
means CPM-fetched dependencies (Catch2 today; libunicode, stb and Tracy later) are
compiled through the cache too. When core-cpp is a subproject it sets no launcher,
and its targets inherit whatever the parent chose.

The module picks the first launcher that is installed and usable:

1. `fastcache-cc`, when a fastcached daemon accepts its probe compile at
   `FASTCACHE_ADDR` (default `127.0.0.1:6674`);
2. `sccache`, only with `-DALLOW_SCCACHE_FALLBACK=ON`;
3. `ccache`;
4. none: the build compiles uncached.

It leaves a launcher that is already set (by `-D`, a preset or a toolchain file)
untouched, and it never fails a configure. Its status lines start with `[cache]`.
`CoreCppTopLevel.cmake` records the selected launcher as the internal cache entry
`CORE_CPP_CXX_COMPILER_LAUNCHER`. The module sets `CMAKE_CXX_COMPILER_LAUNCHER`
only as a normal variable, so that record is what `CMakeCache.txt` shows.

- **Opt out:** `-DUSE_COMPILER_CACHE=OFF`. The `clang-coverage` preset does this.
  A cache hit replays an object whose embedded coverage mapping names the
  checkout that produced it, so the report would describe another tree. Locally,
  leave caching on for every other preset.
- **No daemon yet:** `-DFASTCACHE_AUTO_INSTALL=ON` downloads `fastcache-cc`, and
  `-DFASTCACHE_AUTO_START=ON` starts a `fastcached` daemon in the background.

Its options (`USE_COMPILER_CACHE`, `ALLOW_SCCACHE_FALLBACK`, `FASTCACHE_*`) are
unprefixed because the organisation's projects share them. They are exempt by
path from the `CORE_CPP_` prefix rule that `tests/cmake/check-cmake-hygiene.cmake`
enforces, and so are `FetchTransferBound.cmake`'s `FASTCACHED_FETCH_*` settings.
