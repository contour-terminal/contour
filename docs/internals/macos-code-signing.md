# macOS code signing, notarization and packaging

Shipping a macOS app that opens on someone else's Mac takes four things, and missing any
one of them produces the same unhelpful Gatekeeper dialog:

1. The bundle is **self-contained** — every library it loads is inside it.
2. Every Mach-O in it is **signed with a Developer ID**, inside-out, under the
   **hardened runtime**.
3. The **disk image** is signed too.
4. Both the app and the image are **notarized** by Apple and have the ticket **stapled**.

A correct signature alone is not enough. `spctl` says so plainly:

```
$ spctl --assess --type install Contour-0.7.0-Darwin.dmg
Contour-0.7.0-Darwin.dmg: rejected
source=Unnotarized Developer ID
```

All four steps run automatically from `cpack`. The mechanics live in
[`scripts/macos-bundle.py`](../../scripts/macos-bundle.py), which is driven by the
`install(CODE …)` block in `src/contour/CMakeLists.txt` and by the two CPack hooks
`cmake/MacOSNotarizeApp.cmake` and `cmake/MacOSSignDmg.cmake`.

## Building a distributable .dmg

The `macos-package` preset is the **only** configuration that produces a `.dmg`, and CI
uses it too — there is deliberately no second definition of "a shippable macOS build" to
drift out of sync:

```sh
export QT_ROOT_DIR=$HOME/Qt/6.11.1/macos
export VCPKG_ROOT=$HOME/vcpkg          # git clone microsoft/vcpkg && ./bootstrap-vcpkg.sh
cmake --preset macos-package           # also builds the vcpkg dependencies, once
cmake --build --preset macos-package
ctest --preset macos-package           # what CI runs, same configuration
cpack --preset macos-package
```

The first configure builds openssl, libssh2, yaml-cpp, freetype, harfbuzz and cairo from
source — about 5 minutes, cached in `~/.cache/vcpkg/archives` afterwards. `install-deps.sh`
provides the autotools those ports need on the build host.

The image contains `contour.app` and the `/Applications` symlink, nothing else. That is
not luck: `contour-common` turns `CONTOUR_INSTALL_TOOLS` **on**, and `src/vtbackend`
installs `bench-headless` straight into `contour.app/Contents/MacOS` when it is set — so
the packaging preset turns it back off, and CI asserts the mounted image's contents.
Tests are built (`CONTOUR_TESTING=ON`, inherited) but no test target has an install rule,
so building them cannot affect what ships.

> **Stale build directories lie.** Subproject test options are cached on first configure
> and are not revisited when `CONTOUR_TESTING` changes, so a directory first configured
> with tests off keeps most suites disabled even after the preset turns them on. If
> `ctest --preset macos-package` reports far fewer than the full suite, delete
> `out/macos-package/` and configure again.

That runs, in order: `macdeployqt` → `prune` → `verify` → `sign` → notarize+staple the
app → build the image → sign the image → notarize+staple the image → `spctl`. Every step
is fatal on failure, so a broken bundle stops the build instead of becoming a release.

Two knobs control the Apple round trips, which cost a minute or more each:

| option | effect |
|---|---|
| `CONTOUR_MACOS_NOTARIZE` | Notarize and staple the **.dmg**. This is the one Gatekeeper demands on a downloaded file. |
| `CONTOUR_MACOS_STAPLE_APP` | Additionally notarize and staple the **.app inside** the image, so it launches offline after being dragged to /Applications. |

Both are `ON` in the `macos-package` preset. CI enables the first for `master` and the
second only for `release`, so an ordinary push pays one wait instead of two.

## Why not Homebrew's Qt

Homebrew ships Qt as roughly forty separate per-module formulae (`qtbase`,
`qtdeclarative`, `qtmultimedia`, …), each in its own prefix. `macdeployqt` resolves QML
modules and plugins relative to a *single* Qt prefix, so it silently misses everything in
the other thirty-nine: it prints `ERROR: Cannot resolve rpath` to stderr, exits 0, and
leaves frameworks whose `LC_ID_DYLIB` still points into `/opt/homebrew`. Release builds
therefore use the official Qt binaries, which live under one prefix and are built to be
relocated — the same Qt the Linux and Windows CI jobs already use.

