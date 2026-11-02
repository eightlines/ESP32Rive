#include "rive_thorvg/rive_player.hpp"

#include "rive/animation/state_machine_instance.hpp"
#include "rive/artboard.hpp"
#include "rive/math/aabb.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace rive_thorvg
{

namespace
{
using Clock = std::chrono::steady_clock;

float msSince(Clock::time_point start)
{
  return std::chrono::duration<float, std::milli>(Clock::now() - start)
      .count();
}
} // namespace

RivePlayer::RivePlayer(uint32_t* const* frames,
                       uint32_t frameCount,
                       uint32_t width,
                       uint32_t height,
                       uint32_t laneCount,
                       bool partial)
    : m_frames(frames, frames + std::max<uint32_t>(frameCount, 1)),
      m_width(width),
      m_height(height),
      m_partial(partial),
      m_lanes(std::max<uint32_t>(laneCount, 1))
{
  // Reference counted; the matching term() is in the destructor. No ThorVG
  // worker threads: parallelism comes from the lanes.
  tvg::Initializer::init(0);
}

RivePlayer::~RivePlayer()
{
  m_scene.reset();
  m_artboard.reset();
  for (auto& lane : m_lanes)
  {
    lane.reset();
  }
  m_list.clear(); // drops references to paths, shaders and images
  m_file = nullptr;
  tvg::Initializer::term();
}

void RivePlayer::createLane(uint32_t i)
{
  const uint32_t n = uint32_t(m_lanes.size());
  const uint32_t y0 = m_height * i / n;
  const uint32_t y1 = m_height * (i + 1) / n;
  m_lanes[i] = std::make_unique<ThorvgLane>(m_frames.data(),
                                            uint32_t(m_frames.size()),
                                            m_width,
                                            m_height,
                                            y0,
                                            y1,
                                            m_partial);
  m_lanes[i]->setClearColor(m_clearColor);
  m_lanes[i]->setCulling(m_cull);
  m_lanes[i]->setSkipCoveredBackground(m_skipCoveredBackground);
  m_lanes[i]->setInPlaceUpdates(m_inPlace);
}

void RivePlayer::destroyLane(uint32_t i) { m_lanes[i].reset(); }

bool RivePlayer::load(const uint8_t* data, size_t size)
{
  m_scene.reset();
  m_artboard.reset();
  rive::ImportResult result;
  m_file = rive::File::import(rive::Span<const uint8_t>(data, size),
                              &m_factory,
                              &result);
  if (m_file == nullptr || result != rive::ImportResult::success)
  {
    std::printf("RivePlayer: import failed (result %d)\n",
                static_cast<int>(result));
    m_file = nullptr;
    return false;
  }
  m_artboard = m_file->artboardDefault();
  if (m_artboard == nullptr)
  {
    std::printf("RivePlayer: file has no artboard\n");
    return false;
  }
  m_scene = m_artboard->defaultScene();
  if (m_scene == nullptr)
  {
    std::printf("RivePlayer: artboard has no animation or state machine; "
                "drawing it static\n");
  }
  rive::AABB b = m_artboard->bounds();
  std::printf("RivePlayer: artboard '%s' %.0fx%.0f, scene '%s', "
              "%zu animations, %zu state machines\n",
              m_artboard->name().c_str(),
              b.width(),
              b.height(),
              m_scene ? m_scene->name().c_str() : "-",
              m_artboard->animationCount(),
              m_artboard->stateMachineCount());
  if (m_scene)
  {
    m_scene->advanceAndApply(0.0f);
  }
  else
  {
    m_artboard->advance(0.0f);
  }
  return true;
}

void RivePlayer::setFit(rive::Fit fit, rive::Alignment alignment)
{
  m_fit = fit;
  m_alignment = alignment;
}

void RivePlayer::advance(float seconds)
{
  auto t0 = Clock::now();
  if (m_scene)
  {
    m_scene->advanceAndApply(seconds);
  }
  m_stats.advanceMs = msSince(t0);
}

void RivePlayer::build()
{
  auto t0 = Clock::now();
  m_slot = m_frameNumber % uint32_t(m_frames.size());
  ++m_frameNumber;
  m_renderer.begin(&m_list);
  if (m_artboard)
  {
    m_renderer.save();
    m_renderer.align(m_fit,
                     m_alignment,
                     rive::AABB(0.0f, 0.0f, float(m_width), float(m_height)),
                     m_artboard->bounds());
    if (m_scene)
    {
      m_scene->draw(&m_renderer);
    }
    else
    {
      m_artboard->draw(&m_renderer);
    }
    m_renderer.restore();
  }
  m_renderer.end();
  m_stats.buildMs = msSince(t0);
  m_stats.draws = uint32_t(m_list.cmds.size());
}

void RivePlayer::renderLane(uint32_t i)
{
  ThorvgLane& lane = *m_lanes[i];
  lane.apply(m_list, m_slot);
  lane.draw(m_slot);
}

bool RivePlayer::dirtyRows(uint32_t* y0, uint32_t* y1) const
{
  uint32_t lo = m_height, hi = 0;
  for (const auto& lane : m_lanes)
  {
    if (lane && lane->dirtyY1() > lane->dirtyY0())
    {
      lo = std::min(lo, lane->dirtyY0());
      hi = std::max(hi, lane->dirtyY1());
    }
  }
  if (hi <= lo)
  {
    return false;
  }
  *y0 = lo;
  *y1 = hi;
  return true;
}

uint32_t RivePlayer::laneY0(uint32_t i) const { return m_lanes[i]->y0(); }

bool RivePlayer::balanceLanes()
{
  if (m_lanes.size() != 2 || !m_lanes[0] || !m_lanes[1])
  {
    return false;
  }
  m_balanceTime[0] += m_lanes[0]->stats().totalMs();
  m_balanceTime[1] += m_lanes[1]->stats().totalMs();
  // Decide over several frames: lane times are noisy, and every move makes
  // the next frames (one per frame buffer) redraw everything.
  constexpr uint32_t kWindow = 8;
  if (++m_balanceFrames < kWindow)
  {
    return false;
  }
  const float t0 = m_balanceTime[0], t1 = m_balanceTime[1];
  m_balanceTime[0] = m_balanceTime[1] = 0.0f;
  m_balanceFrames = 0;
  if (t0 + t1 <= 0.0f || std::abs(t0 - t1) < 0.04f * std::max(t0, t1))
  {
    m_balanceWant = 0;
    return false; // within 4%: leave it
  }
  // Assume time is roughly proportional to rows: move half of the estimated
  // distance to balance (damped, to avoid oscillating), at most 1/8 of the
  // frame per move, in steps of 8 rows, but at least one step when the
  // target is a step or more away (at 360 rows, halving a 9-row distance
  // would otherwise round to no move). The slower lane gives rows away.
  const int32_t split = int32_t(m_lanes[1]->y0());
  const int32_t rows0 = split, rows1 = int32_t(m_height) - split;
  // Time per row of each lane; the balanced split s satisfies
  // s * r0 == (h - s) * r1.
  const float r0 = t0 / float(std::max<int32_t>(rows0, 1));
  const float r1 = t1 / float(std::max<int32_t>(rows1, 1));
  const float target = float(m_height) * r1 / (r0 + r1);
  const float distance = target - float(split);
  int32_t step = int32_t(distance * 0.5f);
  if (std::abs(distance) >= 8.0f && std::abs(step) < 8)
  {
    step = distance > 0.0f ? 8 : -8;
  }
  const int32_t maxStep = int32_t(m_height) / 8;
  step = std::max(-maxStep, std::min(maxStep, step));
  step = (step / 8) * 8;
  const int32_t want = step > 0 ? 1 : (step < 0 ? -1 : 0);
  const bool confirmed = want != 0 && want == m_balanceWant;
  m_balanceWant = want;
  if (!confirmed)
  {
    return false;
  }
  const int32_t minRows = int32_t(m_height) / 8;
  const int32_t newSplit = std::max(
      minRows, std::min(int32_t(m_height) - minRows, split + step));
  if (newSplit == split)
  {
    return false;
  }
  m_lanes[0]->setRows(0, uint32_t(newSplit));
  m_lanes[1]->setRows(uint32_t(newSplit), m_height);
  return true;
}

rive::Vec2D RivePlayer::toArtboard(float x, float y) const
{
  rive::Mat2D m = rive::computeAlignment(
      m_fit,
      m_alignment,
      rive::AABB(0.0f, 0.0f, float(m_width), float(m_height)),
      m_artboard->bounds());
  return m.invertOrIdentity() * rive::Vec2D(x, y);
}

void RivePlayer::pointerDown(float x, float y)
{
  if (m_scene && m_artboard)
  {
    m_scene->pointerDown(toArtboard(x, y));
  }
}

void RivePlayer::pointerMove(float x, float y)
{
  if (m_scene && m_artboard)
  {
    m_scene->pointerMove(toArtboard(x, y));
  }
}

void RivePlayer::pointerUp(float x, float y)
{
  if (m_scene && m_artboard)
  {
    m_scene->pointerUp(toArtboard(x, y));
  }
}

} // namespace rive_thorvg
