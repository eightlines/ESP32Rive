// Included into ThorVG's tvgSwFill.cpp (see sw_fill_fast.h), after the linear
// gradient code, so it can use the file's static helpers.
#include "sw_fill_fast.h"

// Optional profiling hook (see sw_fill_fast.h): called once per span.
extern "C" {
void (*tvgFillSpanHook)(unsigned op, unsigned len, int linear) = nullptr;
bool tvgPieEnabled = false;
}

namespace
{

template <SwFastOp Op>
inline uint32_t _fastBlend(uint32_t s, uint32_t d, uint8_t a)
{
  if constexpr (Op == SwFastOp::opBlendSrcOver) return opBlendSrcOver(s, d, a);
  else if constexpr (Op == SwFastOp::opBlendPreNormal) return opBlendPreNormal(s, d, a);
  else if constexpr (Op == SwFastOp::opBlendNormal) return opBlendNormal(s, d, a);
  else return opBlendInterp(s, d, a);
}

// _fixedPixel() for FillSpread::Pad, given the position already biased by
// FIXPT_SIZE / 2: same index, same clamping, no spread switch.
inline uint32_t _padPixel(const uint32_t* ctable, int32_t biased)
{
  int32_t i = biased >> FIXPT_BITS;
  if (i >= SW_COLOR_TABLE) i = SW_COLOR_TABLE - 1;
  else if (i < 0) i = 0;
  return ctable[i];
}

inline bool _inTable(int32_t biased)
{
  int32_t i = biased >> FIXPT_BITS;
  return i >= 0 && i < SW_COLOR_TABLE;
}

constexpr uint32_t kChunk = 8;

// Pad-spread linear span, in chunks of 8 pixels. The table index is linear
// in x, so if the first and last pixel of a chunk index inside the table,
// every pixel in between does too and the chunk needs no clamping. Same
// arithmetic as ThorVG's loop (pos + FIXPT_SIZE / 2) >> FIXPT_BITS, with
// pos advancing by inc2 per pixel, so the output is identical.
template <SwFastOp Op>
void _padSpan(const uint32_t* ctable, uint32_t* dst, uint32_t len, int32_t t2, int32_t inc2, uint8_t a)
{
  int32_t b = t2 + (FIXPT_SIZE / 2);
  uint32_t j = 0;
  for (; j + kChunk <= len; j += kChunk) {
    if (_inTable(b) && _inTable(b + int32_t(kChunk - 1) * inc2)) {
      for (uint32_t k = 0; k < kChunk; ++k, ++dst) {
        *dst = _fastBlend<Op>(ctable[b >> FIXPT_BITS], *dst, a);
        b += inc2;
      }
    } else {
      for (uint32_t k = 0; k < kChunk; ++k, ++dst) {
        *dst = _fastBlend<Op>(_padPixel(ctable, b), *dst, a);
        b += inc2;
      }
    }
  }
  for (; j < len; ++j, ++dst) {
    *dst = _fastBlend<Op>(_padPixel(ctable, b), *dst, a);
    b += inc2;
  }
}

#if TVG_PIE
// PIE (ESP32-P4 SIMD) version of the translucent span: d = s + (d * (256 -
// alpha(s))) >> 8 per byte (ThorVG's ALPHA_BLEND(d, IA(s)) increments the
// factor), 4 pixels per vector. The table lookup stays scalar (one gather
// per pixel into an aligned buffer, plus the factor 256 - alpha replicated
// into all 4 bytes); the multiply-shift and add run on 16 bytes at once:
// esp.vmul.u8 with SAR = 8 gives (d * m) >> 8 per byte, and esp.vadd.u8
// adds the source. The factor only fits a byte while alpha > 0; a chunk
// with a fully transparent source pixel (result: d) is done in scalar.
// (For valid premultiplied pixels no byte carries, so the per-byte add
// equals ThorVG's 32-bit add.) Whether esp.vmul.u8 really behaves like this
// is checked against the C version by a self-test at boot, which sets
// tvgPieEnabled; until then the C version runs.
void _padSpanPreNormalPie(const uint32_t* ctable, uint32_t* dst, uint32_t len, int32_t t2, int32_t inc2)
{
  int32_t b = t2 + (FIXPT_SIZE / 2);
  // Scalar until dst is 16-byte aligned (vector loads/stores need it).
  while (len > 0 && (reinterpret_cast<uintptr_t>(dst) & 15) != 0) {
    *dst = opBlendPreNormal(_padPixel(ctable, b), *dst, 255);
    ++dst; --len; b += inc2;
  }
  alignas(16) uint32_t src[kChunk];
  alignas(16) uint32_t ia[kChunk];
  // PIE instructions only take x8-x15 (the RVC "compressed" registers) as
  // operands, so pin them there with explicit register variables.
  {
    register uint32_t sar asm("a4") = 8;
    asm volatile("esp.movx.w.sar %0" : : "r"(sar));
  }
  while (len >= kChunk) {
    uint32_t transparent = 0; // any source pixel with alpha 0 in the chunk
    if (_inTable(b) && _inTable(b + int32_t(kChunk - 1) * inc2)) {
      for (uint32_t k = 0; k < kChunk; ++k) {
        uint32_t s = ctable[b >> FIXPT_BITS];
        src[k] = s;
        const uint32_t alpha = s >> 24;
        ia[k] = (256u - alpha) * 0x01010101u;
        transparent |= alpha == 0;
        b += inc2;
      }
    } else {
      for (uint32_t k = 0; k < kChunk; ++k) {
        uint32_t s = _padPixel(ctable, b);
        src[k] = s;
        const uint32_t alpha = s >> 24;
        ia[k] = (256u - alpha) * 0x01010101u;
        transparent |= alpha == 0;
        b += inc2;
      }
    }
    if (transparent) {
      for (uint32_t k = 0; k < kChunk; ++k) dst[k] = opBlendPreNormal(src[k], dst[k], 255);
      dst += kChunk;
      len -= kChunk;
      continue;
    }
    register uint32_t* d asm("a0") = dst;
    register const uint32_t* ps asm("a1") = src;
    register const uint32_t* pa asm("a2") = ia;
    asm volatile(
        "esp.vld.128.ip q0, %[d], 0\n"
        "esp.vld.128.ip q1, %[a], 16\n"
        "esp.vld.128.ip q2, %[s], 16\n"
        "esp.vmul.u8 q0, q0, q1\n"
        "esp.vadd.u8 q0, q0, q2\n"
        "esp.vst.128.ip q0, %[d], 16\n"
        "esp.vld.128.ip q3, %[d], 0\n"
        "esp.vld.128.ip q4, %[a], 16\n"
        "esp.vld.128.ip q5, %[s], 16\n"
        "esp.vmul.u8 q3, q3, q4\n"
        "esp.vadd.u8 q3, q3, q5\n"
        "esp.vst.128.ip q3, %[d], 16\n"
        : [d] "+r"(d), [a] "+r"(pa), [s] "+r"(ps)
        :
        : "memory");
    dst += kChunk;
    len -= kChunk;
  }
  while (len > 0) {
    *dst = opBlendPreNormal(_padPixel(ctable, b), *dst, 255);
    ++dst; --len; b += inc2;
  }
}
#endif

template <SwFastOp Op>
void _fastLinearSpan(const SwFill* fill, uint32_t* dst, uint32_t len, int32_t t2, int32_t inc2, uint8_t a)
{
  if (fill->spread == FillSpread::Pad) {
#if TVG_PIE
    if constexpr (Op == SwFastOp::opBlendPreNormal) {
      if (tvgPieEnabled) {
        _padSpanPreNormalPie(fill->ctable, dst, len, t2, inc2);
        return;
      }
    }
#endif
    _padSpan<Op>(fill->ctable, dst, len, t2, inc2, a);
  } else {
    for (uint32_t j = 0; j < len; ++j, ++dst) {
      *dst = _fastBlend<Op>(_fixedPixel(fill, t2), *dst, a);
      t2 += inc2;
    }
  }
}

template <SwFastOp Op>
void _fastConstSpan(uint32_t* dst, uint32_t len, uint32_t color, uint8_t a)
{
  for (uint32_t i = 0; i < len; ++i, ++dst) *dst = _fastBlend<Op>(color, *dst, a);
}

SwBlenderA _slowOp(SwFastOp op)
{
  switch (op) {
    case SwFastOp::opBlendSrcOver: return opBlendSrcOver;
    case SwFastOp::opBlendPreNormal: return opBlendPreNormal;
    case SwFastOp::opBlendNormal: return opBlendNormal;
    default: return opBlendInterp;
  }
}

template <SwFastOp Op>
bool _fastLinear(const SwFill* fill, uint32_t* dst, uint32_t y, uint32_t x, uint32_t len, uint8_t a)
{
  // Mirrors _fillLinear(fill, dst, y, x, len, op, a) expression for expression.
  float rx = x + 0.5f;
  float ry = y + 0.5f;
  float t = (fill->linear.dx * rx + fill->linear.dy * ry + fill->linear.offset) * (SW_COLOR_TABLE - 1);
  float inc = (fill->linear.dx) * (SW_COLOR_TABLE - 1);

  if (tvg::zero(inc)) {
    _fastConstSpan<Op>(dst, len, _fixedPixel(fill, static_cast<int32_t>(t * FIXPT_SIZE)), a);
    return true;
  }

  auto vMax = static_cast<float>(INT32_MAX >> (FIXPT_BITS + 1));
  auto vMin = -vMax;
  auto v = t + (inc * len);
  if (!(v < vMax && v > vMin)) return false;  // float path: leave to ThorVG

  _fastLinearSpan<Op>(fill, dst, len, static_cast<int32_t>(t * FIXPT_SIZE), static_cast<int32_t>(inc * FIXPT_SIZE), a);
  return true;
}

} // namespace

