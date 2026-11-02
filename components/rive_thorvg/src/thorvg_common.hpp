// Internal conversions between Rive and ThorVG types.
#pragma once

#include "rive/math/mat2d.hpp"
#include "rive/math/path_types.hpp"
#include "rive/shapes/paint/blend_mode.hpp"
#include "rive/shapes/paint/color.hpp"
#include "rive/shapes/paint/stroke_cap.hpp"
#include "rive/shapes/paint/stroke_join.hpp"

#include <thorvg.h>

#include <algorithm>
#include <cstdint>

namespace rive_thorvg
{

// Rive's Mat2D is column-major [xx xy yx yy tx ty]:
//   x' = xx*x + yx*y + tx,  y' = xy*x + yy*y + ty
inline tvg::Matrix toTvg(const rive::Mat2D& m)
{
  return tvg::Matrix{m[0], m[2], m[4], m[1], m[3], m[5], 0.0f, 0.0f, 1.0f};
}

inline uint8_t toByte(float v)
{
  return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

inline tvg::FillRule toTvg(rive::FillRule rule)
{
  // ThorVG has no "clockwise" rule; nonZero gives the same result for the
  // paths Rive's editor produces with it.
  return rule == rive::FillRule::evenOdd ? tvg::FillRule::EvenOdd
                                         : tvg::FillRule::NonZero;
}

inline tvg::StrokeJoin toTvg(rive::StrokeJoin join)
{
  switch (join)
  {
    case rive::StrokeJoin::round:
      return tvg::StrokeJoin::Round;
    case rive::StrokeJoin::bevel:
      return tvg::StrokeJoin::Bevel;
    case rive::StrokeJoin::miter:
    default:
      return tvg::StrokeJoin::Miter;
  }
}

inline tvg::StrokeCap toTvg(rive::StrokeCap cap)
{
  switch (cap)
  {
    case rive::StrokeCap::round:
      return tvg::StrokeCap::Round;
    case rive::StrokeCap::square:
      return tvg::StrokeCap::Square;
    case rive::StrokeCap::butt:
    default:
      return tvg::StrokeCap::Butt;
  }
}

// additiveness only matters for BlendMode::additive, where 0 means srcOver
// and 1 means fully additive. ThorVG has no partial mix, so round it.
inline tvg::BlendMethod toTvg(rive::BlendMode mode, float additiveness)
{
  switch (mode)
  {
    case rive::BlendMode::additive:
      return additiveness >= 0.5f ? tvg::BlendMethod::Add
                                  : tvg::BlendMethod::Normal;
    case rive::BlendMode::screen:
      return tvg::BlendMethod::Screen;
    case rive::BlendMode::overlay:
      return tvg::BlendMethod::Overlay;
    case rive::BlendMode::darken:
      return tvg::BlendMethod::Darken;
    case rive::BlendMode::lighten:
      return tvg::BlendMethod::Lighten;
    case rive::BlendMode::colorDodge:
      return tvg::BlendMethod::ColorDodge;
    case rive::BlendMode::colorBurn:
      return tvg::BlendMethod::ColorBurn;
    case rive::BlendMode::hardLight:
      return tvg::BlendMethod::HardLight;
    case rive::BlendMode::softLight:
      return tvg::BlendMethod::SoftLight;
    case rive::BlendMode::difference:
      return tvg::BlendMethod::Difference;
    case rive::BlendMode::exclusion:
      return tvg::BlendMethod::Exclusion;
    case rive::BlendMode::multiply:
      return tvg::BlendMethod::Multiply;
    case rive::BlendMode::hue:
      return tvg::BlendMethod::Hue;
    case rive::BlendMode::saturation:
      return tvg::BlendMethod::Saturation;
    case rive::BlendMode::color:
      return tvg::BlendMethod::Color;
    case rive::BlendMode::luminosity:
      return tvg::BlendMethod::Luminosity;
    case rive::BlendMode::srcOver:
    default:
      return tvg::BlendMethod::Normal;
  }
}

} // namespace rive_thorvg
