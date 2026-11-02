# Shared ThorVG source list, used by the ESP-IDF component (CMakeLists.txt next
# to this file) and by the host test build in /host.
#
# Expects THORVG_DIR to point at a ThorVG v1.1.x checkout.
# Produces THORVG_SRCS, THORVG_PUBLIC_INCS, THORVG_PRIV_INCS, THORVG_DEFINES.
#
# Only the CPU (software) engine and the image loaders Rive needs (PNG, JPG,
# WebP, raw) are compiled. SVG/Lottie/fonts are left out to save flash.

if(NOT EXISTS "${THORVG_DIR}/inc/thorvg.h")
    message(FATAL_ERROR
        "ThorVG sources not found at ${THORVG_DIR}.\n"
        "Run: git submodule update --init --recursive")
endif()

file(GLOB THORVG_SRCS
    "${THORVG_DIR}/src/common/*.cpp"
    "${THORVG_DIR}/src/renderer/*.cpp"
    "${THORVG_DIR}/src/renderer/cpu_engine/*.cpp"
    "${THORVG_DIR}/src/loaders/raw/*.cpp"
    "${THORVG_DIR}/src/loaders/png/*.cpp"
    "${THORVG_DIR}/src/loaders/jpg/*.cpp"
)
file(GLOB_RECURSE THORVG_WEBP_SRCS "${THORVG_DIR}/src/loaders/webp/*.cpp")
list(APPEND THORVG_SRCS ${THORVG_WEBP_SRCS})

# tvg::clamp(x, 0, end) mixes int and int32_t, which only deduce to one type
# where int32_t is int. Pin the template argument.
include("${CMAKE_CURRENT_LIST_DIR}/../patch_source.cmake")
patch_source(THORVG_SRCS "${THORVG_DIR}/src/renderer/cpu_engine/tvgSwRasterCore.cpp"
    "tvg::clamp\\(([a-z]+), 0, end\\)" "tvg::clamp<int32_t>(\\1, 0, end)")

# All tvgSwRaster.cpp patches go through one patch_source() call (each call
# starts again from the original file).
set(_raster_patches "")

# Faster linear-gradient spans (fast/sw_fill_fast.*): the gradient span
# callers pass the blend operator as an enum so the per-pixel loop can be
# specialised. Output is identical. Set THORVG_FAST_GRADIENT=OFF to compare.
if(NOT DEFINED THORVG_FAST_GRADIENT)
    set(THORVG_FAST_GRADIENT ON)
endif()
if(THORVG_FAST_GRADIENT)
    list(APPEND _raster_patches
        "#include \"tvgSwRasterCore.h\""
        "#include \"tvgSwRasterCore.h\"\n#include \"sw_fill_fast.h\""
        "fillRaster\\(fill, (dst|buffer), ([^,;]+), ([^,;]+), ([^,;]+), (opBlendSrcOver|opBlendPreNormal|opBlendNormal|opBlendInterp), "
        "fillRasterOp(fill, \\1, \\2, \\3, \\4, SwFastOp::\\5, ")
    patch_source(THORVG_SRCS "${THORVG_DIR}/src/renderer/cpu_engine/tvgSwFill.cpp"
        "(void fillRaster\\(const SwSurface\\* surface, const SwFill\\* fill, uint32_t\\* dst)"
        "#include \"sw_fill_fast.inc\"\n\n\\1")
endif()

# Bug fix (fast/sw_rle_clip.h): under partial redraw, ThorVG draws unchanged
# gradient shapes once per dirty region without clipping to it, so they are
# drawn several times over their whole area (wrong colours for translucent /
# blended gradients, wrong layering for opaque ones). Clip their spans to the
# region, as the solid-colour paths already do. Needed to keep gradient
# shapes between frames.
if(NOT DEFINED THORVG_FIX_GRADIENT_CLIP)
    set(THORVG_FIX_GRADIENT_CLIP ON)
