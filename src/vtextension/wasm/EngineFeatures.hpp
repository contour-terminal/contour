// SPDX-License-Identifier: Apache-2.0
//
// The WebAssembly features an extension may use: the one table every engine is configured from, so
// that each accepts exactly the modules the others accept. The backend contract holds every engine
// to it.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string_view>

namespace vtextension::wasm
{

/// A WebAssembly feature beyond the MVP, as engines switch them.
enum class EngineFeature : uint8_t
{
    BulkMemory,
    SignExtension,
    SaturatingFloatToInt,
    MutableGlobals,
    MultiValue,
    ReferenceTypes,
    Gc,
    Exceptions,
    Threads,
    Memory64,
    MultiMemory,
    TailCalls,
    Simd,
    RelaxedSimd,
    ComponentModel,
    ExtendedConst,
    FunctionReferences,
    RelaxedSimdDeterministic,
    WideArithmetic,
    BranchHinting,
    CustomPageSizes,
    StackSwitching,
    ComponentModelAsync,
    ComponentModelAsyncBuiltins,
    ComponentModelAsyncStackful,
    ComponentModelMap,
};

/// Whether extensions may use a feature.
enum class FeatureState : uint8_t
{
    Off, ///< Every engine refuses a module that uses it.
    On,  ///< Every engine accepts it.
};

/// Whether an engine can be told to refuse a feature.
enum class FeatureSwitch : uint8_t
{
    Configurable, ///< Every engine takes a switch for it.
    AlwaysOn,     ///< Wasmtime accepts it unconditionally (its C API has no switch), so it must be On.
};

/// One feature, whether extensions may use it, and its name in reports.
struct FeatureRow
{
    EngineFeature feature {};                                  ///< The feature.
    FeatureState state {};                                     ///< Whether extensions may use it.
    std::string_view name;                                     ///< Its name, as the proposal calls it.
    FeatureSwitch featureSwitch = FeatureSwitch::Configurable; ///< Whether an engine can refuse it.
};

/// The one table both engines configure from and the world check reports by name: one row per
/// EngineFeature, in the enum's order. A feature no engine may be told to refuse is On.
inline constexpr auto EngineFeatures = std::to_array<FeatureRow>({
    { .feature = EngineFeature::BulkMemory, .state = FeatureState::On, .name = "bulk-memory" },
    { .feature = EngineFeature::SignExtension,
      .state = FeatureState::On,
      .name = "sign-extension",
      .featureSwitch = FeatureSwitch::AlwaysOn },
    { .feature = EngineFeature::SaturatingFloatToInt,
      .state = FeatureState::On,
      .name = "saturating-float-to-int",
      .featureSwitch = FeatureSwitch::AlwaysOn },
    { .feature = EngineFeature::MutableGlobals,
      .state = FeatureState::On,
      .name = "mutable-globals",
      .featureSwitch = FeatureSwitch::AlwaysOn },
    { .feature = EngineFeature::MultiValue, .state = FeatureState::On, .name = "multi-value" },
    { .feature = EngineFeature::ReferenceTypes, .state = FeatureState::On, .name = "reference-types" },
    { .feature = EngineFeature::Gc, .state = FeatureState::Off, .name = "gc" },
    { .feature = EngineFeature::Exceptions, .state = FeatureState::Off, .name = "exceptions" },
    { .feature = EngineFeature::Threads, .state = FeatureState::Off, .name = "threads" },
    { .feature = EngineFeature::Memory64, .state = FeatureState::Off, .name = "memory64" },
    { .feature = EngineFeature::MultiMemory, .state = FeatureState::Off, .name = "multi-memory" },
    { .feature = EngineFeature::TailCalls, .state = FeatureState::Off, .name = "tail-call" },
    { .feature = EngineFeature::Simd, .state = FeatureState::Off, .name = "simd" },
    { .feature = EngineFeature::RelaxedSimd, .state = FeatureState::Off, .name = "relaxed-simd" },
    { .feature = EngineFeature::ComponentModel, .state = FeatureState::Off, .name = "component-model" },
    { .feature = EngineFeature::ExtendedConst,
      .state = FeatureState::On,
      .name = "extended-const",
      .featureSwitch = FeatureSwitch::AlwaysOn },
    { .feature = EngineFeature::FunctionReferences,
      .state = FeatureState::Off,
      .name = "function-references" },
    { .feature = EngineFeature::RelaxedSimdDeterministic,
      .state = FeatureState::Off,
      .name = "relaxed-simd-deterministic" },
    { .feature = EngineFeature::WideArithmetic, .state = FeatureState::Off, .name = "wide-arithmetic" },
    { .feature = EngineFeature::BranchHinting, .state = FeatureState::Off, .name = "branch-hinting" },
    { .feature = EngineFeature::CustomPageSizes, .state = FeatureState::Off, .name = "custom-page-sizes" },
    { .feature = EngineFeature::StackSwitching, .state = FeatureState::Off, .name = "stack-switching" },
    { .feature = EngineFeature::ComponentModelAsync,
      .state = FeatureState::Off,
      .name = "component-model-async" },
    { .feature = EngineFeature::ComponentModelAsyncBuiltins,
      .state = FeatureState::Off,
      .name = "component-model-async-builtins" },
    { .feature = EngineFeature::ComponentModelAsyncStackful,
      .state = FeatureState::Off,
      .name = "component-model-async-stackful" },
    { .feature = EngineFeature::ComponentModelMap,
      .state = FeatureState::Off,
      .name = "component-model-map" },
});

/// @return The row of @p feature in EngineFeatures.
[[nodiscard]] constexpr FeatureRow const& featureRow(EngineFeature feature) noexcept
{
    return EngineFeatures[static_cast<std::size_t>(feature)];
}

namespace detail
{
    /// @return Whether EngineFeatures holds one row per feature, in the enum's order, and every feature
    /// an engine cannot refuse is On.
    [[nodiscard]] consteval bool engineFeaturesAreWellFormed() noexcept
    {
        auto const rowIsWellFormed = [](std::size_t index) {
            auto const& row = EngineFeatures[index];
            return static_cast<std::size_t>(row.feature) == index && !row.name.empty()
                   && (row.featureSwitch == FeatureSwitch::Configurable || row.state == FeatureState::On);
        };
        return EngineFeatures.size() == static_cast<std::size_t>(EngineFeature::ComponentModelMap) + 1
               && std::ranges::all_of(std::views::iota(std::size_t { 0 }, EngineFeatures.size()),
                                      rowIsWellFormed);
    }
} // namespace detail

static_assert(detail::engineFeaturesAreWellFormed());

} // namespace vtextension::wasm
