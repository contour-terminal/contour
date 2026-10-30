#! /bin/sh
# `contour connect`, headless half, for the contour_e2e_connect test: the target grammar's refusals and
# `--list [--json]`, which print and end without opening a window. $1 is the contour binary.
#
# The picker window itself and what choosing a row starts are covered by contour_gui_test ("[connect]").

CONTOUR="$1"
[ -x "$CONTOUR" ] || { echo "usage: $0 /path/to/contour" >&2; exit 64; }

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

fail() {
    echo "error: $1" >&2
    [ -s "$2" ] && cat "$2" >&2
    exit 1
}

# The developer's own configuration and daemon stay out of it.
XDG_CONFIG_HOME="$OUT/config"
XDG_STATE_HOME="$OUT/state"
XDG_RUNTIME_DIR="$OUT/run"
export XDG_CONFIG_HOME XDG_STATE_HOME XDG_RUNTIME_DIR
mkdir -p "$XDG_CONFIG_HOME/contour" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
cat > "$XDG_CONFIG_HOME/contour/contour.yml" <<YAML
hosts:
  starfive:
    url: ssh://me@starfive
YAML

# --- --list --json: an array holding every kind of target ------------------------------------------
timeout 60 "$CONTOUR" connect --list --json > "$OUT/list.json" 2> "$OUT/list.err" \
    || fail "connect --list --json failed" "$OUT/list.err"
for needle in '"target":"daemon"' '"target":"local"' '"target":"starfive"' '"kind":"saved-host"' '"tag":"SSH"'; do
    grep -q "$needle" "$OUT/list.json" || fail "the JSON list lacks $needle" "$OUT/list.json"
done
case "$(cat "$OUT/list.json")" in
    "["*"]") ;;
    *) fail "the JSON list is not an array" "$OUT/list.json" ;;
esac

# --- --list: one target per line --------------------------------------------------------------------
timeout 60 "$CONTOUR" connect --list > "$OUT/list.txt" 2> "$OUT/list.err" \
    || fail "connect --list failed" "$OUT/list.err"
grep -q '^daemon	' "$OUT/list.txt" || fail "the list lacks the daemon" "$OUT/list.txt"
grep -q '^starfive	ssh://me@starfive$' "$OUT/list.txt" || fail "the list lacks the saved host" "$OUT/list.txt"

# --- refusals end before any window -----------------------------------------------------------------
if timeout 60 "$CONTOUR" connect nowhere > "$OUT/bad.log" 2>&1; then
    fail "an unknown target unexpectedly succeeded" "$OUT/bad.log"
fi
grep -q "neither daemon, local, a saved host" "$OUT/bad.log" || fail "an unknown target is not explained" "$OUT/bad.log"

if timeout 60 "$CONTOUR" connect --json > "$OUT/bad.log" 2>&1; then
    fail "--json without --list unexpectedly succeeded" "$OUT/bad.log"
fi

if timeout 60 "$CONTOUR" connect ssh://-oProxyCommand=x > "$OUT/bad.log" 2>&1; then
    fail "an option-like address unexpectedly succeeded" "$OUT/bad.log"
fi

echo "connect: ok"
