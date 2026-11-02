#include "rive_thorvg/thorvg_lane.hpp"

#include "thorvg_common.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>

// Defined by ThorVG when it is built with the THORVG_SKIP_PARTIAL_CLEAR patch
// (components/thorvg/thorvg_sources.cmake): while true, partial redraw
// doesn't clear dirty regions before redrawing them, because every lane's
// opaque background rectangle repaints them anyway. Weak, so this still
// links (and the address is null) when ThorVG is built without the patch.
extern bool g_tvgSwSkipPartialClear __attribute__((weak));

namespace rive_thorvg
{

namespace
{
using Clock = std::chrono::steady_clock;

// Pictures duplicated from (and released back to) the same decoded image
// update a plain, non-atomic share count inside ThorVG. Rebuilds are rare
// (only when the draw structure changes), so serialise them across lanes.
std::mutex g_rebuildMutex;

float msSince(Clock::time_point start)
{
  return std::chrono::duration<float, std::milli>(Clock::now() - start)
      .count();
}

// Sets everything about a shape's appearance from a path draw.
void applyPaint(tvg::Shape* shape, const DrawCmd& cmd)
{
  const PaintState& p = cmd.paint;
  auto* shader = static_cast<const ThorvgRenderShader*>(p.shader.get());
  const uint8_t r = static_cast<uint8_t>(rive::colorRed(p.color));
  const uint8_t g = static_cast<uint8_t>(rive::colorGreen(p.color));
  const uint8_t b = static_cast<uint8_t>(rive::colorBlue(p.color));

  if (p.stroke)
  {
    shape->fill(0, 0, 0, 0);
    shape->strokeWidth(p.thickness > 0.0f ? p.thickness : 0.0f);
    shape->strokeJoin(toTvg(p.join));
    shape->strokeCap(toTvg(p.cap));
  }
  else
  {
    shape->strokeWidth(0.0f);
  }

  uint8_t opacity = 255;
  if (shader != nullptr)
  {
    // Gradient stops already carry the paint's opacity; only the renderer's
    // modulated opacity is applied on top.
    tvg::Fill* fill = shader->makeFill(p.shaderTransform, p.hasShaderTransform);
    if (p.stroke)
    {
      shape->strokeFill(fill);
    }
    else
    {
      shape->fill(fill);
    }
    opacity = toByte(cmd.opacity);
  }
  else
  {
    uint8_t a = toByte(rive::colorAlpha(p.color) / 255.0f * cmd.opacity);
    if (p.stroke)
    {
      shape->strokeFill(r, g, b, a);
    }
    else
    {
      shape->fill(r, g, b, a);
    }
  }
  shape->opacity(opacity);
  shape->blend(toTvg(p.blendMode, p.additiveness));
}

void applyImage(tvg::Picture* picture, const DrawCmd& cmd)
{
  picture->opacity(toByte(cmd.opacity));
  picture->filter(cmd.nearest ? tvg::FilterMethod::Nearest
                              : tvg::FilterMethod::Bilinear);
  picture->blend(toTvg(cmd.imageBlend, 0.0f));
}

tvg::Shape* makeClipShape(const ClipRef& clip)
{
  tvg::Shape* shape = tvg::Shape::gen();
  clip.path->applyTo(shape);
  shape->transform(toTvg(clip.transform));
  shape->fill(0, 0, 0, 255);
  return shape;
}
} // namespace

ThorvgLane::ThorvgLane(uint32_t* const* frames,
                       uint32_t frameCount,
                       uint32_t width,
                       uint32_t height,
                       uint32_t y0,
                       uint32_t y1,
                       bool partial)
    : m_slots(std::max<uint32_t>(frameCount, 1)),
      m_width(width),
      m_height(height),
      m_y0(y0),
      m_y1(y1)
{
  for (uint32_t i = 0; i < m_slots.size(); ++i)
  {
    Slot& slot = m_slots[i];
    // Note: EngineOption::Default includes SmartRender, so "off" must be
    // None.
    slot.canvas = tvg::SwCanvas::gen(partial ? tvg::EngineOption::SmartRender
                                             : tvg::EngineOption::None);
    slot.frame = frames[i];
    slot.canvas->target(frames[i], width, width, height,
                        tvg::ColorSpace::ARGB8888);
    slot.canvas->viewport(0, int32_t(y0), int32_t(width), int32_t(y1 - y0));
    slot.y0 = y0;
    slot.y1 = y1;

    // Opaque full-frame rectangle as the first paint. ThorVG fills opaque
    // axis-aligned rects with a plain span fill, so this doubles as the
    // clear.
    slot.background = tvg::Shape::gen();
    slot.background->appendRect(0.0f, 0.0f, float(width), float(height));
    slot.background->fill(0, 0, 0, 255);
    slot.canvas->add(slot.background);

    slot.root = tvg::Scene::gen();
    slot.canvas->add(slot.root);
  }
}

ThorvgLane::~ThorvgLane()
{
  std::lock_guard<std::mutex> lock(g_rebuildMutex);
  for (Slot& slot : m_slots)
  {
    delete slot.canvas;
  }
}

void ThorvgLane::setClearColor(uint32_t argb)
{
  // All lanes share the player's clear colour, so one global flag suffices.
  // Set before rendering starts, read-only while lanes render.
  if (&g_tvgSwSkipPartialClear != nullptr)
  {
    g_tvgSwSkipPartialClear = (argb >> 24) == 0xFF;
  }
  for (Slot& slot : m_slots)
  {
    slot.background->fill((argb >> 16) & 0xFF,
                          (argb >> 8) & 0xFF,
                          argb & 0xFF,
                          (argb >> 24) & 0xFF);
  }
}

void ThorvgLane::setRows(uint32_t y0, uint32_t y1)
{
  m_y0 = y0;
  m_y1 = y1;
}

tvg::Paint* ThorvgLane::makePaint(const DrawCmd& cmd)
{
  if (cmd.kind == DrawCmd::Kind::image)
  {
    auto* image = static_cast<const ThorvgRenderImage*>(cmd.image.get());
    tvg::Picture* picture = image->makePicture();
    picture->transform(toTvg(cmd.transform));
    applyImage(picture, cmd);
    return picture;
  }
  tvg::Shape* shape = tvg::Shape::gen();
  cmd.path->applyTo(shape);
  shape->transform(toTvg(cmd.transform));
  applyPaint(shape, cmd);
  return shape;
}

bool ThorvgLane::outside(const DrawCmd& cmd) const
{
  return m_cull && (cmd.yMax <= float(m_y0) || cmd.yMin >= float(m_y1));
}

void ThorvgLane::rebuild(Slot& slot, const DisplayList& list)
{
  std::lock_guard<std::mutex> lock(g_rebuildMutex);
  // Mark the old paints' areas for redraw (see update()), then free them.
  for (tvg::Paint* paint : slot.paints)
  {
    paint->visible(false);
  }
  slot.root->remove(); // frees the old tree
  slot.paints.clear();
  slot.paints.reserve(list.cmds.size());
  slot.parents.clear();
  slot.parents.reserve(list.cmds.size());
  slot.culled.assign(list.cmds.size(), 0);
  slot.groupScenes.assign(list.groups.size(), {});

  // Clip groups are contiguous runs of draws. Each gets a chain of nested
  // scenes (one clip per level); its draws go in the innermost scene.
  tvg::Scene* container = slot.root;
  uint16_t containerGroup = 0;
  for (const DrawCmd& cmd : list.cmds)
  {
    if (cmd.group != containerGroup)
    {
      containerGroup = cmd.group;
      container = slot.root;
      if (cmd.group != 0)
      {
        for (const ClipRef& clip : list.groups[cmd.group].clips)
        {
          tvg::Scene* scene = tvg::Scene::gen();
          container->add(scene);
          scene->clip(makeClipShape(clip));
          slot.groupScenes[cmd.group].push_back(scene);
          container = scene;
        }
      }
    }
    const bool cull = outside(cmd);
    // Culled draws keep their place in the tree as an empty shape, so the
    // tree keeps the same structure as the DisplayList.
    tvg::Paint* paint = cull ? tvg::Shape::gen() : makePaint(cmd);
    container->add(paint);
    slot.culled[slot.paints.size()] = cull ? 1 : 0;
    slot.paints.push_back(paint);
    slot.parents.push_back(container);
    m_stats.culled += cull ? 1 : 0;
  }
}

bool ThorvgLane::changed(const DrawCmd& now, const DrawCmd& before)
{
  if (now.transform != before.transform || now.opacity != before.opacity)
  {
    return true;
  }
  if (now.kind == DrawCmd::Kind::image)
  {
    return now.nearest != before.nearest || now.imageBlend != before.imageBlend;
  }
  // Unchanged gradient draws are kept too. (They used to be replaced every
  // frame to work around wrong colours / layering under partial redraw; the
  // cause was ThorVG drawing unchanged gradient shapes unclipped once per
  // dirty region, fixed by components/thorvg/fast/sw_rle_clip.h.)
  return now.path != before.path || now.paint != before.paint;
}

bool ThorvgLane::coversQuad(const PathData& path,
                            const rive::Mat2D& transform) const
{
  // The path must be a single quad (move + 3 or 4 lines, optional close)
  // with one corner beyond each corner of the frame and every edge staying
  // outside the frame on its side; then every frame pixel is fully covered
  // (no anti-aliased edge inside the frame), whatever the exact coordinates.
  const auto& cmds = path.cmds;
  size_t n = cmds.size();
  if (n > 0 && cmds[n - 1] == tvg::PathCommand::Close)
  {
    --n;
  }
  if (n < 4 || n > 5 || cmds[0] != tvg::PathCommand::MoveTo ||
      path.pts.size() != n)
  {
    return false;
  }
  for (size_t i = 1; i < n; ++i)
  {
    if (cmds[i] != tvg::PathCommand::LineTo)
    {
      return false;
    }
  }
  const float w = float(m_width), h = float(m_height);
  int cls[5];
  for (size_t i = 0; i < n; ++i)
  {
    const rive::Vec2D v = transform * rive::Vec2D(path.pts[i].x, path.pts[i].y);
    const int cx = v.x <= 0.0f ? 0 : (v.x >= w ? 1 : -1);
    const int cy = v.y <= 0.0f ? 0 : (v.y >= h ? 1 : -1);
    if (cx < 0 || cy < 0)
    {
      return false; // a corner inside the frame's extent
    }
    cls[i] = cx | (cy << 1);
  }
  if (n == 5 && cls[4] != cls[0])
  {
    return false; // the 5th point must close the quad
  }
  int seen = 0;
  for (size_t i = 0; i < 4; ++i)
  {
    seen |= 1 << cls[i];
    // Consecutive corners share exactly one side (x or y), so each edge
    // stays in one outside half-plane: a proper quad, not a bow-tie.
    const int d = cls[i] ^ cls[(i + 1) % 4];
    if (d != 1 && d != 2)
    {
      return false;
    }
  }
  return seen == 0xF;
}

bool ThorvgLane::coversFrame(const DisplayList& list) const
{
  if (list.cmds.empty())
  {
    return false;
  }
  const DrawCmd& cmd = list.cmds.front();
  const PaintState& p = cmd.paint;
  if (cmd.kind != DrawCmd::Kind::path || p.stroke || !cmd.path ||
      p.blendMode != rive::BlendMode::srcOver || toByte(cmd.opacity) != 255)
  {
    return false;
  }
  if (p.shader != nullptr)
  {
    if (!static_cast<const ThorvgRenderShader*>(p.shader.get())->opaque())
    {
      return false;
    }
  }
  else if (rive::colorAlpha(p.color) != 255)
  {
    return false;
  }
  // Clips are fine as long as each one covers the whole frame too (Rive
  // clips the artboard to its bounds, which with "contain" is the frame).
  for (const ClipRef& clip : list.groups[cmd.group].clips)
  {
    if (!clip.path || !coversQuad(*clip.path, clip.transform))
    {
      return false;
    }
  }
  return coversQuad(*cmd.path, cmd.transform);
}

bool ThorvgLane::updateInPlace(tvg::Paint* paint,
                               const DrawCmd& now,
                               const DrawCmd& before)
{
  // Opacity changes still get a new paint: ThorVG bakes a gradient's
  // opacity into its colour table and doesn't rebuild the table when only
  // the opacity changes.
  if (now.kind != before.kind || now.opacity != before.opacity)
  {
    return false;
  }
  if (now.kind == DrawCmd::Kind::image)
  {
    if (now.image != before.image || now.nearest != before.nearest ||
        now.imageBlend != before.imageBlend)
    {
      return false;
    }
    paint->transform(toTvg(now.transform));
    return true;
  }
  const PaintState& a = now.paint;
  const PaintState& b = before.paint;
  if (a.stroke != b.stroke || a.thickness != b.thickness || a.join != b.join ||
      a.cap != b.cap || a.blendMode != b.blendMode ||
      a.additiveness != b.additiveness || !now.path)
  {
    return false;
  }
  auto* shape = static_cast<tvg::Shape*>(paint);
  if (now.path != before.path)
  {
    now.path->applyTo(shape);
  }
  if (now.transform != before.transform)
  {
    shape->transform(toTvg(now.transform));
  }
  if (a.color != b.color || a.shader != b.shader ||
      a.shaderTransform != b.shaderTransform ||
      a.hasShaderTransform != b.hasShaderTransform)
  {
    applyPaint(shape, now); // new solid colour or gradient
  }
  return true;
}

void ThorvgLane::update(Slot& slot, const DisplayList& list)
{
  // A changed draw or clip gets a brand-new ThorVG paint in the same place
  // in the tree, instead of having its properties edited in place: ThorVG's
  // incremental update of an existing shape (re-using parts of its outline,
  // fill and stroke data) gives wrong results for some combinations, e.g. a
  // replaced stroke gradient, or a few changed draws inside a clipped scene.
  // Unchanged paints are left alone, so they keep their prepared data and
  // ThorVG's partial redraw only covers what actually moved.
  for (size_t g = 1; g < list.groups.size(); ++g)
  {
    const auto& now = list.groups[g].clips;
    const auto& before = slot.applied.groups[g].clips;
    for (size_t level = 0; level < now.size(); ++level)
    {
      if (now[level] != before[level])
      {
        tvg::Scene* scene = slot.groupScenes[g][level];
        tvg::Shape* clip = m_inPlace ? scene->clip() : nullptr;
        if (clip != nullptr)
        {
          // Edit the existing clip shape (ThorVG re-prepares a scene's clip
          // on every update anyway; this just avoids reallocating it).
          if (now[level].path != before[level].path)
          {
            now[level].path->applyTo(clip);
          }
          if (now[level].transform != before[level].transform)
          {
            clip->transform(toTvg(now[level].transform));
          }
        }
        else
        {
          scene->clip(makeClipShape(now[level]));
        }
        ++m_stats.clips;
      }
    }
  }
  for (size_t i = 0; i < list.cmds.size(); ++i)
  {
    const bool cull = outside(list.cmds[i]);
    const bool wasCulled = slot.culled[i] != 0;
    if (cull)
    {
      ++m_stats.culled;
      if (wasCulled)
      {
        continue; // still outside: nothing to do, whatever changed
      }
    }
    else if (!wasCulled && !changed(list.cmds[i], slot.applied.cmds[i]))
    {
      continue;
    }
    // Changed but still inside the lane: edit the existing paint if possible.
    if (m_inPlace && !cull && !wasCulled &&
        updateInPlace(slot.paints[i], list.cmds[i], slot.applied.cmds[i]))
    {
      ++m_stats.edited;
      continue;
    }
    // Moved out of the lane: placeholder (hiding and removing the real shape
    // below makes ThorVG redraw where it was). Moved in, or changed: a real
    // shape.
    tvg::Paint* fresh = cull ? tvg::Shape::gen() : makePaint(list.cmds[i]);
    slot.culled[i] = cull ? 1 : 0;
    slot.parents[i]->add(fresh, slot.paints[i]); // insert before the old one
    // ThorVG doesn't mark a removed paint's area for redraw (dispose() has
    // no damage), so pixels the old paint covered but the new one doesn't
    // would keep stale content under partial redraw. Hiding it first does
    // mark its area.
    slot.paints[i]->visible(false);
    slot.parents[i]->remove(slot.paints[i]); // and release the old one
    slot.paints[i] = fresh;
    ++m_stats.changed;
  }
}

void ThorvgLane::apply(const DisplayList& list, uint32_t slotIndex)
{
  auto t0 = Clock::now();
  Slot& slot = m_slots[slotIndex % m_slots.size()];
  if (slot.y0 != m_y0 || slot.y1 != m_y1)
  {
    // Rebalanced: move the viewport. Re-setting the target first makes
    // ThorVG treat the next draw as a full redraw; moving only the viewport
    // left rows newly given to this lane partly unpainted with SmartRender.
    slot.canvas->target(slot.frame, m_width, m_width, m_height,
                        tvg::ColorSpace::ARGB8888);
    slot.canvas->viewport(0, int32_t(m_y0), int32_t(m_width),
                          int32_t(m_y1 - m_y0));
    slot.y0 = m_y0;
    slot.y1 = m_y1;
  }
  m_stats.changed = 0;
  m_stats.edited = 0;
  m_stats.clips = 0;
  m_stats.culled = 0;
  m_stats.rebuilt = !list.sameStructure(slot.applied) || slot.paints.empty();
  if (m_stats.rebuilt)
  {
    rebuild(slot, list);
  }
  else
  {
    update(slot, list);
  }
  slot.applied = list;

  // Hide the lane background while the first draw covers the frame. (With
  // the partial-redraw clear skipped in ThorVG, either the background or
  // that draw repaints every dirty region opaquely first.)
  const bool hide = m_skipCoveredBackground && coversFrame(list);
  if (hide != slot.backgroundHidden)
  {
    slot.background->opacity(hide ? 0 : 255);
    slot.backgroundHidden = hide;
  }
  m_stats.backgroundHidden = hide;
  m_stats.applyMs = msSince(t0);
}

void ThorvgLane::draw(uint32_t slotIndex)
{
  Slot& slot = m_slots[slotIndex % m_slots.size()];
  // Paint edits don't notify the canvas, so ask it to look for changes.
  auto t0 = Clock::now();
  slot.canvas->update();
  auto t1 = Clock::now();
  m_stats.prepMs = std::chrono::duration<float, std::milli>(t1 - t0).count();

  m_dirtyY0 = m_dirtyY1 = 0;
  if (slot.canvas->draw(false) == tvg::Result::Success)
  {
    // Which rows were redrawn? (Must be asked between draw() and sync().)
    uint32_t count = slot.canvas->dirtyRegions();
    if (count > 0)
    {
      m_regions.resize(count);
      slot.canvas->dirtyRegions(m_regions.data(), count);
      int32_t lo = int32_t(slot.y1), hi = int32_t(slot.y0);
      for (const tvg::Region& r : m_regions)
      {
        if (r.yMax <= r.yMin || r.xMax <= r.xMin)
        {
          continue;
        }
        lo = std::min(lo, r.yMin);
        hi = std::max(hi, r.yMax);
      }
      lo = std::max(lo, int32_t(slot.y0));
      hi = std::min(hi, int32_t(slot.y1));
      if (hi > lo)
      {
        m_dirtyY0 = uint32_t(lo);
        m_dirtyY1 = uint32_t(hi);
      }
    }
  }
  slot.canvas->sync();
  m_stats.fillMs = msSince(t1);
}

} // namespace rive_thorvg
