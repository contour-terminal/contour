#! /bin/bash
# Runs `apt update` with retries and exponential backoff so a transiently-unreachable mirror does
# not flake the whole CI job. `apt update` exits non-zero (typically 100) if ANY configured
# repository fails to fetch, even when the packages the build actually needs are available; on
# GitHub's shared runners that happens intermittently, and an outage can outlast a couple of quick
# retries. Backing off (5s, 10s, 20s, 40s) rides out a longer hiccup.
#
# If the update still cannot complete after all attempts, WARN but exit successfully rather than
# failing the job: GitHub's runner images ship with pre-populated apt lists, so the subsequent
# `apt install` normally still resolves from cache. If a genuinely-needed package cannot be found,
# that install step fails with a precise, real error — a far more actionable failure than a blanket
# "apt update failed" caused by one unrelated, momentarily-unreachable mirror.
#
# Tunable via APT_UPDATE_ATTEMPTS (default 5) and APT_UPDATE_RETRY_DELAY (default 5s, the first
# backoff; each subsequent wait doubles).
#
# Usage: sudo ./scripts/apt-update.sh   (needs root to write /var/lib/apt/lists)
set -uo pipefail

# The runner image carries a Microsoft package source (packages.microsoft.com) that nothing here installs from. When
# that CDN answers 403, `apt update` refuses the whole run ("no longer signed"), however many times it is retried,
# and so it failed the GCC and Qt install steps of a job. Dropping that source, in either spelling (.list or deb822 .sources), is enough; a runner without it is
# unaffected.
rm -f /etc/apt/sources.list.d/microsoft-prod.list /etc/apt/sources.list.d/microsoft-prod.sources

attempts="${APT_UPDATE_ATTEMPTS:-5}"
delay="${APT_UPDATE_RETRY_DELAY:-5}"

# GitHub's runner images point apt at a mirror list (mirror+file:/etc/apt/apt-mirrors.txt) that names Azure's
# Ubuntu mirror alone. When that host stops resolving, as it did for hours on 2026-11-11, every retry fails the
# same way, so after the first failure the sources are pointed at Ubuntu's own archive instead.
mirrorList='mirror+file:/etc/apt/apt-mirrors.txt'
useCanonicalArchive() {
    local archive
    case "$(dpkg --print-architecture)" in
        amd64 | i386) archive='http://archive.ubuntu.com/ubuntu/' ;;
        *) archive='http://ports.ubuntu.com/ubuntu-ports/' ;;
    esac
    local -a sources
    mapfile -t sources < <(grep -lsF "$mirrorList" /etc/apt/sources.list /etc/apt/sources.list.d/*.list \
                                                   /etc/apt/sources.list.d/*.sources)
    [ "${#sources[@]}" -gt 0 ] || return 1
    echo "::warning::apt's mirror list failed; using ${archive} instead"
    sed -i "s#${mirrorList}#${archive}#g" "${sources[@]}"
}

for i in $(seq 1 "$attempts"); do
    # A host that does not resolve is only a warning to apt, which then exits 0 on stale lists; --error-on=any
    # makes it a failure, so it is retried and, if it is the mirror list's, routed around.
    if apt-get -q update --error-on=any; then
        exit 0
    fi
    if [ "$i" -eq 1 ] && useCanonicalArchive; then
        continue
    fi
    if [ "$i" -lt "$attempts" ]; then
        echo "::warning::apt update failed (attempt ${i}/${attempts}), retrying in ${delay}s…"
        sleep "$delay"
        delay=$((delay * 2))
    fi
done

echo "::warning::apt update did not complete after ${attempts} attempts (likely a transient mirror" \
     "outage); continuing with the existing package lists. A later 'apt install' will fail loudly if" \
     "a required package is genuinely unavailable."
exit 0
