// A horizontal slice of the frame rendered by one ThorVG canvas.
//
// Each lane keeps its ThorVG paints between frames. apply() compares the new
// DisplayList with the one it applied last time:
//  - same structure (same draws, clip groups and images in the same order):
//    only the draws whose geometry, transform, paint or opacity changed get a
//    fresh ThorVG paint (in the same place in the tree), so ThorVG only
//    prepares those, and with SmartRender only redraws the screen regions
//    they cover (before and after);
//  - different structure: the lane's paint tree is rebuilt from scratch and
//    the whole slice is redrawn.
//
// Several lanes can render the same frame in parallel (one per CPU core), all
// targeting the same frame buffer with disjoint viewports. The split between
// lanes can be moved (setRows) to balance their work.
//
// Threading: ThorVG's software renderer takes a thread-local memory pool
// when a canvas is created, so a lane must be constructed on, and only used
// from, the thread that renders it.
#pragma once

#include "rive_thorvg/display_list.hpp"

#include <thorvg.h>

#include <cstdint>
#include <vector>

namespace rive_thorvg
{

struct LaneStats
{
  float applyMs = 0.0f; // DisplayList -> ThorVG paint updates
  float prepMs = 0.0f;  // ThorVG update: transform, tessellate, RLE spans
  float fillMs = 0.0f;  // ThorVG draw: pixels
  bool rebuilt = false; // the paint tree was rebuilt this frame
  uint32_t changed = 0; // paints replaced this frame
  uint32_t edited = 0;  // paints updated in place this frame
  uint32_t clips = 0;   // clip shapes that moved or changed this frame
  uint32_t culled = 0;  // draws outside the lane's rows (not built/prepared)
  bool backgroundHidden = false; // the first draw covers the frame opaquely

  float totalMs() const { return applyMs + prepMs + fillMs; }
};

class ThorvgLane
{
public:
  // Renders rows [y0, y1) of width x height premultiplied ARGB8888 frames
  // (stride = width). There is one ThorVG canvas ("slot") per frame buffer
  // in `frames`; with two buffers the caller alternates between them, so one
  // can be read (copied to the display) while the other is being drawn.
  // Each slot only ever renders into its own buffer, so with `partial`
  // (ThorVG SmartRender) it redraws exactly what changed since that buffer
  // was last drawn.
  ThorvgLane(uint32_t* const* frames,
             uint32_t frameCount,
             uint32_t width,
             uint32_t height,
             uint32_t y0,
             uint32_t y1,
             bool partial);
  ~ThorvgLane();

  ThorvgLane(const ThorvgLane&) = delete;
  ThorvgLane& operator=(const ThorvgLane&) = delete;

  // Background drawn behind everything (premultiplied ARGB).
  void setClearColor(uint32_t argb);

  // When on (default), draws whose row range (DrawCmd::yMin/yMax) misses
  // this lane's rows get an empty placeholder instead of a real ThorVG
  // shape, so the lane neither builds nor prepares them.
  void setCulling(bool on) { m_cull = on; }

  // When on (default), the lane's own background rectangle is hidden while
  // the first draw is an opaque, unclipped, source-over fill that covers the
  // whole frame (typically the artboard's background), saving a full-frame
  // fill that would be painted over anyway.
  void setSkipCoveredBackground(bool on) { m_skipCoveredBackground = on; }

  // When on (default), a draw whose path, transform or fill changed (but
  // not its stroke style, blend mode or opacity) updates its existing
  // ThorVG paint instead of getting a new one. ThorVG then keeps what it
  // can: a moved gradient keeps its colour table, a recoloured shape keeps
  // its coverage spans, and nothing is reallocated.
  void setInPlaceUpdates(bool on) { m_inPlace = on; }

  // Moves the lane to rows [y0, y1). Takes effect the next time each slot is
  // rendered (and makes that slot redraw its whole slice once). Call only
  // while the lane is not rendering.
  void setRows(uint32_t y0, uint32_t y1);
  uint32_t y0() const { return m_y0; }
  uint32_t y1() const { return m_y1; }

  // Brings slot `slot`'s paints up to date with `list` ...
  void apply(const DisplayList& list, uint32_t slot);
  // ... and rasterises the changed parts of the slice into its frame buffer.
  void draw(uint32_t slot);

  // Rows redrawn by the last draw(), as [y0, y1); y0 == y1 if none. With
  // two slots these are the rows that changed since that frame buffer was
  // last drawn (two frames ago).
  uint32_t dirtyY0() const { return m_dirtyY0; }
  uint32_t dirtyY1() const { return m_dirtyY1; }

  const LaneStats& stats() const { return m_stats; }

private:
  // One ThorVG canvas and its retained paint tree, for one frame buffer.
  struct Slot
  {
    tvg::SwCanvas* canvas = nullptr;
    uint32_t* frame = nullptr;        // the frame buffer this canvas draws
    tvg::Shape* background = nullptr; // owned by canvas
    bool backgroundHidden = false;    // background opacity is 0
    tvg::Scene* root = nullptr;       // owned by canvas
    uint32_t y0 = 0, y1 = 0;          // rows the canvas viewport covers
    DisplayList applied;              // what the paints currently show
    std::vector<tvg::Paint*> paints;  // one per DrawCmd, owned by the tree
    std::vector<tvg::Scene*> parents; // the scene each paint sits in
    std::vector<uint8_t> culled;      // paints[i] is a placeholder
    // Per clip group, the nested scene of each clip level.
    std::vector<std::vector<tvg::Scene*>> groupScenes;
  };

  void rebuild(Slot& slot, const DisplayList& list);
  void update(Slot& slot, const DisplayList& list);
  static tvg::Paint* makePaint(const DrawCmd& cmd);
  bool outside(const DrawCmd& cmd) const;
  static bool changed(const DrawCmd& now, const DrawCmd& before);
  bool coversFrame(const DisplayList& list) const;
  static bool updateInPlace(tvg::Paint* paint, const DrawCmd& now,
                            const DrawCmd& before);
  bool coversQuad(const PathData& path, const rive::Mat2D& transform) const;

  std::vector<Slot> m_slots;
  uint32_t m_width;
  uint32_t m_height;
  uint32_t m_y0;
  uint32_t m_y1;
  uint32_t m_dirtyY0 = 0;
  uint32_t m_dirtyY1 = 0;
  bool m_cull = true;
  bool m_skipCoveredBackground = true;
  bool m_inPlace = true;
  std::vector<tvg::Region> m_regions; // scratch for dirty-region queries
  LaneStats m_stats;
};

} // namespace rive_thorvg
