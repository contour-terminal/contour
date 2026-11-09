// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <csignal>
#include <expected>
#include <initializer_list>
#include <memory>
#include <system_error>
#include <utility>

#include <spawn.h>

namespace vtpty
{

/// An initialised posix_spawn() attribute object, destroyed when it goes. Made by create(), which reports
/// a failed initialisation, so that there is no unusable one to check for.
/// @tparam T posix_spawn_file_actions_t or posix_spawnattr_t.
/// @tparam Init Its initialiser.
/// @tparam Destroy Its destructor.
template <typename T, int (*Init)(T*), int (*Destroy)(T*)>
class PosixSpawnObject
{
  public:
    /// @return The object, initialised; or why its initialisation failed.
    [[nodiscard]] static std::expected<PosixSpawnObject, std::error_code> create()
    {
        // On the heap, so that moving it never moves the object itself, which POSIX does not allow for.
        auto object = std::make_unique<T>();
        if (auto const error = Init(object.get()); error != 0)
            return std::unexpected(std::error_code { error, std::generic_category() });
        return PosixSpawnObject { std::move(object) };
    }

    ~PosixSpawnObject()
    {
        if (_object)
            Destroy(_object.get());
    }
    PosixSpawnObject(PosixSpawnObject const&) = delete;
    PosixSpawnObject& operator=(PosixSpawnObject const&) = delete;
    PosixSpawnObject(PosixSpawnObject&&) noexcept = default;
    PosixSpawnObject& operator=(PosixSpawnObject&&) = delete;

    /// @return The object, for the posix_spawn*() functions.
    [[nodiscard]] T* get() noexcept { return _object.get(); }

  private:
    /// @param object An initialised object.
    explicit PosixSpawnObject(std::unique_ptr<T> object) noexcept: _object { std::move(object) } {}

    std::unique_ptr<T> _object; ///< Empty only once moved from.
};

/// posix_spawn_file_actions_t, destroyed when it goes.
using SpawnFileActions = PosixSpawnObject<posix_spawn_file_actions_t,
                                          &::posix_spawn_file_actions_init,
                                          &::posix_spawn_file_actions_destroy>;

/// posix_spawnattr_t, destroyed when it goes.
using SpawnAttributes =
    PosixSpawnObject<posix_spawnattr_t, &::posix_spawnattr_init, &::posix_spawnattr_destroy>;

/// @return @p error, a posix_spawn*() result, as an error code.
[[nodiscard]] inline std::error_code spawnError(int error) noexcept
{
    return std::error_code { error, std::generic_category() };
}

/// Starts the spawned process with an empty signal mask and every catchable signal at its default
/// disposition: whatever this process blocks or ignores (a daemon blocks its shutdown signals, a GUI ignores
/// SIGPIPE) is not the child's to inherit.
/// @param attributes The attributes to set them on.
/// @return Nothing; or why they could not be set.
[[nodiscard]] inline std::expected<void, std::error_code> resetSignals(SpawnAttributes& attributes) noexcept
{
    auto none = sigset_t {};
    sigemptyset(&none);
    auto catchable = sigset_t {};
    sigfillset(&catchable);
    sigdelset(&catchable, SIGKILL);
    sigdelset(&catchable, SIGSTOP);
    for (auto const error:
         { ::posix_spawnattr_setsigmask(attributes.get(), &none),
           ::posix_spawnattr_setsigdefault(attributes.get(), &catchable),
           ::posix_spawnattr_setflags(attributes.get(), POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF) })
        if (error != 0)
            return std::unexpected(spawnError(error));
    return {};
}

} // namespace vtpty
