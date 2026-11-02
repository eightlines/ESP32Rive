# Shared Rive runtime source list, used by the ESP-IDF component (CMakeLists.txt
# next to this file) and by the host test build in /host.
#
# Expects RIVE_DIR to point at a rive-app/rive-runtime checkout, with the
# HarfBuzz, SheenBidi and Yoga checkouts next to it (third_party/).
# Produces RIVE_SRCS, RIVE_PUBLIC_INCS, RIVE_DEFINES (private) and
# RIVE_PUBLIC_DEFINES (must be seen by everything including Rive headers).
#
# Options (default ON): RIVE_WITH_TEXT (HarfBuzz + SheenBidi),
# RIVE_WITH_LAYOUT (Yoga) and RIVE_WITH_AUDIO (miniaudio, external engine:
# the app pulls mixed PCM itself). Built without scripting (Luau). Disabled features are compiled out by the runtime's own WITH_RIVE_*
# guards, so every .cpp can be globbed in. Dependency versions and build
# settings follow rive-runtime/dependencies/premake5_*_v2.lua, without the
# symbol renames (only needed when an app links a second HarfBuzz / Yoga).

if(NOT EXISTS "${RIVE_DIR}/include/rive/file.hpp")
    message(FATAL_ERROR
        "Rive runtime sources not found at ${RIVE_DIR}.\n"
        "Run: git submodule update --init --recursive")
endif()

file(GLOB_RECURSE RIVE_SRCS "${RIVE_DIR}/src/*.cpp")

# color.hpp and color.cpp disagree on `unsigned int` vs `ColorInt`
# (= uint32_t) for two functions. Same type on desktop, different types on
# RISC-V newlib, where uint32_t is `unsigned long`. Make the .cpp match.
include("${CMAKE_CURRENT_LIST_DIR}/../patch_source.cmake")
patch_source(RIVE_SRCS "${RIVE_DIR}/src/shapes/paint/color.cpp"
    "unsigned int colorARGB\\(" "ColorInt colorARGB("
    "float colorOpacity\\(ColorInt value\\)" "float colorOpacity(unsigned int value)")

set(RIVE_PUBLIC_INCS "${RIVE_DIR}/include")
# _RIVE_INTERNAL_ is what Rive's own premake build defines for the library.
set(RIVE_DEFINES _RIVE_INTERNAL_)
set(RIVE_PUBLIC_DEFINES YOGA_EXPORT=)

get_filename_component(RIVE_DEPS_DIR "${RIVE_DIR}/.." ABSOLUTE)
if(NOT DEFINED RIVE_WITH_TEXT)
    set(RIVE_WITH_TEXT ON)
endif()
if(NOT DEFINED RIVE_WITH_LAYOUT)
    set(RIVE_WITH_LAYOUT ON)
endif()
if(NOT DEFINED RIVE_WITH_AUDIO)
    set(RIVE_WITH_AUDIO ON)
endif()

