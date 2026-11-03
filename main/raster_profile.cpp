// On-device breakdown of where ThorVG's software renderer spends its time.
//
// The linker's --wrap option (see main/CMakeLists.txt) redirects ThorVG's
// calls from its renderer into these wrappers, which count CPU cycles per
// core around the real functions. No ThorVG source is modified. Enabled with
// CONFIG_RIVE_RASTER_PROFILE.
//
// Fill (pixels):  rasterShape, rasterGradientShape, rasterStroke,
//                 rasterGradientStroke, rasterClear (partial-redraw clears)
// Prep (spans):   shapeGenRle (of which rleRender), shapeGenStrokeRle,
//                 fillPrepare (gradient setup)
#include "raster_profile.h"

#include "sdkconfig.h"

#if CONFIG_RIVE_RASTER_PROFILE

#include <cinttypes>
#include <cstdint>

#include "esp_cpu.h"
#include "esp_log.h"

namespace
{
enum Counter
{
  kSolidShape,
  kGradientShape,
  kSolidStroke,
  kGradientStroke,
  kClear,
  kShapeRle,
  kRleRender,
  kStrokeRle,
  kFillPrepare,
  kCount
};

const char* const kNames[kCount] = {
    "solid fill",  "gradient fill", "solid stroke", "gradient stroke",
    "clear",       "shape RLE",     "  (rleRender)", "stroke RLE",
    "grad setup"};

struct PerCore
{
  uint64_t cycles[kCount];
  uint32_t calls[kCount];
  // Gradient pixels by blend: opaque, translucent, anti-aliased edge.
  uint64_t gradPx[3];
};
PerCore g_core[2];

// Called by the fast gradient spans (components/thorvg/fast) for each span.
void countGradientSpan(unsigned op, unsigned len, int /*linear*/)
{
  PerCore& p = g_core[esp_cpu_get_core_id() & 1];
  p.gradPx[op == 0 ? 0 : (op == 1 ? 1 : 2)] += len;
}

struct Scope
{
  Counter c;
  uint32_t t0;
  int core;
  explicit Scope(Counter counter)
      : c(counter), t0(esp_cpu_get_cycle_count()), core(esp_cpu_get_core_id())
  {
  }
  ~Scope()
  {
    PerCore& p = g_core[core & 1];
    p.cycles[c] += uint32_t(esp_cpu_get_cycle_count() - t0);
    p.calls[c] += 1;
  }
};
} // namespace

// Signatures as ThorVG declares them, with pointers / references passed as
// void* and uint32_t as unsigned long (RISC-V newlib); the ABI is the same.
#define WRAP(ret, name, params, args, counter)                                 \
  extern "C" ret __real_##name params;                                         \
  extern "C" ret __wrap_##name params                                          \
  {                                                                            \
    Scope s(counter);                                                          \
    return __real_##name args;                                                 \
  }

WRAP(bool, _Z11rasterShapeP9SwSurfaceP7SwShapeRKN3tvg12RenderRegionERNS3_4RGBAE,
     (void* a, void* b, const void* c, void* d), (a, b, c, d), kSolidShape)
WRAP(bool, _Z12rasterStrokeP9SwSurfaceP7SwShapeRKN3tvg12RenderRegionERNS3_4RGBAE,
     (void* a, void* b, const void* c, void* d), (a, b, c, d), kSolidStroke)
WRAP(bool, _Z19rasterGradientShapeP9SwSurfaceP7SwShapeRKN3tvg12RenderRegionEPKNS3_4FillEh,
     (void* a, void* b, const void* c, const void* d, unsigned char e),
     (a, b, c, d, e), kGradientShape)
WRAP(bool, _Z20rasterGradientStrokeP9SwSurfaceP7SwShapeRKN3tvg12RenderRegionEPKNS3_4FillEh,
     (void* a, void* b, const void* c, const void* d, unsigned char e),
     (a, b, c, d, e), kGradientStroke)
WRAP(bool, _Z11rasterClearP9SwSurfacemmmm,
     (void* a, unsigned long b, unsigned long c, unsigned long d, unsigned long e),
     (a, b, c, d, e), kClear)
WRAP(bool, _Z11shapeGenRleR7SwShapePKN3tvg11RenderShapeERKNS1_6MatrixERKNS1_12RenderRegionERS8_P7SwMpooljbb,
     (void* a, const void* b, const void* c, const void* d, void* e, void* f,
      unsigned int g, bool h, bool i),
     (a, b, c, d, e, f, g, h, i), kShapeRle)
WRAP(bool, _Z9rleRenderP5SwRlePK9SwOutlineRKN3tvg12RenderRegionEP7SwMpooljb,
     (void* a, const void* b, const void* c, void* d, unsigned int e, bool f),
     (a, b, c, d, e, f), kRleRender)
WRAP(bool, _Z17shapeGenStrokeRleR7SwShapePKN3tvg11RenderShapeERKNS1_6MatrixERKNS1_12RenderRegionERS8_P7SwMpooljb,
     (void* a, const void* b, const void* c, const void* d, void* e, void* f,
      unsigned int g, bool h),
     (a, b, c, d, e, f, g, h), kStrokeRle)
WRAP(bool, _Z11fillPrepareRP6SwFillPKN3tvg4FillERKNS2_6MatrixEP9SwSurfacehb,
     (void* a, const void* b, const void* c, void* d, unsigned char e, bool f),
     (a, b, c, d, e, f), kFillPrepare)

#if CONFIG_RIVE_THORVG_FAST_GRADIENT
extern "C" void (*tvgFillSpanHook)(unsigned op, unsigned len, int linear);
// Installs the gradient pixel counter before app_main runs.
__attribute__((constructor)) static void installGradientCounter()
{
  tvgFillSpanHook = countGradientSpan;
}
#endif

void raster_profile_report(const char* tag, int frames)
{
  if (frames <= 0)
  {
    return;
  }
  const float mhz = float(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
  for (int core = 1; core >= 0; --core)
  {
    PerCore& p = g_core[core];
    char line[320];
    int n = snprintf(line, sizeof(line), "  core %d ms/frame:", core);
    for (int c = 0; c < kCount && n < int(sizeof(line)); ++c)
    {
      float ms = float(p.cycles[c]) / (mhz * 1000.0f) / float(frames);
      n += snprintf(line + n, sizeof(line) - n, " %s %.1f (%" PRIu32 ")%s",
                    kNames[c], ms, p.calls[c] / uint32_t(frames),
                    c + 1 < kCount ? " |" : "");
      p.cycles[c] = 0;
      p.calls[c] = 0;
    }
    ESP_LOGI(tag, "%s", line);
#if CONFIG_RIVE_THORVG_FAST_GRADIENT
    ESP_LOGI(tag,
             "  core %d linear-gradient px/frame (fast path only): opaque %" PRIu32
             " | translucent %" PRIu32 " | edges %" PRIu32,
             core, uint32_t(p.gradPx[0] / uint32_t(frames)),
             uint32_t(p.gradPx[1] / uint32_t(frames)),
             uint32_t(p.gradPx[2] / uint32_t(frames)));
    p.gradPx[0] = p.gradPx[1] = p.gradPx[2] = 0;
#endif
  }
}

#else

void raster_profile_report(const char*, int) {}

#endif