Homebrew's Qt remains perfectly fine for the `appleclang-debug` / `appleclang-release`
dev presets, which are never packaged. `scripts/install-deps.sh` no longer installs it
by default; pass `CONTOUR_INSTALL_BREW_QT=ON` if you want it.

Install the official Qt with:

```sh
pip install aqtinstall
aqt install-qt mac desktop 6.11.1 clang_64 \
    -m qtmultimedia qt5compat qtshadertools qtspeech -O ~/Qt
```

## One-time credential setup

Two independent credentials are needed: a **certificate** to sign with, and an **API key**
to notarize with. Neither can substitute for the other.

### 1. Developer ID Application certificate (signing)

Check whether you already have a usable one — an "identity" means the certificate *and*
its private key are present, which is exactly what is needed:

```sh
security find-identity -v -p codesigning
# 1) 8E5F... "Developer ID Application: Christian Parpart (6T525MU9UR)"
```

If it is listed, nothing needs to be recreated for local signing.

**If it is missing or expired**, create one at
<https://developer.apple.com/account/resources/certificates>:

1. Keychain Access → menu *Certificate Assistant* → *Request a Certificate From a
   Certificate Authority*. Enter your email and name, choose **Saved to disk**, and keep
   the resulting `CertificateSigningRequest.certSigningRequest`. This also creates the
   private key in your login keychain — the half Apple never sees and never sends back.
2. On the certificates page: **+** → **Developer ID Application** → *Profile Type: G2 Sub-CA*
   → upload the CSR → **Download** the `.cer`.
3. Double-click the `.cer` to install it. `security find-identity -v -p codesigning`
   should now list it.

Note: creating Developer ID certificates requires the **Account Holder** role, and a team
is limited to a small number of them — so revoke an unused one rather than accumulating.

**For CI**, export the identity as a `.p12` (certificate + private key in one file):

1. Keychain Access → *login* keychain → category **My Certificates** → find
   `Developer ID Application: …` → expand it and confirm a private key sits underneath.
2. Right-click → **Export "Developer ID Application: …"** → format **Personal Information
   Exchange (.p12)** → choose a password. That password becomes the `P12_PASSWORD` secret.

Then encode it and set three repository secrets:

```sh
base64 -i DeveloperID.p12 | pbcopy      # -> secret BUILD_CERTIFICATE_BASE64
openssl rand -base64 24                 # -> secret KEYCHAIN_PASSWORD (any random string)
```

| secret | value |
|---|---|
| `BUILD_CERTIFICATE_BASE64` | base64 of the `.p12` |
| `P12_PASSWORD` | the password chosen during export |
| `KEYCHAIN_PASSWORD` | any random string; it only protects the runner's throwaway keychain |

The old `BUILD_PROVISION_PROFILE_BASE64` secret is no longer used — provisioning profiles
belong to App Store and ad-hoc distribution, not to Developer ID. It can be deleted.

### 2. App Store Connect API key (notarization)

Preferred over an Apple ID plus app-specific password: it is tied to no personal account,
is scoped by role, and can be revoked on its own.

1. <https://appstoreconnect.apple.com> → **Users and Access** → **Integrations** tab →
   **App Store Connect API** → **Team Keys** → **+**.
