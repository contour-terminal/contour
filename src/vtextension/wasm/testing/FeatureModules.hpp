// SPDX-License-Identifier: Apache-2.0
//
// Test-only: one hand-assembled module per WebAssembly feature an engine is checked against, and the
// table naming the feature each one uses. Dependency-free (the standard library and EngineFeatures.hpp
// alone), so that both the backend contract (BackendContract.hpp) and the configure-time WAMR probe
// (cmake/WamrTerminateProbe.cpp) check the very same bytes. Each module is embedded with the WebAssembly
// text it was assembled from.
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include <vtextension/wasm/EngineFeatures.hpp>

namespace vtextension::wasm::testing
{

/// Uses SIMD, which extensions may not.
///
///     (module (func (drop (v128.const i64x2 0 0))))
inline constexpr auto SimdModule = std::to_array<std::uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x04, 0x01, 0x60, 0x00, 0x00, 0x03,
    0x02, 0x01, 0x00, 0x0a, 0x17, 0x01, 0x15, 0x00, 0xfd, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1a, 0x0b,
});

/// Declares a shared memory, which needs threads, which extensions may not use.
///
///     (module (memory 1 1 shared))
inline constexpr auto ThreadsModule = std::to_array<std::uint8_t>({
    0x00,
    0x61,
    0x73,
    0x6d,
    0x01,
    0x00,
    0x00,
    0x00,
    0x05,
    0x04,
    0x01,
    0x03,
    0x01,
    0x01,
});

/// Makes a tail call, which extensions may not.
///
///     (module (func (return_call 0)))
inline constexpr auto TailCallModule = std::to_array<std::uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x04, 0x01, 0x60, 0x00,
    0x00, 0x03, 0x02, 0x01, 0x00, 0x0a, 0x06, 0x01, 0x04, 0x00, 0x12, 0x00, 0x0b,
});

/// Declares two memories, which extensions may not.
///
///     (module (memory 1) (memory 1))
inline constexpr auto MultiMemoryModule = std::to_array<std::uint8_t>({
    0x00,
    0x61,
    0x73,
    0x6d,
    0x01,
    0x00,
    0x00,
    0x00,
    0x05,
    0x05,
    0x02,
    0x00,
    0x01,
    0x00,
    0x01,
});

/// Fills memory with a bulk-memory instruction.
///
///     (module (memory 1) (func (memory.fill (i32.const 0) (i32.const 0) (i32.const 0))))
inline constexpr auto BulkMemoryModule = std::to_array<std::uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x04, 0x01, 0x60, 0x00,
    0x00, 0x03, 0x02, 0x01, 0x00, 0x05, 0x03, 0x01, 0x00, 0x01, 0x0a, 0x0d, 0x01,
    0x0b, 0x00, 0x41, 0x00, 0x41, 0x00, 0x41, 0x00, 0xfc, 0x0b, 0x00, 0x0b,
});

/// Sign-extends a byte.
///
///     (module (func (result i32) (i32.extend8_s (i32.const 1))))
inline constexpr auto SignExtensionModule = std::to_array<std::uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x05, 0x01, 0x60, 0x00, 0x01,
    0x7f, 0x03, 0x02, 0x01, 0x00, 0x0a, 0x07, 0x01, 0x05, 0x00, 0x41, 0x01, 0xc0, 0x0b,
});

/// Converts a float to an integer, saturating.
///
///     (module (func (result i32) (i32.trunc_sat_f32_s (f32.const 1))))
inline constexpr auto SaturatingFloatToIntModule = std::to_array<std::uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f, 0x03,
    0x02, 0x01, 0x00, 0x0a, 0x0b, 0x01, 0x09, 0x00, 0x43, 0x00, 0x00, 0x80, 0x3f, 0xfc, 0x00, 0x0b,
});

/// Exports a mutable global.
///
///     (module (global (export "g") (mut i32) (i32.const 0)))
inline constexpr auto MutableGlobalsModule = std::to_array<std::uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x06, 0x06, 0x01, 0x7f,
    0x01, 0x41, 0x00, 0x0b, 0x07, 0x05, 0x01, 0x01, 0x67, 0x03, 0x00,
});

/// Returns two values.
///
///     (module (func (result i32 i32) (i32.const 1) (i32.const 2)))
inline constexpr auto MultiValueModule = std::to_array<std::uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x06, 0x01, 0x60, 0x00, 0x02, 0x7f,
    0x7f, 0x03, 0x02, 0x01, 0x00, 0x0a, 0x08, 0x01, 0x06, 0x00, 0x41, 0x01, 0x41, 0x02, 0x0b,
});

/// Returns a null function reference.
///
///     (module (func (result funcref) (ref.null func)))
inline constexpr auto ReferenceTypesModule = std::to_array<std::uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x05, 0x01, 0x60, 0x00, 0x01,
    0x70, 0x03, 0x02, 0x01, 0x00, 0x0a, 0x06, 0x01, 0x04, 0x00, 0xd0, 0x70, 0x0b,
});

/// Initializes a global with an extended constant expression.
///
///     (module (global i32 (i32.add (i32.const 1) (i32.const 2))))
inline constexpr auto ExtendedConstModule = std::to_array<std::uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x06, 0x09,
    0x01, 0x7f, 0x00, 0x41, 0x01, 0x41, 0x02, 0x6a, 0x0b,
});

/// A module using one feature beyond the MVP, and no other.
struct FeatureModule
{
    EngineFeature feature {};            ///< The feature it uses.
    std::span<std::uint8_t const> bytes; ///< The module.
};

/// One module per feature the contract checks: each engine accepts exactly those whose feature is On in
/// EngineFeatures, and refuses the others.
inline constexpr auto FeatureModules = std::array {
    FeatureModule { .feature = EngineFeature::BulkMemory, .bytes = BulkMemoryModule },
    FeatureModule { .feature = EngineFeature::SignExtension, .bytes = SignExtensionModule },
    FeatureModule { .feature = EngineFeature::SaturatingFloatToInt, .bytes = SaturatingFloatToIntModule },
    FeatureModule { .feature = EngineFeature::MutableGlobals, .bytes = MutableGlobalsModule },
    FeatureModule { .feature = EngineFeature::MultiValue, .bytes = MultiValueModule },
    FeatureModule { .feature = EngineFeature::ReferenceTypes, .bytes = ReferenceTypesModule },
    FeatureModule { .feature = EngineFeature::ExtendedConst, .bytes = ExtendedConstModule },
    FeatureModule { .feature = EngineFeature::Simd, .bytes = SimdModule },
    FeatureModule { .feature = EngineFeature::Threads, .bytes = ThreadsModule },
    FeatureModule { .feature = EngineFeature::TailCalls, .bytes = TailCallModule },
    FeatureModule { .feature = EngineFeature::MultiMemory, .bytes = MultiMemoryModule },
};

} // namespace vtextension::wasm::testing
