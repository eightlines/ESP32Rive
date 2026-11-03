// Loads a .riv file and plays its default scene into a premultiplied ARGB8888
// frame buffer using ThorVG's software renderer. Platform independent: used
// both by the ESP32-P4 firmware and by the host test program.
//
// Per frame:
//   player.advance(dt);        // Rive animation / state machine
//   player.build();            // Rive draw calls -> DisplayList (no pixels);
//                              // also picks this frame's frame buffer
//   player.renderLane(i);      // for every lane, each on its own thread
//   player.frame();            // the frame buffer just drawn
//   player.dirtyRows(&y0,&y1); // rows changed since that buffer's last use
//   player.balanceLanes();     // optional: move the split between lanes
//
// Lanes split the frame into horizontal slices that render in parallel, one
// per CPU core. Each lane keeps its ThorVG paints between frames and only
// updates what changed; with `partial` on, ThorVG also only redraws the
// screen regions that changed, so the frame buffers must keep their contents
// from frame to frame.
//
// With two frame buffers, frames alternate between them, so the previous
// frame can still be read (copied to the display) while the next one is
// drawn. Each lane keeps a separate ThorVG canvas per buffer.
#pragma once

#include "rive/file.hpp"
#include "rive/layout.hpp"
#include "rive/scene.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_runtime.hpp"
#include "rive/viewmodel/viewmodel_instance.hpp"
#include "rive_thorvg/display_list.hpp"
#include "rive_thorvg/thorvg_factory.hpp"
#include "rive_thorvg/thorvg_lane.hpp"
#include "rive_thorvg/thorvg_renderer.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace rive_thorvg
{

struct FrameStats
{
  float advanceMs = 0.0f; // Rive: animation / state machine update
  float buildMs = 0.0f;   // Rive draw calls -> DisplayList
  uint32_t draws = 0;     // draws in the DisplayList
};

class RivePlayer
{
public:
  // `frames` are `frameCount` (1 or 2) buffers of width x height
  // premultiplied ARGB8888 (stride = width); they must outlive the player.
  RivePlayer(uint32_t* const* frames,
             uint32_t frameCount,
             uint32_t width,
             uint32_t height,
             uint32_t laneCount = 1,
             bool partial = true);
  ~RivePlayer();

  RivePlayer(const RivePlayer&) = delete;
  RivePlayer& operator=(const RivePlayer&) = delete;

  // Creates lane i. Must be called on the thread that will render that lane
  // (ThorVG binds a thread-local memory pool to the canvas), before load().
  void createLane(uint32_t i);
  // Destroys lane i (call on its thread, before the player is destroyed).
  void destroyLane(uint32_t i);
  uint32_t laneCount() const { return uint32_t(m_lanes.size()); }

  // Imports a .riv. Picks the artboard's default state machine, falling back
  // to its first state machine, then its first animation. The data must stay
  // valid while the player is alive.
  bool load(const uint8_t* data, size_t size);

  // Audio (with WITH_RIVE_AUDIO): decodes the file's sounds once, at the
  // audio engine's channel count and sample rate, and swaps each for a
  // buffer of ready samples. Playback then only mixes (no per-block
  // decoding or resampling). Sounds longer than maxSeconds keep streaming
  // from their compressed bytes. Call after load(), before playback. Each
  // sound is logged; returns the bytes of samples kept in memory.
  size_t prepareAudio(uint32_t channels, uint32_t sampleRate, float maxSeconds);

  // Data binding. load() binds the artboard's default view model instance
  // (if the file has view models); these read and write its properties by
  // path: "score", or "player/name" for a property of a nested view model.
  // Call from the thread that calls advance(). Values are text:
  //   number "42.5", boolean "true"/"false"/"1"/"0", color "#RRGGBB" or
  //   "#AARRGGBB", enum a value name or its index, string as is.
  struct ViewModelProperty
  {
    std::string path;
    std::string type;  // number, string, boolean, color, enum, trigger, ...
    std::string value; // current value as text (empty for triggers)
  };
  bool hasViewModel() const { return m_viewModelRuntime != nullptr; }
  std::string viewModelName() const;
  // Every property, nested view models flattened into paths.
  std::vector<ViewModelProperty> viewModelProperties() const;
  // Sets a property from text; false (with a reason) if there's no such
  // property or the text doesn't fit its type.
  bool setProperty(const std::string& path,
                   const std::string& value,
                   std::string* error = nullptr);
  bool getProperty(const std::string& path, std::string* value) const;
  bool fireTrigger(const std::string& path);
  // One text command (the board's network protocol, also used by the host
  // test), returning the reply. Commands:
  //   list                 every property: "<path> <type> <value>" lines
  //   get <path>           "ok <value>"
  //   set <path> <value>   "ok"; the value is the rest of the line
  //   fire <path>          fires a trigger
  //   down|move|up <x> <y> pointer input in frame pixels
  //   info / help
  // Errors reply "error <reason>". Call from the thread that calls advance().
  std::string command(const std::string& line);

  // Advances the scene by `seconds`.
  void advance(float seconds);
  // Records the current frame's draw calls and moves on to the next frame
  // buffer.
  void build();
  // Brings lane i up to date with the recorded frame and rasterises it.
  // Lanes may run concurrently, each on its own thread.
  void renderLane(uint32_t i);
  // The frame buffer the current frame is drawn into.
  const uint32_t* frame() const { return m_frames[m_slot]; }
  // Rows [y0, y1) of frame() redrawn by the last renderLane() calls, i.e.
  // changed since that buffer was last drawn (one frame ago with one buffer,
  // two with two). Returns false if nothing changed.
  bool dirtyRows(uint32_t* y0, uint32_t* y1) const;
  // Call after all lanes rendered a frame. Every few frames, moves the split
  // between two lanes toward the slower one so both take about as long.
  // Returns true if the split moved (the next frame redraws everything).
  bool balanceLanes();
  // First row of lane i.
  uint32_t laneY0(uint32_t i) const;

  // Fit/alignment of the artboard inside the frame (default: contain, center).
  void setFit(rive::Fit fit, rive::Alignment alignment);
  // Background behind the artboard (premultiplied ARGB). Call before
  // createLane().
  void setClearColor(uint32_t argb) { m_clearColor = argb; }
  // Lanes skip draws outside their rows (default on). Call before
  // createLane().
  void setCulling(bool on) { m_cull = on; }
  // Lanes hide their background rectangle while the artboard's first draw
  // covers the frame opaquely (default on). Call before createLane().
  void setSkipCoveredBackground(bool on) { m_skipCoveredBackground = on; }
  // Lanes update changed draws in place where possible (default on). Call
  // before createLane().
  void setInPlaceUpdates(bool on) { m_inPlace = on; }
  // Called just before and after rive::File::import in load(), and only
  // around it: the file's static objects are made there, while the
  // artboard instance and its per-frame render paths (hot data for every
  // frame) come after. The board uses this to put the import in PSRAM.
  void setImportHooks(void (*before)(), void (*after)())
  {
    m_importBefore = before;
    m_importAfter = after;
  }
  // Scripting builds: drop scripts whose editor signature doesn't verify
  // (default off: every script runs). Call before load().
  void setRequireSignedScripts(bool on) { m_requireSignedScripts = on; }

  const FrameStats& stats() const { return m_stats; }
  // The draws recorded by the last build().
  const DisplayList& displayList() const { return m_list; }
  const LaneStats& laneStats(uint32_t i) const { return m_lanes[i]->stats(); }
  rive::Scene* scene() const { return m_scene.get(); }
  rive::ArtboardInstance* artboard() const { return m_artboard.get(); }

  // Pointer input in frame pixel coordinates (forwarded to the state
  // machine, if the scene is one).
  void pointerDown(float x, float y);
  void pointerMove(float x, float y);
  void pointerUp(float x, float y);
  // The pointer left the scene (a finger lifting off a touchscreen), so
  // the next touch enters again and hover listeners fire again.
  void pointerExit(float x, float y);

private:
  rive::Vec2D toArtboard(float x, float y) const;

  std::vector<uint32_t*> m_frames;
  uint32_t m_slot = 0; // index into m_frames for the current frame
  uint32_t m_frameNumber = 0;
  uint32_t m_width;
  uint32_t m_height;
  bool m_partial;
  uint32_t m_clearColor = 0xFF000000;
  bool m_cull = true;
  bool m_skipCoveredBackground = true;
  bool m_inPlace = true;

  ThorvgFactory m_factory;
  ThorvgRenderer m_renderer;
  DisplayList m_list;
  std::vector<std::unique_ptr<ThorvgLane>> m_lanes;

  rive::rcp<rive::File> m_file;
  bool m_requireSignedScripts = false;
  void (*m_importBefore)() = nullptr;
  void (*m_importAfter)() = nullptr;
  rive::rcp<rive::ViewModelInstance> m_viewModel;
  rive::rcp<rive::ViewModelInstanceRuntime> m_viewModelRuntime;
  std::unique_ptr<rive::ArtboardInstance> m_artboard;
  std::unique_ptr<rive::Scene> m_scene;
  rive::Fit m_fit = rive::Fit::contain;
  rive::Alignment m_alignment = rive::Alignment::center;
  FrameStats m_stats;

  // Lane balancing (two lanes): accumulated lane times since the last move.
  float m_balanceTime[2] = {};
  uint32_t m_balanceFrames = 0;
  // Direction the previous window asked the split to move (-1, 0, +1); a
  // move needs two windows in a row to agree, so noise doesn't bounce it.
  int32_t m_balanceWant = 0;
};

} // namespace rive_thorvg