2. Name it (e.g. `contour-notary`), set Access to **Developer**, generate.
3. **Download `AuthKey_XXXXXXXXXX.p8` immediately — Apple allows it exactly once.**
4. Note the **Key ID** (10 characters, in the key's row) and the **Issuer ID** (a UUID
   shown above the table, shared by all of the team's keys).

Store it locally under the profile name the build expects:

```sh
xcrun notarytool store-credentials contour-notary \
    --key ~/Downloads/AuthKey_XXXXXXXXXX.p8 \
    --key-id XXXXXXXXXX \
    --issuer aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee

# confirm it works (empty history is a success; an auth error is not)
xcrun notarytool history --keychain-profile contour-notary
```

Override the profile name with `-DCONTOUR_MACOS_NOTARY_PROFILE=…` if you use another.

**For CI**, set three more secrets:

| secret | value |
|---|---|
| `APPLE_API_KEY_P8` | `base64 -i AuthKey_XXXXXXXXXX.p8 \| pbcopy` |
| `APPLE_API_KEY_ID` | the 10-character Key ID |
| `APPLE_API_ISSUER_ID` | the Issuer UUID |

Keep the `.p8` somewhere safe. It cannot be downloaded again, and losing it means
generating a new key and updating the secrets.

## Why not `codesign --deep`

`--deep` is what the earlier attempts here used, and Apple documents it as unsuitable for
distribution signing. It applies the *top-level* entitlements to nested code, and it does
not guarantee that nested code is sealed before its container — so the outer seal can end
up covering a signature that was replaced afterwards. `macos-bundle.py sign` walks the
bundle deepest-first instead: loadable dylibs in `Frameworks/`, `PlugIns/` and
`Resources/qml/`, then each framework's versioned directory, then helper executables, then
the main executable, then the bundle. Only the outermost signature carries entitlements.

`--deep` *is* right for verification, where it means "check every nested signature too":

```sh
codesign --verify --deep --strict --verbose=4 contour.app
```

## Entitlements

[`support/macOS/entitlements.plist`](../../support/macOS/entitlements.plist) is applied to
the app bundle only. It carries no explanatory comments, because the kernel's entitlements
parser (AMFI) rejects XML comments outright — `Failed to parse entitlements:
AMFIUnserializeXML: syntax error` — so the reasoning lives here:

- `com.apple.security.cs.allow-jit` — the QML engine's V4 JIT needs `MAP_JIT`, which the
  hardened runtime otherwise denies.
- `com.apple.security.cs.disable-library-validation` — insurance for Qt plugin loading.
  Everything Contour bundles is re-signed with the same Team ID, which library validation
  already permits, so this is a candidate for removal; verify a notarized build launches
  and loads its plugins before dropping it.

## Inspecting a signature

```sh
# Identity, hardened runtime flag, timestamp, team ID
codesign -dv --verbose=4 contour.app

# Hardened runtime shows as flags=0x10000(runtime). Its absence is invisible to --verify
# and is a hard notarization rejection.
codesign -dv --verbose=4 contour.app 2>&1 | grep -E 'Authority|flags='

# Entitlements actually embedded
codesign -d --entitlements - --xml contour.app | plutil -p -

# What Gatekeeper will decide
spctl --assess --verbose=4 --type exec contour.app
spctl --assess --verbose=4 --type install contour.dmg
xcrun stapler validate contour.dmg
```

## Reproducing what a user sees

`spctl` is an assessment, not the real thing: Gatekeeper only engages when a file carries
the quarantine attribute a browser sets. To test the actual download path:

```sh
xattr -w com.apple.quarantine "0081;00000000;Safari;" contour.dmg
open contour.dmg          # then drag Contour to /Applications and launch it
```

Launching again with networking disabled proves the *stapled* ticket works. Without a
staple, first launch silently depends on Gatekeeper reaching Apple — which is why the app
inside the image is notarized and stapled separately from the image itself.

## Minimum macOS version

`verify` reports the oldest macOS the bundle can run on, computed as the highest `minos`
across every bundled Mach-O:

```
macos-bundle: verify: bundle requires macOS 26.0 or newer, set by 14 binary/binaries
```

The floor is **13.3**, and it is enforced rather than hoped for.

### Why the dependencies come from vcpkg, not Homebrew

Homebrew ships one prebuilt bottle per macOS release and installs the one matching the
build machine. A bundle assembled from Homebrew dylibs therefore inherits *the builder's*
macOS as its minimum — on a developer's up-to-date Mac that meant a `.dmg` requiring
**macOS 26**, and in CI it meant the floor moved silently whenever GitHub rotated the
runner image. Nothing surfaced it: the app was correctly signed, correctly notarizable, and
simply refused to start.

So the six libraries Contour links — openssl, libssh2, yaml-cpp, freetype, harfbuzz, cairo
— are built from source by vcpkg against `CMAKE_OSX_DEPLOYMENT_TARGET`. The whole mechanism
is one triplet file, [`cmake/vcpkg-triplets/arm64-osx-contour.cmake`](../../cmake/vcpkg-triplets/arm64-osx-contour.cmake),
whose `VCPKG_OSX_DEPLOYMENT_TARGET` is the single knob. This is not a new mechanism: the
Windows job has always built its dependencies with vcpkg from the same `vcpkg.json`.

A side benefit: vcpkg's cairo does not drag in the X11 stack that Homebrew's does, so
`libX11`, `libXext`, `libXrender`, `libXau` and `libxcb` no longer ship at all.

### Two macdeployqt details this depends on

- Homebrew dylibs carry an *absolute* install name, so macdeployqt resolved them without
  help. vcpkg's carry `@rpath/libfoo.dylib`, and macdeployqt expands `@rpath` using only
  the binary's own `LC_RPATH` — its `-libpath` option is **not** consulted for this. The
  vcpkg library directory is therefore added to `CMAKE_INSTALL_RPATH` for the deployment
  step, and removed again afterwards so no build-machine path ships in a release binary.
- macdeployqt usually strips the rpaths it consumed itself, so that removal is conditional;
  an unconditional `install_name_tool -delete_rpath` fails hard on a missing `LC_RPATH`.

### The promise is checked

`CONTOUR_MACOS_MIN_SUPPORTED` (13.3 in the `macos-package` preset) is compared against what
the bundled binaries actually demand. If anything exceeds it, `verify` fails and no package
is produced:

```
- the bundle requires macOS 26.0, but this build promises support down to 13.3 --
  users below 26.0 would be unable to launch it
```

It defaults to empty (report only) for the dev presets, which still use Homebrew and whose
floor is whatever the developer's bottles happened to be — a fact, not a promise.

`LSMinimumSystemVersion` in `Contents/Info.plist` is the same number. Qt's Info.plist
template — which `qt_add_executable()` installs, in place of CMake's
`MacOSXBundleInfo.plist.in` — substitutes `CMAKE_OSX_DEPLOYMENT_TARGET` into it, and that is
what the vcpkg triplet pins. So the version macOS shows in a "requires a newer macOS" dialog
and the version `verify` measures across the bundled binaries come from one source and
cannot drift apart. Were they allowed to, a too-low declaration would be worse than none:
macOS would launch the app and let dyld fail on a missing symbol instead of refusing it
with a clear message.

`NSHighResolutionCapable` is likewise absent from the shipped `Info.plist`, and that is the
enabled state, not a gap. Measured on a Retina display: an absent key and an explicit
`<true/>` both give an `NSWindow` a `backingScaleFactor` of 2.0, and only an explicit
`<false/>` drops it to 1.0. Apple's own bundled apps set the key nowhere, and Qt ships a
separate `Info.plist.disable_highdpi` precisely because opting *out* is the case that needs
saying. Declaring it would mean forking Qt's template — and forgoing whatever Qt adds to it
next — to restate a default.

The CI runner image is pinned (`runs-on: macos-15`) for toolchain reproducibility. The
floor no longer depends on it.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `resource fork, Finder information, or similar detritus not allowed` | Extended attributes; `sign` runs `xattr -cr` first for this reason. |
| `The signature of the binary is invalid` from notarytool | Something was modified after signing — `install_name_tool` invalidates a signature, so any relinking must precede signing. |
| `The executable does not have the hardened runtime enabled` | Missing `--options=runtime` on some nested binary. |
| codesign hangs in CI | The key's ACL does not permit codesign; needs `security import -T /usr/bin/codesign` *and* `security set-key-partition-list`. |
| `ERROR: Cannot resolve rpath` from macdeployqt | Split-prefix Qt (see above). It exits 0 anyway; `verify` is what catches it. |
