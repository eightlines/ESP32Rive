// Rive -> ThorVG adapter: the rive::Renderer half.
//
// Rive draws in immediate mode (save/restore, transform, clip, draw). This
// renderer does not touch ThorVG; it records each draw as a DrawCmd with
// immutable snapshots of its path, paint, transform, opacity and clip stack:
//
//   renderer.begin(&list);          // clears the list
//   artboard->draw(&renderer);      // Rive issues draw calls
//   renderer.end();
//   lane.apply(list); lane.draw();  // ThorVG (see thorvg_lane.hpp)
//
// Clipping: Rive clips intersect along the save() stack. Consecutive draws that
// share the same clip stack form one ClipGroup, so each clip is rasterised
// once per group, not once per draw.
#pragma once

#include "rive/renderer.hpp"
#include "rive_thorvg/display_list.hpp"

#include <cstdint>
#include <vector>

namespace rive_thorvg
{

class ThorvgRenderer : public rive::Renderer
{
public:
  ThorvgRenderer();
  ~ThorvgRenderer() override;

  ThorvgRenderer(const ThorvgRenderer&) = delete;
  ThorvgRenderer& operator=(const ThorvgRenderer&) = delete;

  // Starts recording into `list` (cleared) and resets transform/clip/opacity.
  void begin(DisplayList* list);
  void end();

  // rive::Renderer
  void save() override;
  void restore() override;
  void transform(const rive::Mat2D& transform) override;
  void drawPath(rive::RenderPath* path, rive::RenderPaint* paint) override;
  void clipPath(rive::RenderPath* path) override;
  void drawImage(const rive::RenderImage*,
                 rive::ImageSampler,
                 rive::BlendMode,
                 float opacity) override;
  void drawImageMesh(const rive::RenderImage*,
                     rive::ImageSampler,
                     rive::rcp<rive::RenderBuffer> vertices_f32,
                     rive::rcp<rive::RenderBuffer> uvCoords_f32,
                     rive::rcp<rive::RenderBuffer> indices_u16,
                     uint32_t vertexCount,
                     uint32_t indexCount,
                     rive::BlendMode,
                     float opacity) override;
  void modulateOpacity(float opacity) override;
  bool currentTransform(rive::Mat2D* out) const override;
  bool currentModulatedOpacity(float* out) const override;

private:
  struct State
  {
    rive::Mat2D transform;
    float opacity = 1.0f;
    size_t clipCount = 0;
    bool clippedOut = false; // an empty clip path was applied
  };

  // Group index for a draw issued now (starts a new group if the clip stack
  // changed since the last clipped draw).
  uint16_t currentGroup();
  void popClipsTo(size_t count);

  DisplayList* m_list = nullptr;
  std::vector<State> m_stack;
  std::vector<ClipRef> m_clips;
  // Bumped whenever the clip stack changes, so a new group gets started.
  uint32_t m_clipVersion = 0;
  uint32_t m_groupVersion = UINT32_MAX;
  uint16_t m_group = 0;
  bool m_warnedMesh = false;
};

} // namespace rive_thorvg
