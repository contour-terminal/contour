// SPDX-License-Identifier: Apache-2.0
#pragma once

// Where processes and signals are watched with kqueue(2): macOS and the BSDs, which have no signalfd, no
// pidfds and no PR_SET_PDEATHSIG. There the keeper hears its signals and its children's exits through one
// kqueue, and the daemon learns of its keeper's exit through another (EVFILT_PROC, NOTE_EXIT). Linux has
// its own descriptors for each, and the remaining POSIX systems neither.
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__) \
    || defined(__OpenBSD__)
    #define CONTOUR_KEEPER_KQUEUE 1

    #include <sys/event.h>
    #include <sys/types.h>
#endif
