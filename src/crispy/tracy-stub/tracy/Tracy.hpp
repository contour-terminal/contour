// SPDX-License-Identifier: Apache-2.0
#pragma once

// No-op stand-in for Tracy's own <tracy/Tracy.hpp>, used when the build is configured with
// CONTOUR_TRACY=OFF -- which is the default, and what every distro, CI and packaging build does.
//
// Call sites use Tracy's macros unconditionally (there is deliberately no first-party profiling
// abstraction), so the include has to resolve in every configuration. cmake/Tracy.cmake puts this
// directory on the include path ONLY when profiling is off; with CONTOUR_TRACY=ON the real header
// is found through Tracy::TracyClient instead and this file is unreachable.
//
// Each no-op keeps its arguments inside an unevaluated `sizeof`: nothing runs, but the expression is
// still parsed and type-checked. Instrumentation that stops compiling therefore breaks the ORDINARY
// build rather than rotting until someone next configures with CONTOUR_TRACY=ON.
//
// Adding a Tracy macro to src/ means adding it here too. scripts/check-tracy-stub.py enforces that,
// and runs as the `check_tracy_stub` ctest gate.

namespace crispy
{

/// Absorbs the arguments of a disabled Tracy macro without evaluating them.
/// @param ... The macro's arguments; type-checked by the compiler, never evaluated.
/// @return An unused value, present only so the call can appear inside @c sizeof.
template <typename... Ts>
constexpr int tracyStubIgnore(Ts const&... /*args*/) noexcept
{
    return 0;
}

} // namespace crispy

#define CONTOUR_TRACY_STUB_IGNORE(...) ((void) sizeof(::crispy::tracyStubIgnore(__VA_ARGS__)))

// A zone declares a name, exactly as the real macros do -- `tracy::ScopedZone
// ___tracy_scoped_zone` there, a constant here. That is deliberate, and it is what makes the stub a
// faithful proxy rather than a merely permissive one: two zone macros in one scope collide, and
// ZoneText/ZoneValue without a zone in scope name an undeclared identifier -- both errors the real
// header gives, now given by the default build too.
//
// `constexpr` and only ever odr-used inside `sizeof`, so no storage is emitted and the no-codegen
// property holds; [[maybe_unused]] because most zones carry no text or value.
#define ZoneScoped [[maybe_unused]] constexpr int ___tracy_scoped_zone = 0
#define ZoneScopedN(name) \
    [[maybe_unused]] constexpr int ___tracy_scoped_zone = (CONTOUR_TRACY_STUB_IGNORE(name), 0)
#define ZoneText(txt, size) CONTOUR_TRACY_STUB_IGNORE(___tracy_scoped_zone, txt, size)
#define ZoneName(txt, size) CONTOUR_TRACY_STUB_IGNORE(___tracy_scoped_zone, txt, size)
#define ZoneValue(value)    CONTOUR_TRACY_STUB_IGNORE(___tracy_scoped_zone, value)

#define FrameMark            ((void) 0)
#define FrameMarkNamed(name) CONTOUR_TRACY_STUB_IGNORE(name)

#define TracyPlot(name, value) CONTOUR_TRACY_STUB_IGNORE(name, value)
#define TracyMessageL(txt)     CONTOUR_TRACY_STUB_IGNORE(txt)

#define TracyLockable(type, varname) type varname
#define LockableBase(type)           type
