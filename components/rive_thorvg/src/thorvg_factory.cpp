#include "rive_thorvg/thorvg_factory.hpp"

#include "thorvg_common.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rive_thorvg
{

// ---- ThorvgRenderPath ------------------------------------------------------

ThorvgRenderPath::ThorvgRenderPath(rive::RawPath& rawPath,
                                   rive::FillRule fillRule)
    : m_fillRule(fillRule)
{
  // Factory::makeRenderPath allows taking over the caller's arrays.
  m_rawPath.swap(rawPath);
}

void ThorvgRenderPath::rewind()
{
  m_rawPath.rewind();
  markDirty();
}

void ThorvgRenderPath::fillRule(rive::FillRule value)
{
  if (value != m_fillRule)
  {
    m_fillRule = value;
    markDirty();
  }
}

void ThorvgRenderPath::moveTo(float x, float y)
{
  m_rawPath.moveTo(x, y);
  markDirty();
}

void ThorvgRenderPath::lineTo(float x, float y)
{
  m_rawPath.injectImplicitMoveIfNeeded();
  m_rawPath.line({x, y});
  markDirty();
}

void ThorvgRenderPath::cubicTo(float ox,
                               float oy,
                               float ix,
                               float iy,
                               float x,
                               float y)
{
  m_rawPath.injectImplicitMoveIfNeeded();
  m_rawPath.cubic({ox, oy}, {ix, iy}, {x, y});
  markDirty();
}

void ThorvgRenderPath::close()
{
  m_rawPath.close();
  markDirty();
}

void ThorvgRenderPath::addRenderPath(const rive::RenderPath* path,
                                     const rive::Mat2D& transform)
{
  auto* other = static_cast<const ThorvgRenderPath*>(path);
  m_rawPath.addPath(other->m_rawPath, &transform);
  markDirty();
}

void ThorvgRenderPath::addRawPath(const rive::RawPath& path)
{
  m_rawPath.addPath(path, nullptr);
  markDirty();
}

rive::rcp<PathData> ThorvgRenderPath::data()
{
  if (m_data != nullptr)
  {
    return m_data;
  }
  auto d = rive::make_rcp<PathData>();
  auto verbs = m_rawPath.verbs();
  auto points = m_rawPath.points();
  d->cmds.reserve(verbs.size());
  d->pts.reserve(points.size());
  d->fillRule = toTvg(m_fillRule);

  size_t pi = 0;
  rive::Vec2D last{0.0f, 0.0f};
  for (rive::PathVerb verb : verbs)
  {
    switch (verb)
    {
      case rive::PathVerb::move:
        last = points[pi++];
        d->cmds.push_back(tvg::PathCommand::MoveTo);
        d->pts.push_back({last.x, last.y});
        break;
      case rive::PathVerb::line:
        last = points[pi++];
        d->cmds.push_back(tvg::PathCommand::LineTo);
        d->pts.push_back({last.x, last.y});
        break;
      case rive::PathVerb::quad:
      {
        // ThorVG only has cubics: elevate the quadratic.
        rive::Vec2D c = points[pi];
        rive::Vec2D e = points[pi + 1];
        pi += 2;
        rive::Vec2D c1 = last + (c - last) * (2.0f / 3.0f);
        rive::Vec2D c2 = e + (c - e) * (2.0f / 3.0f);
        d->cmds.push_back(tvg::PathCommand::CubicTo);
        d->pts.push_back({c1.x, c1.y});
        d->pts.push_back({c2.x, c2.y});
        d->pts.push_back({e.x, e.y});
        last = e;
        break;
      }
      case rive::PathVerb::cubic:
        d->cmds.push_back(tvg::PathCommand::CubicTo);
        for (int i = 0; i < 3; ++i)
        {
          const rive::Vec2D& p = points[pi++];
          d->pts.push_back({p.x, p.y});
        }
        last = points[pi - 1];
        break;
      case rive::PathVerb::close:
        d->cmds.push_back(tvg::PathCommand::Close);
        break;
    }
  }
  if (!d->pts.empty())
  {
    d->minX = d->maxX = d->pts[0].x;
    d->minY = d->maxY = d->pts[0].y;
    for (const tvg::Point& p : d->pts)
    {
      d->minX = std::min(d->minX, p.x);
      d->maxX = std::max(d->maxX, p.x);
      d->minY = std::min(d->minY, p.y);
      d->maxY = std::max(d->maxY, p.y);
    }
  }
  m_data = std::move(d);
  return m_data;
}

void PathData::applyTo(tvg::Shape* shape) const
{
  shape->reset(); // drops the old path; fill and stroke settings are kept
  if (!cmds.empty())
  {
    shape->appendPath(cmds.data(),
                      static_cast<uint32_t>(cmds.size()),
                      pts.data(),
                      static_cast<uint32_t>(pts.size()));
  }
  shape->fillRule(fillRule);
}

// ---- ThorvgRenderShader ----------------------------------------------------

ThorvgRenderShader::ThorvgRenderShader(Kind kind,
                                       const float params[4],
                                       const rive::ColorInt colors[],
                                       const float stops[],
                                       size_t count)
    : m_kind(kind)
{
  std::memcpy(m_params, params, sizeof(m_params));
  m_stops.reserve(count);
  for (size_t i = 0; i < count; ++i)
  {
    tvg::Fill::ColorStop s;
    s.offset = stops[i];
    s.r = static_cast<uint8_t>(rive::colorRed(colors[i]));
    s.g = static_cast<uint8_t>(rive::colorGreen(colors[i]));
    s.b = static_cast<uint8_t>(rive::colorBlue(colors[i]));
    s.a = static_cast<uint8_t>(rive::colorAlpha(colors[i]));
    m_stops.push_back(s);
  }
}

bool ThorvgRenderShader::opaque() const
{
  for (const tvg::Fill::ColorStop& s : m_stops)
  {
    if (s.a != 255)
    {
      return false;
    }
  }
  return !m_stops.empty();
}

tvg::Fill* ThorvgRenderShader::makeFill(const rive::Mat2D& shaderTransform,
                                        bool hasShaderTransform) const
{
  tvg::Fill* fill = nullptr;
  switch (m_kind)
  {
    case Kind::linear:
    {
      auto* g = tvg::LinearGradient::gen();
      g->linear(m_params[0], m_params[1], m_params[2], m_params[3]);
      fill = g;
      break;
    }
    case Kind::radial:
    {
      auto* g = tvg::RadialGradient::gen();
      g->radial(m_params[0], m_params[1], m_params[2], m_params[0],
                m_params[1], 0.0f);
      fill = g;
      break;
    }
    case Kind::conic:
    {
      // Rive's angular gradient sweeps around the origin, starting on +x;
      // position, rotation and scale come from the paint's shader transform.
      auto* g = tvg::ConicGradient::gen();
      g->conic(0.0f, 0.0f, 0.0f);
      fill = g;
      break;
    }
  }
  fill->colorStops(m_stops.data(), static_cast<uint32_t>(m_stops.size()));
  fill->spread(tvg::FillSpread::Pad);
  if (hasShaderTransform)
  {
    fill->transform(toTvg(shaderTransform));
  }
  return fill;
}

// ---- ThorvgRenderPaint -----------------------------------------------------

void ThorvgRenderPaint::shader(rive::rcp<rive::RenderShader> value)
{
  m_shader = std::move(value);
  // A new shader starts untransformed; Rive sets shaderTransform() after
  // shader() when it needs one (angular gradients).
  m_shaderTransform = rive::Mat2D();
  m_hasShaderTransform = false;
}

void ThorvgRenderPaint::shaderTransform(const rive::Mat2D& value)
{
  m_shaderTransform = value;
  m_hasShaderTransform = value != rive::Mat2D();
}

const ThorvgRenderShader* ThorvgRenderPaint::getShader() const
{
  return static_cast<const ThorvgRenderShader*>(m_shader.get());
}

PaintState ThorvgRenderPaint::snapshot() const
{
  PaintState p;
  p.stroke = isStroke();
  p.color = m_color;
  p.thickness = m_thickness;
  p.join = m_join;
  p.cap = m_cap;
  p.blendMode = m_blendMode;
  p.additiveness = m_additiveness;
  p.shader = m_shader;
  p.shaderTransform = m_shaderTransform;
  p.hasShaderTransform = m_hasShaderTransform;
  return p;
}

// ---- ThorvgRenderImage -----------------------------------------------------

ThorvgRenderImage::ThorvgRenderImage(tvg::Picture* picture)
    : m_picture(picture)
{
  m_picture->ref();
  float w = 0.0f, h = 0.0f;
  m_picture->size(&w, &h);
  m_Width = static_cast<int>(w);
  m_Height = static_cast<int>(h);
}

ThorvgRenderImage::~ThorvgRenderImage() { m_picture->unref(); }

tvg::Picture* ThorvgRenderImage::makePicture() const
{
  return static_cast<tvg::Picture*>(m_picture->duplicate());
}

// ---- ThorvgRenderBuffer ----------------------------------------------------

ThorvgRenderBuffer::ThorvgRenderBuffer(rive::RenderBufferType type,
                                       rive::RenderBufferFlags flags,
                                       size_t sizeInBytes)
    : rive::RenderBuffer(type, flags, sizeInBytes), m_bytes(sizeInBytes)
{}

// ---- ThorvgFactory ---------------------------------------------------------

rive::rcp<rive::RenderBuffer> ThorvgFactory::makeRenderBuffer(
    rive::RenderBufferType type,
    rive::RenderBufferFlags flags,
    size_t sizeInBytes)
{
  return rive::make_rcp<ThorvgRenderBuffer>(type, flags, sizeInBytes);
}

rive::rcp<rive::RenderShader> ThorvgFactory::makeLinearGradient(
    float sx,
    float sy,
    float ex,
    float ey,
    const rive::ColorInt colors[],
    const float stops[],
    size_t count)
{
  const float params[4] = {sx, sy, ex, ey};
  return rive::make_rcp<ThorvgRenderShader>(
      ThorvgRenderShader::Kind::linear, params, colors, stops, count);
}

rive::rcp<rive::RenderShader> ThorvgFactory::makeRadialGradient(
    float cx,
    float cy,
    float radius,
    const rive::ColorInt colors[],
    const float stops[],
    size_t count)
{
  const float params[4] = {cx, cy, radius, 0.0f};
  return rive::make_rcp<ThorvgRenderShader>(
      ThorvgRenderShader::Kind::radial, params, colors, stops, count);
}

rive::rcp<rive::RenderShader> ThorvgFactory::makeAngularGradient(
    const rive::ColorInt colors[],
    const float stops[],
    size_t count)
{
  const float params[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  return rive::make_rcp<ThorvgRenderShader>(
      ThorvgRenderShader::Kind::conic, params, colors, stops, count);
}

rive::rcp<rive::RenderPath> ThorvgFactory::makeRenderPath(
    rive::RawPath& rawPath,
    rive::FillRule fillRule)
{
  return rive::make_rcp<ThorvgRenderPath>(rawPath, fillRule);
}

rive::rcp<rive::RenderPath> ThorvgFactory::makeEmptyRenderPath()
{
  return rive::make_rcp<ThorvgRenderPath>();
}

rive::rcp<rive::RenderPaint> ThorvgFactory::makeRenderPaint()
{
  return rive::make_rcp<ThorvgRenderPaint>();
}

// ThorVG converts a decoded image to the canvas pixel format (and
// premultiplies it) in place, the first time it is drawn. Do that now, on one
// thread, by drawing it once into a 1x1 canvas, so the render lanes can later
// draw duplicates of it concurrently without racing on that conversion.
static void warmUp(tvg::Picture* picture)
{
  uint32_t pixel = 0;
  tvg::SwCanvas* canvas = tvg::SwCanvas::gen();
  if (canvas == nullptr)
  {
    return;
  }
  canvas->target(&pixel, 1, 1, 1, tvg::ColorSpace::ARGB8888);
  canvas->add(picture->duplicate());
  canvas->draw(false);
  canvas->sync();
  delete canvas;
}

rive::rcp<rive::RenderImage> ThorvgFactory::decodeImage(
    rive::Span<const uint8_t> encodedBytes)
{
  tvg::Picture* picture = tvg::Picture::gen();
  // Empty mime type: ThorVG sniffs PNG / JPEG / WebP itself. copy=true because
  // Rive may free the encoded bytes after this call.
  auto result =
      picture->load(reinterpret_cast<const char*>(encodedBytes.data()),
                    static_cast<uint32_t>(encodedBytes.size()),
                    "",
                    nullptr,
                    true);
  if (result != tvg::Result::Success)
  {
    std::printf("rive_thorvg: image decode failed (%u bytes, result %d)\n",
                static_cast<unsigned>(encodedBytes.size()),
                static_cast<int>(result));
    tvg::Paint::rel(picture);
    return nullptr;
  }
  warmUp(picture);
  return rive::make_rcp<ThorvgRenderImage>(picture);
}

} // namespace rive_thorvg
