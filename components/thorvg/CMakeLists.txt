# ThorVG v1.1.x (software renderer only), built from the third_party/thorvg
# git submodule.
set(THORVG_DIR "${CMAKE_CURRENT_LIST_DIR}/../../third_party/thorvg")
if(CONFIG_RIVE_THORVG_FAST_GRADIENT)
    set(THORVG_FAST_GRADIENT ON)
else()
    set(THORVG_FAST_GRADIENT OFF)
endif()
if(CONFIG_RIVE_THORVG_SKIP_PARTIAL_CLEAR)
    set(THORVG_SKIP_PARTIAL_CLEAR ON)
else()
    set(THORVG_SKIP_PARTIAL_CLEAR OFF)
endif()
set(THORVG_RLE_CELL_PERCENT ${CONFIG_RIVE_THORVG_RLE_CELL_PERCENT})
include("${CMAKE_CURRENT_LIST_DIR}/thorvg_sources.cmake")

idf_component_register(
    SRCS ${THORVG_SRCS}
    INCLUDE_DIRS ${THORVG_PUBLIC_INCS}
    PRIV_INCLUDE_DIRS ${THORVG_PRIV_INCS}
    # std::thread / std::mutex when CONFIG_RIVE_THORVG_THREADS is enabled
    PRIV_REQUIRES pthread
    LDFRAGMENTS linker.lf
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC ${THORVG_DEFINES})
# PIE (SIMD) gradient span in fast/sw_fill_fast.inc, ESP32-P4 only.
if(CONFIG_IDF_TARGET_ESP32P4 AND CONFIG_RIVE_THORVG_FAST_GRADIENT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TVG_PIE=1)
endif()
# Third-party code: keep the build output clean. The rasteriser is the hot
# path, so it gets -O3 regardless of the project optimisation level.
target_compile_options(${COMPONENT_LIB} PRIVATE -w -O3 -fno-math-errno)
