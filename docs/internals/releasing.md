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