endif()
if(THORVG_FIX_GRADIENT_CLIP)
    list(APPEND _raster_patches
        "(#include \"tvgSwRasterCore.h\")"
        "\\1\n#include \"sw_rle_clip.h\""
        "else if \\(shape->rle && shape->rle->valid\\(\\)\\) return _rasterGradientRle\\(surface, shape->rle, shape->fill\\)<SEMI>"
        "else if (shape->rle && shape->rle->valid()) { auto clipped = swClipRle(shape->rle, bbox)<SEMI> return clipped->valid() ? _rasterGradientRle(surface, clipped, shape->fill) : true<SEMI> }"
        "return _rasterGradientRle\\(surface, shape->strokeRle, shape->stroke->fill\\)<SEMI>"
        "{ auto clipped = swClipRle(shape->strokeRle, bbox)<SEMI> return clipped->valid() ? _rasterGradientRle(surface, clipped, shape->stroke->fill) : true<SEMI> }")
endif()

# Bigger scanline-converter cell buffer. rleRender() sizes its cell buffer
# as max(w, h) * 0.75 cells and derives the band height from it; when a
# band needs more cells it is thrown away, halved and redone. On the car at
# 720x720 that happened ~13 times per frame; at 2x (200%) almost never,
# cutting rleRender's instructions by 20%. Output is identical (coverage is
# exact per cell). THORVG_RLE_CELL_PERCENT: 75 = stock.
if(NOT DEFINED THORVG_RLE_CELL_PERCENT)
    set(THORVG_RLE_CELL_PERCENT 200)
endif()
if(NOT THORVG_RLE_CELL_PERCENT EQUAL 75)
    patch_source(THORVG_SRCS "${THORVG_DIR}/src/renderer/cpu_engine/tvgSwRle.cpp"
        "\\* 0\\.75f\\)" "* (${THORVG_RLE_CELL_PERCENT} / 100.0f))")
endif()

if(_raster_patches)
    patch_source(THORVG_SRCS "${THORVG_DIR}/src/renderer/cpu_engine/tvgSwRaster.cpp"
        ${_raster_patches})
endif()

# Partial redraw clears every dirty region to transparent before redrawing
# it. Our lanes always draw an opaque full-frame background first, which
# repaints those regions anyway, so the clear is wasted memory traffic
# (12-22 ms per lane at 720x720). With this patch ThorVG defines
# g_tvgSwSkipPartialClear (false = stock behaviour) and skips the clear while
# it's true. rive_thorvg sets it (ThorvgLane::setClearColor, only for an
# opaque background) through a weak reference, so it links without the patch.
if(NOT DEFINED THORVG_SKIP_PARTIAL_CLEAR)
    set(THORVG_SKIP_PARTIAL_CLEAR ON)
endif()
if(THORVG_SKIP_PARTIAL_CLEAR)
    patch_source(THORVG_SRCS "${THORVG_DIR}/src/renderer/cpu_engine/tvgSwRenderer.cpp"
        "(bool SwRenderer::preRender\\(\\))"
        "bool g_tvgSwSkipPartialClear = false<SEMI>\n\n\\1"
        "(//clear buffer for partial regions)"
        "\\1\n    if (!g_tvgSwSkipPartialClear)")
endif()

set(THORVG_PUBLIC_INCS "${THORVG_DIR}/inc")
set(THORVG_PRIV_INCS
    "${CMAKE_CURRENT_LIST_DIR}/config"
    "${CMAKE_CURRENT_LIST_DIR}/fast"
    "${THORVG_DIR}/src/common"
    "${THORVG_DIR}/src/renderer"
    "${THORVG_DIR}/src/renderer/cpu_engine"
    "${THORVG_DIR}/src/loaders/raw"
    "${THORVG_DIR}/src/loaders/png"
    "${THORVG_DIR}/src/loaders/jpg"
    "${THORVG_DIR}/src/loaders/webp"
)
set(THORVG_DEFINES TVG_STATIC)