// Test entry points for the boot self-test (main/pie_selftest.cpp): the
// translucent Pad span over a raw 1024-entry colour table.
extern "C" void tvgPreNormalSpanC(uint32_t* dst, const uint32_t* ctable, int32_t t2, int32_t inc2, uint32_t len)
{
  _padSpan<SwFastOp::opBlendPreNormal>(ctable, dst, len, t2, inc2, 255);
}

extern "C" void tvgPreNormalSpanRef(uint32_t* dst, const uint32_t* ctable, int32_t t2, int32_t inc2, uint32_t len)
{
  // ThorVG's original per-pixel loop for this case, for reference.
  for (uint32_t j = 0; j < len; ++j, ++dst) {
    *dst = opBlendPreNormal(_padPixel(ctable, t2 + (FIXPT_SIZE / 2)), *dst, 255);
    t2 += inc2;
  }
}

#if TVG_PIE
extern "C" void tvgPreNormalSpanPie(uint32_t* dst, const uint32_t* ctable, int32_t t2, int32_t inc2, uint32_t len)
{
  _padSpanPreNormalPie(ctable, dst, len, t2, inc2);
}
#endif

void fillRasterOp(const SwFill* fill, uint32_t* dst, uint32_t y, uint32_t x, uint32_t len, SwFastOp op, uint8_t a)
{
  if (tvgFillSpanHook) tvgFillSpanHook(static_cast<unsigned>(op), len, fill->type == Type::LinearGradient);
  if (fill->type == Type::LinearGradient) {
    bool done;
    switch (op) {
      case SwFastOp::opBlendSrcOver: done = _fastLinear<SwFastOp::opBlendSrcOver>(fill, dst, y, x, len, a); break;
      case SwFastOp::opBlendPreNormal: done = _fastLinear<SwFastOp::opBlendPreNormal>(fill, dst, y, x, len, a); break;
      case SwFastOp::opBlendNormal: done = _fastLinear<SwFastOp::opBlendNormal>(fill, dst, y, x, len, a); break;
      default: done = _fastLinear<SwFastOp::opBlendInterp>(fill, dst, y, x, len, a); break;
    }
    if (done) return;
  }
  fillRaster(fill, dst, y, x, len, _slowOp(op), a);
}
