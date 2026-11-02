# Rive C++ runtime (file loading, animations, state machines), built from the
# third_party/rive-runtime git submodule. Rendering is provided separately by
# the rive_thorvg component.
set(RIVE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../third_party/rive-runtime")
if(CONFIG_RIVE_TEXT)
    set(RIVE_WITH_TEXT ON)
else()
    set(RIVE_WITH_TEXT OFF)
endif()
if(CONFIG_RIVE_LAYOUT)
    set(RIVE_WITH_LAYOUT ON)
else()
    set(RIVE_WITH_LAYOUT OFF)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/rive_sources.cmake")

idf_component_register(
    SRCS ${RIVE_SRCS}
    INCLUDE_DIRS ${RIVE_PUBLIC_INCS}
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE ${RIVE_DEFINES})
target_compile_definitions(${COMPONENT_LIB} PUBLIC ${RIVE_PUBLIC_DEFINES})
# Rive targets C++17; ESP-IDF's default C++ dialect is newer. HarfBuzz is
# built for size, as in rive-runtime's own release builds.
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
    -w)
if(RIVE_HARFBUZZ_SRCS)
    set_source_files_properties(${RIVE_HARFBUZZ_SRCS} ${RIVE_SHEENBIDI_SRCS}
        PROPERTIES COMPILE_OPTIONS "-Os")
endif()
