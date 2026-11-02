# patch_source(<sources_var> <file> <regex> <replacement> [<regex> <replacement> ...])
#
# Small portability fixes to third-party sources without forking the
# submodules: copies <file> into the build directory with each <regex>
# replaced and swaps it into <sources_var>. Patterns that are no longer found
# (fixed upstream) are skipped; if none match, the original file is used.
#
# Needed on the ESP32 RISC-V toolchain, where int32_t/uint32_t are `long`
# types rather than `int`.
#
# CMake splits arguments on ';', so write <SEMI> for a semicolon in a regex
# or replacement.
function(patch_source sources_var file)
    file(READ "${file}" content)
    set(pairs ${ARGN})
    set(applied 0)
    list(LENGTH pairs n)
    set(i 0)
    while(i LESS n)
        list(GET pairs ${i} regex)
        math(EXPR j "${i} + 1")
        list(GET pairs ${j} replacement)
        string(REPLACE "<SEMI>" ";" regex "${regex}")
        string(REGEX MATCH "${regex}" found "${content}")
        if(found)
            string(REGEX REPLACE "${regex}" "${replacement}" content "${content}")
            math(EXPR applied "${applied} + 1")
        else()
            message(STATUS "patch_source: '${regex}' not found in ${file} (fixed upstream?)")
        endif()
        math(EXPR i "${i} + 2")
    endwhile()
    if(applied EQUAL 0)
        return()
    endif()
    get_filename_component(name "${file}" NAME)
    set(out "${CMAKE_CURRENT_BINARY_DIR}/patched/${name}")
    string(REPLACE "<SEMI>" ";" content "${content}")
    file(WRITE "${out}.tmp" "${content}")
    # Only touch the file when it changes, to avoid needless rebuilds.
    file(COPY_FILE "${out}.tmp" "${out}" ONLY_IF_DIFFERENT)
    file(REMOVE "${out}.tmp")
    set(_srcs ${${sources_var}})
    list(REMOVE_ITEM _srcs "${file}")
    list(APPEND _srcs "${out}")
    set(${sources_var} ${_srcs} PARENT_SCOPE)
endfunction()

# patch_header(<include_dir_var> <root> <relative path> <regex> <replacement> ...)
#
# Same, for a header: writes the patched copy to
# ${CMAKE_CURRENT_BINARY_DIR}/patched_include/<relative path> and sets
# <include_dir_var> to that include directory, which must come before <root>
# in the include path (the header must be included by that relative path).
function(patch_header include_dir_var root relpath)
    set(file "${root}/${relpath}")
    file(READ "${file}" content)
    set(pairs ${ARGN})
    set(applied 0)
    list(LENGTH pairs n)
    set(i 0)
    while(i LESS n)
        list(GET pairs ${i} regex)
        math(EXPR j "${i} + 1")
        list(GET pairs ${j} replacement)
        string(REPLACE "<SEMI>" ";" regex "${regex}")
        string(REGEX MATCH "${regex}" found "${content}")
        if(found)
            string(REGEX REPLACE "${regex}" "${replacement}" content "${content}")
            math(EXPR applied "${applied} + 1")
        else()
            message(STATUS "patch_header: '${regex}' not found in ${file} (fixed upstream?)")
        endif()
        math(EXPR i "${i} + 2")
    endwhile()
    set(dir "${CMAKE_CURRENT_BINARY_DIR}/patched_include")
    set(out "${dir}/${relpath}")
    if(applied EQUAL 0)
        file(REMOVE "${out}")
        set(${include_dir_var} "" PARENT_SCOPE)
        return()
    endif()
    string(REPLACE "<SEMI>" ";" content "${content}")
    file(WRITE "${out}.tmp" "${content}")
    file(COPY_FILE "${out}.tmp" "${out}" ONLY_IF_DIFFERENT)
    file(REMOVE "${out}.tmp")
    set(${include_dir_var} "${dir}" PARENT_SCOPE)
endfunction()
