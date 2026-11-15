#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# R11: how git behaves in a worktree an agent controls. The orchestrator runs git in such a worktree to
# decide whether it may remove it; this measures whether those cleanup commands can be made to run a
# program the agent configured, and what each candidate command prints in the states cleanup must tell
# apart. Everything happens in a fresh `mktemp -d` directory; no real repository is touched.
#
# Usage: r11.sh [fixture-dir] > r11.txt   (the output carries no paths, hashes or times, so it diffs cleanly)
#
# With a fixture directory, step 3 also writes the raw `git worktree list --porcelain -z` captures there
# (core/testdata/git/ is where the tests read them), their scratch prefix rewritten to /home/u.
#
# Linux only: it relies on GNU `touch -d @<epoch>`, GNU `sed -i` and `\b` in GNU sed regular expressions.

set -euo pipefail

FIXTURE_DIR="${1:-}"
if [[ -n "$FIXTURE_DIR" ]]; then
    mkdir -p "$FIXTURE_DIR"
    FIXTURE_DIR="$(cd "$FIXTURE_DIR" && pwd -P)"
fi

# --- Isolation: nothing from the caller's environment or configuration may change a result. ---------
while IFS= read -r name; do
    case "$name" in
        CONTOUR_* | GIT_*) unset "$name" ;;
        *) ;;
    esac
done < <(compgen -e)

SCRATCH="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$SCRATCH"' EXIT

export HOME="$SCRATCH/home"
export XDG_CONFIG_HOME="$HOME/.config"
export GIT_CEILING_DIRECTORIES="$SCRATCH"
mkdir -p "$HOME"
export LC_ALL=C LANG=C TZ=UTC
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME=agent GIT_AUTHOR_EMAIL=agent@example.invalid GIT_AUTHOR_DATE='2026-01-01T00:00:00Z'
export GIT_COMMITTER_NAME=agent GIT_COMMITTER_EMAIL=agent@example.invalid GIT_COMMITTER_DATE='2026-01-01T00:00:00Z'
export GIT_EDITOR=true GIT_MERGE_AUTOEDIT=no

readonly EMPTY_TREE=4b825dc642cb6eb9a060e54bf8d69288fbee4904
readonly EMPTY_TREE_SHA256=6ef19b41225c5369f1c104d45d8d85efa9b057b53b14b4b9b939dd74decc5321
ATTR_TREE="$EMPTY_TREE"

# The manifest's `command-env`: every declared command gets it (spec §5.1).
readonly -a COMMAND_ENV=(
    GIT_TERMINAL_PROMPT=0
    GIT_ALLOW_PROTOCOL=https:ssh:git:file
    GIT_CONFIG_COUNT=2
    GIT_CONFIG_KEY_0=core.fsmonitor GIT_CONFIG_VALUE_0=false
    GIT_CONFIG_KEY_1=core.hooksPath GIT_CONFIG_VALUE_1=/dev/null
)

# The argv of `git-status`: untracked files are listed whatever `status.showUntrackedFiles` a
# configuration sets, and submodules are compared whatever it ignores (section (e) shows why).
readonly -a GIT_STATUS=(git status --porcelain --untracked-files=normal --ignore-submodules=none)

# The argv of `git-head-reachable`: only refs that outlive the worktree count, not its own
# `refs/worktree/*` (section (f) shows why).
readonly -a HEAD_REACHABLE=(git for-each-ref --count=1 --contains HEAD refs/heads refs/tags refs/remotes)

# The proposed argv of `git-operation-state` (section (b) shows why).
readonly -a OPERATION_STATE=(git -c status.displayCommentPrefix=false -c color.status=false status --long)

# The argv of `git-index-flags` and of `git-common-dir` (the check the owner's R11 decision added).
readonly -a INDEX_FLAGS=(git ls-files -v --stage)
readonly -a COMMON_DIR=(git rev-parse --path-format=absolute --git-common-dir)

# The cleanup commands' own environment (owner's R11 decision). It follows COMMAND_ENV, so its
# GIT_CONFIG_COUNT replaces the global one and it carries every pair that count names.
readonly -a CLEANUP_ENV=(
    GIT_CONFIG_COUNT=3
    GIT_CONFIG_KEY_0=core.fsmonitor GIT_CONFIG_VALUE_0=false
    GIT_CONFIG_KEY_1=core.hooksPath GIT_CONFIG_VALUE_1=/dev/null
    GIT_CONFIG_KEY_2=core.attributesFile GIT_CONFIG_VALUE_2=/dev/null
    GIT_OPTIONAL_LOCKS=0
)

# How the core reads that output: a line beginning in column 0 with one of these prefixes names the
# operation in progress; no such line means none.
readonly -a STATE_LINES=(
    "interactive rebase in progress; onto |rebase"
    "rebase in progress; onto |rebase"
    "You are currently rebasing|rebase"
    "You are currently editing a commit while rebasing|rebase"
    "You are currently splitting a commit while rebasing|rebase"
    "You have unmerged paths.|unmerged"
    "All conflicts fixed but you are still merging.|merge"
    "You are currently cherry-picking commit |cherry-pick"
    "Cherry-pick currently in progress.|cherry-pick"
    "You are currently reverting commit |revert"
    "Revert currently in progress.|revert"
    "You are currently bisecting|bisect"
    "You are in the middle of an am session.|am"
)

# classify <file> — the operation the first matching state line names, or "none". Strict on purpose:
# no colour stripping, no trimming, so a config that changes the leading characters shows up as a miss.
classify() {
    local line entry
    while IFS= read -r line; do
        for entry in "${STATE_LINES[@]}"; do
            if [[ "$line" == "${entry%|*}"* ]]; then
                echo "${entry##*|}"
                return 0
            fi
        done
    done <"$1"
    echo none
}