# Text: HarfBuzz (shaping, rive-app/harfbuzz@rive_13.1.1) and SheenBidi
# (bidirectional text, Tehreer/SheenBidi@v2.6). Rive turns glyphs into
# ordinary render paths, so the renderer needs nothing extra.
if(RIVE_WITH_TEXT)
    set(HARFBUZZ_DIR "${RIVE_DEPS_DIR}/harfbuzz")
    set(SHEENBIDI_DIR "${RIVE_DEPS_DIR}/sheenbidi")
    if(NOT EXISTS "${HARFBUZZ_DIR}/src/hb.h" OR NOT EXISTS "${SHEENBIDI_DIR}/Source/SheenBidi.c")
        message(FATAL_ERROR
            "HarfBuzz / SheenBidi not found in ${RIVE_DEPS_DIR}.\n"
            "Run: git submodule update --init --recursive")
    endif()
    set(RIVE_HARFBUZZ_SRCS
        hb-aat-layout.cc
        hb-aat-map.cc
        hb-blob.cc
        hb-buffer.cc
        hb-common.cc
        hb-draw.cc
        hb-face-builder.cc
        hb-face.cc
        hb-font.cc
        hb-map.cc
        hb-number.cc
        hb-ot-cff1-table.cc
        hb-ot-cff2-table.cc
        hb-ot-color.cc
        hb-ot-face.cc
        hb-ot-font.cc
        hb-ot-layout.cc
        hb-ot-map.cc
        hb-ot-math.cc
        hb-ot-meta.cc
        hb-ot-metrics.cc
        hb-ot-name.cc
        hb-ot-shaper-arabic.cc
        hb-ot-shaper-default.cc
        hb-ot-shaper-hangul.cc
        hb-ot-shaper-hebrew.cc
        hb-ot-shaper-indic-table.cc
        hb-ot-shaper-indic.cc
        hb-ot-shaper-khmer.cc
        hb-ot-shaper-myanmar.cc
        hb-ot-shaper-syllabic.cc
        hb-ot-shaper-thai.cc
        hb-ot-shaper-use.cc
        hb-ot-shaper-vowel-constraints.cc
        hb-ot-shape-fallback.cc
        hb-ot-shape-normalize.cc
        hb-ot-shape.cc
        hb-ot-tag.cc
        hb-ot-var.cc
        hb-set.cc
        hb-shape-plan.cc
        hb-shape.cc
        hb-shaper.cc
        hb-static.cc
        hb-ucd.cc
        hb-unicode.cc
        hb-vector.cc
        hb-paint.cc
        hb-paint-bounded.cc
        hb-paint-extents.cc
        hb-outline.cc
        hb-style.cc
        OT/Var/VARC/VARC.cc)
    list(TRANSFORM RIVE_HARFBUZZ_SRCS PREPEND "${HARFBUZZ_DIR}/src/")
    # Same feature trimming as rive-runtime's premake5_harfbuzz_v2.lua.
    set_source_files_properties(${RIVE_HARFBUZZ_SRCS} PROPERTIES COMPILE_DEFINITIONS
        "HB_ONLY_ONE_SHAPER;HAVE_OT;HB_DISABLE_DEPRECATED;HB_NO_BUFFER_SERIALIZE;HB_NO_BUFFER_VERIFY;HB_NO_BUFFER_MESSAGE;HB_NO_FALLBACK_SHAPE;HB_NO_WIN1256;HB_NO_VERTICAL;HB_NO_MATH;HB_NO_BASE;HB_NO_OT_SHAPE_FRACTIONS;HB_NO_OT_SHAPE_FALLBACK;HB_NO_LEGACY;HB_NO_LAYOUT_COLLECT_GLYPHS;HB_NO_LAYOUT_RARELY_USED;HB_NO_LAYOUT_UNUSED;HB_NO_OT_FONT_GLYPH_NAMES;HB_NO_HINTING;HB_NO_NAME;HB_NO_META;HB_NO_METRICS;HB_NO_SVG;HB_NO_AVAR2;HB_NO_VAR_HVF;HB_NO_VAR_COMPOSITES;HB_NO_EXTERN_HELPERS;HB_NO_SETLOCALE;HB_NO_LANGUAGE_LONG;HB_NO_MMAP;HB_NO_ATEXIT;HB_NO_ERRNO;HB_NO_GETENV;HB_NO_OPEN;HB_OPTIMIZE_SIZE;HB_NO_UCD_UNASSIGNED")
    # SheenBidi as one translation unit (its release configuration).
    set(RIVE_SHEENBIDI_SRCS "${SHEENBIDI_DIR}/Source/SheenBidi.c")
    set_source_files_properties(${RIVE_SHEENBIDI_SRCS} PROPERTIES COMPILE_DEFINITIONS "SB_CONFIG_UNITY")
    list(APPEND RIVE_SRCS ${RIVE_HARFBUZZ_SRCS} ${RIVE_SHEENBIDI_SRCS})
    # RISC-V newlib: uint32_t is unsigned long, but HarfBuzz wants an
    # unsigned int* here.
    patch_source(RIVE_SRCS "${RIVE_DIR}/src/text/font_hb.cpp"
        "uint32_t n = 1<SEMI>" "unsigned int n = 1<SEMI>")
    list(APPEND RIVE_PUBLIC_INCS "${HARFBUZZ_DIR}/src" "${SHEENBIDI_DIR}/Headers")
    list(APPEND RIVE_PUBLIC_DEFINES WITH_RIVE_TEXT)
