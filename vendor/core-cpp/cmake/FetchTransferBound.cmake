# SPDX-License-Identifier: Apache-2.0
#
# Bounds every dependency transfer a configure performs, so one that stops
# delivering ENDS rather than hanging forever (#526).
#
# ---------------------------------------------------------------------------
# What was measured
#
# Three configures wedged in one evening, on two dependencies, across three
# fresh worktrees: lz4 twice and Catch2 once, at 40, 40 and 53 minutes, all of
# them parked in `read()` on a socket that had delivered nothing. From /proc:
#
#     read_bytes: 0   write_bytes: 0    -- and unchanged 8s later
#
# `write_bytes: 0` is what rules the filesystem out. DrvFs is genuinely slow on
# this machine, but a process blocked on slow I/O has *written* something; these
# had written nothing. The network was fine throughout -- `git ls-remote`
# against the same URL answered in under a second in every case. The connection
# established and then went silent; it did not fail.
#
# `http.lowSpeedLimit` and `http.lowSpeedTime` are unset by default, so git
# applies no bound to a transfer that stops delivering, and a stalled connection
# is therefore permanent. This is the rulebook's own rule reached in the build
# system: an unbounded drain does not avoid an ending, it hands the choice to
# the supervisor -- which here was a developer at forty minutes and a parker's
# deadline at fifty.
#
# ---------------------------------------------------------------------------
# Why the numbers are these numbers
#
# The hard half is that a bound must still let a slow-but-working transfer
# finish. A bound that fires on healthy traffic is worse than none: it trades a
# hang, which is visible, for a broken build, which gets blamed on the change
# that happened to be in the tree.
#
# The one signal the evidence gives that separates a wedge from slowness is
# ZERO versus NONZERO -- that is the whole content of `read_bytes: 0`. No
# magnitude calibrates: a transfer throttled by DrvFs and a transfer that has
# died look alike at any threshold above zero. So the rate floor is set as close
# to zero as the mechanism allows (1 byte/second, curl's smallest meaningful
# `CURLOPT_LOW_SPEED_LIMIT`) and the whole judgement is carried by the window.
#
# The window is what a HEALTHY transfer may be silent for. Conditions, rather
# than one number, because a figure quoted without its conditions is how this
# repository has been wrong before:
#
#   | condition                                   | silence     | verdict |
#   |---------------------------------------------|-------------|---------|
#   | server-side pack generation (GitHub         | seconds to  | must    |
#   | enumerating/compressing before first byte)  | tens of s   | survive |
#   | transfer throttled by a slow filesystem     | > 1 B/s by  | must    |
#   | draining the socket (DrvFs, #545)           | construction| survive |
#   | the wedge this file exists for              | 40-53 min   | must be |
#   |                                             | at 0 B/s    | refused |
#
# 120 seconds sits with roughly two-fold headroom over the worst silence a
# healthy transfer plausibly shows, and ends the observed wedge about twenty
# times sooner than the human who ended it. It is a bound on SILENCE, never on
# total duration: a transfer delivering anything at all runs as long as it
# likes, so a genuinely slow clone still completes.
#
# Note the interaction with #545, which points the configure at a populated CPM
# source cache. Without it a fetch lands under `out/build/.../_deps`, on DrvFs
# here -- the only regime in which a healthy transfer could be throttled far
# enough to approach a bound at all. #545 moves it off DrvFs and removes that
# regime. That is an argument for landing #545; it is not a reason to loosen
# this, which is sized to hold without it.
#
# ---------------------------------------------------------------------------
# How it is applied
#
# Two transports fetch DEPENDENCIES, and both take their numbers from here:
#
#   * git, for every `CPMAddPackage` that misses the cache. Bounded through
#     `GIT_HTTP_LOW_SPEED_LIMIT` / `GIT_HTTP_LOW_SPEED_TIME`, which git reads
#     from the environment and which therefore reach every git subprocess
#     FetchContent spawns -- no per-dependency argument, so a dependency added
#     later cannot be added unbounded.
#   * CMake's own libcurl, for the CPM bootstrap's `file(DOWNLOAD)`. That one
#     takes `INACTIVITY_TIMEOUT` as an argument, so it reads the seconds below
#     directly. It is the same defect one step earlier in the same path, and
#     the worse instance of it: it is the first network operation of a fresh
#     configure, and unlike the clone case it leaves no `_deps` directory for
#     anyone to inspect.
#
# What this file is NOT is the only place in the tree that states the policy,
# and saying so would be an overclaim worth exactly nothing. `cmake/portable/
# CompileCache.cmake` downloads the launcher and its release metadata, carries
# its own `INACTIVITY_TIMEOUT` numbers, and runs BEFORE this module is even
# included -- and it must, because it is required to stay stock-CMake-only and
# may not include anything of ours. That duplicate is accepted with its reason
# rather than written around; what closes it is that the guard asserts the
# PROPERTY over the whole tree -- every `file(DOWNLOAD)` anywhere carries an
# inactivity bound -- so a third download added by anyone is caught by the same
# row rather than by somebody remembering these two line numbers.
#
# Two limits on the git half, stated because the rationale above would
# otherwise read as if the mechanism were unconditional:
#
#   * `GIT_HTTP_LOW_SPEED_LIMIT`/`_TIME` are read by git 2.29 and later. On an
#     older git the export is a silent no-op. The guard's bounded leg passes the
#     same variables and requires the clone to END, so an old git turns that
#     guard RED rather than leaving the bound quietly absent.
#   * `http.*` binds the HTTP transport only. A dependency declared over ssh
#     (`git@github.com:...`) would be unbounded; every dependency this project
#     declares is https, and CPM's `GITHUB_REPOSITORY` cannot produce anything
#     else.
#
# The environment variables override a value in the developer's own gitconfig,
# which is deliberate -- this is a project policy about this project's fetches.
# The escape hatch is the cache variables below, not an unbounded default.
#
# Proven by `ctest -R fetch-transfer-bound`, which stands up a listener that
# accepts a connection and answers nothing, and watches both transports refuse
# it -- and watches both of them NOT refuse it with the bound removed, because a
# guard whose subject cannot be made to hang proves nothing about the guard.
# That guard reads these numbers by INCLUDING this file, so nothing here needs
# a printing mode of its own and nothing there restates a value.

