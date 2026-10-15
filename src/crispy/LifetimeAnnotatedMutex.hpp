// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <mutex>

// Whether this translation unit is built with ThreadSanitizer: GCC says so with __SANITIZE_THREAD__,
// clang with __has_feature(thread_sanitizer). Local to this header; undefined at its end.
#ifdef __SANITIZE_THREAD__
    #define CRISPY_LIFETIME_ANNOTATED_MUTEX_TSAN
#endif
#ifdef __has_feature
    #if __has_feature(thread_sanitizer)
        #define CRISPY_LIFETIME_ANNOTATED_MUTEX_TSAN
    #endif
#endif

#ifdef CRISPY_LIFETIME_ANNOTATED_MUTEX_TSAN
    #include <sanitizer/tsan_interface.h>
#endif

namespace crispy
{

/// A std::mutex that tells ThreadSanitizer when it ends.
///
/// libstdc++'s std::mutex has a trivial destructor and never calls pthread_mutex_destroy(), so
/// ThreadSanitizer never learns that one is gone. Its deadlock detector knows a mutex by its address
/// and keeps every lock order it saw there: a new mutex that later takes the address -- a member of an
/// object built on the stack where an earlier one lay, at another offset -- inherits the dead one's
/// orders, and two unrelated mutexes become one node that closes lock-order cycles no thread can
/// deadlock in. This one ends that identity with the object.
///
/// It is a std::mutex, so lock guards take it as before. A std::condition_variable waits only on a
/// std::unique_lock<std::mutex>, so a lock that is waited on must name that type: class template
/// argument deduction would name this one. Without the sanitizer it is a plain std::mutex.
class LifetimeAnnotatedMutex: public std::mutex
{
  public:
    /// An unlocked mutex, as std::mutex's constructor makes it.
    LifetimeAnnotatedMutex() = default;
    LifetimeAnnotatedMutex(LifetimeAnnotatedMutex const&) = delete;
    LifetimeAnnotatedMutex(LifetimeAnnotatedMutex&&) = delete;
    LifetimeAnnotatedMutex& operator=(LifetimeAnnotatedMutex const&) = delete;
    LifetimeAnnotatedMutex& operator=(LifetimeAnnotatedMutex&&) = delete;

    /// Tells ThreadSanitizer the mutex is destroyed, so that it forgets the lock orders it holds for
    /// the address. Nothing without the sanitizer.
#ifdef CRISPY_LIFETIME_ANNOTATED_MUTEX_TSAN
    ~LifetimeAnnotatedMutex() { __tsan_mutex_destroy(native_handle(), 0); }
#else
    ~LifetimeAnnotatedMutex() = default;
#endif
};

} // namespace crispy

#undef CRISPY_LIFETIME_ANNOTATED_MUTEX_TSAN
