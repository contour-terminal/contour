# coro — C++23 coroutine vocabulary types

`Task`, `WhenAll`, `WhenAny`, `Cancellation`, `Awaitable`: the dependency-free coroutine
building blocks used by `src/net`'s reactor and the multiplexer daemon.

## Provenance and re-sync

This module is a near-verbatim copy of `src/coro/` from the
[Endo](https://github.com/christianparpart/endo) project (Apache-2.0, same author),
taken at commit `178cb496`. Keep it byte-close to upstream so fixes flow both ways.

To re-sync against a newer Endo checkout:

```sh
for f in Awaitable.hpp Cancellation.hpp Task.hpp WhenAll.hpp WhenAny.hpp \
         Task_test.cpp WhenAny_test.cpp; do
    sed 's/endo::coro/coro/g' "$ENDO/src/coro/$f" > "src/coro/$f"
done
git diff   # expected deltas: this rename, a few de-Endo'd doc comments, test_main.cpp
```

Deliberate local deltas (re-created on every re-sync, keep them small):

- namespace `endo::coro` → `coro`;
- doc comments no longer reference Endo-only types (`endo::Generator`);
- `test_main.cpp` follows this repository's Catch2 runner template
  (`crispy::suppressWindowsDialogs`);
- `WhenAll_test.cpp` is Contour-only (upstream has no dedicated WhenAll test).

Conventions inherited from upstream and enforced by this repository's clang-tidy:
coroutine parameters are pointers, never references
(`cppcoreguidelines-avoid-reference-coroutine-parameters`).
