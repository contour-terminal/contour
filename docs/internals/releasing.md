# Releasing

A release is cut by pushing an annotated version tag, `v<major>.<minor>.<patch>`. The run that tag
starts builds, tests and packages exactly the tagged commit and creates the draft release for that
tag. Binaries keep the run number as a fourth version field (`0.9.0.9500`); the tag does not.

## Cutting a release

1. A pull request from `bump/release-<version>` stamps `metainfo.xml`: the version, `date=`, and no
   `type="development"`. It adds the `ReleasedProtocols` row and the transcripts that
   [vthost.md](vthost.md) describes. It merges through the merge queue like any change.
2. Check out the merge commit (the one on `origin/master`, not the pull request's own head, which the
   merge queue rewrites) and confirm with `git merge-base --is-ancestor <merge-commit> origin/master`
   that it is on master. Then run `python3 scripts/check-release.py --tag v<version>`.
3. Tag the merge commit and push that one tag, never `--tags`:

    ```sh
    git tag -a v<version> -m "Release <version>" <merge-commit>
    git push origin v<version>
    ```

4. The tag's run builds, tests and packages that commit and creates the draft release.
5. Publish the draft.

## Submitting the release to winget

Publishing the release (the draft made public; not a pre-release) starts
[`winget.yml`](https://github.com/contour-terminal/contour/blob/master/.github/workflows/winget.yml). Its `manifest` job reads the published
release and has Komac render the winget manifests for `contour-terminal.Contour` at the package version
`<version>.<run>` from the two MSIs and the portable ZIP, with their scopes and UpgradeCodes. Komac cannot finish
two things without a terminal, so `scripts/winget-release.py finalize` does: it gives the ZIP its nested
installer (the program it runs, read from the ZIP itself) and an `InstallerLocale`, and it removes the VC++
runtime dependency Komac carries over from older versions (Contour ships the runtime, and a dependency would make a
per-user install ask for an administrator). `check` then verifies the finished files, and the `submit` job
uploads exactly those files with `komac submit`; nothing is rendered a second time.

**Setting it up**, once, before the first release (the workflow names the environment, and GitHub creates a missing
one without any protection):

1. Create the environment **`winget`** (Settings, Environments).
2. Under *Deployment branches and tags* choose *Selected branches and tags* and add the tag rule `v*` and the
   branch `master`. A branch dispatch of a changed workflow then cannot reach the token.
3. Add the secret **`WINGET_TOKEN`** to the environment: a classic personal access token with `public_repo` scope.
   Give it an expiry and put the renewal in your calendar. The token belongs to a **dedicated machine account**
   that owns the `winget-pkgs` fork, has no access to the contour-terminal repositories, and has an expiry of
   its own; a token of your personal account could write to every public repository you can, workflows included.
4. Optional: add *Required reviewers*, to hold the submission until someone approves.

- **What `winget install` picks.** `--scope user` the per-user MSI, `--scope machine` the per-machine MSI, and a
  plain install the per-user MSI: winget's setting `installBehavior.preferences.scope` defaults to `user`, and it
  ranks an installer of the preferred scope strongly above the other one, in whatever order the manifest lists them
  (winget-cli `ManifestComparator.cpp` and `UserSettings.h`). The portable ZIP ignores `--scope` but ranks below an
  MSI, and its `InstallerLocale` matches the MSIs', so it is never picked unasked (`--installer-type portable`).
  `scripts/winget-release.py check` models that choice and fails when the finished manifest would pick otherwise;
  `docs/install.md` promises exactly that.
- **A dry run.** `gh workflow run winget.yml -f tag=v<version>` renders, finishes and checks the manifests of a
  published release and submits nothing. They are the run's `winget-manifests` artifact (also kept when a step
  failed).
- **Submitting by hand**, for example after fixing the token: the same command with `-f submit=true`.
- **A pull request that already exists** for that version, open or merged, is reported in the run's summary, and the
  job opens none. A pull request that was closed without merging does not count: dispatching with `submit=true`
  submits the version again.
- **Releases made by a workflow.** A release published with the workflow `GITHUB_TOKEN` does not start
  `winget.yml`; use the dispatch above.
