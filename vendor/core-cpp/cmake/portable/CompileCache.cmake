# SPDX-License-Identifier: Apache-2.0
#
# Compiler-cache launcher selection.
#
# Three launchers are supported, in preference order:
#
#   1. fastcache-cc — the launcher from the fastcached project. Its entries are
#      portable across checkout paths, so a CI runner and a developer working
#      from different directories share cache hits. It must already be installed
#      and on PATH. It is configured purely through the environment and caches
#      nothing unless FASTCACHE_ADDR / FASTCACHE_SOURCE_DIR / FASTCACHE_BINARY_DIR
#      are all set; the address defaults to fastcached's own port,
#      127.0.0.1:6674, and the two roots are injected here via `cmake -E env`,
#      because CMake already knows them. Selecting it is conditional on a daemon
#      actually answering there — see the probe below.
#   2. sccache — the usual third-party launcher, supporting shared (Redis/S3/...)
#      caches. **Never selected automatically**: it is opt-in behind
#      `-DALLOW_SCCACHE_FALLBACK=ON`, because being the silent answer to
#      fastcache-cc being unusable is exactly how a project stops noticing that
#      its own cache is not in use (#815). Opted into, it keeps its rank above
#      ccache — "explicitly requested" means requested. **Under MSVC or clang-cl,
#      selecting it emits a warning**, and the warning is the point: there sccache
#      replays a hit's /showIncludes stream with the absolute paths of the build
#      that stored it, so two checkouts sharing one cache stop rebuilding on a
#      header change while the build stays green. GCC and Clang are unaffected. It
#      is a caveat rather than a `check` that skips the row, because nothing here
#      can tell whether the cache is shared and the choice belongs to whoever is
#      building. See the row below.
#   3. ccache — the classic local cache, used when fastcache-cc is unavailable or
#      unconfigured and sccache has not been asked for.
#
# Launchers are wired in as compiler launchers, so CPM-/FetchContent-fetched
# dependencies get cached too. If a launcher is already set (e.g. via the
# command line or a preset), it is left untouched.
#
# This file is self-contained on purpose — it uses nothing but stock CMake and
# names no target of the project it sits in — so it can be dropped into another
# repository verbatim. Two things follow from that and must be preserved: it
# cannot use CPM or FetchContent (a consuming project may have neither, and here
# it is included before CPM is even bootstrapped), and it must never fail a
# configure, since a build that has no cache should still be a build.
#
# When none of the three is installed, -DFASTCACHE_AUTO_INSTALL=ON fetches
# fastcache-cc from GitHub Releases rather than leaving the build uncached; see
# the block below. That alone still leaves a genuinely clean machine uncached,
# because a launcher with no daemon to talk to caches nothing —
# -DFASTCACHE_AUTO_START=ON additionally stages and starts a fastcached daemon
# in the background when none answers at FASTCACHE_ADDR (issue #90); off by
# default, and independently of auto-install, since starting a background
# process is a bigger side effect than downloading a file and CI relies on no
# daemon answering by default. To disable everything: -DUSE_COMPILER_CACHE=OFF.

# --- the debug-path mapping, as a function so it can be tested ---------------
#
# Which `-fdebug-prefix-map` rules a given pair of roots deserves is a pure
# computation over two strings, and it is the half of #203 that got the layout
# wrong twice before it was run (see the block near the end of this file for
# what each rule is for). Testing it through a real configure costs two
# ~40-second configures and a compiler; testing it here costs nothing and covers
# the layouts nobody has locally.
#
# @param binaryDir        The build tree.
# @param sourceDir        The source tree.
# @param outRules         Set to the `<from>=<to>` rules, build tree first.
# @param outSourceMapped  Set to ON/OFF: was the source root mapped at all.
function(_fc_debug_prefix_map_rules binaryDir sourceDir outRules outSourceMapped)
    # A root with a SPACE in it is not mapped at all, and that is a refusal rather
    # than a nicety. These rules are spliced into `CMAKE_<LANG>_FLAGS`, which is a
    # space-separated string, so a rule naming `/Users/john doe/proj` reaches the
    # driver as two arguments and every compile dies with
    # `invalid argument '/Users/john' to '-fdebug-prefix-map'` -- measured. This
    # file may never break a build, and `check_compiler_flag` cannot catch it
    # because it probes a synthetic `/a=/b`. Quoting was the alternative and it is
    # generator-dependent; declining is not.
    if(binaryDir MATCHES " " OR sourceDir MATCHES " ")
        set(${outRules} "" PARENT_SCOPE)
        set(${outSourceMapped} OFF PARENT_SCOPE)
        return()
    endif()

    file(RELATIVE_PATH back "${binaryDir}" "${sourceDir}")
    # `file(RELATIVE_PATH)` answers with a TRAILING SEPARATOR (`../../../`), and
    # leaving it on is not cosmetic: the compiler replaces the matched root with
    # this text and the remainder already begins with one, so the rewritten path
    # would read `../../..//src/tu.cpp` -- not the spelling the build system
    # passes for the same file, so the absolute and relative spellings would stop
    # converging and the mapping would buy less than it appears to.
    string(REGEX REPLACE "/$" "" back "${back}")

    # The replacement must itself be checkout-independent, and "relative" does not
    # imply that. Measured: an out-of-tree build at `/tmp/x` against a source root
    # under `/mnt/d/...` yields `../../mnt/d/fastcached/...` -- a relative path
    # carrying the entire root inside it, so mapping to it would replace the
    # checkout's path with the checkout's path and read as working. A `..`-only
    # chain is exactly the case where it cannot: it says the build tree lies UNDER
    # the source tree. Anything else drops the rule rather than guessing; the
    # build-tree rule still lands, and that is the one carrying `DW_AT_comp_dir`.
    set(rules "")
    if(back MATCHES "^\.\.(/\.\.)*$")
        list(APPEND rules "${sourceDir}=${back}")
        set(${outSourceMapped} ON PARENT_SCOPE)
    else()
        set(${outSourceMapped} OFF PARENT_SCOPE)
    endif()

    # The build tree is mapped LAST, and that is the whole of the ordering: GCC and
    # Clang honour the LAST matching rule, not the first. Measured directly off
    # `DW_AT_comp_dir` on gcc 14 and clang 20, nested build tree `src/out/build/x`:
    #
    #   no rules      /abs/.../src/out/build/x
    #   build first   ../../../out/build/x     <- the source rule wins
    #   source first  .                        <- what is wanted
    #
    # This comment said the opposite for three commits, and no object comparison
    # could catch it: `.` and `../../../out/build/x` are BOTH checkout-independent,
    # so two checkouts still produced byte-identical objects and the e2e read
    # green. What it costs is narrower and sharper -- two machines whose build
    # trees sit at the same depth under different NAMES (`out/build/gcc-release`
    # against `out/build/ci-release`) relativize to one key, because both flags
    # tokenize to the same canonical text, while their objects differ. That is a
    # mis-serve, and it is the #203 symptom reappearing inside the fix for it.
    list(APPEND rules "${binaryDir}=.")
    set(${outRules} "${rules}" PARENT_SCOPE)
endfunction()

# Resolve the compile-cache address from what the environment PRESENTED, keeping
# "set but empty" distinct from "not set at all".
#
# **The two are different instructions and folding them together broke the
# documented opt-out** (#372). `docs/tools/fastcache-cc.md` promises that a set
# but empty `FASTCACHE_ADDR` means no caching, and the launcher honours that when
# it reads its own environment at run time. This module decided whether the
# launcher is used AT ALL from `if("$ENV{FASTCACHE_ADDR}" STREQUAL "")`, which is
# true for both -- so `export FASTCACHE_ADDR=` was turned straight back into the
# default and kept caching, on every shell and every platform.
#
# That is the same defect this codebase has now fixed three times in three places:
# an explicitly emptied value is a PIN, and an empty default is not a licence to
# drop it. `--storage=` is the recorded instance (#349) -- `ParseText` never fails,
# so an empty one is a reachable operator instruction meaning "persist nowhere" --
# and `OptionSpec::explicitBit` exists because a value comparison cannot see an
# operator who typed the default. CMake can tell the two apart with
# `DEFINED ENV{...}`; nothing but this predicate was in the way.
#
# **The resolved address is the whole answer, and the change-detection record
# stores IT rather than a second encoding of the environment.** The record exists
# so a reconfigure notices the environment moving; what it has to notice is the
# address moving, and unset and set-empty already resolve to different addresses
# -- `127.0.0.1:6674` against the empty opt-out. A separate presence-carrying
# token would additionally separate *absent* from *explicitly set to the default*,
# which differ in no way this module acts on: the retarget would fire, recompute
# the same address, and print a line saying it had changed nothing.
#
# Pure: it reads no environment and touches no cache, so
# `check-fastcache-addr-opt-out.cmake` can drive the case a developer cannot
# easily stage -- two runs differing only in whether the name was present.
#
# @param present  TRUE when the environment defines the name at all.
# @param value    Its value; meaningful only when @p present.
# @param outWanted  The address this module wants; empty means opt out.
function(_fc_resolve_addr present value outWanted)
    if(present)
        # Whatever they set, empty INCLUDED. An empty one reaches
        # `if(NOT FASTCACHE_ADDR)` below and opts out by name.
        set(${outWanted} "${value}" PARENT_SCOPE)
    else()
        # Nobody said anything, so the compiled-in default: a stock `fastcached`
        # listens there, and so does a `fastcache-compile-node` whose
        # --listen-cache defaults to the same address for exactly this reason.
        set(${outWanted} "127.0.0.1:6674" PARENT_SCOPE)
    endif()
endfunction()

# What does a launcher's REJECTION deserve to be said about it, and what replaced it?
#
# **Two facts this file already prints, eighty lines apart, neither mentioning the
# other** (#658). A `fastcache-cc` row rejected for a WIRE VERSION mismatch reports
# at `STATUS` -- correctly for most reasons, since "falling through in silence looks
# exactly like it never being installed" -- and configure then falls through to
# another launcher, whose caveat this file may spend a paragraph on. The operator is
# left to join them.
#
# A version rejection says something the other reasons do not: a daemon **is**
# running, it is one the operator installed, and it is out of step with the
# launcher. That is operator-fixable and usually transient -- and it is precisely
# the state that hands an MSVC-family build to the launcher that can silently
# produce a wrong object. Observed on a real machine with a service 591 commits
# behind master answering on the port.
#
# **Which reasons are worth saying out loud is a COLUMN of the candidate table, not
# a condition in here.** The first version hardcoded `unsupported-version` and
# `chosen STREQUAL "sccache"`, which is a special case layered on a file that is
# otherwise strictly table-driven -- and the second clause was already implied by
# the first guard below, so it could only ever matter by going SILENT if a caveat
# ever moved to another row. A migration that quietly disables a warning while
# nothing fails is this project's #447, and it is not worth re-earning.
#
# **And the WINNER decides a paragraph, never whether there is a message** (#815).
# #658 returned empty unless the winner carried a caveat, which is a property of the
# COMPILER: true on MSVC and clang-cl, false on GCC and Clang. So the host that
# actually reported this -- Linux, ccache installed, daemon older than the launcher
# -- hit every clause of #658 and was told nothing louder than a `STATUS` line among
# a hundred others. The three cases the operator can be in are a safe replacement, a
# hazardous replacement, and NO replacement at all, and the last is the loudest of
# the three: the build compiles uncached. All three warn; only the wording differs.
#
# Not a refusal to configure: a stale daemon is a normal state during a rollout, the
# module's contract is that it may never fail a configure (see the file header), and
# a `SEND_ERROR` here would turn "your cache is not the one you think" into "you
# cannot build". A `message(WARNING)` is the loudest level that leaves the build
# buildable, which is the whole of what this decision is.
#
# @param reason     why the rejected row was rejected; empty when nothing was.
# @param predicts   that row's `predicts` pattern; empty when it predicts nothing.
# @param rejected   a human label for the rejected launcher.
# @param winner     a human label for the launcher that won; empty when none did.
# @param caveat     the winner's caveat; empty when it carries none.
# @param detail     what to say about fixing the rejected one.
# @param outVar     the warning text, or empty when this is not that situation.
function(_fc_cache_rejection_warning reason predicts rejected winner caveat detail outVar)
    set(${outVar} "" PARENT_SCOPE)

    # Nothing rejected, or this row's rejection predicts nothing.
    if(reason STREQUAL "" OR predicts STREQUAL "")
        return()
    endif()
    if(NOT reason MATCHES "${predicts}")
        return()
    endif()

    # What replaced it is a THIRD state and not a boolean: another launcher that is
    # safe, another launcher that carries a hazard, or nothing at all. #658 folded
    # the first and the third into "say nothing", which is why the host that filed
    # #815 -- Linux, so no caveat, and ccache installed -- was told nothing above
    # `STATUS` while its first-party cache went unused for weeks. The caveat now
    # only decides a PARAGRAPH; the rejection alone decides that there is a message.
    if(winner STREQUAL "")
        set(_replacement "and nothing has replaced it: this build compiles with no compiler cache at all.")
    elseif(caveat STREQUAL "")
        set(_replacement "and this build has fallen through to ${winner}.")
    else()
        set(_replacement "and this build has fallen through to ${winner} -- which carries the correctness hazard warned about below.")
    endif()

    set(${outVar}
"[cache] ${rejected} was refused, ${_replacement}

${detail}

It is called out here because THIS rejection reason means a daemon is there and answering, so the cache you installed is not the one this build is using. The others -- not installed, no answer, an uncacheable probe -- describe a cache nobody set up, and warning on them is how a warning becomes one people learn to skip."
        PARENT_SCOPE)
