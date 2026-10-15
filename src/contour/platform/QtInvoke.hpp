// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QObject>
#include <QtCore/Qt>

#include <type_traits>
#include <utility>

// Whether this translation unit is built with ThreadSanitizer: GCC says so with __SANITIZE_THREAD__,
// clang with __has_feature(thread_sanitizer). Local to this header; undefined at its end.
#ifdef __SANITIZE_THREAD__
    #define CONTOUR_POSTED_CALL_TSAN
#endif
#ifdef __has_feature
    #if __has_feature(thread_sanitizer)
        #define CONTOUR_POSTED_CALL_TSAN
    #endif
#endif

#ifdef CONTOUR_POSTED_CALL_TSAN
    #include <sanitizer/tsan_interface.h>
#endif

namespace contour::platform
{

namespace detail
{
    /// Tells ThreadSanitizer that what this thread wrote so far happens before a later
    /// acquiredAt() of the same @p address on another thread. A no-op without the sanitizer.
    /// @param address The address both sides name.
    inline void releasedAt([[maybe_unused]] void* address) noexcept
    {
#ifdef CONTOUR_POSTED_CALL_TSAN
        __tsan_release(address);
#endif
    }

    /// The receiving half of releasedAt(). A no-op without the sanitizer.
    /// @param address The address both sides name.
    inline void acquiredAt([[maybe_unused]] void* address) noexcept
    {
#ifdef CONTOUR_POSTED_CALL_TSAN
        __tsan_acquire(address);
#endif
    }

    /// A functor on its way through Qt's posted-event queue to another thread.
    ///
    /// The posting thread writes the functor's captures, the receiving thread reads them, and what
    /// orders the two is the mutex of the receiving thread's posted-event list: postEvent() appends
    /// under it, and sendPostedEvents() takes the event out under it before it is delivered. That is a
    /// QMutex inside libQt6Core, which is not instrumented and locks through a futex rather than a
    /// pthread call, so ThreadSanitizer never sees it and would take every capture for a race. This
    /// holder declares that ordering itself: it releases its own address once its captures are written,
    /// and acquires it before they are read or destroyed. Without the sanitizer it is the functor and
    /// nothing more.
    template <typename F>
    class PostedCall
    {
      public:
        explicit PostedCall(F fun): _fun { std::move(fun) } { releasedAt(this); }
        PostedCall(PostedCall&& other) noexcept(std::is_nothrow_move_constructible_v<F>):
            _fun { std::move(other._fun) }
        {
            releasedAt(this);
        }
        PostedCall(PostedCall const&) = delete;
        PostedCall& operator=(PostedCall const&) = delete;
        PostedCall& operator=(PostedCall&&) = delete;
        ~PostedCall() { acquiredAt(this); }

        /// Runs the functor, on the receiving thread.
        void operator()()
        {
            acquiredAt(this);
            _fun();
        }

      private:
        F _fun;
    };
} // namespace detail

/// Posts a functor to be executed on the event loop of the thread that owns @p obj.
///
/// Uses QMetaObject::invokeMethod with Qt::QueuedConnection to ensure the functor
/// is invoked via a QMetaCallEvent, which Qt's event loop processes promptly. Every queued
/// functor in Contour goes through here, so that ThreadSanitizer sees the hand-over.
template <typename F>
void postToObject(QObject* obj, F fun)
{
    QMetaObject::invokeMethod(obj, detail::PostedCall<F> { std::move(fun) }, Qt::QueuedConnection);
}

} // namespace contour::platform

#undef CONTOUR_POSTED_CALL_TSAN
