// Rive -> ThorVG adapter: the rive::Factory half.
//
// Rive's C++ runtime does not rasterise anything itself. It asks a Factory to
// create paths, paints, gradients and images, and then issues draw calls on a
// Renderer. This Factory creates objects that keep enough state to be turned
// into ThorVG paints. ThorvgRenderer records draw calls as immutable
// snapshots (PathData, PaintState) so ThorVG objects can be kept between
// frames and only updated where something changed.
#pragma once

#include "rive/factory.hpp"
#include "rive/math/raw_path.hpp"
#include "rive/renderer.hpp"

#include <thorvg.h>

#include <vector>

namespace rive_thorvg
{

// Immutable ThorVG form of a path at one point in time. A new PathData is
// created whenever the path changes, so pointer equality means "same
// geometry".
struct PathData : public rive::RefCnt<PathData>
{
  std::vector<tvg::PathCommand> cmds;
  std::vector<tvg::Point> pts;
  tvg::FillRule fillRule = tvg::FillRule::NonZero;
  // Bounding box of all points (control points included, so it contains the
  // curve), in path space.
  float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;

  bool empty() const { return cmds.empty(); }
  // Replaces the shape's path with this one (keeps its fill and stroke).
  void applyTo(tvg::Shape* shape) const;
};

// A Rive path, kept as a RawPath and lazily converted to a PathData the
// first time it is drawn after a change.
class ThorvgRenderPath : public rive::RenderPath
{
public:
  ThorvgRenderPath() = default;
  ThorvgRenderPath(rive::RawPath& rawPath, rive::FillRule fillRule);

  // rive::CommandPath
  void rewind() override;
  void fillRule(rive::FillRule value) override;
  void moveTo(float x, float y) override;
  void lineTo(float x, float y) override;
  void cubicTo(float ox, float oy, float ix, float iy, float x, float y)
      override;
  void close() override;

  // rive::RenderPath
  void addRenderPath(const rive::RenderPath* path,
                     const rive::Mat2D& transform) override;
  void addRawPath(const rive::RawPath& path) override;

  // Adapter helpers
  bool empty() const { return m_rawPath.empty(); }
  rive::FillRule getFillRule() const { return m_fillRule; }
  // The current geometry. Rebuilt (as a new object) only after a change.
  rive::rcp<PathData> data();

private:
  void markDirty() { m_data = nullptr; }

  rive::RawPath m_rawPath;
  rive::FillRule m_fillRule = rive::FillRule::nonZero;
  rive::rcp<PathData> m_data;
};

// Linear, radial or angular (conic) gradient. Immutable once created; a fresh
// tvg::Fill is produced for every draw because ThorVG paints own their fills.
class ThorvgRenderShader : public rive::RenderShader
{
public:
  enum class Kind
  {
    linear,
    radial,
    conic
  };

  ThorvgRenderShader(Kind kind,
                     const float params[4],
                     const rive::ColorInt colors[],
                     const float stops[],
                     size_t count);

  // Creates a new tvg::Fill. Ownership passes to the caller (normally a
  // tvg::Shape via fill()/strokeFill()).
  tvg::Fill* makeFill(const rive::Mat2D& shaderTransform,
                      bool hasShaderTransform) const;

  // True when every colour stop is fully opaque.
  bool opaque() const;

private:
  Kind m_kind;
  float m_params[4]; // linear: sx sy ex ey | radial: cx cy r - | conic: -
  std::vector<tvg::Fill::ColorStop> m_stops;
};

// Everything about a paint that affects how a draw looks, captured at draw
// time. Compared between frames to decide whether a ThorVG shape needs its
// fill / stroke updated.
struct PaintState
{
  bool stroke = false;
  rive::ColorInt color = 0xFF000000;
  float thickness = 1.0f;
  rive::StrokeJoin join = rive::StrokeJoin::miter;
  rive::StrokeCap cap = rive::StrokeCap::butt;
  rive::BlendMode blendMode = rive::BlendMode::srcOver;
  float additiveness = 0.0f;
  rive::rcp<rive::RenderShader> shader; // immutable; compared by pointer
  rive::Mat2D shaderTransform;
  bool hasShaderTransform = false;