# --- Helper programs: each appends to the marker file named by its first argument. ----------------
BIN="$SCRATCH/bin"
mkdir -p "$BIN"
cat >"$BIN/filter-cat" <<'EOF'
#!/bin/sh
echo ran >>"$1"
exec cat
EOF
cat >"$BIN/lfs-clean" <<'EOF'
#!/bin/sh
echo ran >>"$1"
exec sed 's/^content:/pointer:/'
EOF
cat >"$BIN/lfs-smudge" <<'EOF'
#!/bin/sh
echo ran >>"$1"
exec sed 's/^pointer:/content:/'
EOF
chmod +x "$BIN"/*

# --- Cases: each gets a fresh repository with one linked worktree. --------------------------------
CASE_COUNTER=0
CASE_DIR=""
REPO=""
WT_ROOT=""
WT=""
MARKER=""
EXTRA_ENV=()

sanitize() {
    sed -E \
        -e "s#${SCRATCH//./\\.}/c[0-9]+#<case>#g" \
        -e "s#${SCRATCH//./\\.}#<scratch>#g" \
        -e 's/\b[0-9a-f]{7,64}\b/<hash>/g'
}

commit_all() { # commit_all <dir> <message>
    git -C "$1" add -A 2>/dev/null
    git -C "$1" commit -q -m "$2"
}

# new_case [plain|crlf|lfs|sha256|inner-root] — a repository `repo` on `main` with one commit, and a
# worktree `worktrees/wt` on branch `agent/x`. `inner-root` puts the worktree root inside the repository.
new_case() {
    local variant="${1:-plain}"
    CASE_COUNTER=$((CASE_COUNTER + 1))
    CASE_DIR="$SCRATCH/c$CASE_COUNTER"
    REPO="$CASE_DIR/repo"
    WT_ROOT="$CASE_DIR/worktrees"
    [[ "$variant" == inner-root ]] && WT_ROOT="$REPO/.claude/worktrees"
    WT="$WT_ROOT/wt"
    MARKER="$CASE_DIR/marker"
    EXTRA_ENV=()
    ATTR_TREE="$EMPTY_TREE"
    mkdir -p "$CASE_DIR"
    local -a init=(git init -q -b main)
    [[ "$variant" == sha256 ]] && init+=(--object-format=sha256)
    "${init[@]}" "$REPO"
    printf 'base\n' >"$REPO/a.txt"
    printf 'b\n' >"$REPO/b.txt"
    case "$variant" in
        crlf) printf '*.txt text eol=crlf\n' >"$REPO/.gitattributes" ;;
        lfs)
            git -C "$REPO" config filter.lfs.clean "$BIN/lfs-clean $MARKER"
            git -C "$REPO" config filter.lfs.smudge "$BIN/lfs-smudge $MARKER"
            git -C "$REPO" config filter.lfs.required true
            printf '*.bin filter=lfs\n' >"$REPO/.gitattributes"
            printf 'content:data\n' >"$REPO/a.bin"
            ;;
        *) ;;
    esac
    commit_all "$REPO" base
    mkdir -p "$WT_ROOT"
    git -C "$REPO" worktree add -q -b agent/x "$WT"
    rm -f "$MARKER"
}

# Make every tracked file stat-dirty, so git must hash it again (and run a clean filter to do so).
touch_tracked() {
    git -C "$WT" ls-files -z | (cd "$WT" && xargs -0 touch -d @1000000000)
}

# The state an index reaches in normal use: files older than the index, entries refreshed by an
# ordinary `git status` without GIT_ATTR_SOURCE, so no entry is racily clean or stat-dirty.
age_index() {
    touch_tracked
    (cd "$WT" && git status --porcelain >/dev/null)
}

# run <off|on> <cwd> <argv…> — with the manifest's command-env, with or without GIT_ATTR_SOURCE.
# Sets RC, OUT and ERR (sanitized).
RC=0
OUT=""
ERR=""
run() {
    local attr="$1" cwd="$2"
    shift 2
    local -a envv=("${COMMAND_ENV[@]}")
    [[ "$attr" == on ]] && envv+=("GIT_ATTR_SOURCE=$ATTR_TREE")
    envv+=(${EXTRA_ENV[@]+"${EXTRA_ENV[@]}"})
    RC=0
    (cd "$cwd" && env "${envv[@]}" "$@") >"$SCRATCH/out" 2>"$SCRATCH/err" || RC=$?
    OUT="$(sanitize <"$SCRATCH/out")"
    ERR="$(sanitize <"$SCRATCH/err")"
}

# run_bare <cwd> <argv…> — like run, but without the command-env (what a plain git would do).
run_bare() {
    local cwd="$1"
    shift
    RC=0
    (cd "$cwd" && env ${EXTRA_ENV[@]+"${EXTRA_ENV[@]}"} "$@") >"$SCRATCH/out" 2>"$SCRATCH/err" || RC=$?
    OUT="$(sanitize <"$SCRATCH/out")"
    ERR="$(sanitize <"$SCRATCH/err")"
}

# The cleanup commands (spec §6.3), by name. `remove@wt` runs in the worktree, `remove@repo` in the
# repository.
readonly -a CLEANUP=(status reachable opstate clean remove@wt remove@repo)
# Every cleanup command the core runs, in its order, with the in-repository remove for comparison.
readonly -a ADOPTED_CLEANUP=(common-dir reachable opstate index-flags status clean remove@wt remove@repo)
cleanup() { # cleanup <name> <off|on>
    case "$1" in
        status) run "$2" "$WT" "${GIT_STATUS[@]}" ;;
        reachable) run "$2" "$WT" "${HEAD_REACHABLE[@]}" ;;
        opstate) run "$2" "$WT" "${OPERATION_STATE[@]}" ;;
        clean)
            [[ -e "$WT/STATUS.md" ]] || printf 'status\n' >"$WT/STATUS.md"
            run "$2" "$WT" git clean -f -- STATUS.md
            ;;
        index-flags) run "$2" "$WT" "${INDEX_FLAGS[@]}" ;;
        common-dir) run "$2" "$WT" "${COMMON_DIR[@]}" ;;
        remove@wt) run "$2" "$WT" git worktree remove "$WT" ;;
        remove@repo) run "$2" "$REPO" git worktree remove "$WT" ;;
        *) return 1 ;;
    esac
}

filter_ran() { if [[ -e "$MARKER" ]]; then echo yes; else echo no; fi; }

indent() { sed -e 's/^/        | /' -e 's/[[:space:]]*$//'; }

show_output() { # show_output [always] — prints OUT and ERR of the last run, indented
    [[ "${1:-}" == always && -z "$OUT" && -z "$ERR" ]] && echo "      (no output)"
    [[ -n "$OUT" ]] && printf '      stdout:\n%s\n' "$(indent <<<"$OUT")"
    [[ -n "$ERR" ]] && printf '      stderr:\n%s\n' "$(indent <<<"$ERR")"
    return 0
}

# ====================================================================================================
echo "R11 — git in an agent-controlled worktree"
echo "git: $(git --version | sed 's/^git version //')"
echo "GIT_ATTR_SOURCE=$EMPTY_TREE (the empty tree) when \"on\"; every run carries the manifest's"
echo "command-env (core.fsmonitor=false, core.hooksPath=/dev/null via GIT_CONFIG_COUNT)."
echo "Cleanup commands: status = ${GIT_STATUS[*]};"
echo "reachable = ${HEAD_REACHABLE[*]};"
echo "opstate = ${OPERATION_STATE[*]}; clean = git clean -f -- STATUS.md;"
echo "remove@wt / remove@repo = git worktree remove <abs path>, cwd the worktree / the repository."

# ----------------------------------------------------------------------------------------------------
echo
echo "== Step 1: does a cleanup command run a clean filter the agent can name? =="
echo "Each run: fresh repository + worktree, filter x defined as a program that leaves a marker, every"
echo "tracked file made stat-dirty, then the one command. Columns: exit code, filter ran."
echo

# Every case defines filter x (clean = filter-cat) somewhere and selects it somewhere.
setup_filter_case() {
    new_case
    case "$1" in
        baseline-no-filter-named)
            git -C "$REPO" config filter.x.clean "$BIN/filter-cat $MARKER"
            ;;
        worktree-gitattributes)
            printf '* filter=x\n' >"$WT/.gitattributes"
            commit_all "$WT" attrs
            git -C "$REPO" config filter.x.clean "$BIN/filter-cat $MARKER"
            ;;
        worktree-gitattributes+global-config)
            printf '* filter=x\n' >"$WT/.gitattributes"
            commit_all "$WT" attrs
            printf '[filter "x"]\n\tclean = %s %s\n' "$BIN/filter-cat" "$MARKER" >"$CASE_DIR/global"
            ;;
        common-info-attributes)
            git -C "$REPO" config filter.x.clean "$BIN/filter-cat $MARKER"
            mkdir -p "$REPO/.git/info"
            printf '* filter=x\n' >"$REPO/.git/info/attributes"
            ;;
        core.attributesFile)
            git -C "$REPO" config filter.x.clean "$BIN/filter-cat $MARKER"
            printf '* filter=x\n' >"$CASE_DIR/attributes"
            git -C "$REPO" config core.attributesFile "$CASE_DIR/attributes"
            ;;
        xdg-config-git-attributes)
            git -C "$REPO" config filter.x.clean "$BIN/filter-cat $MARKER"
            mkdir -p "$CASE_DIR/xdg/git"
            printf '* filter=x\n' >"$CASE_DIR/xdg/git/attributes"
            ;;
        commondir-redirect+info-attributes | commondir-redirect+worktree-gitattributes)
            if [[ "$1" == *worktree-gitattributes ]]; then
                printf '* filter=x\n' >"$WT/.gitattributes"
                commit_all "$WT" attrs
            fi
            cp -a "$REPO/.git" "$CASE_DIR/evil.git"
            git --git-dir="$CASE_DIR/evil.git" config filter.x.clean "$BIN/filter-cat $MARKER"
            if [[ "$1" == *info-attributes ]]; then
                mkdir -p "$CASE_DIR/evil.git/info"
                printf '* filter=x\n' >"$CASE_DIR/evil.git/info/attributes"
            fi
            printf '%s\n' "$CASE_DIR/evil.git" >"$REPO/.git/worktrees/wt/commondir"
            ;;
        *) return 1 ;;
    esac
    [[ "$1" == *global-config ]] && EXTRA_ENV=("GIT_CONFIG_GLOBAL=$CASE_DIR/global")
    [[ "$1" == xdg-* ]] && EXTRA_ENV=("XDG_CONFIG_HOME=$CASE_DIR/xdg")
    touch_tracked
    rm -f "$MARKER"
}

readonly -a FILTER_CASES=(
    baseline-no-filter-named
    worktree-gitattributes
    worktree-gitattributes+global-config
    common-info-attributes
    core.attributesFile
    xdg-config-git-attributes
    commondir-redirect+info-attributes
    commondir-redirect+worktree-gitattributes
)

printf '  %-44s %-12s %-14s %s\n' case command "attr off" "attr on"
failures=()
for fcase in "${FILTER_CASES[@]}"; do
    for cmd in "${CLEANUP[@]}"; do
        cells=()
        for attr in off on; do
            setup_filter_case "$fcase"
            cleanup "$cmd" "$attr"
            cells+=("exit=$RC ran=$(filter_ran)")
            [[ "$RC" != 0 ]] && failures+=("$fcase $cmd attr $attr: $(head -n1 <<<"$ERR")")
        done
        printf '  %-44s %-12s %-14s %s\n' "$fcase" "$cmd" "${cells[0]}" "${cells[1]}"
    done
done
echo
echo "  Non-zero exits above, first line of stderr:"
for failure in "${failures[@]}"; do echo "    $failure"; done

echo
echo "-- Forcing core.attributesFile=/dev/null as a third command-env key (attr on, git status --porcelain) --"
for fcase in core.attributesFile xdg-config-git-attributes common-info-attributes commondir-redirect+info-attributes; do
    setup_filter_case "$fcase"
    EXTRA_ENV+=(GIT_CONFIG_COUNT=3 GIT_CONFIG_KEY_2=core.attributesFile GIT_CONFIG_VALUE_2=/dev/null)
    cleanup status on
    printf '  %-44s exit=%s ran=%s\n' "$fcase" "$RC" "$(filter_ran)"
done

echo
echo "-- What git itself reports about the common directory (attr on, cwd = worktree) --"
for fcase in baseline-no-filter-named commondir-redirect+info-attributes; do
    setup_filter_case "$fcase"
    run on "$WT" git rev-parse --path-format=absolute --git-common-dir
    printf '  %-44s git rev-parse --path-format=absolute --git-common-dir -> exit=%s %s\n' "$fcase" "$RC" "$OUT"
done

echo
echo "-- LFS-style smudge filter on git worktree add (repository commits '*.bin filter=lfs') --"
for attr in off on; do
    new_case lfs
    run "$attr" "$REPO" git worktree add -q -b agent/y "$CASE_DIR/worktrees/y"
    printf '  attr %-3s exit=%s smudge ran=%s  a.bin in the new worktree: %s\n' \
        "$attr" "$RC" "$(filter_ran)" "$(cat "$CASE_DIR/worktrees/y/a.bin")"
done
echo "  gh pr checkout: not measured — needs a real GitHub pull request (owner)."

echo
echo "-- core.fsmonitor and core.hooksPath set in the repository config --"
echo "  (\"bare\" = no command-env; \"env\" = with the manifest's command-env)"
cat >"$BIN/fsmonitor-hook" <<'EOF'
#!/bin/sh
echo ran >>"$(dirname "$0")/../fsmonitor-marker"
exit 1
EOF
chmod +x "$BIN/fsmonitor-hook"
for mode in bare env; do
    new_case
    git -C "$REPO" config core.fsmonitor "$BIN/fsmonitor-hook"
    rm -f "$SCRATCH/fsmonitor-marker"
    if [[ "$mode" == bare ]]; then run_bare "$WT" git status --porcelain; else run off "$WT" git status --porcelain; fi
    ran=no
    [[ -e "$SCRATCH/fsmonitor-marker" ]] && ran=yes
    printf '  core.fsmonitor=<hook>  git status --porcelain  %-4s exit=%s hook ran=%s\n' "$mode" "$RC" "$ran"
done
readonly -a HOOK_NAMES=(
    applypatch-msg pre-applypatch post-applypatch pre-commit pre-merge-commit prepare-commit-msg
    commit-msg post-commit pre-rebase post-checkout post-merge pre-push pre-receive update
    proc-receive post-receive post-update reference-transaction push-to-checkout pre-auto-gc
    post-rewrite sendemail-validate fsmonitor-watchman p4-changelist p4-prepare-changelist
    p4-post-changelist p4-pre-submit post-index-change
)
make_hooks() { # make_hooks <dir>
    mkdir -p "$1"
    local hook
    for hook in "${HOOK_NAMES[@]}"; do
        printf '#!/bin/sh\necho %s >>"%s"\n' "$hook" "$MARKER" >"$1/$hook"
        chmod +x "$1/$hook"
    done
}
for where in core.hooksPath common-hooks-dir; do
    for cmd in "${CLEANUP[@]}"; do
        cells=()
        for mode in bare env; do
            new_case
            if [[ "$where" == core.hooksPath ]]; then
                make_hooks "$CASE_DIR/hooks"
                git -C "$REPO" config core.hooksPath "$CASE_DIR/hooks"
            else
                make_hooks "$REPO/.git/hooks"
            fi
            touch_tracked
            rm -f "$MARKER"
            if [[ "$mode" == bare ]]; then
                case "$cmd" in
                    status) run_bare "$WT" "${GIT_STATUS[@]}" ;;
                    reachable) run_bare "$WT" "${HEAD_REACHABLE[@]}" ;;
                    opstate) run_bare "$WT" "${OPERATION_STATE[@]}" ;;
                    clean) printf 's\n' >"$WT/STATUS.md" && run_bare "$WT" git clean -f -- STATUS.md ;;
                    remove@wt) run_bare "$WT" git worktree remove "$WT" ;;
                    remove@repo) run_bare "$REPO" git worktree remove "$WT" ;;
                    *) ;;
                esac
            else
                cleanup "$cmd" off
            fi
            hooks="none"
            [[ -e "$MARKER" ]] && hooks="$(sort -u "$MARKER" | paste -sd, -)"
            cells+=("exit=$RC hooks=$hooks")
        done
        printf '  hooks in %-17s %-12s bare: %-26s env: %s\n' "$where" "$cmd" "${cells[0]}" "${cells[1]}"
    done
done

echo
echo "-- The adopted cleanup environment, every cleanup command (owner's R11 decision) --"
echo "  Each cleanup command's own environment replaces the global GIT_CONFIG_COUNT with the whole set:"
echo "  core.fsmonitor=false, core.hooksPath=/dev/null, core.attributesFile=/dev/null; plus GIT_ATTR_SOURCE"
echo "  and GIT_OPTIONAL_LOCKS=0. common-dir = ${COMMON_DIR[*]}; index-flags = ${INDEX_FLAGS[*]}."
echo "  Expected to run: the main repository's own info/attributes (the documented residual), and the"
echo "  redirected common dir (closed by the common-dir check, which runs first and reads no attributes)."
for fcase in "${FILTER_CASES[@]}"; do
    for cmd in "${ADOPTED_CLEANUP[@]}"; do
        setup_filter_case "$fcase"
        EXTRA_ENV+=("${CLEANUP_ENV[@]}")
        cleanup "$cmd" on
        printf '  %-44s %-12s exit=%s ran=%s\n' "$fcase" "$cmd" "$RC" "$(filter_ran)"
    done
done
echo "  The global keys survive the override (hooks and fsmonitor in the repository config):"
for where in core.hooksPath common-hooks-dir; do
    for cmd in "${ADOPTED_CLEANUP[@]}"; do
        new_case
        if [[ "$where" == core.hooksPath ]]; then
            make_hooks "$CASE_DIR/hooks"
            git -C "$REPO" config core.hooksPath "$CASE_DIR/hooks"
        else
            make_hooks "$REPO/.git/hooks"
        fi
        git -C "$REPO" config core.fsmonitor "$BIN/fsmonitor-hook"
        touch_tracked
        rm -f "$MARKER" "$SCRATCH/fsmonitor-marker"
        EXTRA_ENV=("${CLEANUP_ENV[@]}")
        cleanup "$cmd" on
        hooks="none"
        [[ -e "$MARKER" ]] && hooks="$(sort -u "$MARKER" | paste -sd, -)"
        fsmonitor=no
        [[ -e "$SCRATCH/fsmonitor-marker" ]] && fsmonitor=yes
        printf '  hooks in %-17s %-12s exit=%s hooks=%s fsmonitor ran=%s\n' "$where" "$cmd" "$RC" "$hooks" "$fsmonitor"
    done
done

echo
echo "-- The common-dir check: git's answer against <main checkout>/.git (adopted env, cwd = worktree) --"
echo "  <main checkout> is the first entry of git worktree list --porcelain, run in the repository, as the"
echo "  core reads it; the comparison is exact, one line end allowed."
for rcase in intact commondir-absolute commondir-relative repository-via-symlink; do
    new_case
    cwd="$WT"
    case "$rcase" in
        commondir-absolute)
            cp -a "$REPO/.git" "$CASE_DIR/evil.git"
            printf '%s\n' "$CASE_DIR/evil.git" >"$REPO/.git/worktrees/wt/commondir"
            ;;
        commondir-relative)
            cp -a "$REPO/.git" "$CASE_DIR/evil.git"
            printf '../../../../evil.git\n' >"$REPO/.git/worktrees/wt/commondir"
            ;;
        repository-via-symlink)
            ln -s "$REPO" "$CASE_DIR/link"
            git -C "$CASE_DIR/link" worktree add -q -b agent/z "$WT_ROOT/via-link"
            cwd="$WT_ROOT/via-link"
            ;;
        *) ;;
    esac
    main="$(git -C "$REPO" worktree list --porcelain | sed -n '1s/^worktree //p')"
    gitfile="$(sed 's/^gitdir: //' "$cwd/.git")"
    [[ "$gitfile" == "$main/.git/worktrees/$(basename "$cwd")" ]] && intact=yes || intact=no
    EXTRA_ENV=("${CLEANUP_ENV[@]}")
    run on "$cwd" "${COMMON_DIR[@]}"
    # Exactly as the core compares: the path, optionally followed by one line end.
    raw="$(cat "$SCRATCH/out" && printf x)"
    raw="${raw%x}"
    [[ "$raw" == "$main/.git" || "$raw" == "$main/.git"$'\n' ]] && equal=yes || equal=no
    printf '  %-24s .git file intact=%-3s exit=%s output=%-22s equals <main>/.git=%s\n' \
        "$rcase" "$intact" "$RC" "$OUT" "$equal"
done

echo
echo "-- A nested repository whose own config defines filter f and whose info/attributes names it --"
echo "  (adopted env). gitlink = committed as a submodule (mode 160000); untracked = never added;"
echo "  uncached = committed, then git rm --cached. clean = just checked out; stat-dirty = its file"
echo "  touched; modified = its file changed. index = what the index check (${INDEX_FLAGS[*]}) shows."
for ncase in gitlink untracked uncached; do
    for nstate in clean stat-dirty modified; do
        cells=()
        index=""
        for cmd in "${ADOPTED_CLEANUP[@]}"; do
            new_case
            git init -q -b main "$WT/sub"
            printf 's\n' >"$WT/sub/s.txt"
            commit_all "$WT/sub" sub
            if [[ "$ncase" != untracked ]]; then
                git -C "$WT" add sub 2>/dev/null
                git -C "$WT" commit -q -m gitlink
            fi
            [[ "$ncase" == uncached ]] && git -C "$WT" rm -q --cached sub
            git -C "$WT/sub" config filter.f.clean "$BIN/filter-cat $MARKER"
            mkdir -p "$WT/sub/.git/info"
            printf '* filter=f\n' >"$WT/sub/.git/info/attributes"
            case "$nstate" in
                stat-dirty) touch -d @1000000000 "$WT/sub/s.txt" ;;
                modified) printf 't\n' >>"$WT/sub/s.txt" ;;
                *) ;;
            esac
            rm -f "$MARKER"
            EXTRA_ENV=("${CLEANUP_ENV[@]}")
            cleanup "$cmd" on
            cells+=("$cmd:exit=$RC,ran=$(filter_ran)")
            [[ "$cmd" == index-flags ]] && index="$(grep -c ' 160000 ' <<<"$OUT" || true) gitlink"
        done
        printf '  %-9s %-10s index=%s\n' "$ncase" "$nstate" "$index"
        printf '      %s\n' "${cells[@]}"
    done
done

# ----------------------------------------------------------------------------------------------------
echo
echo "== Step 2 (a): is every cleanup command's output unchanged by GIT_ATTR_SOURCE? =="
echo "Each run: fresh repository + worktree in the named state; exit code, stdout and stderr compared."
echo

# setup_state <state>
setup_state() {
    case "$1" in
        clean) new_case ;;
        clean-stat-dirty) new_case && touch_tracked ;;
        dirty)
            new_case
            printf 'changed\n' >"$WT/a.txt"
            printf 'staged\n' >"$WT/b.txt"
            git -C "$WT" add b.txt
            printf 'new\n' >"$WT/c.txt"
            ;;
        untracked-STATUS.md-only) new_case && printf 'status\n' >"$WT/STATUS.md" ;;
        ignored-only)
            new_case
            printf '*.o\n' >"$WT/.gitignore"
            commit_all "$WT" ignore
            printf 'obj\n' >"$WT/x.o"
            ;;
        committed-eol=crlf) new_case crlf ;;
        committed-eol=crlf-stat-dirty) new_case crlf && touch_tracked ;;
        committed-lfs-style-filter) new_case lfs ;;
        committed-lfs-style-filter-stat-dirty) new_case lfs && touch_tracked ;;
        sha256-repository) new_case sha256 ;;
        sha256-repository-stat-dirty) new_case sha256 && touch_tracked ;;
        sha256-repository-sha256-empty-tree) new_case sha256 && touch_tracked && ATTR_TREE="$EMPTY_TREE_SHA256" ;;
        clean-aged) new_case && age_index ;;
        committed-eol=crlf-aged) new_case crlf && age_index ;;
        committed-lfs-style-filter-aged) new_case lfs && age_index ;;
        sha256-repository-aged) new_case sha256 && age_index ;;
        *) return 1 ;;
    esac
    rm -f "$MARKER"
}

readonly -a STATES=(
    clean clean-stat-dirty dirty untracked-STATUS.md-only ignored-only
    committed-eol=crlf committed-eol=crlf-stat-dirty
    committed-lfs-style-filter committed-lfs-style-filter-stat-dirty
    sha256-repository sha256-repository-stat-dirty sha256-repository-sha256-empty-tree
    clean-aged committed-eol=crlf-aged committed-lfs-style-filter-aged sha256-repository-aged
)

for state in "${STATES[@]}"; do
    echo "  -- $state"
    for cmd in "${CLEANUP[@]}"; do
        setup_state "$state"
        cleanup "$cmd" off
        rc_off=$RC out_off=$OUT err_off=$ERR ran_off=$(filter_ran)
        setup_state "$state"
        cleanup "$cmd" on
        rc_on=$RC ran_on=$(filter_ran)
        same=no
        [[ "$rc_off" == "$RC" && "$out_off" == "$OUT" && "$err_off" == "$ERR" ]] && same=yes
        printf '    %-12s identical=%-3s exit off=%s on=%s  filter ran off=%s on=%s\n' \
            "$cmd" "$same" "$rc_off" "$rc_on" "$ran_off" "$ran_on"
        if [[ "$same" == no ]]; then
            echo "      off:"
            OUT=$out_off ERR=$err_off show_output always
            setup_state "$state"
            cleanup "$cmd" on
            echo "      on:"
            show_output always
        elif [[ "$cmd" == status || "$cmd" == remove@wt ]]; then
            show_output
        fi
    done
done

# ----------------------------------------------------------------------------------------------------
echo
echo "== Step 2 (b): which single invocation tells an operation in progress from none? =="
echo "The worktree's branch agent/x has commits w1, w2 changing a.txt; main has m1 changing a.txt"
echo "(conflicts with both) and side changes s.txt only (conflicts with nothing)."
echo

setup_history() {
    new_case
    printf 'side\n' >"$REPO/s.txt"
    git -C "$REPO" checkout -q -b side
    commit_all "$REPO" s1
    git -C "$REPO" checkout -q main
    printf 'main\n' >"$REPO/a.txt"
    commit_all "$REPO" m1
    printf 'w1\n' >"$WT/a.txt"
    commit_all "$WT" w1
    printf 'w2\n' >"$WT/a.txt"
    commit_all "$WT" w2
}

# Leave the worktree in the named operation state. Commands that stop with a conflict exit non-zero.
enter_state() {
    local -a g=(git -C "$WT")
    case "$1" in
        none) ;;
        rebase-conflict) "${g[@]}" rebase main >/dev/null 2>&1 || true ;;
        rebase-apply-conflict) "${g[@]}" rebase --apply main >/dev/null 2>&1 || true ;;
        rebase-i-break)
            GIT_SEQUENCE_EDITOR="sed -i 1ibreak" "${g[@]}" rebase -i side >/dev/null 2>&1 || true
            ;;
        merge-conflict) "${g[@]}" merge main >/dev/null 2>&1 || true ;;
        merge-no-commit) "${g[@]}" merge --no-commit --no-ff side >/dev/null 2>&1 || true ;;
        cherry-pick-conflict) "${g[@]}" cherry-pick main >/dev/null 2>&1 || true ;;
        cherry-pick-sequence-resolved)
            "${g[@]}" cherry-pick main side >/dev/null 2>&1 || true
            "${g[@]}" checkout -q --theirs a.txt
            "${g[@]}" add a.txt
            "${g[@]}" commit -q --no-edit >/dev/null 2>&1
            ;;
        revert-conflict) "${g[@]}" revert --no-edit HEAD~1 >/dev/null 2>&1 || true ;;
        bisect-started) "${g[@]}" bisect start >/dev/null 2>&1 ;;
        bisect-running) "${g[@]}" bisect start HEAD main~1 >/dev/null 2>&1 ;;
        stash-pop-conflict)
            printf 'stashed\n' >"$WT/a.txt"
            "${g[@]}" stash -q
            printf 'w3\n' >"$WT/a.txt"
            commit_all "$WT" w3
            "${g[@]}" stash pop >/dev/null 2>&1 || true
            ;;
        checkout-m-conflict)
            git -C "$REPO" branch other main
            printf 'local\n' >"$WT/a.txt"
            "${g[@]}" checkout -m other >/dev/null 2>&1 || true
            ;;
        am-conflict)
            git -C "$REPO" format-patch -q -1 main -o "$CASE_DIR/patches"
            "${g[@]}" am "$CASE_DIR"/patches/*.patch >/dev/null 2>&1 || true
            ;;
        *) return 1 ;;
    esac
}

readonly -a OP_STATES=(
    none rebase-conflict rebase-apply-conflict rebase-i-break merge-conflict merge-no-commit
    cherry-pick-conflict cherry-pick-sequence-resolved revert-conflict bisect-started bisect-running
    am-conflict stash-pop-conflict checkout-m-conflict
)
readonly -a CANDIDATES=(
    "git -c status.displayCommentPrefix=false -c color.status=false status --long"
    "git status --long"
    "git status"
    "git status --porcelain"
    "git status --porcelain=v2 --branch"
    "git for-each-ref --include-root-refs --format=%(refname)"
    "git branch --list"
    "git rev-parse --verify --quiet REBASE_HEAD"
    "git rev-parse --verify --quiet MERGE_HEAD"
    "git rev-parse --verify --quiet CHERRY_PICK_HEAD"
    "git rev-parse --verify --quiet REVERT_HEAD"
    "git bisect log"
    "git rebase --show-current-patch"
    "git rev-parse --git-path rebase-merge"
)

for state in "${OP_STATES[@]}"; do
    echo "  -- $state"
    for candidate in "${CANDIDATES[@]}"; do
        read -r -a argv <<<"$candidate"
        setup_history
        enter_state "$state"
        run off "$WT" "${argv[@]}"
        printf '    %-78s exit=%s\n' "$candidate" "$RC"
        case "$candidate" in
            "git -c status.displayCommentPrefix=false -c color.status=false status --long")
                echo "      classify: $(classify "$SCRATCH/out")"
                show_output
                ;;
            "git status --porcelain=v2 --branch" | "git for-each-ref"* | "git branch --list" | "git rev-parse --git-path"*)
                show_output
                ;;
            "git status --long" | "git status")
                # Only whether it prints what the proposed argv prints (no colour is configured here).
                plain="$OUT"
                setup_history
                enter_state "$state"
                run off "$WT" "${OPERATION_STATE[@]}"
                [[ "$plain" == "$OUT" ]] && echo "      (stdout identical to ${OPERATION_STATE[*]})"
                ;;
            *) ;;
        esac
    done
    setup_history
    enter_state "$state"
    run off "$WT" "${OPERATION_STATE[@]}"
    rc_off=$RC out_off=$OUT
    setup_history
    enter_state "$state"
    run on "$WT" "${OPERATION_STATE[@]}"
    same=no
    [[ "$rc_off" == "$RC" && "$out_off" == "$OUT" ]] && same=yes
    echo "    ${OPERATION_STATE[*]}: identical with GIT_ATTR_SOURCE=$same"
done

echo
echo "-- Does a configuration change how the state lines are read? --"
echo "Each configuration is set in the user's global config file. Per state, the operation classify()"
echo "reads from plain \`git status\` and from the proposed argv (strict column-0 prefixes, no colour"
echo "stripping); ESC = whether stdout contains an escape sequence in any state."
readonly -a ROBUST_STATES=(
    none rebase-conflict rebase-i-break merge-conflict merge-no-commit cherry-pick-sequence-resolved
    revert-conflict bisect-running am-conflict
)
readonly -a ROBUST_CONFIGS=(
    "(none)"
    "status.displayCommentPrefix=true"
    "status.displayCommentPrefix=true core.commentChar=;"
    "color.status=always"
    "color.ui=always"
    "status.short=true"
    "status.branch=true"
    "status.relativePaths=false"
    "status.showUntrackedFiles=all"
    "status.aheadBehind=false"
    "status.showStash=true"
    "status.submoduleSummary=true"
    "status.renames=false"
    "advice.statusHints=false"
    "advice.statusUoption=false"
    "advice.statusAheadBehind=false"
    "status.displayCommentPrefix=true core.commentChar=; color.ui=always color.status=always status.short=true status.branch=true status.relativePaths=false status.showUntrackedFiles=all status.showStash=true advice.statusHints=false"
)
echo "  states: ${ROBUST_STATES[*]}"
expected=""
for config in "${ROBUST_CONFIGS[@]}"; do
    plain=()
    proposed=()
    esc_plain=no
    esc_proposed=no
    for state in "${ROBUST_STATES[@]}"; do
        setup_history
        enter_state "$state"
        : >"$CASE_DIR/global"
        if [[ "$config" != "(none)" ]]; then
            read -r -a pairs <<<"$config"
            for pair in "${pairs[@]}"; do
                git config --file "$CASE_DIR/global" "${pair%%=*}" "${pair#*=}"
            done
        fi
        EXTRA_ENV=("GIT_CONFIG_GLOBAL=$CASE_DIR/global")
        run off "$WT" git status
        plain+=("$(classify "$SCRATCH/out")")
        grep -q $'\e' "$SCRATCH/out" && esc_plain=yes
        run off "$WT" "${OPERATION_STATE[@]}"
        proposed+=("$(classify "$SCRATCH/out")")
        grep -q $'\e' "$SCRATCH/out" && esc_proposed=yes
        EXTRA_ENV=()
    done
    [[ -z "$expected" ]] && expected="${proposed[*]}"
    verdict_plain=MISREAD
    [[ "${plain[*]}" == "$expected" ]] && verdict_plain=correct
    verdict_proposed=MISREAD
    [[ "${proposed[*]}" == "$expected" ]] && verdict_proposed=correct
    echo "  config: $config"
    printf '    %-9s %-8s ESC=%-3s %s\n' "git status" "$verdict_plain" "$esc_plain" "${plain[*]}"
    printf '    %-9s %-8s ESC=%-3s %s\n' "proposed" "$verdict_proposed" "$esc_proposed" "${proposed[*]}"
done

echo
echo "-- The proposed argv under a translated locale (rebase-conflict, line 2) --"
setup_history
enter_state rebase-conflict
EXTRA_ENV=(LC_ALL=de_DE.UTF-8 LANGUAGE=de)
run off "$WT" "${OPERATION_STATE[@]}"
printf '  LC_ALL=de_DE.UTF-8 LANGUAGE=de           exit=%s classify=%s line 2: %s\n' "$RC" "$(classify "$SCRATCH/out")" "$(sed -n 2p <<<"$OUT")"
EXTRA_ENV=(LANG=de_DE.UTF-8 LANGUAGE=de LC_ALL=C)
run off "$WT" "${OPERATION_STATE[@]}"
printf '  LANG=de_DE.UTF-8 LANGUAGE=de LC_ALL=C    exit=%s classify=%s line 2: %s\n' "$RC" "$(classify "$SCRATCH/out")" "$(sed -n 2p <<<"$OUT")"
EXTRA_ENV=()

# ----------------------------------------------------------------------------------------------------
echo
echo "== Step 2 (c): where must git worktree remove run? =="
echo

remove_case() { # remove_case <label> <cwd> <path>
    run off "$2" git worktree remove "$3"
    local gone=no branch=no
    [[ -e "$WT" ]] || gone=yes
    git -C "$REPO" rev-parse --verify --quiet refs/heads/agent/x >/dev/null && branch=yes
    printf '  %-58s exit=%s worktree gone=%s branch kept=%s\n' "$1" "$RC" "$gone" "$branch"
    show_output
}

new_case
remove_case "cwd=worktree, path=absolute" "$WT" "$WT"
new_case
remove_case "cwd=worktree, path=." "$WT" .
new_case
remove_case "cwd=repository, path=absolute" "$REPO" "$WT"
new_case
remove_case "cwd=repository, path=relative (../worktrees/wt)" "$REPO" ../worktrees/wt
new_case
remove_case "cwd=worktree root outside the repository, path=absolute" "$WT_ROOT" "$WT"
new_case inner-root
remove_case "cwd=worktree root inside the repository, path=absolute" "$WT_ROOT" "$WT"
new_case
git -C "$REPO" worktree add -q -b agent/sibling "$WT_ROOT/sibling"
remove_case "cwd=a sibling worktree, path=absolute" "$WT_ROOT/sibling" "$WT"
new_case
printf 'changed\n' >"$WT/a.txt"
remove_case "dirty (modified a.txt), cwd=worktree" "$WT" "$WT"
new_case
printf 'new\n' >"$WT/c.txt"
remove_case "untracked c.txt only, cwd=worktree" "$WT" "$WT"
new_case
printf 'status\n' >"$WT/STATUS.md"
remove_case "untracked STATUS.md only, cwd=worktree" "$WT" "$WT"
new_case
printf 'status\n' >"$WT/STATUS.md"
run off "$WT" git clean -f -- STATUS.md
printf '  git clean -f -- STATUS.md exit=%s stdout: %s\n' "$RC" "$OUT"
remove_case "after git clean -f -- STATUS.md, cwd=worktree" "$WT" "$WT"
new_case
printf '*.o\n' >"$WT/.gitignore"
commit_all "$WT" ignore
printf 'obj\n' >"$WT/x.o"
remove_case "ignored x.o only, cwd=worktree" "$WT" "$WT"

# ----------------------------------------------------------------------------------------------------
echo
echo "== Step 2 (d): a commit on a detached HEAD =="
echo

new_case
printf 'onbranch\n' >"$WT/a.txt"
commit_all "$WT" on-branch
run off "$WT" git for-each-ref --contains HEAD
printf '  commit on agent/x:     for-each-ref --contains HEAD exit=%s lines=%s\n' "$RC" "$(grep -c . <<<"$OUT" || true)"
show_output
git -C "$WT" checkout -q --detach
printf 'detached\n' >"$WT/a.txt"
commit_all "$WT" detached
detached_commit="$(git -C "$WT" rev-parse HEAD)"
run off "$WT" git for-each-ref --contains HEAD
printf '  commit on detached HEAD: for-each-ref --contains HEAD exit=%s lines=%s\n' "$RC" "$(grep -c . <<<"$OUT" || true)"
run off "$WT" git status --porcelain
printf '  commit on detached HEAD: status --porcelain exit=%s lines=%s\n' "$RC" "$(grep -c . <<<"$OUT" || true)"
run off "$WT" git worktree remove "$WT"
printf '  commit on detached HEAD: worktree remove exit=%s\n' "$RC"
run off "$REPO" git for-each-ref --contains "$detached_commit"
printf '  afterwards, refs containing the detached commit: %s\n' "$(grep -c . <<<"$OUT" || true)"
run off "$REPO" git for-each-ref --contains "$detached_commit" refs/heads refs/tags refs/remotes
printf '  afterwards, refs/heads, refs/tags or refs/remotes containing it: %s\n' "$(grep -c . <<<"$OUT" || true)"

# ----------------------------------------------------------------------------------------------------
echo
echo "== Step 2 (e): untracked work under status.showUntrackedFiles=no =="
echo "An agent can write the shared repository config (.git/config) from its worktree; the user's global"
echo "config can carry the same key. The unforced remove reads the same configuration."
echo

untracked_case() { # untracked_case <repo-config|global-config>
    new_case
    if [[ "$1" == repo-config ]]; then
        git -C "$WT" config status.showUntrackedFiles no
    else
        printf '[status]\n\tshowUntrackedFiles = no\n' >"$CASE_DIR/global"
        EXTRA_ENV=("GIT_CONFIG_GLOBAL=$CASE_DIR/global")
    fi
    printf 'work\n' >"$WT/c.txt"
    mkdir -p "$WT/new"
    printf 'more\n' >"$WT/new/d.txt"
    echo "  -- $1: untracked c.txt and new/d.txt"
    run on "$WT" git status --porcelain
    printf '    %-74s exit=%s lines=%s\n' "git status --porcelain" "$RC" "$(grep -c . <<<"$OUT" || true)"
    show_output
    run on "$WT" "${GIT_STATUS[@]}"
    printf '    %-74s exit=%s lines=%s\n' "${GIT_STATUS[*]}" "$RC" "$(grep -c . <<<"$OUT" || true)"
    show_output
    run on "$WT" git worktree remove "$WT"
    local gone=no kept=no
    [[ -e "$WT" ]] || gone=yes
    [[ -e "$WT/c.txt" ]] && kept=yes
    printf '    %-74s exit=%s worktree gone=%s c.txt kept=%s\n' "git worktree remove <abs path>" "$RC" "$gone" "$kept"
    show_output
}
untracked_case repo-config
untracked_case global-config

# ----------------------------------------------------------------------------------------------------
echo
echo "== Step 2 (f): a commit reachable only from the worktree's own refs/worktree/* =="
echo

new_case
printf 'onbranch\n' >"$WT/a.txt"
commit_all "$WT" on-branch
run off "$WT" "${HEAD_REACHABLE[@]}"
printf '  commit on agent/x:        %-66s exit=%s lines=%s\n' "${HEAD_REACHABLE[*]}" "$RC" "$(grep -c . <<<"$OUT" || true)"
show_output
git -C "$WT" checkout -q --detach
printf 'detached\n' >"$WT/a.txt"
commit_all "$WT" detached
run off "$WT" "${HEAD_REACHABLE[@]}"
printf '  commit on detached HEAD:  %-66s exit=%s lines=%s\n' "${HEAD_REACHABLE[*]}" "$RC" "$(grep -c . <<<"$OUT" || true)"
git -C "$WT" update-ref refs/worktree/x HEAD
worktree_commit="$(git -C "$WT" rev-parse HEAD)"
echo "  after git update-ref refs/worktree/x HEAD:"
run off "$WT" git for-each-ref --contains HEAD
printf '    %-74s exit=%s lines=%s\n' "git for-each-ref --contains HEAD" "$RC" "$(grep -c . <<<"$OUT" || true)"
show_output
run off "$WT" "${HEAD_REACHABLE[@]}"
printf '    %-74s exit=%s lines=%s\n' "${HEAD_REACHABLE[*]}" "$RC" "$(grep -c . <<<"$OUT" || true)"
show_output
run off "$WT" git status --porcelain
printf '    %-74s exit=%s lines=%s\n' "git status --porcelain" "$RC" "$(grep -c . <<<"$OUT" || true)"
run off "$REPO" git rev-parse --verify --quiet worktrees/wt/refs/worktree/x
printf '    from the repository, worktrees/wt/refs/worktree/x resolves: exit=%s\n' "$RC"
run off "$WT" git worktree remove "$WT"
gone=no
[[ -e "$WT" ]] || gone=yes
printf '    %-74s exit=%s worktree gone=%s\n' "git worktree remove <abs path>" "$RC" "$gone"
run off "$REPO" git rev-parse --verify --quiet worktrees/wt/refs/worktree/x
printf '    afterwards, worktrees/wt/refs/worktree/x resolves: exit=%s\n' "$RC"
run off "$REPO" git for-each-ref --contains "$worktree_commit"
printf '    afterwards, refs containing the commit: %s\n' "$(grep -c . <<<"$OUT" || true)"

# ----------------------------------------------------------------------------------------------------
echo
echo "== Step 3: what git worktree list --porcelain -z prints =="
echo
echo "A repository with a worktree of each kind cleanup and the orphan list meet: on an agent branch, detached,"
echo "locked with a reason holding a space and a line end, prunable (its directory deleted), outside the"
echo "worktree root with a space in its path, and two whose paths hold a line end, one of them forging a"
echo "\`locked\` line. Fields are shown one per line, escaped as bash's printf %q does; <end> is the empty"
echo "field that closes a record. The /home/u prefix stands for the scratch directory, <hash> for HEAD."
echo

LIST_HOME="$SCRATCH/wl"
LIST_REPO="$LIST_HOME/projects/contour"
LIST_ROOT="$LIST_HOME/worktrees/contour"
mkdir -p "$LIST_REPO" "$LIST_ROOT" "$LIST_HOME/my work"
git init -q -b master "$LIST_REPO"
printf 'base\n' >"$LIST_REPO/a.txt"
commit_all "$LIST_REPO" base
git -C "$LIST_REPO" worktree add -q -b agent/pr-2334-20261113-150847 "$LIST_ROOT/pr-2334-20261113-150847"
git -C "$LIST_REPO" worktree add -q --detach "$LIST_ROOT/detached-20261113-150847"
git -C "$LIST_REPO" worktree add -q -b agent/issue-12-20261113-150847 "$LIST_ROOT/issue-12-20261113-150847"
git -C "$LIST_REPO" worktree lock --reason $'busy now\nreally' "$LIST_ROOT/issue-12-20261113-150847"
git -C "$LIST_REPO" worktree add -q -b agent/issue-17-20261113-150847 "$LIST_ROOT/issue-17-20261113-150847"
rm -rf "$LIST_ROOT/issue-17-20261113-150847"
git -C "$LIST_REPO" worktree add -q -b feature/x "$LIST_HOME/my work/contour feature"
git -C "$LIST_REPO" worktree add -q -b agent/new-line "$LIST_ROOT/new"$'\n'"line"
git -C "$LIST_REPO" worktree add -q -b agent/forged "$LIST_ROOT/forged"$'\n'"locked"

LIST_BARE="$LIST_HOME/projects/contour.git"
git clone -q --bare "$LIST_REPO" "$LIST_BARE"
git -C "$LIST_BARE" worktree add -q -b agent/pr-7-20261113-150847 "$LIST_ROOT/pr-7-20261113-150847"

# rehome — the scratch prefix becomes /home/u, byte for byte; nothing else changes.
rehome() { LC_ALL=C sed -z "s#${LIST_HOME//./\\.}#/home/u#g"; }

# hide_heads — HEAD's object id, the one value that is not fixed here, becomes <hash>.
hide_heads() { sed -E 's/^(HEAD\\? )[0-9a-f]{40,64}$/\1<hash>/'; }

# show_fields <file> — one field per line, escaped; an empty field closes a record.
show_fields() {
    local field
    while IFS= read -r -d '' field; do
        if [[ -z "$field" ]]; then
            echo "        | <end>"
        else
            printf '%q\n' "$field" | hide_heads | sed -e 's/^/        | /'
        fi
    done <"$1"
}

list_capture() { # list_capture <label> <repo> <fixture name>
    local raw="$SCRATCH/$3"
    git -C "$2" worktree list --porcelain -z | rehome >"$raw"
    printf '  %s: git worktree list --porcelain -z, %s bytes, last byte NUL: %s\n' \
        "$1" "$(wc -c <"$raw")" "$([[ "$(tail -c 1 "$raw" | od -An -tx1 | tr -d ' ')" == 00 ]] && echo yes || echo no)"
    show_fields "$raw"
    [[ -n "$FIXTURE_DIR" ]] && cp "$raw" "$FIXTURE_DIR/$3"
    return 0
}

list_capture "checkouts" "$LIST_REPO" worktree-list.porcelain-z
echo
list_capture "bare" "$LIST_BARE" worktree-list-bare.porcelain-z
echo
echo "  the same repository without -z: a line end in a path splits its line; the lock reason is quoted"
git -C "$LIST_REPO" worktree list --porcelain | rehome | hide_heads | indent
if [[ -n "$FIXTURE_DIR" ]]; then
    git --version >"$FIXTURE_DIR/VERSION"
fi

