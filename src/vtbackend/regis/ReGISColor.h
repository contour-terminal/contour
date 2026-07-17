// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Color.h>
#include <vtbackend/regis/ReGISTables.h>

#include <array>

namespace vtbackend::regis
{

/// Converts a DEC HLS colour to RGB.
///
/// @param hueDeg Hue in degrees (0..360). DEC places blue at 0 deg; the conversion applies the same
///               -120 deg offset the Sixel colour introducer uses, so both protocols agree.
/// @param lightnessPct Lightness as a percentage (0..100).
/// @param saturationPct Saturation as a percentage (0..100).
/// @return The converted RGB colour.
[[nodiscard]] RGBColor hlsToRgb(unsigned hueDeg, unsigned lightnessPct, unsigned saturationPct) noexcept;

/// Converts an RGB triplet given as percentages to an 8-bit RGB colour (the RLogin @c R..G..B.. form).
/// @param redPct,greenPct,bluePct Channel intensities as percentages (0..100).
/// @return The converted RGB colour.
[[nodiscard]] RGBColor rgbPercentToRgb(unsigned redPct, unsigned greenPct, unsigned bluePct) noexcept;

/// Converts a grayscale lightness percentage (0..100, the @c L.. form) to an RGB colour.
/// @param lightnessPct Lightness as a percentage (0..100).
/// @return The gray RGB colour.
[[nodiscard]] RGBColor grayToRgb(unsigned lightnessPct) noexcept;

/// The colour-register map a ReGIS context draws through (VT340: @ref ColorRegisterCount registers).
///
/// Register indices from the wire wrap modulo the register count, matching the VT340 which has a
/// fixed-size map. The map is initialised to the VT340 default palette.
class ReGISColorRegisters
{
  public:
    ReGISColorRegisters() noexcept { reset(); }

    /// Restores the VT340 default palette.
    void reset() noexcept;

    /// @return the colour in register @p index (wrapping modulo @ref count).
    [[nodiscard]] RGBColor at(unsigned index) const noexcept;

    /// Programs register @p index (wrapping modulo @ref count) with @p color.
    void set(unsigned index, RGBColor color) noexcept;

    /// @return the number of colour registers.
    [[nodiscard]] unsigned count() const noexcept { return static_cast<unsigned>(_registers.size()); }

    /// @return the index of the register whose colour is nearest @p color (Euclidean in RGB).
    ///
    /// A colour spec that names an explicit colour (not a bare register index) selects the closest
    /// existing register rather than allocating, matching xterm's behaviour.
    [[nodiscard]] unsigned findClosest(RGBColor color) const noexcept;

  private:
    std::array<RGBColor, ColorRegisterCount> _registers {};
};

} // namespace vtbackend::regis