endfunction()

# Everything below needs a project: there is no compiler to probe, no target to
# front and no build to cache without one. `cmake -P` therefore stops here and
# takes the pure computations above -- which is how `check-debug-prefix-map.cmake`
# tests them. A stock condition rather than a variable of our own, so a project
# that vendors this file inherits no undocumented mode flag whose only meaning is
# that fastcached is testing itself.
if(CMAKE_SCRIPT_MODE_FILE)
    return()
endif()

option(USE_COMPILER_CACHE
       "Use a compiler-cache launcher when one is available (fastcache-cc when a daemon answers, else ccache; sccache only with ALLOW_SCCACHE_FALLBACK=ON) [default: ON]"
       ON)

# sccache is never chosen FOR you.
#
# It is a good launcher and this project uses it deliberately on Windows CI. What
# it must not be is the automatic answer to fastcache-cc being unusable: on the
# host that filed #815 the daemon was an old package while the launcher was a
# current build, every exchange was refused on the wire version, sccache took over,
# and the project spent weeks believing it dogfooded its own cache. Every part of
# that mechanism worked. Nobody was told, because a fallback nobody chose is a
# fallback nobody looks at.
#
# So it is opt-in, and the opt-in is the announcement. The ROW is gated rather than
# having callers set `CMAKE_C/CXX_COMPILER_LAUNCHER` themselves, which would also
# work: this module returns early when a launcher is already set, so an external
# one takes the `/showIncludes` caveat warning below out of the log with it -- and
# that warning is the whole subject of #170. Gating the row keeps it.
#
# Unprefixed, like `USE_COMPILER_CACHE`: this file is vendored into other
# repositories verbatim and names nothing of the project it sits in.
option(ALLOW_SCCACHE_FALLBACK
       "Allow sccache to be selected automatically as a compiler-cache launcher [default: OFF]"
       OFF)

# Respect a launcher provided externally (command line, preset, toolchain).
# Check both C and CXX: a toolchain may set only one of them, and we must not
# override either (nor silently set the other alongside it).
if(DEFINED CMAKE_CXX_COMPILER_LAUNCHER OR DEFINED CMAKE_C_COMPILER_LAUNCHER)
    message(STATUS "[cache] Compiler launcher already set externally "
                   "(C='${CMAKE_C_COMPILER_LAUNCHER}', CXX='${CMAKE_CXX_COMPILER_LAUNCHER}'); leaving it untouched.")
    # A build tree configured before this module existed carries the launcher of
    # the day in its cache, and would keep it forever without a word about why
    # the selection below never runs.
    if(DEFINED CACHE{CMAKE_CXX_COMPILER_LAUNCHER} OR DEFINED CACHE{CMAKE_C_COMPILER_LAUNCHER})
        message(STATUS "[cache] That value comes from the CMake cache (a -D, a preset, or an older configure); "
                       "reconfigure with --fresh to let this module choose instead.")
    endif()
    return()
endif()

find_program(FASTCACHE_CC fastcache-cc DOC "fastcache-cc tool path; needs a fastcached daemon to be used")
find_program(SCCACHE sccache DOC "sccache tool path")
find_program(CCACHE ccache DOC "ccache tool path")

# find_program never revisits a cache entry it already filled, which is right for
# a PATH lookup and wrong for the auto-installed launcher below: that one lives in
# a per-user cache directory, and a cache cleaner emptying it would otherwise leave
# this pointing at a binary that is no longer there for the life of the build tree.
# Forget a path that has stopped existing so the lookup — and the fetch — can run
# again.
if(FASTCACHE_CC AND NOT EXISTS "${FASTCACHE_CC}")
    unset(FASTCACHE_CC CACHE)
    find_program(FASTCACHE_CC fastcache-cc DOC "fastcache-cc tool path; needs a fastcached daemon to be used")
endif()

# Where the cache is: FASTCACHE_ADDR from the environment, else localhost's own
# port. That default reaches whichever of the two serves it -- a stock `fastcached`
# (and the service the installers register) listens there, and so does a
# `fastcache-compile-node`, whose --listen-cache defaults to the same address for
# exactly this reason. An empty value opts out of fastcache-cc entirely -- either
# spelling: `-DFASTCACHE_ADDR=` on this configure, or a set-but-empty
# `FASTCACHE_ADDR` in the environment, which is what the operator documentation
# promises and what #372 made true here.
# `DEFINED ENV{}` rather than an emptiness test: see `_fc_resolve_addr`, and #372.
set(_fc_addr_env_present FALSE)
if(DEFINED ENV{FASTCACHE_ADDR})
    set(_fc_addr_env_present TRUE)
endif()
_fc_resolve_addr("${_fc_addr_env_present}" "$ENV{FASTCACHE_ADDR}" _fc_addr_wanted)

# Ordinary cache semantics would freeze the address at whatever the first
# configure saw, so exporting FASTCACHE_ADDR to reach a remote daemon would do
# nothing until the build tree was wiped. Track the environment across
# configures instead and let a *change* to it retarget the cache entry — while
# leaving a -D from this very run alone, which is the one instruction more
# deliberate than the environment. The two are told apart by whether the cache
# still holds what this module last put there, which is also why the retarget
# needs a previous configure to compare against: on a first configure there is
# no bookkeeping yet, both tests hold vacuously, and a -DFASTCACHE_ADDR= meant
# to opt out would be overwritten by an address merely left in the environment.
if(NOT DEFINED CACHE{FASTCACHE_ADDR})
    set(FASTCACHE_ADDR "${_fc_addr_wanted}" CACHE STRING
        "host:port of the fastcached compile-cache daemon, 127.0.0.1:6674 by default (empty disables the fastcache-cc launcher)")
elseif(DEFINED CACHE{_FASTCACHE_ADDR_APPLIED}
       AND NOT _fc_addr_wanted STREQUAL "${_FASTCACHE_ADDR_ENV_SEEN}"
       AND FASTCACHE_ADDR STREQUAL "${_FASTCACHE_ADDR_APPLIED}")
    message(STATUS "[cache] FASTCACHE_ADDR changed in the environment; retargeting to ${_fc_addr_wanted}")
    set(FASTCACHE_ADDR "${_fc_addr_wanted}" CACHE STRING
        "host:port of the fastcached compile-cache daemon, 127.0.0.1:6674 by default (empty disables the fastcache-cc launcher)"
        FORCE)
endif()
# The RESOLVED address, not the raw environment value: unset and set-to-empty
# resolve differently (the default against the empty opt-out), which is exactly the
# distinction the record has to keep, and storing the resolution keeps one encoding
# of it rather than two. An existing build tree holds a pre-#372 record in the old
# format, so its first reconfigure reads as a change and recomputes -- which lands
# on the same address unless the environment now genuinely says otherwise, and is
# how a tree that was silently ignoring an opt-out starts honouring it.
set(_FASTCACHE_ADDR_ENV_SEEN "${_fc_addr_wanted}" CACHE INTERNAL
    "the address FASTCACHE_ADDR last resolved to, to notice a change on reconfigure")
set(_FASTCACHE_ADDR_APPLIED "${FASTCACHE_ADDR}" CACHE INTERNAL
    "the address this module last applied, to tell its own value from one set externally")

# How fastcache-cc is configured, in one place: the probe below must test the
# very environment the build will use, or it would vouch for a configuration
# nothing else runs.
set(_fc_fastcache_env
    "FASTCACHE_ADDR=${FASTCACHE_ADDR}"
    "FASTCACHE_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
    "FASTCACHE_BINARY_DIR=${CMAKE_BINARY_DIR}")

# Optional auto-install of fastcache-cc from this project's GitHub Releases.
#
# Vendored into another repository, everything above buys that repository
# nothing unless fastcache-cc is already on its PATH — a manual, out-of-band
# step on every new checkout and every fresh machine. With
# FASTCACHE_AUTO_INSTALL=ON this fetches the launcher itself: a prebuilt binary
# from the latest stable release, picked by host OS and architecture.
#
# Prebuilt rather than built from source, because building it would need a
# compiler, a build tree and several minutes at configure time — inside the very
# module whose job is to make compiling faster.
#
# Opt-in, because reaching out to the network during `cmake` is a meaningful
# change from "use what is already installed", and because it is what makes this
# module write outside the build tree for the first time. The binary is staged
# in a per-user directory shared by every repository and every build tree, so it
# is fetched once per machine and version rather than once per build.
#
# Nothing here can fail a configure. Every error path — no binary published for
# this platform, no network, a corrupt download, a binary that will not run —
# reports one status line and leaves FASTCACHE_CC empty, so selection falls
# through to sccache, then ccache, then no caching, exactly as it does today.
#
# That last property is why the download is deliberately *not* checked with
# `file(DOWNLOAD ... EXPECTED_HASH)`: a mismatch there is fatal even when STATUS
# is captured (measured), which is the one thing this must never be. The hash is
# compared by hand below instead — the same guarantee, minus the abort.

option(FASTCACHE_AUTO_INSTALL
       "Download fastcache-cc from GitHub Releases when no compiler-cache launcher is installed [default: OFF]"
       OFF)

set(FASTCACHE_AUTO_INSTALL_REPO "LASTRADA-Software/fastcached" CACHE STRING
    "owner/name of the GitHub repository to fetch prebuilt fastcache-cc binaries from")
set(FASTCACHE_AUTO_INSTALL_API "https://api.github.com" CACHE STRING
    "base URL of the GitHub API used to resolve the latest release")
set(FASTCACHE_AUTO_INSTALL_DOWNLOAD_BASE "https://github.com" CACHE STRING
    "base URL release archives are downloaded from (point at a mirror to install without reaching GitHub)")
set(FASTCACHE_AUTO_INSTALL_VERSION "" CACHE STRING
    "exact fastcache-cc version to install (empty resolves the latest stable release)")
set(FASTCACHE_AUTO_INSTALL_TTL_HOURS "24" CACHE STRING
    "how long a resolved latest release is reused before the GitHub API is asked again")
set(FASTCACHE_AUTO_INSTALL_HOST_SYSTEM "" CACHE STRING
    "system to fetch fastcache-cc for (empty asks CMAKE_HOST_SYSTEM_NAME)")
set(FASTCACHE_AUTO_INSTALL_HOST_PROCESSOR "" CACHE STRING
    "processor to fetch fastcache-cc for (empty asks CMAKE_HOST_SYSTEM_PROCESSOR)")

