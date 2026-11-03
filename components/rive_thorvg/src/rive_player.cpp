#include "rive_thorvg/rive_player.hpp"

#include "rive/animation/state_machine_instance.hpp"
#include "rive/artboard.hpp"
#include "rive/math/aabb.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_boolean_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_color_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_enum_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_number_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_string_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_trigger_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_runtime.hpp"
#include "rive/viewmodel/viewmodel_instance.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef WITH_RIVE_AUDIO
#include "miniaudio.h"
#include "rive/assets/audio_asset.hpp"
#include "rive/audio/audio_source.hpp"
#endif

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
  if (m_importBefore)
  {
    m_importBefore();
  }
  m_file = rive::File::import(rive::Span<const uint8_t>(data, size),
                              &m_factory,
                              &result,
                              static_cast<rive::FileAssetLoader*>(nullptr),
                              nullptr, // scripts get a default Luau VM
                              m_requireSignedScripts);
  if (m_importAfter)
  {
    m_importAfter();
  }
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
  // Data binding: give the artboard its view model's default instance (as
  // Rive's own players do with autoBind), so bound properties, lists and
  // nested artboards get their values. Files without a view model get
  // nullptr and are unaffected.
  m_viewModel = nullptr;
  m_viewModelRuntime = nullptr;
  if (m_file->viewModelCount() > 0)
  {
    m_viewModel = m_file->createDefaultViewModelInstance(m_artboard.get());
    if (m_viewModel != nullptr)
    {
      m_viewModelRuntime = rive::make_rcp<rive::ViewModelInstanceRuntime>(
          m_viewModel);
      if (m_scene)
      {
        m_scene->bindViewModelInstance(m_viewModel);
      }
      else
      {
        m_artboard->bindViewModelInstance(m_viewModel);
      }
    }
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

size_t RivePlayer::prepareAudio(uint32_t channels,
                                uint32_t sampleRate,
                                float maxSeconds)
{
  size_t kept = 0;
#ifdef WITH_RIVE_AUDIO
  if (m_file == nullptr || channels == 0 || sampleRate == 0)
  {
    return 0;
  }
  static const char* const kFormats[] = {
      "unknown", "wav", "flac", "mp3", "vorbis", "buffered"};
  const uint64_t maxFrames = uint64_t(maxSeconds * float(sampleRate));
  for (const auto& asset : m_file->assets())
  {
    if (!asset->is<rive::AudioAsset>())
    {
      continue;
    }
    auto* audio = static_cast<rive::AudioAsset*>(asset.get());
    rive::rcp<rive::AudioSource> source = audio->audioSource();
    if (source == nullptr || source->isBuffered())
    {
      continue; // hosted (not in the file) or already buffered
    }
    const auto start = Clock::now();
    const int format = int(source->format());
    const uint32_t srcRate = source->sampleRate();
    const uint32_t srcChannels = source->channels();

    // The decoder converts to the engine's format, channel count and rate
    // while decoding, so playback needs no conversion at all.
    ma_decoder decoder;
    ma_decoder_config config =
        ma_decoder_config_init(ma_format_f32, channels, sampleRate);
    const auto bytes = source->bytes();
    if (ma_decoder_init_memory(bytes.data(), bytes.size(), &config,
                               &decoder) != MA_SUCCESS)
    {
      std::printf("RivePlayer: sound '%s' could not be decoded\n",
                  audio->name().c_str());
      continue;
    }
    std::vector<float> pcm;
    constexpr uint64_t kChunk = 4096;
    bool tooLong = false;
    for (;;)
    {
      const size_t at = pcm.size();
      pcm.resize(at + kChunk * channels);
      ma_uint64 got = 0;
      ma_decoder_read_pcm_frames(&decoder, pcm.data() + at, kChunk, &got);
      pcm.resize(at + size_t(got) * channels);
      if (pcm.size() / channels > maxFrames)
      {
        tooLong = true;
        break;
      }
      if (got < kChunk)
      {
        break;
      }
    }
    ma_decoder_uninit(&decoder);
    const char* fmt = format >= 0 && format < 6 ? kFormats[format] : "?";
    if (tooLong || pcm.empty())
    {
      std::printf("RivePlayer: sound '%s' (%s, %u Hz, %u ch, %zu bytes): "
                  "%s, streamed\n",
                  audio->name().c_str(), fmt, unsigned(srcRate),
                  unsigned(srcChannels), bytes.size(),
                  tooLong ? "longer than the buffering limit" : "empty");
      continue;
    }
    const size_t frames = pcm.size() / channels;
    audio->audioSource(rive::rcp<rive::AudioSource>(new rive::AudioSource(
        rive::Span<float>(pcm.data(), pcm.size()), channels, sampleRate)));
    kept += pcm.size() * sizeof(float);
    std::printf("RivePlayer: sound '%s' (%s, %u Hz, %u ch, %zu bytes) "
                "decoded to %.2f s of %u Hz %u ch float (%zu KB) in %.0f "
                "ms\n",
                audio->name().c_str(), fmt, unsigned(srcRate),
                unsigned(srcChannels), bytes.size(),
                double(frames) / sampleRate, unsigned(sampleRate),
                unsigned(channels), pcm.size() * sizeof(float) / 1024,
                double(msSince(start)));
  }
#endif
  return kept;
}

namespace
{
const char* typeName(rive::DataType t)
{
  switch (t)
  {
    case rive::DataType::string: return "string";
    case rive::DataType::number: return "number";
    case rive::DataType::boolean: return "boolean";
    case rive::DataType::color: return "color";
    case rive::DataType::list: return "list";
    case rive::DataType::enumType: return "enum";
    case rive::DataType::trigger: return "trigger";
    case rive::DataType::viewModel: return "viewModel";
    case rive::DataType::integer: return "integer";
    case rive::DataType::assetImage: return "image";
    case rive::DataType::artboard: return "artboard";
    default: return "other";
  }
}

std::string colorText(int argb)
{
  char buf[16];
  std::snprintf(buf, sizeof(buf), "#%08X", unsigned(argb));
  return buf;
}

std::string numberText(float v)
{
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%g", double(v));
  return buf;
}

void listProperties(const rive::ViewModelInstanceRuntime& vm,
                    const std::string& prefix,
                    std::vector<RivePlayer::ViewModelProperty>* out,
                    int depth)
{
  for (const auto& p : vm.properties())
  {
    const std::string path = prefix + p.name;
    if (p.type == rive::DataType::viewModel)
    {
      auto child = vm.propertyViewModel(p.name);
      if (child != nullptr && depth < 8)
      {
        listProperties(*child, path + "/", out, depth + 1);
      }
      continue;
    }
    RivePlayer::ViewModelProperty prop{path, typeName(p.type), ""};
    switch (p.type)
    {
      case rive::DataType::number:
        if (auto* v = vm.propertyNumber(p.name)) prop.value = numberText(v->value());
        break;
      case rive::DataType::string:
        if (auto* v = vm.propertyString(p.name)) prop.value = v->value();
        break;
      case rive::DataType::boolean:
        if (auto* v = vm.propertyBoolean(p.name)) prop.value = v->value() ? "true" : "false";
        break;
      case rive::DataType::color:
        if (auto* v = vm.propertyColor(p.name)) prop.value = colorText(v->value());
        break;
      case rive::DataType::enumType:
        if (auto* v = vm.propertyEnum(p.name))
        {
          prop.value = v->value();
          prop.type += " (";
          const auto values = v->values();
          for (size_t i = 0; i < values.size(); ++i)
          {
            prop.type += (i ? "|" : "") + values[i];
          }
          prop.type += ")";
        }
        break;
      default:
        break;
    }
    out->push_back(std::move(prop));
  }
}

bool parseFloat(const std::string& s, float* out)
{
  char* end = nullptr;
  const float v = std::strtof(s.c_str(), &end);
  if (end == s.c_str() || *end != '\0')
  {
    return false;
  }
  *out = v;
  return true;
}
} // namespace

std::string RivePlayer::viewModelName() const
{
  return m_viewModelRuntime ? m_viewModelRuntime->viewModelName() : "";
}

std::vector<RivePlayer::ViewModelProperty> RivePlayer::viewModelProperties()
    const
{
  std::vector<ViewModelProperty> out;
  if (m_viewModelRuntime)
  {
    listProperties(*m_viewModelRuntime, "", &out, 0);
  }
  return out;
}

bool RivePlayer::getProperty(const std::string& path, std::string* value) const
{
  for (const auto& p : viewModelProperties())
  {
    if (p.path == path)
    {
      *value = p.value;
      return true;
    }
  }
  return false;
}

bool RivePlayer::setProperty(const std::string& path,
                             const std::string& value,
                             std::string* error)
{
  auto fail = [&](const char* why) {
    if (error)
    {
      *error = why;
    }
    return false;
  };
  if (!m_viewModelRuntime)
  {
    return fail("the file has no view model");
  }
  auto& vm = *m_viewModelRuntime;
  if (auto* p = vm.propertyNumber(path))
  {
    float v;
    if (!parseFloat(value, &v))
    {
      return fail("not a number");
    }
    p->value(v);
    return true;
  }
  if (auto* p = vm.propertyString(path))
  {
    p->value(value);
    return true;
  }
  if (auto* p = vm.propertyBoolean(path))
  {
    if (value == "true" || value == "1" || value == "on")
    {
      p->value(true);
    }
    else if (value == "false" || value == "0" || value == "off")
    {
      p->value(false);
    }
    else
    {
      return fail("not a boolean (true/false/1/0)");
    }
    return true;
  }
  if (auto* p = vm.propertyColor(path))
  {
    std::string hex = value;
    if (!hex.empty() && hex[0] == '#')
    {
      hex.erase(0, 1);
    }
    if ((hex.size() != 6 && hex.size() != 8) ||
        hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
    {
      return fail("not a color (#RRGGBB or #AARRGGBB)");
    }
    unsigned long v = std::strtoul(hex.c_str(), nullptr, 16);
    if (hex.size() == 6)
    {
      v |= 0xFF000000ul;
    }
    p->value(int(uint32_t(v)));
    return true;
  }
  if (auto* p = vm.propertyEnum(path))
  {
    const auto values = p->values();
    for (size_t i = 0; i < values.size(); ++i)
    {
      if (values[i] == value)
      {
        p->valueIndex(uint32_t(i));
        return true;
      }
    }
    char* end = nullptr;
    const unsigned long i = std::strtoul(value.c_str(), &end, 10);
    if (end != value.c_str() && *end == '\0' && i < values.size())
    {
      p->valueIndex(uint32_t(i));
      return true;
    }
    return fail("not one of the enum's values");
  }
  if (vm.propertyTrigger(path))
  {
    return fireTrigger(path);
  }
  return fail("no such property (try list)");
}

bool RivePlayer::fireTrigger(const std::string& path)
{
  if (!m_viewModelRuntime)
  {
    return false;
  }
  if (auto* p = m_viewModelRuntime->propertyTrigger(path))
  {
    p->trigger();
    return true;
  }
  return false;
}

std::string RivePlayer::command(const std::string& rawLine)
{
  std::string line = rawLine;
  while (!line.empty() &&
         (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
  {
    line.pop_back();
  }
  const size_t sp = line.find(' ');
  const std::string verb = line.substr(0, sp);
  std::string rest = sp == std::string::npos ? "" : line.substr(sp + 1);
  while (!rest.empty() && rest.front() == ' ')
  {
    rest.erase(0, 1);
  }
  const size_t sp2 = rest.find(' ');
  const std::string arg = rest.substr(0, sp2);
  const std::string value =
      sp2 == std::string::npos ? "" : rest.substr(sp2 + 1);

  if (verb == "list")
  {
    if (!hasViewModel())
    {
      return "error the file has no view model\n";
    }
    std::string out;
    for (const auto& p : viewModelProperties())
    {
      out += p.path + " " + p.type + " " + p.value + "\n";
    }
    return out + "ok\n";
  }
  if (verb == "get")
  {
    std::string v;
    return getProperty(arg, &v) ? "ok " + v + "\n"
                                : "error no such property (try list)\n";
  }
  if (verb == "set")
  {
    std::string why;
    return setProperty(arg, value, &why) ? "ok\n" : "error " + why + "\n";
  }
  if (verb == "fire")
  {
    return fireTrigger(arg) ? "ok\n" : "error no such trigger (try list)\n";
  }
  if (verb == "down" || verb == "move" || verb == "up")
  {
    float x, y;
    if (!parseFloat(arg, &x) || !parseFloat(value, &y))
    {
      return "error usage: " + verb + " <x> <y>\n";
    }
    if (verb == "down")
    {
      pointerMove(x, y);
      pointerDown(x, y);
    }
    else if (verb == "move")
    {
      pointerMove(x, y);
    }
    else
    {
      pointerUp(x, y);
      pointerExit(x, y);
    }
    return "ok\n";
  }
  if (verb == "info")
  {
    std::string out = "artboard " + (m_artboard ? m_artboard->name() : "-") +
                      "\nscene " + (m_scene ? m_scene->name() : "-") +
                      "\nviewmodel " +
                      (hasViewModel() ? viewModelName() : "-") + "\n";
    return out + "ok\n";
  }
  if (verb == "help" || verb.empty())
  {
    return "commands: list | get <path> | set <path> <value> | fire <path> "
           "| down|move|up <x> <y> | info\nok\n";
  }
  return "error unknown command '" + verb + "' (try help)\n";
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

void RivePlayer::pointerExit(float x, float y)
{
  if (m_scene && m_artboard)
  {
    m_scene->pointerExit(toArtboard(x, y));
  }
}

} // namespace rive_thorvg
