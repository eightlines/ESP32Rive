# Shared Rive runtime source list, used by the ESP-IDF component (CMakeLists.txt
# next to this file) and by the host test build in /host.
#
# Expects RIVE_DIR to point at a rive-app/rive-runtime checkout.
# Produces RIVE_SRCS, RIVE_PUBLIC_INCS, RIVE_DEFINES.
#
# Built without text (HarfBuzz/SheenBidi), audio (miniaudio), layout (Yoga) and
# scripting (Luau). Those features are compiled out by the runtime's own
# WITH_RIVE_* guards, so every .cpp can be globbed in.

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
set(RIVE_DEFINES _RIVE_INTERNAL_ YOGA_EXPORT=)