# Auto-installing the launcher alone leaves a genuinely clean machine exactly
# where it started: fastcache-cc caches nothing unless a fastcached daemon
# answers at FASTCACHE_ADDR, and nothing installs one. FASTCACHE_AUTO_START is
# the separate opt-in that does — separate because starting a long-lived
# background process from a `cmake` configure is a materially bigger side
# effect than downloading a file, and a user who wants one but not the other
# must be able to say so.
#
# Defaulting OFF is what keeps CI's existing, relied-upon behaviour intact:
# several downstream projects run CI with no fastcached daemon reachable on
# purpose, so a build transparently falls through to whatever else is installed
# and usable -- which since #815 means ccache, or sccache where it was asked
# for, or nothing. The boundary is unaffected by which of those it lands on;
# what matters is that nothing gets started here. A default of ON
# would remove that boundary for everyone who has not opted out, rather than
# adding it only for those who opt in. No CI-environment sniffing backs this
# up — an explicit -DFASTCACHE_AUTO_START=ON is trusted at face value, the
# same way every other flag in this module is; guessing at "is this CI" from
# environment variables would just be a second, less legible way to be wrong.
option(FASTCACHE_AUTO_START
       "Start a fetched fastcached daemon in the background when none answers at FASTCACHE_ADDR [default: OFF]"
       OFF)

