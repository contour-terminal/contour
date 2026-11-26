// SPDX-License-Identifier: Apache-2.0
//
// ThreadSanitizer's default options for both executables that load Qt, contour and contour_gui_test. Linked
// only into a ThreadSanitizer build of them: the runtime applies what __tsan_default_options() returns before
// it reads TSAN_OPTIONS, so a direct run of either applies them, and a caller's TSAN_OPTIONS can override
// them.
//
// ignore_noninstrumented_modules=1. The distro Qt (and the glib and libdbus under it, Qt Multimedia's
// PulseAudio) is not instrumented, so its allocator and teardown traffic shows as races the sanitizer cannot
// order. Qt hands memory between its threads through synchronization it cannot see -- the futex-based QMutex
// and QSemaphore, QObject's connection locks -- so the QML type loader's thread, a queued event or slot
// object posted from NativeController's client thread and freed by the GUI thread's event loop, the thread
// pool and QPainter's parallel span fills (the popup shadow's mask), glib's wakeup eventfd, and a
// QDBusConnection hooking a receiver's destroyed() from its own thread all read as races. The option drops
// the memory accesses of interceptors those libraries call (malloc, free, memcpy, write, ...) and keeps their
// mutex and condition-variable synchronization; Contour's own accesses stay checked, in QThread::run(),
// QThreadPool tasks and queued slots alike.
//
// A suppressions list cannot say this as narrowly. A `race:` entry hides a report when any frame of either
// stack matches, ours included; a `called_from_lib:` entry for libQt6Core also drops the mutex calls Qt
// makes, and with them the ordering of every queued slot -- it turns the hand-offs above into reports of
// their own.
//
// The blind spot: code that runs inside an interceptor called from uninstrumented code runs unchecked -- the
// bodies of std::call_once and std::async through libstdc++, pthread_once and qsort callbacks. A race planted
// in a std::call_once body or a std::async task goes unreported here; one planted in QThread::run(), a
// QThreadPool task or a queued slot does not. So threaded logic that runs in such bodies --
// FontLocatorProvider's and SystemdScopePlacement's std::call_once, the libssh2 initialisation in SshSession
// -- must have its races tested in an unsilenced module suite (text_shaper_test, vtpty_test), not through
// contour_gui_test or the application.

extern "C" char const* threadSanitizerDefaultOptions() __asm__("__tsan_default_options");

/// @return The options ThreadSanitizer applies before TSAN_OPTIONS.
extern "C" char const* threadSanitizerDefaultOptions()
{
    return "ignore_noninstrumented_modules=1";
}
