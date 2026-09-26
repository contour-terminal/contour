// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Portable C++23 coroutine generator.
///
/// Prefers the standard `std::generator` (P2502) where the standard library
/// provides it, and otherwise falls back to a minimal, self-contained
/// implementation built on the core `<coroutine>` header. The fallback exists
/// because some standard libraries (notably libc++, including the libc++ 17 of
/// emsdk 3.1.56) ship `<coroutine>` but not yet `<generator>`. Both spellings
/// expose the same `core::Generator<T>` alias so call sites are identical
/// across platforms.
///
/// libstdc++ gets the fallback although it has `<generator>` (GCC 14): GCC's
/// `-Wnull-dereference` reports a null `coroutine_handle` inside libstdc++'s own
/// `std::generator` at `-O2`, which the zero-warning policy makes an error and which
/// no code of core-cpp's can change.
///
/// The choice is made from `<version>`, included first, so every translation unit
/// makes the same one whatever it included before this header: `Generator<T>` is
/// the return type of virtual functions (`core::platform::FileSystem`), and two
/// translation units that disagreed about it would disagree about a vtable.
///
/// The fallback is always defined, as @c detail::GeneratorFallback, so it is tested
/// on every platform, including those whose `Generator` is `std::generator`.

// Define CORE_GENERATOR_FORCE_FALLBACK to make Generator the self-contained fallback even
// when std::generator is available. It has to be defined the same way in every translation
// unit of a program, or they disagree about what Generator<T> is.

#include <version>

#if defined(__cpp_lib_generator) && __cpp_lib_generator >= 202207L && !defined(__GLIBCXX__) \
    && !defined(CORE_GENERATOR_FORCE_FALLBACK)
    #define CORE_GENERATOR_IS_STD_GENERATOR 1
    #include <generator>
#else
    #define CORE_GENERATOR_IS_STD_GENERATOR 0
#endif

#include <coroutine>

#if !defined(__cpp_impl_coroutine) || __cpp_impl_coroutine < 201902L
    #error "core::Generator requires C++20 coroutine language support (__cpp_impl_coroutine)."
#endif

#include <cstddef>
#include <exception>
#include <iterator>
#include <memory>
#include <utility>

namespace core
{

namespace detail
{

    /// Lazy, single-pass coroutine generator yielding values of type @c T: the fallback
    /// @c Generator is where the standard library has no `std::generator`.
    ///
    /// A coroutine returning `GeneratorFallback<T>` produces values lazily via `co_yield`.
    /// The generator is a move-only, input range: iterate it with a range-based for
    /// loop, which resumes the coroutine for each element. Breaking out of the loop
    /// destroys the (suspended) coroutine frame.
    ///
    /// Lifetime invariant: each yielded value lives in the coroutine frame while the
    /// coroutine is suspended at its `co_yield`, so the reference obtained from the
    /// iterator is valid until the next increment. Yield owning values (not views
    /// into a buffer that is overwritten after the yield).
    ///
    /// @tparam T The element type produced by the generator.
    template <typename T>
    class GeneratorFallback
    {
      public:
        /// The coroutine promise that records the currently-yielded value and any
        /// exception escaping the coroutine body. The coroutine machinery refers to
        /// it through the mandatory `promise_type` alias below; the struct itself is
        /// named per the project convention.
        struct PromiseType
        {
            T const* value = nullptr;     ///< Points at the value live in the frame.
            std::exception_ptr exception; ///< Captured exception, rethrown to the consumer.

            /// @return The owning generator wrapping this coroutine.
            GeneratorFallback get_return_object() noexcept
            {
                return GeneratorFallback { std::coroutine_handle<PromiseType>::from_promise(*this) };
            }

            /// Start suspended so the first value is produced on the first iteration.
            [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }

            /// Suspend at the end so the handle stays valid for `done()` queries.
            [[nodiscard]] std::suspend_always final_suspend() const noexcept { return {}; }