# Where a fetched launcher is kept. Per user rather than per build tree, so the
# second build tree on a machine costs nothing, and per version, so a new release
# never overwrites a binary a configured tree still points at.
#
# The platform split mirrors the launcher's own choice of state directory
# (src/apps/fastcache-cc/Stats.cpp) — with the *cache* spellings rather than the
# state ones, since a re-downloadable binary is by definition cache and belongs
# where a cache cleaner may take it.
if(NOT DEFINED FASTCACHE_AUTO_INSTALL_DIR)
    if(DEFINED ENV{FASTCACHE_CACHE_DIR} AND NOT "$ENV{FASTCACHE_CACHE_DIR}" STREQUAL "")
        set(_fc_auto_dir_default "$ENV{FASTCACHE_CACHE_DIR}")
    elseif(CMAKE_HOST_WIN32 AND NOT "$ENV{LOCALAPPDATA}" STREQUAL "")
        set(_fc_auto_dir_default "$ENV{LOCALAPPDATA}/fastcache-cc")
    elseif(CMAKE_HOST_APPLE AND NOT "$ENV{HOME}" STREQUAL "")
        set(_fc_auto_dir_default "$ENV{HOME}/Library/Caches/fastcache-cc")
    elseif(NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
        set(_fc_auto_dir_default "$ENV{XDG_CACHE_HOME}/fastcache-cc")
    elseif(NOT "$ENV{HOME}" STREQUAL "")
        set(_fc_auto_dir_default "$ENV{HOME}/.cache/fastcache-cc")
    else()
        # No home to speak of (a bare container, a service account). Fall back to
        # the build tree: it still works, it just cannot be shared.
        set(_fc_auto_dir_default "${CMAKE_BINARY_DIR}/CMakeFiles/fastcache-cc")
    endif()
    get_filename_component(_fc_auto_dir_default "${_fc_auto_dir_default}" ABSOLUTE)
    set(FASTCACHE_AUTO_INSTALL_DIR "${_fc_auto_dir_default}" CACHE PATH
        "directory prebuilt fastcache-cc binaries are staged in, shared across repositories and build trees")
endif()

# Where an auto-started daemon keeps its cache. Persistent rather than
# in-memory, so the cache survives the daemon being restarted (a reboot, a
# manual kill) rather than starting cold every time — a subdirectory of the
# same per-user tree the staged binaries live in (FASTCACHE_AUTO_INSTALL_DIR),
# since both are per-machine, re-creatable state that a cache cleaner may take.
if(NOT DEFINED FASTCACHE_AUTO_START_STORAGE_DIR)
    set(FASTCACHE_AUTO_START_STORAGE_DIR "${FASTCACHE_AUTO_INSTALL_DIR}/daemon-storage" CACHE PATH
        "storage directory for a daemon FASTCACHE_AUTO_START starts, so its cache survives a restart")
endif()

# Published-asset table, one row per (host system, host architecture) this
# project releases a binary for. A host with no row is not an error — it simply
# falls back, which is what every platform does today. Publishing a new platform
# is a new row here and nothing below changes.
#
#   _fc_asset_<id>_system        CMAKE_HOST_SYSTEM_NAME this row serves
#   _fc_asset_<id>_arch          host processor spellings mapping to this row
#   _fc_asset_<id>_platform      asset-name infix, after "fastcached-<version>-"
#   _fc_asset_<id>_ext           archive extension
#   _fc_asset_<id>_member        path to the launcher inside the archive's
#                                top-level directory, which differs per
#                                platform because the packages install to
#                                different prefixes
#   _fc_asset_<id>_exe           staged launcher's filename
#   _fc_asset_<id>_daemon_member path to the daemon inside the same archive,
#                                staged only when FASTCACHE_AUTO_START asks
#                                for it — see _fc_auto_start_fastcached below
#   _fc_asset_<id>_daemon_exe    staged daemon's filename
set(_fc_asset_rows linux_x86_64 darwin_arm64 windows_amd64)

set(_fc_asset_linux_x86_64_system "Linux")
set(_fc_asset_linux_x86_64_arch x86_64 amd64 AMD64)
set(_fc_asset_linux_x86_64_platform "Linux-x86_64")
set(_fc_asset_linux_x86_64_ext "tar.gz")
set(_fc_asset_linux_x86_64_member "usr/bin/fastcache-cc")
set(_fc_asset_linux_x86_64_exe "fastcache-cc")
set(_fc_asset_linux_x86_64_daemon_member "usr/bin/fastcached")
set(_fc_asset_linux_x86_64_daemon_exe "fastcached")

set(_fc_asset_darwin_arm64_system "Darwin")
set(_fc_asset_darwin_arm64_arch arm64 aarch64)
set(_fc_asset_darwin_arm64_platform "Darwin-arm64")
set(_fc_asset_darwin_arm64_ext "tar.gz")
set(_fc_asset_darwin_arm64_member "opt/fastcached/bin/fastcache-cc")
set(_fc_asset_darwin_arm64_exe "fastcache-cc")
set(_fc_asset_darwin_arm64_daemon_member "opt/fastcached/bin/fastcached")
set(_fc_asset_darwin_arm64_daemon_exe "fastcached")

# The Windows archive is a plain ZIP rather than the MSI beside it: an installer
# is not something this can open, and the launcher is one self-contained file
# with no VC++ redistributable behind it. Its interior differs from the two
# above because the Windows package is the only one not rooted at /, so its
# binaries sit at the conventional bin/ rather than usr/bin or opt/fastcached.
set(_fc_asset_windows_amd64_system "Windows")
set(_fc_asset_windows_amd64_arch AMD64 x86_64 x64)
set(_fc_asset_windows_amd64_platform "Windows-AMD64")
set(_fc_asset_windows_amd64_ext "zip")
set(_fc_asset_windows_amd64_member "bin/fastcache-cc.exe")
set(_fc_asset_windows_amd64_exe "fastcache-cc.exe")
set(_fc_asset_windows_amd64_daemon_member "bin/fastcached.exe")
set(_fc_asset_windows_amd64_daemon_exe "fastcached.exe")

# The host the table above is asked about: this machine, unless
# FASTCACHE_AUTO_INSTALL_HOST_SYSTEM / _HOST_PROCESSOR state another.
#
# `scripts/check-compile-cache-autoinstall.cmake` states one, because each of
# its rows exercises a different way to decline and on a host no binary is
# published for every one of them stops at THIS question first and never reaches
# its own (#1432's aarch64 leg). So it pins a published platform for all of them
# and asks this question deliberately, in a row of its own.
#
# Stating a platform this host cannot run fetches a binary the smoke run after
# staging then refuses, which is one more decline and never a failed configure.
#
# @param systemVar Receives the host system name.
# @param processorVar Receives the host processor.
function(_fc_auto_install_host systemVar processorVar)
    set(_system "${CMAKE_HOST_SYSTEM_NAME}")
    set(_processor "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    if(NOT "${FASTCACHE_AUTO_INSTALL_HOST_SYSTEM}" STREQUAL "")
        set(_system "${FASTCACHE_AUTO_INSTALL_HOST_SYSTEM}")
    endif()
    if(NOT "${FASTCACHE_AUTO_INSTALL_HOST_PROCESSOR}" STREQUAL "")
        set(_processor "${FASTCACHE_AUTO_INSTALL_HOST_PROCESSOR}")
    endif()
    set(${systemVar} "${_system}" PARENT_SCOPE)
    set(${processorVar} "${_processor}" PARENT_SCOPE)
endfunction()

# Pick the row serving this host.
# @param outVar Receives the row id, or empty when no binary is published for it.
function(_fc_auto_install_select_row outVar)
    set(${outVar} "" PARENT_SCOPE)
    _fc_auto_install_host(_system _processor)
    foreach(_id IN LISTS _fc_asset_rows)
        if(NOT _system STREQUAL "${_fc_asset_${_id}_system}")
            continue()
        endif()
        # Architecture spellings vary by OS and by how CMake was told about the
        # host, so a row lists every name that means it rather than one.
        foreach(_arch IN LISTS _fc_asset_${_id}_arch)
            if(_processor STREQUAL "${_arch}")
                set(${outVar} "${_id}" PARENT_SCOPE)
                return()
            endif()
        endforeach()
    endforeach()
endfunction()

# Fetch (or reuse) the GitHub release metadata describing the latest stable
# release. GitHub's "latest" already excludes drafts and prereleases, so it is
# the stable release by definition.
#
# The response is cached on disk with a time-to-live for two reasons that both
# bite in CI: unauthenticated API access is limited to 60 requests per hour per
# IP, which a fleet of runners behind one egress address exhausts quickly, and a
# machine that has resolved once should keep working when the network is gone.
# A token in the environment raises that limit and is used when present.
#
# @param outVar Receives the release JSON.
# @param reasonVar Receives a short diagnostic when outVar is empty.
function(_fc_auto_install_release_json outVar reasonVar)
    set(${outVar} "" PARENT_SCOPE)

    # One cache file per repository: the staging directory is shared, and two
    # projects pointing at different forks must not read each other's answer.
    string(REPLACE "/" "_" _repoSlug "${FASTCACHE_AUTO_INSTALL_REPO}")
    set(_cacheFile "${FASTCACHE_AUTO_INSTALL_DIR}/${_repoSlug}-latest.json")

    if(EXISTS "${_cacheFile}")
        file(TIMESTAMP "${_cacheFile}" _stamp "%s")
        string(TIMESTAMP _now "%s" UTC)
        if(_stamp AND _now)
            math(EXPR _ageHours "(${_now} - ${_stamp}) / 3600")
            if(_ageHours LESS ${FASTCACHE_AUTO_INSTALL_TTL_HOURS})
                file(READ "${_cacheFile}" _cached)
                set(${outVar} "${_cached}" PARENT_SCOPE)
                set(${reasonVar} "" PARENT_SCOPE)
                return()
            endif()
        endif()
    endif()

    set(_headers HTTPHEADER "Accept: application/vnd.github+json")
    # Either spelling, because gh and actions/checkout each export their own.
    foreach(_tokenVar GITHUB_TOKEN GH_TOKEN)
        if(NOT "$ENV{${_tokenVar}}" STREQUAL "")
            list(APPEND _headers HTTPHEADER "Authorization: Bearer $ENV{${_tokenVar}}")
            break()
        endif()
    endforeach()

    set(_tmp "${CMAKE_BINARY_DIR}/CMakeFiles/fastcache-download/latest.json")
    file(DOWNLOAD
        "${FASTCACHE_AUTO_INSTALL_API}/repos/${FASTCACHE_AUTO_INSTALL_REPO}/releases/latest"
        "${_tmp}"
        ${_headers}
        STATUS _status
        TIMEOUT 30
        INACTIVITY_TIMEOUT 15)
    list(GET _status 0 _code)

    if(NOT _code EQUAL 0)
        list(GET _status 1 _message)
        # A stale answer beats no answer: the release we last saw is still a real
        # release, and a machine that is merely offline should keep building.
        if(EXISTS "${_cacheFile}")
            file(READ "${_cacheFile}" _cached)
            message(STATUS "[cache] GitHub API unreachable (${_message}); reusing the last known release")
            set(${outVar} "${_cached}" PARENT_SCOPE)
            set(${reasonVar} "" PARENT_SCOPE)
        else()
            set(${reasonVar} "cannot reach the GitHub API (${_message})" PARENT_SCOPE)
        endif()
        return()
    endif()

    file(READ "${_tmp}" _json)
    file(WRITE "${_cacheFile}" "${_json}")
    set(${outVar} "${_json}" PARENT_SCOPE)
    set(${reasonVar} "" PARENT_SCOPE)
endfunction()

# Resolve which release row, version and (when not pinned) release-metadata
# JSON an auto-install should use — the part of _fc_auto_install_fastcache_cc
# that has nothing to do with the launcher specifically. Split out so
# _fc_auto_start_fastached (below) resolves to the *same* release the launcher
# did rather than asking the GitHub API a second time and risking a different
# "latest" answering the two calls of one configure.
#
# @param rowVar Set to the asset-table row id for this host.
# @param versionVar Set to the resolved version, a bare numeric triple.
# @param jsonVar Set to the release metadata, or empty when the version was
#                pinned.
# @param reasonVar Set to a short diagnostic when resolution fails.
function(_fc_auto_install_resolve_release rowVar versionVar jsonVar reasonVar)
    set(${rowVar} "" PARENT_SCOPE)
    set(${versionVar} "" PARENT_SCOPE)
    set(${jsonVar} "" PARENT_SCOPE)

    _fc_auto_install_select_row(_row)
    if(NOT _row)
        _fc_auto_install_host(_system _processor)
        set(${reasonVar} "no prebuilt binary is published for ${_system}-${_processor}" PARENT_SCOPE)
        return()
    endif()

    # A pinned version is an answer in itself and costs no API call, which is
    # also what makes an auto-installing build reproducible.
    set(_json "")
    if(FASTCACHE_AUTO_INSTALL_VERSION)
        set(_version "${FASTCACHE_AUTO_INSTALL_VERSION}")
    else()
        _fc_auto_install_release_json(_json _why)
        if(NOT _json)
            set(${reasonVar} "${_why}" PARENT_SCOPE)
            return()
        endif()
        string(JSON _tag ERROR_VARIABLE _jsonErr GET "${_json}" tag_name)
        if(_jsonErr OR NOT _tag)
            set(${reasonVar} "the GitHub API answered without a release tag" PARENT_SCOPE)
            return()
        endif()
        set(_version "${_tag}")
    endif()

    # Asset names carry a bare numeric triple, so anything else cannot name a
    # file that exists — the same shape cmake/Version.cmake insists on.
    if(NOT _version MATCHES "^v?([0-9]+\\.[0-9]+\\.[0-9]+)$")
        set(${reasonVar} "'${_version}' is not a numeric X.Y.Z version" PARENT_SCOPE)
        return()
    endif()

    set(${rowVar} "${_row}" PARENT_SCOPE)
    set(${versionVar} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    set(${jsonVar} "${_json}" PARENT_SCOPE)
    set(${reasonVar} "" PARENT_SCOPE)
endfunction()

# Download and stage a prebuilt fastcache-cc for this host.
# @param outVar Set to the staged executable's path, or empty on any failure.
# @param reasonVar Set to a short diagnostic when outVar is empty.
function(_fc_auto_install_fastcache_cc outVar reasonVar)
    set(${outVar} "" PARENT_SCOPE)

    # An empty address is the documented way to opt out of fastcache-cc, so
    # fetching it would quietly override an instruction not to use it. Reported
    # rather than skipped: asking for the fetch and getting nothing, with no word
    # about why, is the silent fall-through the rest of this module avoids.
    if(NOT FASTCACHE_ADDR)
        set(${reasonVar} "FASTCACHE_ADDR is empty, which opts out of fastcache-cc" PARENT_SCOPE)
        return()
    endif()

    _fc_auto_install_resolve_release(_row _version _json _why)
    if(NOT _row)
        set(${reasonVar} "${_why}" PARENT_SCOPE)
        return()
    endif()

    # Cached for the lifetime of this configure so a later auto-start of the
    # daemon (from the very same release) resolves nothing a second time.
    set(_FASTCACHE_RESOLVED_ROW "${_row}" CACHE INTERNAL "")
    set(_FASTCACHE_RESOLVED_VERSION "${_version}" CACHE INTERNAL "")
    set(_FASTCACHE_RESOLVED_JSON "${_json}" CACHE INTERNAL "")

    # Keyed by platform as well as version. A home directory is not always local
    # to one machine — a shared or synchronised $HOME is normal — and two hosts of
    # different architectures reaching the same staging directory must not be
    # handed each other's binary.
    set(_stagedDir "${FASTCACHE_AUTO_INSTALL_DIR}/${_version}/${_fc_asset_${_row}_platform}")
    set(_staged "${_stagedDir}/${_fc_asset_${_row}_exe}")

    if(NOT EXISTS "${_staged}")
        _fc_auto_install_download("${_row}" "${_version}" "${_json}" "${_stagedDir}" _why)
        if(_why)
            set(${reasonVar} "${_why}" PARENT_SCOPE)
            return()
        endif()
    endif()

    # Asked of a binary just downloaded and of one staged by an earlier run
    # alike. A download can arrive intact and still be unusable here — too old a
    # libc, a truncated file no digest was published to catch — and a binary
    # staged last month can have been made unusable since, by the very sharing
    # the layout above accounts for. Finding out now costs one process; finding
    # out later costs every translation unit.
    execute_process(COMMAND "${_staged}" --version
                    TIMEOUT 10
                    RESULT_VARIABLE _rc
                    OUTPUT_QUIET
                    ERROR_QUIET)
    if(NOT _rc EQUAL 0)
        file(REMOVE "${_staged}")
        set(${reasonVar} "the downloaded launcher does not run here (${_rc})" PARENT_SCOPE)
        return()
    endif()

    set(${outVar} "${_staged}" PARENT_SCOPE)
    set(${reasonVar} "" PARENT_SCOPE)
endfunction()

# Fetch one published archive into the shared per-build-tree download cache and
# unpack it, without staging anything out of it yet. Split out of
# _fc_auto_install_download (below) so that staging the launcher and staging
# the daemon from the *same* release — the ordinary case, since both binaries
# live in one archive — costs one download and one unpack rather than two:
# fetching a several-megabyte archive twice in one configure for two files it
# already contains would be a needless round trip on every clean machine.
#
# @param row Asset-table row id describing this host.
# @param version Release version, a bare numeric triple.
# @param json Release metadata, or empty when the version was pinned.
# @param unpackDirVar Set to the directory the archive was unpacked into
#                      (its "${_stem}/..." layout preserved), on success.
# @param reasonVar Set to a short diagnostic on failure, empty on success.
function(_fc_auto_install_fetch_archive row version json unpackDirVar reasonVar)
    set(${reasonVar} "" PARENT_SCOPE)
    set(${unpackDirVar} "" PARENT_SCOPE)
    set(_row "${row}")
    set(_version "${version}")
    set(_json "${json}")

    set(_stem "fastcached-${_version}-${_fc_asset_${_row}_platform}")
    set(_assetName "${_stem}.${_fc_asset_${_row}_ext}")
    set(_url "")
    set(_digest "")

    # Prefer the URL and digest the API reported. A pinned version has no
    # metadata to consult, so its URL is composed from the documented layout —
    # which is also the path an internal mirror takes, since pinning a version
    # and pointing FASTCACHE_AUTO_INSTALL_DOWNLOAD_BASE elsewhere installs
    # without reaching GitHub at all.
    if(_json)
        string(JSON _assetCount ERROR_VARIABLE _jsonErr LENGTH "${_json}" assets)
        if(_jsonErr OR NOT _assetCount GREATER 0)
            set(${reasonVar} "release ${_version} publishes no assets" PARENT_SCOPE)
            return()
        endif()
        math(EXPR _lastAsset "${_assetCount} - 1")
        foreach(_i RANGE 0 ${_lastAsset})
            string(JSON _name ERROR_VARIABLE _e GET "${_json}" assets ${_i} name)
            if(_e OR NOT _name STREQUAL "${_assetName}")
                continue()
            endif()
            string(JSON _url ERROR_VARIABLE _e GET "${_json}" assets ${_i} browser_download_url)
            if(_e)
                set(_url "")
            endif()
            # Present on releases GitHub has digested; absent on older ones.
            string(JSON _digest ERROR_VARIABLE _e GET "${_json}" assets ${_i} digest)
            if(_e)
                set(_digest "")
            endif()
            break()
        endforeach()
        if(NOT _url)
            set(${reasonVar} "release ${_version} publishes no ${_assetName}" PARENT_SCOPE)
            return()
        endif()
    else()
        set(_url "${FASTCACHE_AUTO_INSTALL_DOWNLOAD_BASE}/${FASTCACHE_AUTO_INSTALL_REPO}/releases/download/v${_version}/${_assetName}")
    endif()

    message(STATUS "[cache] Fetching the fastcached ${_version} release archive for ${_fc_asset_${_row}_platform}")

    set(_work "${CMAKE_BINARY_DIR}/CMakeFiles/fastcache-download")
    set(_archive "${_work}/${_assetName}")
    file(DOWNLOAD "${_url}" "${_archive}"
         STATUS _status
         TIMEOUT 120
         INACTIVITY_TIMEOUT 30)
    list(GET _status 0 _code)
    if(NOT _code EQUAL 0)
        list(GET _status 1 _message)
        file(REMOVE "${_archive}")
        set(${reasonVar} "downloading ${_assetName} failed (${_message})" PARENT_SCOPE)
        return()
    endif()

    if(_digest MATCHES "^sha256:([0-9a-fA-F]+)$")
        file(SHA256 "${_archive}" _actual)
        string(TOLOWER "${CMAKE_MATCH_1}" _expected)
        if(NOT _actual STREQUAL "${_expected}")
            file(REMOVE "${_archive}")
            set(${reasonVar} "${_assetName} failed its SHA256 check" PARENT_SCOPE)
            return()
        endif()
    endif()

    # Unpack beside the archive, inside the build tree, so two build trees
    # unpacking the same release at the same time cannot collide.
    set(_unpack "${_work}/unpack")
    file(REMOVE_RECURSE "${_unpack}")
    file(MAKE_DIRECTORY "${_unpack}")
    file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_unpack}")

    set(${unpackDirVar} "${_unpack}/${_stem}" PARENT_SCOPE)
endfunction()

# Copy one member out of an already-unpacked archive into a staging directory.
# Split out so the launcher and the daemon — two members of one archive — are
# each placed with the same race-safe move-into-place sequence, without
# duplicating it.
#
# @param unpackDir Directory _fc_auto_install_fetch_archive unpacked into.
# @param member Path to the executable inside unpackDir.
# @param exe Filename to give the staged copy.
# @param stagedDir Directory the executable is placed in.
# @param reasonVar Set to a short diagnostic on failure, empty on success.
function(_fc_auto_install_stage_member unpackDir member exe stagedDir reasonVar)
    set(${reasonVar} "" PARENT_SCOPE)

    set(_member "${unpackDir}/${member}")
    if(NOT EXISTS "${_member}")
        set(${reasonVar} "the archive does not contain ${member}" PARENT_SCOPE)
        return()
    endif()

    # Move into place rather than write in place: two configures racing on the
    # same shared staging directory must never expose a half-written binary to
    # the one that arrives second. The temporary name is derived from the build
    # tree, so the two racers cannot pick the same one either.
    string(SHA256 _treeHash "${CMAKE_BINARY_DIR}")
    string(SUBSTRING "${_treeHash}" 0 12 _treeHash)
    set(_pending "${stagedDir}/.staging-${_treeHash}-${exe}")
    file(MAKE_DIRECTORY "${stagedDir}")
    file(REMOVE "${_pending}")
    file(COPY_FILE "${_member}" "${_pending}" RESULT _copyFailed)
    if(_copyFailed)
        set(${reasonVar} "cannot write to ${stagedDir} (${_copyFailed})" PARENT_SCOPE)
        return()
    endif()
    file(CHMOD "${_pending}"
         PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    file(RENAME "${_pending}" "${stagedDir}/${exe}" RESULT _renameFailed)
    if(_renameFailed AND NOT EXISTS "${stagedDir}/${exe}")
        # Losing the race is success; failing to write at all is not.
        set(${reasonVar} "cannot stage ${exe} (${_renameFailed})" PARENT_SCOPE)
        return()
    endif()
    file(REMOVE "${_pending}")
endfunction()

# Fetch one published archive and stage the launcher out of it. Thin wrapper
# over the two functions above, kept so every existing caller (and the
# existing diagnostics they depend on) is unaffected by the split.
# @param row Asset-table row id describing this host.
# @param version Release version, a bare numeric triple.
# @param json Release metadata, or empty when the version was pinned.
# @param stagedDir Directory the launcher is placed in.
# @param reasonVar Set to a short diagnostic on failure, empty on success.
function(_fc_auto_install_download row version json stagedDir reasonVar)
    set(${reasonVar} "" PARENT_SCOPE)

    _fc_auto_install_fetch_archive("${row}" "${version}" "${json}" _unpackDir _why)
    if(_why)
        set(${reasonVar} "${_why}" PARENT_SCOPE)
        return()
    endif()

    _fc_auto_install_stage_member(
        "${_unpackDir}" "${_fc_asset_${row}_member}" "${_fc_asset_${row}_exe}"
        "${stagedDir}" _why)
    if(_why)
        set(${reasonVar} "${_why}" PARENT_SCOPE)
        return()
    endif()
endfunction()

# Split from the connect attempt below only by host/port parsing, itself
# already done once for FASTCACHE_ADDR — kept as its own function so the race
# check (does *anything* answer) and the future probe share no code by
# accident growing shared state.
#
# @param addr host:port to parse (FASTCACHE_ADDR's own shape).
# @param hostVar Set to the host part.
# @param portVar Set to the port part.
function(_fc_split_host_port addr hostVar portVar)
    # IPv6 in brackets ("[::1]:6674") is the one shape a naive last-colon split
    # gets wrong, so the bracket form is matched first.
    if(addr MATCHES "^\\[(.+)\\]:([0-9]+)$")
        set(${hostVar} "${CMAKE_MATCH_1}" PARENT_SCOPE)
        set(${portVar} "${CMAKE_MATCH_2}" PARENT_SCOPE)
    elseif(addr MATCHES "^(.+):([0-9]+)$")
        set(${hostVar} "${CMAKE_MATCH_1}" PARENT_SCOPE)
        set(${portVar} "${CMAKE_MATCH_2}" PARENT_SCOPE)
    else()
        set(${hostVar} "${addr}" PARENT_SCOPE)
        set(${portVar} "" PARENT_SCOPE)
    endif()
endfunction()

# The first non-blank line of a captured stream, for a status message.
#
# One line rather than the whole capture, because a child that died noisily would
# otherwise bury the configure's own output under something nobody asked for -- and
# because the sentence that names the cause is the first one every loader, libc and
# `main()` in question emits. A stream that is empty or entirely blank yields the
# empty string, which callers read as "the child said nothing" and fall back on.
#
# @param text The captured stream.
# @param outVar Set to its first non-blank line, stripped, or to the empty string.
function(_fc_first_line text outVar)
    # Deliberately NOT a split-into-a-list-and-walk-it. That form has to escape and
    # restore semicolons, because a CMake list is semicolon-separated and a loader
    # message naming a search path is full of them -- and it only ever wants the first
    # element anyway.
    #
    # And deliberately NO REGEX, which is not a style choice. Before CMake 4, a regex
    # that MATCHES AN EMPTY STRING is a hard error in `string(REGEX ...)` -- for MATCH
    # as well as REPLACE -- and this module may never fail a configure. The obvious
    # spelling, `REGEX REPLACE "^[ \t\r\n]*"`, matches empty on any capture with no
    # leading whitespace, which is the ORDINARY case rather than an edge one, so it
    # failed every configure on a CMake older than 4.
    #
    # The tempting repair is `*` -> `+` on that line, and it is NOT sufficient: the
    # second regex, `MATCH "^[^\r\n]*"`, matches empty whenever the capture is empty
    # or blank, which is the case this function exists to answer. Both had to go, and
    # a form with no regex at all cannot raise the question a third time.
    #
    # Measured 2026-09-20 against cmake 3.31.6 and 4.2.3, nine inputs each, this
    # implementation and the all-`+` one agreeing on every one: an ordinary message,
    # a leading-blank one, one behind blank lines, empty, blanks-only, newlines-only,
    # one carrying a semicolon-separated search path, one with trailing blanks, and a
    # CRLF one. 4.2.3 accepts every form and so proves nothing on its own -- the
    # version that discriminates is the OLD one, which is what CI runs and what no
    # developer here had.
    string(STRIP "${text}" _line)
    string(FIND "${_line}" "\n" _break)
    if(_break GREATER -1)
        string(SUBSTRING "${_line}" 0 ${_break} _line)
    endif()
    # Again, so a CRLF capture does not keep its carriage return.
    string(STRIP "${_line}" _line)
    set(${outVar} "${_line}" PARENT_SCOPE)
endfunction()

# Is anything at all answering at FASTCACHE_ADDR? Deliberately cheaper and
# less discerning than _fc_probe_fastcache_cc: that probe needs a real
# compiler invocation and answers "does the CACHE work", which is not the
# question here. This only answers "would starting a daemon collide with one
# already running" — ours from an earlier configure, or someone else's — so
# two configures racing on the same port must not both attempt a bind. Losing
# this race is deliberately not a failure case: whichever configure's spawn
# loses treats the winner's daemon as its own, which is exactly what
# _fc_probe_fastcache_cc then evaluates.
#
# CMake has no raw-socket primitive, so this shells out to the daemon binary
# itself with --version against the address — the one operation every
# fastcached build already exposes for exactly this kind of check — rather
# than reaching for a platform-specific netcat/Test-NetConnection that may not
# be installed either. A future fastcached could grow a lighter "are you
# there" ping; until then this costs one short-lived process.
#
# @param outVar Set to TRUE when something answers, FALSE otherwise.
function(_fc_daemon_answering addr outVar)
    set(${outVar} FALSE PARENT_SCOPE)
    _fc_split_host_port("${addr}" _host _port)
    if(NOT _port)
        return()
    endif()
    # A tiny C program would be a fifth language in a module that promises to
    # stay stock CMake; `cmake -E` has no TCP-connect verb either. The staged
    # fastcache-cc's own probe path already proves connectivity end-to-end at
    # negligible cost (it fails fast, ~10ms, against a closed port), so reuse
    # it here instead of inventing a second way to ask the same question.
    if(NOT FASTCACHE_CC)
        return()
    endif()
    set(_dir "${CMAKE_BINARY_DIR}/CMakeFiles/fastcache-probe")
    file(MAKE_DIRECTORY "${_dir}")
    set(_src "${_dir}/portcheck.cpp")
    if(NOT EXISTS "${_src}")
        file(WRITE "${_src}" "int fastcachePortCheck() { return 0; }\n")
    endif()
    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set(_args /nologo /c "${_src}" "/Fo${_dir}/portcheck.obj")
    else()
        set(_args -c "${_src}" -o "${_dir}/portcheck.o")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                "FASTCACHE_ADDR=${addr}"
                "FASTCACHE_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                "FASTCACHE_BINARY_DIR=${CMAKE_BINARY_DIR}"
                "FASTCACHE_VERBOSE=1"
                "FASTCACHE_NO_STATS=1"
                "FASTCACHE_TIMEOUT=500ms"
                "${FASTCACHE_CC}" "${CMAKE_CXX_COMPILER}" ${_args}
        WORKING_DIRECTORY "${_dir}"
        TIMEOUT 3
        RESULT_VARIABLE _rc
        OUTPUT_QUIET
        ERROR_VARIABLE _err)
    if(_err MATCHES "fastcache-cc: (HIT|MISS) key=")
        set(${outVar} TRUE PARENT_SCOPE)
    endif()
endfunction()

# Stage and start a fastcached daemon in the background when FASTCACHE_ADDR is
# otherwise unreachable, so FASTCACHE_AUTO_INSTALL's launcher has something to
# talk to on a genuinely clean machine (issue #90). Nothing here may fail a
# configure, exactly like _fc_auto_install_fastcache_cc: every failure is one
# status line and a fall-through to the probe finding no daemon, which is the
# behaviour without FASTCACHE_AUTO_START at all.
function(_fc_auto_start_fastcached)
    _fc_daemon_answering("${FASTCACHE_ADDR}" _answering)
    if(_answering)
        # Ours from an earlier configure, or a pre-existing daemon started by
        # hand — either way there is nothing to start, and starting a second
        # one would only fight the first for the port.
        return()
    endif()

    _fc_split_host_port("${FASTCACHE_ADDR}" _bindHost _bindPort)
    if(NOT _bindPort)
        message(STATUS "[cache] Not starting a daemon: FASTCACHE_ADDR '${FASTCACHE_ADDR}' has no port")
        return()
    endif()

    # Reuse the launcher's own resolved release when this configure already
    # auto-installed one, rather than asking the GitHub API a second time —
    # which could, on a second call, resolve to a *different* "latest" than
    # the launcher just staged, if a release published in between the two
    # calls. Falling through to a fresh resolution below still works when the
    # launcher came from PATH and resolved nothing.
    if(DEFINED _FASTCACHE_RESOLVED_ROW AND _FASTCACHE_RESOLVED_ROW)
        set(_row "${_FASTCACHE_RESOLVED_ROW}")
        set(_version "${_FASTCACHE_RESOLVED_VERSION}")
        set(_json "${_FASTCACHE_RESOLVED_JSON}")
    else()
        _fc_auto_install_resolve_release(_row _version _json _why)
        if(NOT _row)
            message(STATUS "[cache] Not starting a daemon: ${_why}")
            return()
        endif()
    endif()

    if(NOT _fc_asset_${_row}_daemon_member)
        message(STATUS "[cache] Not starting a daemon: no fastcached binary is published for ${_fc_asset_${_row}_platform}")
        return()
    endif()

    set(_stagedDir "${FASTCACHE_AUTO_INSTALL_DIR}/${_version}/${_fc_asset_${_row}_platform}")
    set(_staged "${_stagedDir}/${_fc_asset_${_row}_daemon_exe}")

    if(NOT EXISTS "${_staged}")
        _fc_auto_install_fetch_archive("${_row}" "${_version}" "${_json}" _unpackDir _why)
        if(NOT _unpackDir)
            message(STATUS "[cache] Not starting a daemon: ${_why}")
            return()
        endif()
        _fc_auto_install_stage_member(
            "${_unpackDir}" "${_fc_asset_${_row}_daemon_member}" "${_fc_asset_${_row}_daemon_exe}"
            "${_stagedDir}" _why)
        if(_why)
            message(STATUS "[cache] Not starting a daemon: ${_why}")
            return()
        endif()
    endif()

    file(MAKE_DIRECTORY "${FASTCACHE_AUTO_START_STORAGE_DIR}")

    # Bind whatever host FASTCACHE_ADDR actually named — including a LAN
    # address someone deliberately pointed a shared daemon at — and default
    # only when it named none, since FASTCACHE_ADDR's own default
    # (127.0.0.1:6674, set near the top of this file) is loopback-only and an
    # auto-started daemon should not be reachable from the network by
    # surprise.
    set(_bindArg "${_bindHost}")
    if(NOT _bindArg)
        set(_bindArg "127.0.0.1")
    endif()

    set(_daemonArgs
        "--bind=${_bindArg}"
        "--port=${_bindPort}"
        "--storage=${FASTCACHE_AUTO_START_STORAGE_DIR}")

    if(CMAKE_HOST_WIN32)
        # No double-fork equivalent outside the SCM path (see
        # .agent/rules/platform-service-and-config.md on --daemon and
        # launchd/SCM supervisors) — this is a plain background
        # process, not a registered service, so `cmake -E env` launches it
        # directly. execute_process has no detach flag of its own; started
        # this way the child is not joined to cmake's own console and outlives
        # the configure once cmake exits, which is standard Windows
        # child-process behaviour for a process that does not inherit the
        # parent's console. --pidfile is POSIX-daemon-mode only (ServiceControl
        # names it so explicitly) and is never written by the plain
        # ForegroundHost this runs under here, so it is omitted rather than
        # passed for a file that would never appear.
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env "${_staged}" ${_daemonArgs}
            RESULT_VARIABLE _rc
            TIMEOUT 5
            OUTPUT_VARIABLE _startOut
            ERROR_VARIABLE _startErr)
    else()
        # --daemon double-forks and the parent exits immediately once the
        # child has detached, so execute_process returns right away with the
        # daemon already backgrounded — this is the one place in the whole
        # module that flag is the right tool, as opposed to a *service*
        # registration, which --daemon must never be combined with (see
        # .agent/rules/platform-service-and-config.md: "the supervisor's launch
        # arguments must not pass --daemon"). A well-known --pidfile is what
        # turns "who started this"
        # into an answerable question — for an operator who wants to stop a
        # daemon this module left running (its own stated lifetime: until the
        # machine reboots or a human kills it), and for a test that needs to
        # clean one up without a PID execute_process never hands back, since
        # --daemon double-forks.
        set(_pidfile "${FASTCACHE_AUTO_START_STORAGE_DIR}/fastcached.pid")
        execute_process(
            COMMAND "${_staged}" --daemon ${_daemonArgs} "--pidfile=${_pidfile}"
            RESULT_VARIABLE _rc
            TIMEOUT 5
            OUTPUT_VARIABLE _startOut
            ERROR_VARIABLE _startErr)
    endif()
    if(NOT _rc EQUAL 0)
        # The exit status alone is not a diagnosis, and for the commonest failure it
        # is actively misleading: a dynamic loader that cannot resolve a library
        # answers 127, which reads as "not found" for a binary this module has just
        # staged and knows the path of. The daemon's own first line says which
        # library, and it was captured by nobody until
        # [#1538](https://github.com/LASTRADA-Software/fastcached/issues/1538) --
        # `OUTPUT_QUIET ERROR_QUIET` here threw away the one sentence that explains
        # the number. An empty capture degrades to the number alone, which is what
        # this said before.
        _fc_first_line("${_startErr}" _startWhy)
        if(NOT _startWhy)
            _fc_first_line("${_startOut}" _startWhy)
        endif()
        if(_startWhy)
            message(STATUS "[cache] Not starting a daemon: fastcached exited immediately (${_rc}): ${_startWhy}")
        else()
            message(STATUS "[cache] Not starting a daemon: fastcached exited immediately (${_rc})")
        endif()
        return()
    endif()

    # The daemon forks/spawns and returns before it is necessarily listening
    # yet, so give it a short, bounded window to come up rather than letting
    # the very next thing that runs (the probe) be the first and only check —
    # a poll loop rather than a fixed sleep, since a fast machine should not
    # pay a worst-case wait. _fc_daemon_answering itself costs ~10ms once the
    # daemon is up, so ten attempts is a bounded ~1s beyond whatever the
    # daemon itself needed to bind.
    set(_upAttempt 0)
    set(_up FALSE)
    while(_upAttempt LESS 10 AND NOT _up)
        _fc_daemon_answering("${FASTCACHE_ADDR}" _up)
        if(NOT _up)
            execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.1)
        endif()
        math(EXPR _upAttempt "${_upAttempt} + 1")
    endwhile()

    if(_up)
        message(STATUS "[cache] Auto-started fastcached (${_staged}) listening on ${FASTCACHE_ADDR}, storage=${FASTCACHE_AUTO_START_STORAGE_DIR}")
    else()
        message(STATUS "[cache] Started fastcached (${_staged}) but it is not answering on ${FASTCACHE_ADDR} yet; the probe below will report the outcome")
    endif()