endif()

# Layout: Yoga (flexbox, rive-app/yoga@rive_yoga_dee88d2dfcad).
if(RIVE_WITH_LAYOUT)
    set(YOGA_DIR "${RIVE_DEPS_DIR}/yoga")
    if(NOT EXISTS "${YOGA_DIR}/yoga/Yoga.cpp")
        message(FATAL_ERROR
            "Yoga not found in ${RIVE_DEPS_DIR}.\n"
            "Run: git submodule update --init --recursive")
    endif()
    set(RIVE_YOGA_SRCS
        yoga/Utils.cpp yoga/YGConfig.cpp yoga/YGLayout.cpp yoga/YGEnums.cpp
        yoga/YGNodePrint.cpp yoga/YGNode.cpp yoga/YGValue.cpp yoga/YGStyle.cpp
        yoga/Yoga.cpp yoga/event/event.cpp yoga/log.cpp
        yoga/YGGridTrackList.cpp yoga/grid/GridLayout.cpp)
    list(TRANSFORM RIVE_YOGA_SRCS PREPEND "${YOGA_DIR}/")
    list(APPEND RIVE_SRCS ${RIVE_YOGA_SRCS})
    # RISC-V newlib: std::max(int, int32_t) doesn't deduce (int32_t is long).
    patch_header(RIVE_YOGA_PATCHED_INC "${YOGA_DIR}" "yoga/grid/AutoPlacement.h"
        "std::max\\(largestColumnSpan, columnPlacement\\.span\\)"
        "std::max<int32_t>(largestColumnSpan, columnPlacement.span)")
    # The patched header directory must come before Yoga's own.
    list(APPEND RIVE_PUBLIC_INCS ${RIVE_YOGA_PATCHED_INC} "${YOGA_DIR}")
    list(APPEND RIVE_PUBLIC_DEFINES WITH_RIVE_LAYOUT)
endif()

# Audio: miniaudio (decoding + mixing, rive-app/miniaudio@rive_changes_11),
# as rive-runtime's `--with_rive_audio=external`: no device I/O, the app
# pulls mixed frames with AudioEngine::readAudioFrames and plays them itself.
# The MA_* defines change miniaudio's struct layouts, so they are public and
# must match between miniaudio.c and Rive.
if(RIVE_WITH_AUDIO)
    set(MINIAUDIO_DIR "${RIVE_DEPS_DIR}/miniaudio")
    if(NOT EXISTS "${MINIAUDIO_DIR}/miniaudio.c")
        message(FATAL_ERROR
            "miniaudio not found in ${RIVE_DEPS_DIR}.\n"
            "Run: tools/setup_submodules (sh or ps1)")
    endif()
    set(RIVE_MINIAUDIO_SRCS "${MINIAUDIO_DIR}/miniaudio.c")
    list(APPEND RIVE_SRCS ${RIVE_MINIAUDIO_SRCS})
    # No dlopen (no device backends to load). ESP-IDF's pthreads lack the
    # scheduling-policy attributes miniaudio uses for device threads (none
    # are created here): miniaudio_esp_shim.h stubs them.
    if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
        set_source_files_properties(${RIVE_MINIAUDIO_SRCS} PROPERTIES
            COMPILE_DEFINITIONS MA_NO_RUNTIME_LINKING
            COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/miniaudio_esp_shim.h")
    endif()
    list(APPEND RIVE_PUBLIC_INCS "${MINIAUDIO_DIR}")
    # RISC-V newlib: uint32_t is unsigned long, miniaudio wants ma_uint32*.
    patch_source(RIVE_SRCS "${RIVE_DIR}/src/audio/audio_engine.cpp"
        "uint32_t channelCount = channels\\(\\)<SEMI>"
        "ma_uint32 channelCount = channels()<SEMI>")
    list(APPEND RIVE_PUBLIC_DEFINES
        WITH_RIVE_AUDIO EXTERNAL_RIVE_AUDIO_ENGINE
        MA_NO_DEVICE_IO MA_NO_RESOURCE_MANAGER MA_NO_ENCODING MA_NO_GENERATION)
endif()
