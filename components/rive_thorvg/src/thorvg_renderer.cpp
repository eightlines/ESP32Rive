#include "rive_thorvg/thorvg_renderer.hpp"

#include "rive_thorvg/thorvg_factory.hpp"

#include <algorithm>
#include <cstdio>

namespace rive_thorvg
{

namespace
{
// Frame rows covered by the box (x0, y0)-(x1, y1) under `m`, widened by
// `pad` (in box units, e.g. half a stroke width) plus a pixel for
// anti-aliasing.
void rowRange(const rive::Mat2D& m,
              float x0,
              float y0,
              float x1,
              float y1,
              float pad,
              float* outMin,
              float* outMax)
{
  x0 -= pad;
  y0 -= pad;
  x1 += pad;
  y1 += pad;
  const float xs[2] = {x0, x1};
  const float ys[2] = {y0, y1};
  float lo = 0.0f, hi = 0.0f;
  bool first = true;
  for (float x : xs)
  {
    for (float y : ys)
    {
      // y' = xy*x + yy*y + ty  (Mat2D is [xx xy yx yy tx ty])
      float sy = m[1] * x + m[3] * y + m[5];
      lo = first ? sy : std::min(lo, sy);
      hi = first ? sy : std::max(hi, sy);
      first = false;
    }
  }
  *outMin = lo - 1.0f;
  *outMax = hi + 1.0f;
}
} // namespace

ThorvgRenderer::ThorvgRenderer() { m_stack.emplace_back(); }

ThorvgRenderer::~ThorvgRenderer() = default;

void ThorvgRenderer::begin(DisplayList* list)
{
  m_list = list;
  m_list->clear();
  m_stack.clear();
  m_stack.emplace_back();
  m_clips.clear();
  ++m_clipVersion;
  m_groupVersion = UINT32_MAX;
  m_group = 0;
}

void ThorvgRenderer::end()
{
  m_clips.clear();
  m_list = nullptr;
}

void ThorvgRenderer::popClipsTo(size_t count)
{
  if (m_clips.size() > count)
  {
    m_clips.resize(count);
    ++m_clipVersion;
  }
}

void ThorvgRenderer::save() { m_stack.push_back(m_stack.back()); }

void ThorvgRenderer::restore()
{
  if (m_stack.size() <= 1)
  {
    return; // unbalanced restore; keep the root state
  }
  m_stack.pop_back();
  popClipsTo(m_stack.back().clipCount);
}

void ThorvgRenderer::transform(const rive::Mat2D& m)
{
  State& s = m_stack.back();
  s.transform = s.transform * m;
}

void ThorvgRenderer::modulateOpacity(float opacity)
{
  m_stack.back().opacity *= opacity;
}

bool ThorvgRenderer::currentTransform(rive::Mat2D* out) const
{
  *out = m_stack.back().transform;
  return true;
}

bool ThorvgRenderer::currentModulatedOpacity(float* out) const
{
  *out = m_stack.back().opacity;
  return true;
}

void ThorvgRenderer::clipPath(rive::RenderPath* renderPath)
{
  auto* path = static_cast<ThorvgRenderPath*>(renderPath);
  State& s = m_stack.back();
  if (path->empty())
  {
    s.clippedOut = true;
    return;
  }
  m_clips.push_back(ClipRef{path->data(), s.transform});
  s.clipCount = m_clips.size();
  ++m_clipVersion;
}

uint16_t ThorvgRenderer::currentGroup()
{
  if (m_clips.empty())
  {
    return 0;
  }
  if (m_groupVersion != m_clipVersion)
  {
    m_list->groups.push_back(ClipGroup{m_clips});
    m_group = static_cast<uint16_t>(m_list->groups.size() - 1);
    m_groupVersion = m_clipVersion;
  }
  return m_group;
}

void ThorvgRenderer::drawPath(rive::RenderPath* renderPath,
                              rive::RenderPaint* renderPaint)
{
  if (m_list == nullptr)
  {
    return;
  }
  const State& s = m_stack.back();
  auto* path = static_cast<ThorvgRenderPath*>(renderPath);
  auto* paint = static_cast<ThorvgRenderPaint*>(renderPaint);

  DrawCmd cmd;
  cmd.kind = DrawCmd::Kind::path;
  cmd.group = currentGroup();
  cmd.transform = s.transform;
  // Draws that end up invisible are still recorded (at opacity 0), so the
  // list keeps the same structure from frame to frame and the ThorVG tree
  // can be updated in place instead of rebuilt.
  cmd.opacity = s.clippedOut ? 0.0f : s.opacity;
  cmd.path = path->data();
  cmd.paint = paint->snapshot();
  // Strokes reach half their width beyond the path; miter joins up to the
  // miter limit (4) times that.
  const float pad = cmd.paint.stroke ? cmd.paint.thickness * 2.0f : 0.0f;
  rowRange(cmd.transform, cmd.path->minX, cmd.path->minY, cmd.path->maxX,
           cmd.path->maxY, pad, &cmd.yMin, &cmd.yMax);
  m_list->cmds.push_back(std::move(cmd));
}

void ThorvgRenderer::drawImage(const rive::RenderImage* renderImage,
                               rive::ImageSampler sampler,
                               rive::BlendMode blendMode,
                               float opacity)
{
  if (m_list == nullptr || renderImage == nullptr)
  {
    return;
  }
  const State& s = m_stack.back();
  DrawCmd cmd;
  cmd.kind = DrawCmd::Kind::image;
  cmd.group = currentGroup();
  // Rive draws an image with its top-left corner at the origin, at its
  // natural size, under the current transform.
  cmd.transform = s.transform;
  cmd.opacity = s.clippedOut ? 0.0f : s.opacity * opacity;
  cmd.image = rive::ref_rcp(const_cast<rive::RenderImage*>(renderImage));
  cmd.nearest = sampler.filter == rive::ImageFilter::nearest;
  rowRange(cmd.transform, 0.0f, 0.0f, float(renderImage->width()),
           float(renderImage->height()), 0.0f, &cmd.yMin, &cmd.yMax);
  cmd.imageBlend = blendMode;
  m_list->cmds.push_back(std::move(cmd));
}

void ThorvgRenderer::drawImageMesh(const rive::RenderImage*,
                                   rive::ImageSampler,
                                   rive::rcp<rive::RenderBuffer>,
                                   rive::rcp<rive::RenderBuffer>,
                                   rive::rcp<rive::RenderBuffer>,
                                   uint32_t,
                                   uint32_t,
                                   rive::BlendMode,
                                   float)
{
  // ThorVG 1.x has no textured-triangle API, so mesh-deformed images are
  // skipped for now.
  if (!m_warnedMesh)
  {
    std::printf("rive_thorvg: drawImageMesh is not supported yet; "
                "mesh-deformed images will not be drawn\n");
    m_warnedMesh = true;
  }
}

} // namespace rive_thorvg