endfunction()

# sccache counts as "already installed" here only when it could actually be
# selected. It is opt-in since #815, so an installed-but-not-opted-into sccache is
# not a decision anybody made about this build -- and reading it as one would leave
# a machine that explicitly asked for auto-install with no cache at all, which is
# the opposite of what the flag is for.
set(_fc_sccache_selectable OFF)
if(SCCACHE AND ALLOW_SCCACHE_FALLBACK)
    set(_fc_sccache_selectable ON)
endif()

# Only when there is nothing else to use. A launcher the user installed is a
# decision already made, and preempting it would be the behaviour change this is
# careful not to be.
if(USE_COMPILER_CACHE
   AND FASTCACHE_AUTO_INSTALL
   AND NOT FASTCACHE_CC
   AND NOT _fc_sccache_selectable
   AND NOT CCACHE)
    _fc_auto_install_fastcache_cc(_fc_auto_installed _fc_auto_why_not)
    if(_fc_auto_installed)
        message(STATUS "[cache] Auto-installed fastcache-cc (${_fc_auto_installed})")
        set(FASTCACHE_CC "${_fc_auto_installed}" CACHE FILEPATH
            "fastcache-cc tool path; needs a fastcached daemon to be used" FORCE)
    else()
        message(STATUS "[cache] Not auto-installing fastcache-cc: ${_fc_auto_why_not}")
    endif()
