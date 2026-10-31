#!/usr/bin/env bash
# Prepares an already-configured tree for clang-tidy: generates the headers a translation unit
# includes (moc/autogen output, generated tables and data headers) without compiling any object.
#
#   scripts/ci-build-tidy-prerequisites.sh BUILD_DIR
#
# The autogen steps are run as the bare `cmake -E cmake_autogen` commands ninja would run, not as
# targets: CMake makes every `*_autogen` target wait for the libraries it links, so building the
# target compiles the tree; first-party generated headers get the same treatment for the order-only
# library dependencies they carry. The third-party tables (libunicode) are built as targets. All are
# discovered from `ninja -t targets all`, so a new Qt target or generated header is picked up
# without editing this script.
set -euo pipefail

build_dir="${1:?usage: ci-build-tidy-prerequisites.sh BUILD_DIR}"

all_targets="$(ninja -C "$build_dir" -t targets all)"

mapfile -t autogen < <(sed -n -E 's/^(src\/[^ :]*_autogen\/timestamp): CUSTOM_COMMAND$/\1/p' <<<"$all_targets" | sort -u)
# Generated headers, by where ninja names them: build-relative `src/...` are ours; third-party ones sit
# under `_deps/` -- build-relative when the dependency is fetched into the build tree, absolute when it
# is embedded from the source tree's _deps/sources (the CI layout). Build-tree duplicates spelled
# absolutely are dropped.
absolute_build_dir="$(cd "$build_dir" && pwd)"
mapfile -t generated < <(
    sed -n -E 's/^([^ :]+\.(hpp|h)): CUSTOM_COMMAND$/\1/p' <<<"$all_targets" \
        | grep -v '/CMakeFiles/' | grep -v -F "$absolute_build_dir/" | sort -u
)
mapfile -t dependency_headers < <(printf '%s\n' "${generated[@]}" | grep '_deps/')
mapfile -t own_headers < <(printf '%s\n' "${generated[@]}" | grep '^src/')
if [[ ${#autogen[@]} -eq 0 || ${#generated[@]} -eq 0 ]]; then
    echo "ci-build-tidy-prerequisites: found ${#autogen[@]} autogen and ${#generated[@]} generated-header targets in $build_dir; expected both" >&2
    exit 1
fi

echo "ci-build-tidy-prerequisites: ${#generated[@]} generated header(s), ${#autogen[@]} autogen step(s)"
# Never call ninja without targets: it would build the default target, i.e. the whole tree.
if [[ ${#dependency_headers[@]} -gt 0 ]]; then
    ninja -C "$build_dir" "${dependency_headers[@]}"
fi
for target in "${own_headers[@]}" "${autogen[@]}"; do
    ninja -C "$build_dir" -t commands -s "$target" | bash -e
done
