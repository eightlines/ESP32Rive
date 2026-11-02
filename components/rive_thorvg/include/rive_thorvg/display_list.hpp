// One frame of Rive drawing, recorded as plain data.
//
// ThorvgRenderer fills a DisplayList while Rive draws; ThorvgLane turns it
// into ThorVG paints. Keeping the two apart lets each lane keep its ThorVG
// objects between frames and update only what changed, and lets several
// lanes (one per CPU core) render the same frame.
#pragma once

#include "rive/math/mat2d.hpp"
#include "rive/renderer.hpp"
#include "rive_thorvg/thorvg_factory.hpp"

#include <cstdint>
#include <vector>

namespace rive_thorvg
{

struct DrawCmd
{
  enum class Kind : uint8_t
  {
    path,
    image
  };

  Kind kind = Kind::path;
  // Clip group (index into DisplayList::groups); 0 = not clipped.
  uint16_t group = 0;
  rive::Mat2D transform;
  // Modulated opacity from the renderer (times the image opacity for images).
  float opacity = 1.0f;
  // Conservative range of frame rows the draw can touch (from its bounds,
  // transform and stroke width). Lanes skip draws outside their rows.
  float yMin = 0.0f, yMax = 0.0f;

  // Kind::path
  rive::rcp<PathData> path;
  PaintState paint;

  // Kind::image
  rive::rcp<rive::RenderImage> image;
  bool nearest = false;
  rive::BlendMode imageBlend = rive::BlendMode::srcOver;
};

struct ClipRef
{
  rive::rcp<PathData> path;
  rive::Mat2D transform;

  bool operator==(const ClipRef& o) const
  {
    return path == o.path && transform == o.transform;
  }
  bool operator!=(const ClipRef& o) const { return !(*this == o); }
};

// Draws that share the same clip stack. In the ThorVG tree this is a chain
// of nested scenes, one per clip, holding the draws.
struct ClipGroup
{
  std::vector<ClipRef> clips;
};

struct DisplayList
{
  std::vector<DrawCmd> cmds;
  // groups[0] is the "no clip" group and is always empty.
  std::vector<ClipGroup> groups{ClipGroup{}};

  void clear()
  {
    cmds.clear();
    groups.resize(1);
  }

  // True when `o` would produce the same ThorVG tree shape (same draws in
  // the same order and clip groups, same images); only per-draw properties
  // may differ.
  bool sameStructure(const DisplayList& o) const
  {
    if (cmds.size() != o.cmds.size() || groups.size() != o.groups.size())
    {
      return false;
    }
    for (size_t i = 0; i < groups.size(); ++i)
    {
      if (groups[i].clips.size() != o.groups[i].clips.size())
      {
        return false;
      }
    }
    for (size_t i = 0; i < cmds.size(); ++i)
    {
      const DrawCmd& a = cmds[i];
      const DrawCmd& b = o.cmds[i];
      if (a.kind != b.kind || a.group != b.group ||
          (a.kind == DrawCmd::Kind::image && a.image != b.image))
      {
        return false;
      }
    }
    return true;
  }
};

} // namespace rive_thorvg