- **The first submission of 0.9.** Komac carries every field it does not render over from the newest version in
  winget-pkgs. If you also want to catch the 0.8.0 release up, submit it **before** the first 0.9 submission: 0.8.0
  does need the `Dependencies` entry that 0.9 drops, and the newest version is the one that is carried forward.
- **Bumping Komac** means changing two lines in `scripts/ci/install-komac.sh`: the version and the SHA-256 of its
  `.deb`.

## What `check_release` checks

`scripts/check-release.py --tag NAME` reads the first `<release>` of `metainfo.xml` and fails unless:

- the version is `major.minor.patch`, the release has a `date=`, and it is not `type="development"`;
- the tag is exactly `v` plus that version (`v0.9.1` on a 0.9.0 file, or the old `v0.9.0.9500`, fail);
- the tag is annotated, not lightweight;
- the release date is the tag's date, taken in the tagger's own timezone;
- an existing tag points at the checked-out commit, so a re-run after the tag moved fails instead of
  drafting one commit's binaries under a tag that names another.

The date is the tag's rather than today's so that a release build can be re-run from its tag days
later. Before the tag exists, as in step 2 above, the date must be today. The script's `--self-test`
covers each rule.

If the bump merged on an earlier day than the tag is made, the tag run fails with "release date ... is
not the tag's date". Merge a pull request that fixes the date, delete the unpublished tag (the owner
bypasses the ruleset) and tag the new merge commit.

## Re-running a release build

Use Re-run on the tag's run. Artifacts are kept for 7 days; after that, re-run all jobs. GitHub
re-runs a run for 30 days only; after that, a rebuild means deleting and re-pushing the tag, which gives
a new run number. The draft release is replaced, so exactly one draft holds only the newest run's files,
and notes edited by hand in a draft are lost: edit `metainfo.xml` instead. A published release is never
changed.

## The release's files

`.github/release.json` lists them, one `fnmatch` pattern per file with `[0-9]*` where the version
goes. `scripts/release-assets.py select` takes exactly one file per pattern, fails if a pattern matches
none or several, and only reports files no pattern claims (such as the Fedora rawhide RPM, which never
ships). Adding a package therefore means one row, plus a job artifact whose name starts with `contour`
and contains `-<version>-` (what `do_release` downloads), and that job listed in `do_release`'s
`needs`, or the download can run before the upload. The same file is what `/publish-release` checks a
draft against.

The owner's release tooling also reads this file: it deep-merges the whole file over the release model
it detects, so a top-level key that collides with a name of that model (`trigger`, `changelog`,
`tagPrefix`, ...) silently overrides it. Add no key besides `artifacts` without checking, and do not
rename it.

## For workflow authors

- Release-only jobs use `if: startsWith(github.ref, 'refs/tags/v')`, spelled exactly that way.
- Steps and scripts read `CONTOUR_BUILD_CHANNEL` (`release` or `prerelease`), which `build.yml` sets
  once at workflow level.
- Compiler caches are saved by master pushes only; a tag run must not save one.
- `scripts/check-workflows.py` enforces the spelling, bans the retired head-ref test for the old
  release flow, and requires that only jobs gated this way create releases.

## A patch release of an older line

This works for lines from 0.9.0 on. An older line carries the old `build.yml`, which has no tag trigger
and no `maintenance/**` pull-request branch, so it would need `build.yml`, `.github/release.json`, the
`check-release.py` and `release-assets.py` scripts and `ci-set-vars.*` backported first.

1. Create `maintenance/<major>.<minor>` from that line's tag.
2. Fixes and the version bump arrive by pull request; pull requests into it run CI. `spelling.yml` and
   `changelog.yml` must list `maintenance/**` too, or their required checks never report there.
3. Tag its merge commit. There is no push trigger for the line, so the tag's run is its first full run
   (packaging, valgrind, conformance).
4. Carry the changelog entry to master.
5. Publish the release without marking it latest.

## Who may release

A tag ruleset on `v*` must limit creating, moving and deleting release tags to the owner; it is a
repository setting, not something this repository enforces.
