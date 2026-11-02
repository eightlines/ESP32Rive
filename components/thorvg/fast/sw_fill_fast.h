// Faster linear-gradient spans for ThorVG's software renderer.
//
// ThorVG's 32-bit gradient span loop calls the blend operator through a
// function pointer and re-evaluates the spread mode for every pixel. On the
// ESP32-P4's in-order core that dominates gradient fills. The span callers in
// tvgSwRaster.cpp are patched (thorvg_sources.cmake) to call fillRasterOp()
// with the operator as an enum; it runs a loop specialised per operator and
// spread mode, with exactly the same arithmetic, so the output is identical.
// Anything it doesn't handle falls back to ThorVG's own fillRaster().
#pragma once

#include "tvgSwCommon.h"

enum class SwFastOp : uint8_t
{
  opBlendSrcOver,   // s
  opBlendPreNormal, // s + d * (255 - alpha(s))
  opBlendNormal,    // s * a, then over d
  opBlendInterp,    // lerp(d, s, a)
};

// Profiling: when set, called once per gradient span with the operator
// (SwFastOp as unsigned), its length in pixels and whether the gradient is
// linear. Not thread-aware; the callee sorts out which core it runs on.
extern "C" {
extern void (*tvgFillSpanHook)(unsigned op, unsigned len, int linear);

// Use the PIE (ESP32-P4 SIMD) translucent gradient span. Off until the boot
// self-test (main/pie_selftest.cpp) has checked it against the C version.
extern bool tvgPieEnabled;

// Test entry points: a translucent (premultiplied source-over) Pad gradient
// span over a raw 1024-entry colour table, as ThorVG's original loop (Ref),
// the chunked C loop used here (C) and the PIE loop (Pie, P4 builds only).
void tvgPreNormalSpanRef(uint32_t* dst, const uint32_t* ctable, int32_t t2, int32_t inc2, uint32_t len);
void tvgPreNormalSpanC(uint32_t* dst, const uint32_t* ctable, int32_t t2, int32_t inc2, uint32_t len);
void tvgPreNormalSpanPie(uint32_t* dst, const uint32_t* ctable, int32_t t2, int32_t inc2, uint32_t len);
}

// Built with the PIE code (set by components/thorvg/CMakeLists.txt for the
// ESP32-P4).
#ifndef TVG_PIE
#define TVG_PIE 0
#endif

void fillRasterOp(const SwFill* fill, uint32_t* dst, uint32_t y, uint32_t x,
                  uint32_t len, SwFastOp op, uint8_t a);