endif()

# Belongs after auto-install rather than inside it: starting a daemon is
# useful whether fastcache-cc came from PATH or was just staged above, and
# either way FASTCACHE_CC has to be a real path before _fc_daemon_answering
# can shell out to it to test the port.
if(USE_COMPILER_CACHE
   AND FASTCACHE_AUTO_START
   AND FASTCACHE_CC
   AND FASTCACHE_ADDR)
    _fc_auto_start_fastcached()
endif()

# Ask fastcache-cc itself whether the cache works, by compiling one tiny
# translation unit through it with FASTCACHE_VERBOSE=1 and requiring a reported
# cache outcome. A launcher that cannot reach its daemon still compiles fine —
# it just runs the real compiler — so nothing but an end-to-end exchange tells
# "the cache works" apart from "every TU will silently pay a failed connect,
# with precompiled headers disabled for nothing and ccache passed over".
#
# The match is positive (HIT/MISS only): should the launcher's diagnostics ever
# be reworded, this reports unusable and the build falls back to the next
# launcher, which is the harmless direction to be wrong in.
#
# @param outVar Set to TRUE when the cache served the probe, FALSE otherwise.
# @param reasonVar Set to a short diagnostic when outVar is FALSE.
function(_fc_probe_fastcache_cc outVar reasonVar)
    set(${outVar} FALSE PARENT_SCOPE)

    set(_dir "${CMAKE_BINARY_DIR}/CMakeFiles/fastcache-probe")
    set(_src "${_dir}/probe.cpp")
    # Both the file and its content are fixed, so the probe itself is a cache
    # hit from the second configure onwards — which exercises FETCH rather than
    # just STORE, and costs less than the first run.
    file(WRITE "${_src}" "int fastcacheProbe() { return 0; }\n")

    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set(_args /nologo /c "${_src}" "/Fo${_dir}/probe.obj")
    else()
        set(_args -c "${_src}" -o "${_dir}/probe.o")
    endif()

    # A probe that answers takes ~0.1s locally and little more over a LAN, so ten
    # seconds is generous for a working daemon and a bounded wait for a broken
    # one. The cap has to live here: FASTCACHE_TIMEOUT bounds the launcher's
    # send/recv but not its connect(), so an address that drops packets rather
    # than refusing them — a firewall, a downed VPN, a host that is simply gone —
    # stalls on the TCP connect timeout instead (measured: 2m30s), and every
    # configure would pay it.
    set(_timeoutSeconds 10)

    # NO_STATS keeps the probe out of `fastcache-cc --show-stats`, where it would
    # read as a build that never hits. FASTCACHE_TIMEOUT bounds a daemon that accepts
    # the connection and then stalls; builds keep the launcher's own default.
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                ${_fc_fastcache_env}
                "FASTCACHE_VERBOSE=1"
                "FASTCACHE_NO_STATS=1"
                "FASTCACHE_TIMEOUT=2s"
                "${FASTCACHE_CC}" "${CMAKE_CXX_COMPILER}" ${_args}
        WORKING_DIRECTORY "${_dir}"
        TIMEOUT ${_timeoutSeconds}
        RESULT_VARIABLE _rc
        OUTPUT_QUIET
        ERROR_VARIABLE _err)

    if(_rc MATCHES "[Tt]imeout")
        set(${reasonVar} "no answer within ${_timeoutSeconds}s" PARENT_SCOPE)
    elseif(NOT _rc EQUAL 0)
        set(${reasonVar} "probe compile failed (${_rc})" PARENT_SCOPE)
    elseif(_err MATCHES "fastcache-cc: (HIT|MISS) key=")
        set(${outVar} TRUE PARENT_SCOPE)
        set(${reasonVar} "" PARENT_SCOPE)
    elseif(_err MATCHES "fastcache-cc: (cache unavailable|not caching) \\(([^)]*)\\)")
        # Two lead-ins, because the launcher distinguishes "the cache let us down"
        # from "this compile is deliberately not cacheable" and says which. Both
        # end in a probe that did not cache, which is all this has to report.
        set(${reasonVar} "${CMAKE_MATCH_2}" PARENT_SCOPE)
    else()
        set(${reasonVar} "no cache outcome reported" PARENT_SCOPE)
    endif()
endfunction()

# Candidate table, most-preferred first. Each row <id> is described by:
#   _fc_cache_<id>_label     human-readable name for the status message
#   _fc_cache_<id>_program   the found program (empty when not installed)
#   _fc_cache_<id>_requires  extra condition; the row is skipped when falsy
#   _fc_cache_<id>_requires_note  what to say when `requires` is falsy while the
#                            program IS installed -- a launcher sitting right
#                            there and not being used is the silence #815 is
#                            about. Empty for a row whose absence explains
#                            itself, which is why fastcache-cc has none: an
#                            emptied FASTCACHE_ADDR is a documented opt-out (#372)
#   _fc_cache_<id>_env       NAME=VALUE pairs to inject around the invocation
#   _fc_cache_<id>_check     function deciding usability at configure time
#                            (empty when being installed is enough); called as
#                            <fn>(<outVar> <reasonVar>) and only for a row that
#                            already passed program and requires
#   _fc_cache_<id>_detail    extra words for the status message (empty for none)
#   _fc_cache_<id>_caveat    a correctness hazard this launcher carries, warned
#                            about when it is the row that WINS (empty for none)
#   _fc_cache_<id>_predicts  a regex over THIS row's rejection reason; when it
#                            matches, the rejection is warned about rather than
#                            merely reported, and the winner's caveat -- if it has
#                            one -- is folded into the same message (empty when no
#                            reason predicts anything) -- see
#                            `_fc_cache_rejection_warning` (#658, #815)
#   _fc_cache_<id>_predicts_detail  what to tell the operator about fixing this
#                            row, printed with that warning
# Supporting a fourth launcher is adding an id here plus its ten variables.
set(_fc_cache_candidates fastcache_cc sccache ccache)

# Render "<label>[ <detail>]" for a row, so a launcher and where it points are
# named the same way whether it won or was passed over. Diagnosing a daemon that
# did not answer starts with knowing which address was tried.
# @param id Row id from _fc_cache_candidates.
# @param outVar Receives the rendered text.
function(_fc_cache_describe id outVar)
    set(_text "${_fc_cache_${id}_label}")
    if(_fc_cache_${id}_detail)
        string(APPEND _text " ${_fc_cache_${id}_detail}")
    endif()
    set(${outVar} "${_text}" PARENT_SCOPE)
endfunction()

set(_fc_cache_fastcache_cc_label "fastcache-cc")
set(_fc_cache_fastcache_cc_program "${FASTCACHE_CC}")
set(_fc_cache_fastcache_cc_requires "${FASTCACHE_ADDR}")
# No note: an empty FASTCACHE_ADDR is the documented way to opt out of this row
# (#372), so a machine that took it does not need telling what it just asked for.
set(_fc_cache_fastcache_cc_requires_note "")
set(_fc_cache_fastcache_cc_env ${_fc_fastcache_env})
set(_fc_cache_fastcache_cc_check _fc_probe_fastcache_cc)
# A WIRE VERSION mismatch, and only that. "not installed", "no answer" and "the
# probe was uncacheable" predict nothing about the replacement, and warning on
# them is how a warning becomes one people learn to skip.
#
# `unsupported-version` is the launcher's own spelling, from the wire error table
# in `Protocol/CompileCacheWire.hpp`. Nothing connects the two, so a rename there
# disarms this silently -- narrower than the display-sentence parsing it replaced,
# and recorded rather than hidden.
set(_fc_cache_fastcache_cc_predicts "unsupported-version")
# Names the remedy, and names the DIRECTION, which is the step #815's acceptance
# clause 2 says the refusal stops short of. `unsupported wire version 3; this
# server speaks 1..1` is accurate and leaves the reader to work out which end is
# behind -- and the sentence is written by the DAEMON, so an old daemon will go on
# sending the old wording forever and cannot be fixed there. It is read here.
set(_fc_cache_fastcache_cc_predicts_detail
    "A daemon IS answering at ${FASTCACHE_ADDR} and it is one you installed; it is simply out of step with this fastcache-cc, so this build is not using the cache you think it is. Read the refusal above as `unsupported wire version <N>; this server speaks <lo>..<hi>`: <N> is what this launcher speaks and <lo>..<hi> is what the daemon accepts, so an <hi> below <N> means the DAEMON is the older of the two and is the one to replace. Compare `fastcache-cc --version` against the daemon's, and check which binary the service actually starts -- a packaged daemon on PATH ahead of a hand-built launcher is how #815 was found. This is a normal state during a rollout, which is why it is a warning and not a refusal to configure.")
