// Host test: renders frames of a .riv through the same RivePlayer / ThorVG
// adapter used on the ESP32-P4 and writes them as PPM images.
//
//   rive_host_render <file.riv> <out_dir> [frames=10] [fps=30] [size=720]
//                    [lanes=2] [partial=1] [buffers=2] [balance=1] [cull=1]
//                    [bgskip=1] [inplace=1]
//
// lanes > 1 renders the extra lanes on worker threads, like the board does
// on its second core. partial=1 enables ThorVG's partial redraw. buffers=2
// alternates two frame buffers (one ThorVG canvas per buffer per lane), as
// the board does to overlap drawing with the copy to the display.
// balance=1 moves the split between two lanes toward balance.
#include "rive_thorvg/rive_player.hpp"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static std::vector<uint8_t> readFile(const char* path)
{
  std::vector<uint8_t> bytes;
  FILE* f = std::fopen(path, "rb");
  if (!f)
  {
    return bytes;
  }
  std::fseek(f, 0, SEEK_END);
  long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  bytes.resize(size);
  if (std::fread(bytes.data(), 1, size, f) != size_t(size))
  {
    bytes.clear();
  }
  std::fclose(f);
  return bytes;
}

static bool writePpm(const std::string& path,
                     const uint32_t* px,
                     uint32_t w,
                     uint32_t h)
{
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f)
  {
    return false;
  }
  std::fprintf(f, "P6\n%u %u\n255\n", w, h);
  std::vector<uint8_t> row(w * 3);
  for (uint32_t y = 0; y < h; ++y)
  {
    for (uint32_t x = 0; x < w; ++x)
    {
      uint32_t c = px[y * w + x]; // opaque background, so no unpremultiply
      row[x * 3 + 0] = (c >> 16) & 0xFF;
      row[x * 3 + 1] = (c >> 8) & 0xFF;
      row[x * 3 + 2] = c & 0xFF;
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
  return true;
}

// A worker thread that owns one lane: creates it, renders it when asked,
// destroys it at the end (ThorVG canvases must stay on their thread).
class LaneWorker
{
public:
  LaneWorker(rive_thorvg::RivePlayer& player, uint32_t lane)
      : m_player(player), m_lane(lane), m_thread([this] { run(); })
  {
    wait(); // lane created
  }
  ~LaneWorker()
  {
    post(Cmd::quit);
    m_thread.join();
  }
  void startFrame() { post(Cmd::render); }
  void wait()
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [this] { return m_cmd == Cmd::idle; });
  }

private:
  enum class Cmd
  {
    create,
    idle,
    render,
    quit
  };
  void post(Cmd c)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cmd = c;
    m_cv.notify_all();
  }
  void run()
  {
    m_player.createLane(m_lane);
    post(Cmd::idle);
    while (true)
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_cv.wait(lock, [this] { return m_cmd != Cmd::idle; });
      Cmd c = m_cmd;
      lock.unlock();
      if (c == Cmd::quit)
      {
        m_player.destroyLane(m_lane);
        return;
      }
      m_player.renderLane(m_lane);
      post(Cmd::idle);
    }
  }

  rive_thorvg::RivePlayer& m_player;
  uint32_t m_lane;
  std::mutex m_mutex;
  std::condition_variable m_cv;
  Cmd m_cmd = Cmd::create;
  std::thread m_thread;
};

int main(int argc, char** argv)
{
  if (argc < 3)
  {
    std::fprintf(stderr,
                 "usage: %s <file.riv> <out_dir> [frames] [fps] [size] "
                 "[lanes] [partial]\n",
                 argv[0]);
    return 2;
  }
  const int frames = argc > 3 ? std::atoi(argv[3]) : 10;
  const float fps = argc > 4 ? float(std::atof(argv[4])) : 30.0f;
  const uint32_t size = argc > 5 ? uint32_t(std::atoi(argv[5])) : 720;
  const uint32_t lanes = argc > 6 ? uint32_t(std::atoi(argv[6])) : 2;
  const bool partial = argc > 7 ? std::atoi(argv[7]) != 0 : true;
  const uint32_t buffers = argc > 8 ? uint32_t(std::atoi(argv[8])) : 2;
  const bool balance = argc > 9 ? std::atoi(argv[9]) != 0 : true;
  const bool cull = argc > 10 ? std::atoi(argv[10]) != 0 : true;
  const bool bgskip = argc > 11 ? std::atoi(argv[11]) != 0 : true;
  const bool inplace = argc > 12 ? std::atoi(argv[12]) != 0 : true;

  std::vector<uint8_t> riv = readFile(argv[1]);
  if (riv.empty())
  {
    std::fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }

  std::vector<std::vector<uint32_t>> pixels(buffers,
                                            std::vector<uint32_t>(size * size));
  std::vector<uint32_t*> frameBufs;
  for (auto& p : pixels)
  {
    frameBufs.push_back(p.data());
  }
  {
    rive_thorvg::RivePlayer player(
        frameBufs.data(), buffers, size, size, lanes, partial);
    player.setClearColor(0xFF202020);
    player.setCulling(cull);
    player.setSkipCoveredBackground(bgskip);
    player.setInPlaceUpdates(inplace);
    player.createLane(0);
    std::vector<std::unique_ptr<LaneWorker>> workers;
    for (uint32_t i = 1; i < lanes; ++i)
    {
      workers.push_back(std::make_unique<LaneWorker>(player, i));
    }
    if (!player.load(riv.data(), riv.size()))
    {
      return 1;
    }

    double totalBuild = 0, totalAdvance = 0, totalLane0 = 0, totalDirty = 0;
    int rebuilds = 0, moves = 0;
    for (int i = 0; i < frames; ++i)
    {
      if (i > 0)
      {
        player.advance(1.0f / fps);
      }
      player.build();
      for (auto& w : workers)
      {
        w->startFrame();
      }
      player.renderLane(0);
      for (auto& w : workers)
      {
        w->wait();
      }
      uint32_t y0 = 0, y1 = 0;
      bool dirty = player.dirtyRows(&y0, &y1);
      if (balance && player.balanceLanes())
      {
        ++moves;
      }
      const auto& s = player.stats();
      const auto& l0 = player.laneStats(0);
      totalAdvance += s.advanceMs;
      totalBuild += s.buildMs;
      totalLane0 += l0.applyMs + l0.prepMs + l0.fillMs;
      totalDirty += dirty ? double(y1 - y0) / size : 0.0;
      rebuilds += l0.rebuilt ? 1 : 0;
      char name[64];
      std::snprintf(name, sizeof(name), "/frame_%03d.ppm", i);
      writePpm(std::string(argv[2]) + name, player.frame(), size, size);
    }
    for (uint32_t l = 0; l < lanes; ++l)
    {
      std::printf("lane %u: rows from %u, %u of %u draws culled, "
                  "background %s\n",
                  l, player.laneY0(l), player.laneStats(l).culled,
                  player.stats().draws,
                  player.laneStats(l).backgroundHidden ? "hidden" : "drawn");
    }
    std::printf("%d frames, %u lanes, partial %d, %u buffers: advance %.2f  "
                "build %.2f  lane0 %.2f ms (avg) | dirty rows %.0f%% | "
                "rebuilds %d | split moves %d (now row %u) | %u draws\n",
                frames,
                lanes,
                int(partial),
                buffers,
                totalAdvance / frames,
                totalBuild / frames,
                totalLane0 / frames,
                100.0 * totalDirty / frames,
                rebuilds,
                moves,
                lanes > 1 ? player.laneY0(1) : size,
                player.stats().draws);
    workers.clear();
    player.destroyLane(0);
  }
  return 0;
}