  bool operator==(const PaintState& o) const
  {
    return stroke == o.stroke && color == o.color &&
           thickness == o.thickness && join == o.join && cap == o.cap &&
           blendMode == o.blendMode && additiveness == o.additiveness &&
           shader == o.shader && shaderTransform == o.shaderTransform &&
           hasShaderTransform == o.hasShaderTransform;
  }
  bool operator!=(const PaintState& o) const { return !(*this == o); }
};

class ThorvgRenderPaint : public rive::RenderPaint
{
public:
  void style(rive::RenderPaintStyle value) override { m_style = value; }
  void color(rive::ColorInt value) override { m_color = value; }
  void thickness(float value) override { m_thickness = value; }
  void join(rive::StrokeJoin value) override { m_join = value; }
  void cap(rive::StrokeCap value) override { m_cap = value; }
  void blendMode(rive::BlendMode value) override { m_blendMode = value; }
  void additiveness(float value) override { m_additiveness = value; }
  void shader(rive::rcp<rive::RenderShader> value) override;
  void shaderTransform(const rive::Mat2D& value) override;
  void invalidateStroke() override {}
  // feather(): not supported by ThorVG's shape API (soft edges are ignored).

  bool isStroke() const { return m_style == rive::RenderPaintStyle::stroke; }
  rive::ColorInt getColor() const { return m_color; }
  float getThickness() const { return m_thickness; }
  rive::StrokeJoin getJoin() const { return m_join; }
  rive::StrokeCap getCap() const { return m_cap; }
  rive::BlendMode getBlendMode() const { return m_blendMode; }
  float getAdditiveness() const { return m_additiveness; }
  const ThorvgRenderShader* getShader() const;
  const rive::Mat2D& getShaderTransform() const { return m_shaderTransform; }
  bool hasShaderTransform() const { return m_hasShaderTransform; }
  PaintState snapshot() const;

private:
  rive::RenderPaintStyle m_style = rive::RenderPaintStyle::fill;
  rive::ColorInt m_color = 0xFF000000;
  float m_thickness = 1.0f;
  rive::StrokeJoin m_join = rive::StrokeJoin::miter;
  rive::StrokeCap m_cap = rive::StrokeCap::butt;
  rive::BlendMode m_blendMode = rive::BlendMode::srcOver;
  float m_additiveness = 0.0f;
  rive::rcp<rive::RenderShader> m_shader;
  rive::Mat2D m_shaderTransform;
  bool m_hasShaderTransform = false;
};

// A decoded image. Holds one referenced tvg::Picture that is duplicated for
// each draw (duplicates share the decoded pixels). The decoded pixels are
// converted to the canvas format once, at decode time, so pictures can then
// be drawn from several threads at the same time.
class ThorvgRenderImage : public rive::RenderImage
{
public:
  explicit ThorvgRenderImage(tvg::Picture* picture);
  ~ThorvgRenderImage() override;

  // Returns a new Picture owned by the caller.
  tvg::Picture* makePicture() const;

private:
  tvg::Picture* m_picture;
};

// Plain CPU memory. Only used by Rive for image meshes.
class ThorvgRenderBuffer : public rive::RenderBuffer
{
public:
  ThorvgRenderBuffer(rive::RenderBufferType type,
                     rive::RenderBufferFlags flags,
                     size_t sizeInBytes);
  const uint8_t* data() const { return m_bytes.data(); }

protected:
  void* onMap() override { return m_bytes.data(); }
  void onUnmap() override {}

private:
  std::vector<uint8_t> m_bytes;
};

class ThorvgFactory : public rive::Factory
{
public:
  rive::rcp<rive::RenderBuffer> makeRenderBuffer(rive::RenderBufferType,
                                                 rive::RenderBufferFlags,
                                                 size_t sizeInBytes) override;

  rive::rcp<rive::RenderShader> makeLinearGradient(float sx,
                                                   float sy,
                                                   float ex,
                                                   float ey,
                                                   const rive::ColorInt colors[],
                                                   const float stops[],
                                                   size_t count) override;

  rive::rcp<rive::RenderShader> makeRadialGradient(float cx,
                                                   float cy,
                                                   float radius,
                                                   const rive::ColorInt colors[],
                                                   const float stops[],
                                                   size_t count) override;

  rive::rcp<rive::RenderShader> makeAngularGradient(
      const rive::ColorInt colors[],
      const float stops[],
      size_t count) override;

  rive::rcp<rive::RenderPath> makeRenderPath(rive::RawPath&,
                                             rive::FillRule) override;
  rive::rcp<rive::RenderPath> makeEmptyRenderPath() override;
  rive::rcp<rive::RenderPaint> makeRenderPaint() override;
  rive::rcp<rive::RenderImage> decodeImage(
      rive::Span<const uint8_t> encodedBytes) override;
};

} // namespace rive_thorvg