set(_fc_cache_fastcache_cc_detail "at ${FASTCACHE_ADDR}")
set(_fc_cache_fastcache_cc_caveat "")

set(_fc_cache_sccache_label "sccache")
set(_fc_cache_sccache_program "${SCCACHE}")
# Opt-in, never automatic (#815). The column already existed, so this is a value
# and not a branch: the row is skipped exactly as any other unsatisfied row is,
# and opting in restores it in its original position -- above ccache, because
# asking for sccache is asking for sccache.
set(_fc_cache_sccache_requires "${ALLOW_SCCACHE_FALLBACK}")
set(_fc_cache_sccache_requires_note
    "not selected automatically; sccache is opt-in here, so pass -DALLOW_SCCACHE_FALLBACK=ON to use it (#815)")
set(_fc_cache_sccache_env "")
set(_fc_cache_sccache_check "")
set(_fc_cache_sccache_predicts "")
set(_fc_cache_sccache_predicts_detail "")
set(_fc_cache_sccache_detail "")
# Not a `check`, because the row stays usable: this is a hazard a developer has to
# be able to weigh, not a condition this module can evaluate. Nothing here can
# tell whether the cache about to be used is shared with another checkout.
#
# Carried only where the hazard exists, which is a property of the COMPILER
# rather than of sccache alone. Measured both ways, because warning where it
# cannot happen is how a warning becomes one people learn to skip:
#
#   * MSVC and clang-cl are exposed. sccache preprocesses them with `/EP`, which
#     emits no `#line` markers, so the text it hashes carries no paths at all and
#     two checkouts hash identically. `/showIncludes` then reports ABSOLUTE paths,
#     so the dependency stream replayed into the second checkout names the first.
#     Measured on fastcached: two worktrees at one commit, 137 cross-worktree
#     hits, 1097 recorded dependency edges pointing into the wrong tree and none
#     into its own, and `ninja: no work to do` after a real edit to a header.
#
#   * GCC and Clang are not. Their preprocessed output carries `# n "path"` line
#     markers, so with the absolute include paths CMake generates the hashed text
#     differs between checkouts and there is no hit to replay (measured on Ubuntu
#     24.04: 0 hits, 2 misses). Spell the same compile with relative paths and
#     there IS a hit -- but then the depfile is relative too and resolves inside
#     the consuming tree, which is correct. Self-consistent either way.
#
# Both languages are tested, not just CXX: this module wires
# CMAKE_C_COMPILER_LAUNCHER as well, so a `project(x LANGUAGES C)` on MSVC is
# exposed exactly as much and would otherwise be the one configuration that gets
# sccache with no word about it.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"
   OR CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
    set(_fc_cache_sccache_caveat
        "Under MSVC and clang-cl, sccache replays a cache hit's /showIncludes stream verbatim -- the ABSOLUTE paths spelled by the build that STORED it -- while the text it hashes to find that hit carries no paths at all, because it preprocesses with /EP and /EP emits no line markers. Two checkouts therefore share entries and then record each other's headers as their dependencies. Editing a header in the checkout you are building rebuilds nothing: the build stays green and the objects are stale.

It bites an INCREMENTAL build across two checkouts sharing one sccache cache. A clean build has no dependency graph to corrupt, and checkouts that all sit at the same absolute path replay paths that are correct -- CI is normally both, and unaffected. GCC and Clang are unaffected everywhere: their preprocessed output carries the paths, so the two checkouts do not share entries in the first place.

To avoid it: drop -DALLOW_SCCACHE_FALLBACK, which is the only reason sccache was selected here at all -- it is opt-in since #815 and this build asked for it; or install fastcache-cc and run a fastcached daemon, which rewrites a hit's paths into the consuming checkout and so does not have this failure mode; or pass -DSCCACHE= to fall through to ccache, which is unaffected; or configure with -DUSE_COMPILER_CACHE=OFF.")
else()
    set(_fc_cache_sccache_caveat "")
endif()

set(_fc_cache_ccache_label "ccache")
set(_fc_cache_ccache_program "${CCACHE}")
set(_fc_cache_ccache_requires ON)
set(_fc_cache_ccache_requires_note "")
set(_fc_cache_ccache_env "")
set(_fc_cache_ccache_check "")
set(_fc_cache_ccache_predicts "")
set(_fc_cache_ccache_predicts_detail "")
set(_fc_cache_ccache_detail "")
# ccache's default `base_dir` is empty, which is documented to mean it does not
# rewrite absolute paths and so does not share entries between checkouts -- the
# precondition the sccache hazard above needs. Setting `base_dir` deliberately
# opts into that sharing, which is a choice its own documentation covers.
set(_fc_cache_ccache_caveat "")

set(_fc_cache_chosen "")
set(_fc_cache_rejected "")
if(USE_COMPILER_CACHE)
    foreach(_id IN LISTS _fc_cache_candidates)
        if(NOT _fc_cache_${_id}_program)
            continue()
        endif()
        # A row whose `requires` is falsy while its PROGRAM is right there is a
        # launcher present and not used, and saying nothing about it would rebuild
        # #815's shape one row over. Rows whose absence explains itself carry no
        # note and stay silent.
        if(NOT _fc_cache_${_id}_requires)
            if(_fc_cache_${_id}_requires_note)
                _fc_cache_describe("${_id}" _fc_cache_desc)
                list(APPEND _fc_cache_rejected "${_fc_cache_desc}: ${_fc_cache_${_id}_requires_note}")
            endif()
            continue()
        endif()
        if(_fc_cache_${_id}_check)
            cmake_language(CALL ${_fc_cache_${_id}_check} _fc_cache_usable _fc_cache_why_not)
            if(NOT _fc_cache_usable)
                # Remember why, so a fall-through to a slower launcher explains
                # itself rather than looking like the faster one was never there.
                _fc_cache_describe("${_id}" _fc_cache_desc)
                list(APPEND _fc_cache_rejected "${_fc_cache_desc}: ${_fc_cache_why_not}")
                # Kept per row rather than only inside the joined sentence: the
                # warning below has to ask what the REASON was, and parsing it back
                # out of display text is how a message rewording silently disables a
                # warning (#658).
                set(_fc_cache_${_id}_rejected_why "${_fc_cache_why_not}")
                continue()
            endif()
        endif()
        set(_fc_cache_chosen "${_id}")
        break()
    endforeach()
endif()

# Say why a preferred launcher was passed over, whatever the outcome: falling
# through in silence looks exactly like it never being installed.
foreach(_rejection IN LISTS _fc_cache_rejected)
    message(STATUS "[cache] Not using ${_rejection}")
endforeach()

# ...and WARN about the ones whose reason says a warning is warranted.
#
# **Outside `if(_fc_cache_chosen)`, and that placement is the #815 half.** This
# loop used to live inside it, so the case where NOTHING replaced the rejected
# launcher -- the build compiles uncached, which is the loudest of the three --
# produced no message at all. The winner is now an INPUT to the decision rather
# than a precondition for reaching it: see `_fc_cache_rejection_warning`.
#
# Over the CANDIDATES rather than one named row, so a launcher that gains a
# `predicts` column is covered without editing this.
if(_fc_cache_chosen)
    _fc_cache_describe("${_fc_cache_chosen}" _fc_cache_winner_desc)
    set(_fc_cache_winner_caveat "${_fc_cache_${_fc_cache_chosen}_caveat}")
else()
    set(_fc_cache_winner_desc "")
    set(_fc_cache_winner_caveat "")
endif()
foreach(_id IN LISTS _fc_cache_candidates)
    if(NOT DEFINED _fc_cache_${_id}_rejected_why)
        continue()
    endif()
    _fc_cache_describe("${_id}" _fc_cache_rejected_desc)
    _fc_cache_rejection_warning(
        "${_fc_cache_${_id}_rejected_why}"
        "${_fc_cache_${_id}_predicts}"
        "${_fc_cache_rejected_desc}"
        "${_fc_cache_winner_desc}"
        "${_fc_cache_winner_caveat}"
        "${_fc_cache_${_id}_predicts_detail}"
        _fc_cache_rejection_text)
    if(_fc_cache_rejection_text)
        message(WARNING "${_fc_cache_rejection_text}")
    endif()
endforeach()

if(_fc_cache_chosen)
    set(_fc_cache_program "${_fc_cache_${_fc_cache_chosen}_program}")

    # `cmake -E env NAME=VALUE ... <program>` is the only way to attach
    # environment to a compiler launcher; rows without env invoke the program
    # directly so they pay no extra process.
    if(_fc_cache_${_fc_cache_chosen}_env)
        set(_fc_cache_launcher
            "${CMAKE_COMMAND}" -E env
            ${_fc_cache_${_fc_cache_chosen}_env}
            "${_fc_cache_program}")
    else()
        set(_fc_cache_launcher "${_fc_cache_program}")
    endif()

    _fc_cache_describe("${_fc_cache_chosen}" _fc_cache_desc)
    message(STATUS "[cache] Enabling ${_fc_cache_desc} (${_fc_cache_program}) for C/C++ compilation")

    # A launcher that can silently produce a WRONG build says so, at the moment a
    # build is opted into it. A warning rather than a status line because the
    # symptom arrives hours later and somewhere else entirely -- a stale object,
    # a green build, and a crash in code nobody touched -- so the one line naming
    # it has to still be findable in the log afterwards.
    #
    # Never fatal, and never a `check` that skips the row: which launcher to run
    # is the developer's call, and a module vendored into other repositories does
    # not get to make it for them. It only has to make it an informed one.
    #
    # It is the SECOND warning, not the first: the rejection warnings above have
    # already said why the safe launcher is not being used, and this says what the
    # replacement costs. Two messages eighty lines apart were #658's defect and
    # that ordering is what fixed it.
    if(_fc_cache_${_fc_cache_chosen}_caveat)
        message(WARNING "[cache] ${_fc_cache_${_fc_cache_chosen}_caveat}")
    endif()

    set(CMAKE_C_COMPILER_LAUNCHER ${_fc_cache_launcher})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${_fc_cache_launcher})

    # None of the launchers reproduces anything but the object file on a cache
    # hit, so a precompiled header (a second, separately produced artefact)
    # cannot be served from cache.
    set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)

    # CMake's C++20 module support puts scanning flags on every compile line
    # (-fmodules-ts -fmodule-mapper=<per-object modmap> on GCC). A preprocess-only
    # run with those flags fails, so a launcher that derives its key by
    # preprocessing falls back on *every* translation unit. This project has no
    # module units, so the scan is pure overhead; turn it off while a launcher is
    # in use.
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

    # For the same reason none of them supports /Zi (shared PDB). Force MSVC to
    # embed debug info in .obj files (/Z7) via the modern CMake knob (CMP0141),
    # and also fix up any legacy /Zi already present in FLAGS_DEBUG /
    # FLAGS_RELWITHDEBINFO.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        set(CMAKE_POLICY_DEFAULT_CMP0141 NEW)
        set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
        foreach(_var
                CMAKE_CXX_FLAGS_DEBUG
                CMAKE_C_FLAGS_DEBUG
                CMAKE_CXX_FLAGS_RELWITHDEBINFO
                CMAKE_C_FLAGS_RELWITHDEBINFO)
            string(REGEX REPLACE "([-/])Zi" "\\1Z7" ${_var} "${${_var}}")
        endforeach()
    endif()

    # --- make the objects relocatable, where the driver can be asked to -------
    #
    # A cache hit replays an object BUILT SOMEWHERE ELSE, and a compiler with
    # debug info on records where it was built: DWARF's `DW_AT_comp_dir` is the
    # working directory, which appears on no command line and which no key can
    # relativize. So a replayed object names the producing checkout, a debugger
    # looks for sources in a tree this machine does not have, and nothing fails.
    # Issue #203; issue #489 is the same defect approached from the key end.
    #
    # Measured on one TU, byte-identical in two roots differing only in name,
    # against a same-root baseline of 0 differing bytes:
    #
    #   driver      debug info off   debug info on    prefix-map
    #   g++         identical        differs, 143 B   -> identical
    #   clang++     identical        differs,   6 B   -> identical
    #   clang-cl    identical        differs,  23 B   -> STILL 23 B
    #   cl          differs, 11 B    differs, 28-31 B  no such switch exists
    #
    # Hence GNU spellings only, and hence the honest gap: on COFF a replayed
    # object's debug records name the producing checkout and no flag closes it.
    # `-ffile-prefix-map` does not help clang-cl -- CodeView's `S_OBJNAME` and the
    # embedded `-cc1` line are not remapped -- and `cl` has no path-map switch at
    # all. `.agent/rules/compile-cache.md` records that as an accepted cost.
    #
    # `-fdebug-prefix-map` and not `-ffile-prefix-map`, which also implies
    # `-fmacro-prefix-map` and would rewrite `__FILE__`. That buys nothing here:
    # the preprocessor expands `__FILE__` and the cache key hashes preprocessed
    # output raw, so whenever the expansion is checkout-dependent it is
    # checkout-dependent in the hashed text too and the two checkouts never share
    # a key. Changing program-visible strings to fix a defect that cannot occur is
    # a bad trade.
    #
    # The mapping MUST be identical on every machine sharing the cache, or two
    # producers write different objects. That is not left as advice: the launcher
    # relativizes the flag's root and leaves its replacement literal, so a machine
    # mapping somewhere else computes a different key and misses rather than
    # mis-serving (`PathValueRole::PrefixMap`).
    #
    # --- what this flag costs the FALLBACK launchers, per launcher (#816) ------
    #
    # `fastcache-cc` is unaffected, for the reason in the paragraph above. `ccache`
    # and `sccache` hash the command line, so the roots below appear in their keys
    # and cross-checkout reuse is at stake. The answer is NOT the same for the two,
    # so it is recorded per launcher rather than as one sentence about "the
    # fallbacks".
    #
    # Measured 2026-09-10, ccache 4.9.1 / sccache 0.7.7, clang++ 22.1.8, ext4 under
    # WSL2 Ubuntu-24.04. Conditions pinned rather than pointed at: they describe one
    # host at one instant and must not silently start describing another. Every arm
    # carries BOTH controls -- an identical recompile that must HIT and an `-O0` to
    # `-O2` change that must MISS -- because a MISS means nothing from a harness that
    # cannot be seen to hit, and a HIT means nothing from one that cannot be seen to
    # miss. An earlier run of this measurement used `-DX=1` as its must-miss control
    # on a source that never mentions `X`; ccache's preprocessed mode hashed
    # identical text and hit, correctly, and the control was the thing that was wrong.
    #
    #   arrangement                              ccache 4.9.1      sccache 0.7.7
    #   two build dirs, ONE source tree          HIT (as shipped)  MISS
    #   two WORKTREES, as shipped                MISS              MISS
    #   two WORKTREES, CCACHE_BASEDIR set        HIT               n/a
    #
    # So: **ccache has a lever and sccache has none.** `CCACHE_BASEDIR`, pointed at a
    # directory containing the worktrees, rewrites the absolute paths -- these roots
    # included -- to CWD-relative form before hashing, and cross-worktree reuse then
    # works. It is a CLIENT setting (the environment variable, or `base_dir` in
    # ccache.conf) and cannot be set from here: a compile line cannot configure the
    # cache that is about to read it. `hash_dir=false` was measured alongside and
    # added nothing once `base_dir` was set.
    #
    # `sccache` 0.7.7 exposes no equivalent -- nothing in `--help` rewrites, relativises
    # or bases paths -- so for sccache this IS an accepted cost: **the prefix-map makes
    # cross-checkout reuse impossible, and that is the price of an object that names no
    # checkout.** Written down here because an accepted cost that is recorded is a
    # decision and one that is not is a defect waiting to be rediscovered. CI's fallback
    # is sccache and CI builds each preset in its own directory, so this caps CI's hit
    # rate structurally rather than by tuning.
    #
    # Removing the flag to buy hit rate is NOT the trade (#319, #506, and the
    # `comp_dir` work): it exists so a replayed object names no checkout.
    #
    # And the reuse that ccache does give is SOUND rather than merely fast, which was
    # checked rather than assumed -- the hazard being a hit that serves an object
    # naming the producing worktree, which is #660's shape. Read out of the served
    # object with `readelf --debug-dump=info`, never by comparing bytes:
    #
    #   w1 cold, stores           DW_AT_name ../src/u.cpp   comp_dir .
    #   w2 served cross-worktree  DW_AT_name ../src/u.cpp   comp_dir .
    #
    # Zero occurrences of the producing worktree's name in the served object. Note
    # that a cache-OFF build of the same source records `./u.cpp` instead, because
    # `base_dir` hands the compiler a relative input path: both spellings are
    # checkout-independent and neither leaks a tree, but objects built under
    # `CCACHE_BASEDIR` are not byte-identical to objects built without it. Compare
    # cold against warm, never control against warm.
    #
    # Never fatal, per this file's contract. `check_<lang>_compiler_flag` is stock
    # CMake and reports rather than refuses, which matters because a bad flag left
    # in CMAKE_<LANG>_FLAGS fails the compiler ABI check and takes the whole
    # configure down -- exactly what this module may not do.
    #
    # Per LANGUAGE throughout, because the C and C++ compilers need not be the
    # same product -- which the previous shape asserted in a comment while asking
    # `CMAKE_CXX_COMPILER_ID` once and applying the answer to both.
    _fc_debug_prefix_map_rules("${CMAKE_BINARY_DIR}" "${CMAKE_SOURCE_DIR}"
                               _fc_prefix_maps _fc_source_mapped)
    list(TRANSFORM _fc_prefix_maps PREPEND "-fdebug-prefix-map=" OUTPUT_VARIABLE _fc_prefix_map_flags)
    list(JOIN _fc_prefix_map_flags " " _fc_prefix_map_flags)

    # A language this project has not ENABLED is asked nothing: `check_compiler_flag`
    # is a hard `CMake Error` ("C: needs to be enabled before use") otherwise, which
    # fails the configure -- the one thing this file may not do. Measured rather than
    # anticipated: this module is included from a `project()` that lists CXX before C
    # is added, and the first run of the check ended the configure outright. A
    # language nobody enabled also has no compile lines to put a flag on, so skipping
    # it is right rather than merely safe.
    get_property(_fc_enabled_languages GLOBAL PROPERTY ENABLED_LANGUAGES)
    set(_fc_prefix_map_langs "")
    set(_fc_prefix_map_skipped "")
    include(CheckCompilerFlag)
    foreach(_lang IN ITEMS C CXX)
        if(NOT "${_lang}" IN_LIST _fc_enabled_languages)
            continue()
        endif()

        # The MSVC family is excluded by NAME rather than left to the probe. `cl`
        # does not reject an unknown `-f...` outright -- it reports D9002 and exits
        # 0 -- so the check would succeed and the flag would be appended to a
        # compiler that ignores it, on the one platform where this cannot work at
        # all. `clang-cl` matches "Clang" and is excluded by the simulate-id clause,
        # which is what the 23-byte measurement above is about.
        if(NOT CMAKE_${_lang}_COMPILER_ID MATCHES "GNU|Clang"
           OR CMAKE_${_lang}_SIMULATE_ID STREQUAL "MSVC")
            list(APPEND _fc_prefix_map_skipped "${_lang} (no path-map switch on this driver)")
            continue()
        endif()

        # `check_compiler_flag` rather than the per-language modules, and cached in
        # `FASTCACHE_<LANG>_HAS_DEBUG_PREFIX_MAP`, so a reconfigure pays nothing.
        check_compiler_flag(${_lang} "-fdebug-prefix-map=/a=/b" FASTCACHE_${_lang}_HAS_DEBUG_PREFIX_MAP)
        if(FASTCACHE_${_lang}_HAS_DEBUG_PREFIX_MAP)
            string(APPEND CMAKE_${_lang}_FLAGS " ${_fc_prefix_map_flags}")
            list(APPEND _fc_prefix_map_langs ${_lang})
        else()
            list(APPEND _fc_prefix_map_skipped "${_lang} (driver rejected the flag)")
        endif()
    endforeach()

    # Say which languages got it AND which did not, rather than only the happy
    # half: a driver that cannot take the flag still caches, still shares, and
    # still replays objects naming another checkout. Applied, rejected, no such
    # switch, and not enabled are four different situations, and a line reporting
    # only the first reads identically in all of them.
    if(_fc_prefix_map_langs)
        list(JOIN _fc_prefix_map_langs "/" _fc_prefix_map_langs)
        message(STATUS "[cache] Mapping debug paths for ${_fc_prefix_map_langs} so replayed objects "
                       "name no checkout: ${_fc_prefix_map_flags}")
        if(NOT _fc_source_mapped)
            message(STATUS "[cache] The source root is NOT mapped: the build tree lies outside it, so the "
                           "relative path back would carry the checkout's own path")
        endif()
        # #816: these roots are in ccache's and sccache's keys, because both hash the
        # command line. ccache has a lever and sccache has none -- the measurement and
        # the reasoning are in the long comment beside the flag above.
        #
        # Said HERE and only when it is ACTIONABLE, which means when ccache is the
        # launcher that actually won AND the setting it needs is absent. A line printed
        # unconditionally would be advice for sccache users who cannot act on it and for
        # `fastcache-cc` users who never had the problem, and a caveat that does not
        # apply to the reader is one the reader learns to skip.
        #
        # STATUS rather than WARNING: nothing is wrong. A build directory that never
        # shares with another checkout loses nothing by leaving it unset, and this file
        # may not fail a configure in any case.
        if(_fc_cache_chosen STREQUAL "ccache" AND NOT DEFINED ENV{CCACHE_BASEDIR})
            message(STATUS "[cache] ccache will not reuse these objects across checkouts: the roots above are "
                           "in its key. Set CCACHE_BASEDIR to a directory containing your worktrees to "
                           "recover that (measured on ccache 4.9.1); it is a client setting, so a compile "
                           "line cannot set it here")
        endif()
    endif()
    if(_fc_prefix_map_skipped)
        list(JOIN _fc_prefix_map_skipped ", " _fc_prefix_map_skipped)
        message(STATUS "[cache] Debug paths NOT mapped for ${_fc_prefix_map_skipped}; replayed objects "
                       "will carry the producing checkout's paths in their debug info")
    endif()
else()
    # Define the launchers as empty rather than leaving them unset. Fetched
    # dependencies bring their own cache modules that auto-enable ccache when the
    # launcher is merely *undefined* (libunicode's cmake/EnableCcache.cmake does
    # exactly that), which would quietly re-enable caching for their targets.
    # An empty definition is inert for us and keeps USE_COMPILER_CACHE=OFF honest.
    set(CMAKE_C_COMPILER_LAUNCHER "")
    set(CMAKE_CXX_COMPILER_LAUNCHER "")

    if(NOT USE_COMPILER_CACHE)
        message(STATUS "[cache] Compiler caching disabled by USE_COMPILER_CACHE=OFF")
    elseif(_fc_cache_rejected)
        # Something WAS found and passed over, and the `Not using` line(s) above
        # have already named which and why -- so this must not claim nothing was
        # found. It used to name sccache and ccache outright, which stopped being
        # true the moment sccache could be installed and merely not opted into.
        message(STATUS "[cache] No compiler-cache launcher was usable; caching disabled "
                       "(the `Not using` line(s) above say why each was passed over)")
    else()
        message(STATUS "[cache] No compiler-cache launcher found (fastcache-cc, sccache, ccache); caching disabled")
    endif()
endif()
