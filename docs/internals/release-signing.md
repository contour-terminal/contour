# Windows code signing

Windows shows "Unknown publisher" and SmartScreen warnings for installers and programs that carry
no trusted signature. The Windows job of [`build.yml`](https://github.com/contour-terminal/contour/blob/master/.github/workflows/build.yml)
is ready to sign `contour.exe`, the DLLs it ships and both installers; it signs once a signer is
configured, and until then it ships unsigned packages exactly as before. This page says what the
pipeline does, what the owner sets up on GitHub and with the signing vendor, and how to switch it
on and off.

## What the pipeline does

Four stages, each a verb of [`scripts/sign-windows.ps1`](https://github.com/contour-terminal/contour/blob/master/scripts/sign-windows.ps1):

1. **Plan** (`plan`): decides which signer this build uses from the `WINDOWS_SIGNER` and
   `WINDOWS_SIGNING_BUILDS` repository variables and the kind of build (a release tag, a push to
   master, or anything else), and prints the decision. The steps follow that output. It runs on every
   packaging run, so a misconfiguration fails on master too.
2. **Sign the payload** (`stage`, then `sign`, or `export` and `import` for SignPath): installs the
   build into a staging folder with the components CPack packages, and signs the portable
   executables in it. The result is written to a *manifest* (`out/signing/payload.tsv`): for each
   executable of the stage its path, the signed copy, and its SHA-256 before and after. The ones
   that are not signed (Qt's and Microsoft's own, already signed) are listed too, with `-` for the
   signed copy.
3. **Package** (`cpack`): CPack re-runs the install rules into its own staging directory, so the
   signed files reach the packages through a hook, `cmake/WindowsSignedOverlay.cmake`. It replaces
   each staged file named in the manifest with the signed copy, refuses a staged file that is
   neither the unsigned nor the signed bytes, and refuses a staged `.exe` or `.dll` that the manifest
   does not name at all. When nothing is signed, the manifest is an identity one (before equals after)
   and the hook changes nothing, so the hook runs on every packaging run.
4. **Sign the installers** (`sign -Kind Packages`, or the SignPath round trip): the two MSIs are
   signed last, with a description (`Contour`) so the UAC prompt names them.

Then the checks run on exactly what ships:

- With no real signer, a **dry run** (`dry-run`) signs a copy of the tree and the installers with a
  throwaway self-signed certificate and verifies the result. It proves the machinery on every
  packaging run without publishing an untrusted signature. The throwaway key lives in a named
  container of the Windows key store and is deleted afterwards.
- **Verification** (`verify`) looks at the ZIP and at the files each installer installs (it unpacks
  an administrative image with `msiexec /a`, which registers nothing). With signer `none` it fails if
  anything is signed (the *leak guard*: a dry-run signature must never be shipped); with a real
  signer it fails unless the signed payload is in the ZIP and in both installers, every signature is
  trusted by Windows and made by the signer's `Publisher`, and `contour.exe` is among them.

A signed build makes about 18 signatures: 16 files of the payload and 2 installers.

[`scripts/windows-signing/signers.psd1`](https://github.com/contour-terminal/contour/blob/master/scripts/windows-signing/signers.psd1)
is the one place a signer is described: its secrets, the tools it runs (each pinned by release and
SHA-256, each with a probe that starts it), the command line of its payload and installer steps, the
`Publisher` its certificate must have, and which builds it may sign. A new vendor is a new row there,
plus its secrets in the one `env` block the signing steps of the Windows job share. Its self-test,
`scripts/windows-signing/Test-WindowsSigning.ps1`, runs as the `contour_windows_signing` test.

## The GitHub environment

Credentials live in an environment, not in the repository's secrets, so that only a build that signs
can read them.

1. Make sure the `v*` tag ruleset of "Who may release" in [releasing.md](releasing.md) exists
   (creating, moving and deleting release tags limited to the owner) **before** a secret goes into
   the environment. The deployment rule below admits any run of a `v*` tag, and a tag run uses the
   workflow file of the tagged commit: whoever may create such a tag can run code with the secrets.
2. Create the environment **`release-signing`** (Settings, Environments).
3. Under *Deployment branches and tags* choose *Selected branches and tags* and add the tag rule
   `v*`. Add the branch `master` only if you set `WINDOWS_SIGNING_BUILDS=releases-and-master`, which is
   not recommended (see below). Releases are cut from version tags, so a tag rule is all a release
   needs.
4. Set **Required reviewers** to the owner. They hold the whole Windows job before it starts, which
   is also the way to stop a signature that nobody asked for.
5. Put the signer's **secrets** on the environment, and only there, never as repository secrets
   (never their values anywhere else):

    | Signer | Secrets |
    |--------|---------|
    | `certum` | `CERTUM_EMAIL`, `CERTUM_OTP` |
    | `sslcom` | `ESIGNER_USERNAME`, `ESIGNER_PASSWORD`, `ESIGNER_CREDENTIAL_ID`, `ESIGNER_TOTP_SECRET` |
    | `signpath` | `SIGNPATH_API_TOKEN`, and the environment variables `SIGNPATH_ORGANIZATION_ID`, `SIGNPATH_PROJECT_SLUG`, `SIGNPATH_SIGNING_POLICY_SLUG` |

6. Set two **repository** variables (Settings, Secrets and variables, Actions, Variables):
    - `WINDOWS_SIGNER`: `none`, `signpath`, `certum` or `sslcom`. Unset means `none`.
    - `WINDOWS_SIGNING_BUILDS`: `releases` (the default and the recommended setting) or
      `releases-and-master`. The second would expose the credentials, and spend about 18 of a monthly
      allowance of signatures, on every push to master without anyone looking.

   They must be repository variables, not environment variables: the job chooses its environment
   from them, and an environment's own variables are not visible before the job has entered it.
   The condition that decides it is spelled twice in `build.yml` (the `environment:` key and the
   concurrency group); the two copies must stay identical.

Pull requests never package, and a job that does not sign never enters the environment.

**Time.** The Windows job may run 150 minutes, or 300 with `WINDOWS_SIGNER=signpath`. With SignPath
the job waits for the owner twice, once for the payload and once for the installers, up to an hour
each from the moment the request appears. Approve each within an hour, or the release has no Windows
packages and a re-run makes two new requests.

**Signing jobs run one at a time**: they share a concurrency group, because a one-time code is
single use and two jobs logging in within its 30 seconds collide. GitHub keeps only one waiting job
per group, so tags pushed in quick succession can lose the middle one; cut releases one at a time.

**A residual risk.** The secrets belong to the whole Windows job, not to the signing steps alone: the
runner hands them over when the job starts, and the 70 minutes of build and test steps before the
signing run beside them. Every action of the job is therefore pinned to the commit of a release tag
(with the tag in a comment), so that a moved tag cannot change it, but the build still runs vcpkg,
Qt's installer and the project's own scripts next to the secrets. If any of that is ever suspected,
rotate what the signer holds (below) and review the signing history of the account. Moving the
signing into a job of its own, which would take only the build tree, is the stronger design and is
recorded as follow-up work.

## Certum "Open Source Code Signing in the Cloud"

This is the likely route for an open-source project: a certificate for an individual open-source
developer whose key stays in Certum's cloud HSM (SimplySign).

1. Buy the *open-source, cloud* variant (SimplySign), not the card-reader set.
2. Pass identity verification as an individual open-source developer.
3. Activate the certificate in the SimplySign app. **Save the TOTP seed at activation.** The QR code
   encodes an `otpauth://totp/...?secret=...` URI: decode it before (or instead of) scanning, store
   the `secret` value, then add it to the phone as well. CI generates the one-time codes from it.
4. Put the account e-mail in `CERTUM_EMAIL` and the TOTP seed in `CERTUM_OTP`.
5. After the first signature, set `Publisher` of the `certum` row in `signers.psd1` to the subject
   of your certificate (the pattern there, `*Christian Parpart*`, is a guess); every signature the
   signer makes is compared with it.

Signatures are timestamped by `http://time.certum.pl`, then by DigiCert's and Sectigo's servers in
turn when one is down; the plan includes 5,000 signatures a month.

**The risk, named.** CI signs through `ssign` ([`Le-Syl21/ssign`](https://github.com/Le-Syl21/ssign)), an
unofficial, MIT-licensed community client, not affiliated with Certum, started in July 2026 and run
by one main maintainer. It is not supported by Certum and can break whenever Certum changes its
interface; check Certum's terms on automated use. It handles the owner's SimplySign credentials and
TOTP seed, and **the seed plus the e-mail address is the signing key**: whoever has them can sign as
you, indefinitely, until the QR code is re-issued. Putting the seed into GitHub moves the second
factor off the phone. After a login, ssign caches a token on disk (it signs for about 30 minutes);
the signing step deletes that session afterwards.

The pinned version is `v0.1.10` (`ssign-windows-x86_64.zip`, SHA-256
`68acfc7535830abb716c85a08f28d36dd46ed3098e3cf5c6f0ebe3c8c6694516`; the PKCS#11 module is pinned in
`signers.psd1` too). The pin fixes bytes; it does not prove that they match any reviewed source, and
the project publishes no build attestation. **Review that version before adding the secrets**, and the
source diff again before every bump (`auth.rs` and `client.rs`, the network endpoints, first), or
build it from a reviewed commit (`cargo install --locked --git https://github.com/Le-Syl21/ssign --rev <sha>
ssign ssign-pkcs11`) and pin that. The alternatives, which need no unofficial client, are SignPath
and SSL.com below, and SimplySign Desktop on the owner's PC with the `signtool` row: the seed then
never leaves the phone, but signing each release is manual (see *Signing locally*).

Use GitHub-hosted runners only: the session token lives on disk until the step deletes it, and a
self-hosted runner's next job could use it.

The installers are signed with osslsigncode using libp11 and ssign's PKCS#11 module, because
`ssign` has no native MSI signing yet. The chain was probed without credentials: provider and
module load, and the module refuses with "CERTUM_EMAIL is not set", as expected. Nothing was
signed with a real account yet. **Before setting `WINDOWS_SIGNER=certum`, sign a locally staged tree
with the real account** (see *Signing locally*).

**If the seed may have leaked**: re-issue the SimplySign QR code (that replaces the seed), update
`CERTUM_OTP`, review Certum's signing history for signatures you did not make, and ask Certum to
revoke the certificate if there is misuse.

## SSL.com individual (IV) certificate on eSigner

1. Order an individual validation (IV) code signing certificate with **eSigner cloud key
   storage**. A YubiKey or USB token cannot sign in CI.
2. Enrol in eSigner and **save the TOTP secret** shown at enrolment.
3. Find the credential ID: `CodeSignTool get_credential_ids -username=... -password=...`.
4. Set the secrets `ESIGNER_USERNAME`, `ESIGNER_PASSWORD`, `ESIGNER_CREDENTIAL_ID` and
   `ESIGNER_TOTP_SECRET`, and `Publisher` of the `sslcom` row as for Certum.

CI signing through CodeSignTool is officially supported by SSL.com. `batch_sign` signs a directory
with a single one-time code (up to 100 files). Billing is by signature count. CodeSignTool
timestamps through its own configuration (`TSA_URL=http://ts.ssl.com`), so unlike the other signers
it cannot fall back to another timestamp server. It takes the credentials as command-line arguments,
where any process of the same user can read them: use it on GitHub-hosted runners only, which are
ephemeral and mask the values in the log. The module runs the bundled Java directly and never the
`CodeSignTool.bat`, which cmd.exe would parse again.

## SignPath Foundation

SignPath signs open-source projects for free.

1. Apply to SignPath Foundation. Before that, meet its requirements: an OSI-approved license, an
   active and released project, and a published code signing policy page (the text SignPath
   Foundation prescribes, with the team roles committers, reviewers and approvers, and MFA for
   all of them).
2. After acceptance, note the **organization ID** and create the project with slug `contour`.
3. Add the signing policy `release-signing` with manual approval. There is no test-signing policy:
   master builds stay unsigned, because a test signature published as a build would be untrusted.
4. Link the GitHub trusted build system to the repository.
5. Paste the artifact configurations from `src/contour/packaging/signpath/` under the slugs
   `payload` (`payload.xml`) and `packages` (`packages.xml`).
6. Check origin verification now: whether SignPath accepts a build whose ref is a version tag.
   The release flow depends on it.
7. Create a CI user and put its API token into `SIGNPATH_API_TOKEN`. Set the environment
   variables `SIGNPATH_ORGANIZATION_ID`, `SIGNPATH_PROJECT_SLUG` and
   `SIGNPATH_SIGNING_POLICY_SLUG`.

SignPath's terms allow signing only the project's own binaries, so `contour.exe` and the
installers are signed and the vcpkg DLLs stay unsigned (the `signpath` row says `Own`, and
`OwnBinaries` in `signers.psd1` and `payload.xml` must name the same files). Each release makes
two signing requests, one for the payload and one for the installers, and each must be approved in
SignPath within an hour (see *Time* above). No long-lived credential sits in CI beyond the API token
of the CI user.

## Switching on, and off

1. Put the secrets and variables of the chosen signer in place (above).
2. Set the repository variable `WINDOWS_SIGNER`.
3. Push a release tag. The Windows job enters `release-signing` (a deployment shows on the run),
   `Signing: plan` prints the signer, `cpack` prints `WindowsSignedOverlay: <n> files from the
   signing manifest`, and `Signing: verify what ships` ends with what the signer ships.
4. Check a downloaded artifact:

        Get-AuthenticodeSignature .\contour-<version>-win64.msi | Format-List Status, SignerCertificate
        signtool verify /pa /v .\contour-<version>-win64.msi

   For the ZIP, extract it and check `bin\contour.exe` the same way.

`WINDOWS_SIGNER=none` switches back to unsigned packages with the dry run; nothing else changes.

## Signing locally

The same verbs sign a build on the owner's PC. This is also the way to test an account before CI
uses it, and the official Certum fallback. Run them from the checkout's root, each as a process of
its own (`pwsh -NoProfile -File`), with `$signer` the row to use: `certum` with the two variables
set in this session, or `signtool` with SimplySign Desktop logged in and the certificate's thumbprint
in `WINDOWS_SIGNING_CERT_SHA1`.

    $signer = 'signtool'
    $env:WINDOWS_SIGNING_CERT_SHA1 = '<thumbprint>'
    $sign = 'scripts/sign-windows.ps1'
    pwsh -NoProfile -File $sign stage -BuildDirectory out/build/msvc-release -Destination out/signing/stage
    pwsh -NoProfile -File $sign sign -Kind Payload -Root out/signing/stage -Signer $signer -Manifest out/signing/payload.tsv

    cpack --preset msvc-release -D "CPACK_CONTOUR_SIGNING_MANIFEST=$PWD/out/signing/payload.tsv"
    cpack --preset msvc-release-user -D "CPACK_CONTOUR_SIGNING_MANIFEST=$PWD/out/signing/payload.tsv"

    $msi = 'out/build/msvc-release/contour-<version>-win64.msi,out/build/msvc-release/contour-<version>-win64-user.msi'
    pwsh -NoProfile -File $sign sign -Kind Packages -Root out/build/msvc-release -Files $msi -Signer $signer
    pwsh -NoProfile -File $sign verify -Signer $signer -Manifest out/signing/payload.tsv -Zip out/build/msvc-release/contour-<version>-win64.zip -Msi $msi

`<version>` is the one CPack printed (three fields, `0.9.0`, in a local build; CI adds the run number
as a fourth). Packages signed this way carry a different name than the ones CI built for a release,
so they do not replace them on the draft release: use this flow to test an account and a certificate,
not to publish. `pwsh -NoProfile -File $sign tools -Signer $signer` downloads and starts the pinned tools of a
signer without signing anything.