            /// Records the address of the yielded value and suspends.
            std::suspend_always yield_value(T const& v) noexcept
            {
                value = std::addressof(v);
                return {};
            }

            void return_void() const noexcept {}

            /// Captures an exception escaping the coroutine body for later rethrow.
            void unhandled_exception() noexcept { exception = std::current_exception(); }

            /// This is a synchronous pull-range; `co_await` is not supported.
            void await_transform() = delete;
        };

        /// Name the coroutine machinery requires; the C++ standard looks up
        /// `GeneratorFallback<T>::promise_type` to drive the coroutine.
        using promise_type = PromiseType;

        /// The handle of the coroutine this generator owns.
        using HandleType = std::coroutine_handle<PromiseType>;

        /// Sentinel marking the end of the sequence.
        struct Sentinel
        {
        };

        /// Input iterator that resumes the coroutine on increment.
        class Iterator
        {
          public:
            using iterator_category = std::input_iterator_tag;
            using value_type = T;
            using difference_type = std::ptrdiff_t;
            using reference = T const&;
            using pointer = T const*;

            Iterator() noexcept = default;

            /// @param handle The coroutine to resume, suspended at the element to read first.
            explicit Iterator(HandleType handle) noexcept: _handle(handle) {}

            /// Resumes the coroutine to its next element.
            Iterator& operator++()
            {
                resume();
                return *this;
            }

            /// Resumes the coroutine to its next element. An input iterator's postfix
            /// increment returns nothing: the prior position no longer exists.
            void operator++(int) { resume(); }

            /// @return The element the coroutine is suspended at.
            [[nodiscard]] reference operator*() const noexcept { return *_handle.promise().value; }

            /// @return The element the coroutine is suspended at.
            [[nodiscard]] pointer operator->() const noexcept { return _handle.promise().value; }

            /// @return Whether the coroutine has run to its end.
            [[nodiscard]] bool operator==(Sentinel /*end*/) const noexcept
            {
                return !_handle || _handle.done();
            }

          private:
            void resume()
            {
                _handle.resume();
                if (_handle.done())
                    rethrowIfFailed(_handle);
            }

            HandleType _handle {};
        };

        GeneratorFallback() noexcept = default;

        /// @param handle The coroutine this generator takes ownership of.
        explicit GeneratorFallback(HandleType handle) noexcept: _handle(handle) {}

        GeneratorFallback(GeneratorFallback&& other) noexcept: _handle(std::exchange(other._handle, {})) {}

        GeneratorFallback& operator=(GeneratorFallback&& other) noexcept
        {
            if (this != &other)
            {
                if (_handle)
                    _handle.destroy();
                _handle = std::exchange(other._handle, {});
            }
            return *this;
        }

        GeneratorFallback(GeneratorFallback const&) = delete;
        GeneratorFallback& operator=(GeneratorFallback const&) = delete;

        ~GeneratorFallback()
        {
            if (_handle)
                _handle.destroy();
        }

        /// Resumes to the first element and returns an iterator to it.
        [[nodiscard]] Iterator begin()
        {
            if (_handle)
            {
                _handle.resume();
                if (_handle.done())
                    rethrowIfFailed(_handle);
            }
            return Iterator { _handle };
        }

        /// @return The sentinel every iterator of a finished coroutine equals.
        [[nodiscard]] Sentinel end() noexcept { return {}; }

      private:
        /// Rethrows an exception captured by the coroutine body, if any.
        static void rethrowIfFailed(HandleType handle)
        {
            if (auto& e = handle.promise().exception)
                std::rethrow_exception(e);
        }

        HandleType _handle {};
    };

} // namespace detail

/// Lazy, single-pass coroutine generator yielding values of type @c T:
/// `std::generator<T>` where the standard library has it, @c detail::GeneratorFallback
/// otherwise.
#if CORE_GENERATOR_IS_STD_GENERATOR
template <typename T>
using Generator = std::generator<T>;
#else
template <typename T>
using Generator = detail::GeneratorFallback<T>;
#endif

} // namespace core
