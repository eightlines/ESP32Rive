# Rive C++ runtime (file loading, animations, state machines), built from the
# third_party/rive-runtime git submodule. Rendering is provided separately by
# the rive_thorvg component.
set(RIVE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../third_party/rive-runtime")
include("${CMAKE_CURRENT_LIST_DIR}/rive_sources.cmake")

idf_component_register(
    SRCS ${RIVE_SRCS}
    INCLUDE_DIRS ${RIVE_PUBLIC_INCS}
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE ${RIVE_DEFINES})
# Rive targets C++17; ESP-IDF's default C++ dialect is newer.
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
    -w)