include_guard(GLOBAL)

set(FASTCACHED_FETCH_SILENCE_SECONDS 120 CACHE STRING
    "Seconds a dependency transfer may deliver essentially nothing before it is abandoned")
set(FASTCACHED_FETCH_MIN_BYTES_PER_SECOND 1 CACHE STRING
    "Bytes/second below which a dependency transfer counts as delivering nothing")

# Both values are refused unless they are positive integers, and the reason is
# that the two ways of getting this wrong are both SILENT.
#
# Empty is the louder half and still unhelpful: `INACTIVITY_TIMEOUT ${empty}`
# collapses the bootstrap's argument list, and the configure dies inside CPM
# with a CMake argument error naming neither the setting nor the cause.
#
# Zero is the dangerous half. It is how BOTH transports spell *no bound*, so
# `-DFASTCACHED_FETCH_SILENCE_SECONDS=0` restores #526 exactly while every
# structural check still sees a bound being passed -- the token is there, the
# wiring is there, and the number disarms it. A setting whose zero means "off"
# has to say so or refuse it, and there is nothing here that "off" is for.
foreach(fetchBoundSetting
        FASTCACHED_FETCH_SILENCE_SECONDS
        FASTCACHED_FETCH_MIN_BYTES_PER_SECOND)
    if(NOT "${${fetchBoundSetting}}" MATCHES "^[0-9]+$" OR "${${fetchBoundSetting}}" EQUAL 0)
        message(FATAL_ERROR
            "${fetchBoundSetting}=\"${${fetchBoundSetting}}\" is not a positive integer. "
            "It bounds how long a stalled dependency fetch is waited on; zero and empty "
            "both mean no bound at all, which is the defect this file exists to close "
            "(#526). Raise it rather than clearing it.")
    endif()
endforeach()

set(ENV{GIT_HTTP_LOW_SPEED_LIMIT} "${FASTCACHED_FETCH_MIN_BYTES_PER_SECOND}")
set(ENV{GIT_HTTP_LOW_SPEED_TIME} "${FASTCACHED_FETCH_SILENCE_SECONDS}")
