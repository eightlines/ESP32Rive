// Fix for gradient shapes under ThorVG's partial redraw (SmartRender).
//
// With partial redraw, ThorVG draws a shape that didn't change once per
// dirty region it touches, passing that region as the bounding box. The
// solid-colour paths clip their spans to it; the gradient paths
// (rasterGradientShape / rasterGradientStroke -> _rasterGradientRle) ignore
// it and draw the whole shape every time. So an unchanged gradient shape is
// drawn several times over its whole area: a translucent or blended
// gradient (e.g. "difference") is applied repeatedly (wrong colour), and an
// opaque one is painted over shapes above it outside the redrawn regions,
// which aren't redrawn (wrong layering). Freshly prepared shapes are drawn
// once over their whole area, which hides the bug.
//
// thorvg_sources.cmake patches those two calls to pass the shape's spans
// through swClipRle() first.
#pragma once

#include "tvgSwCommon.h"

#include <algorithm>

// `rle` restricted to `bbox`: the original when all its spans are inside,
// otherwise a per-thread scratch copy holding the clipped spans (valid
// until the next call on the same thread).
static inline const SwRle* swClipRle(const SwRle* rle, const RenderRegion& bbox)
{
  if (!rle || rle->invalid()) return rle;
  const SwSpan* end;
  const SwSpan* begin = rle->fetch(bbox, &end);
  bool inside = begin == rle->spans.begin() && end == rle->spans.end();
  for (auto s = begin; inside && s < end; ++s) {
    if (s->x < bbox.xMin || s->x + s->len > bbox.xMax) inside = false;
  }
  if (inside) return rle;

  static thread_local SwRle clipped;
  clipped.spans.clear();
  for (auto s = begin; s < end; ++s) {
    int32_t x, len;
    if (!s->fetch(bbox, x, len)) continue;
    SwSpan c = *s;
    c.x = x;
    c.len = len;
    clipped.spans.push(c);
  }
  return &clipped;
}
