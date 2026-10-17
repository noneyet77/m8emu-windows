# Sources a user's Mac needs to build the native engine from its own firmware:
# the generators and headers in aot/, the driver, pinned capstone and the
# recorded compile recipe of m8aot, m8native-step and m8native-regions.
# Configure: include, then m8_engine_source() after those targets exist.
# Script mode stages ${OUTPUT} and writes its content identity to ENGINE_ID.
if(CMAKE_SCRIPT_MODE_FILE)
    foreach(required SOURCE RECIPE DRIVER OUTPUT)
        if(NOT DEFINED ${required})
            message(FATAL_ERROR "M8EngineSource: ${required} is required")
        endif()
    endforeach()
    file(MAKE_DIRECTORY "${OUTPUT}")
    file(COPY "${SOURCE}/aot" DESTINATION "${OUTPUT}" PATTERN "__pycache__" EXCLUDE)
    file(COPY "${DRIVER}" "${RECIPE}" DESTINATION "${OUTPUT}")
    if(CAPSTONE)
        string(REPLACE "|" ";" CAPSTONE "${CAPSTONE}")
        file(COPY ${CAPSTONE} DESTINATION "${OUTPUT}/python"
            PATTERN "__pycache__" EXCLUDE PATTERN "include" EXCLUDE)
    endif()
    file(GLOB_RECURSE files RELATIVE "${OUTPUT}" "${OUTPUT}/*")
    list(SORT files)
    set(identity "")
    foreach(file IN LISTS files)
        if(file STREQUAL "ENGINE_ID" OR file MATCHES "__pycache__")
            continue()
        endif()
        file(SHA256 "${OUTPUT}/${file}" digest)
        string(APPEND identity "${digest}  ${file}\n")
    endforeach()
    string(SHA256 identity "${identity}")
    file(WRITE "${OUTPUT}/ENGINE_ID.tmp" "${identity}\n")
    file(RENAME "${OUTPUT}/ENGINE_ID.tmp" "${OUTPUT}/ENGINE_ID")
    return()
endif()
include_guard(GLOBAL)

function(m8_engine_json_list result)
    set(items "")
    foreach(item IN LISTS ARGN)
        list(APPEND items "\"${item}\"")
    endforeach()
    list(JOIN items ", " items)
    set(${result} "[${items}]" PARENT_SCOPE)
endfunction()

function(m8_engine_source)
    set(targets m8aot m8native-step m8native-regions)
    foreach(target IN LISTS targets)
        if(NOT TARGET ${target})
            return()
        endif()
    endforeach()
    if(NOT APPLE OR NOT M8_NATIVE_CORPUS_CATALOG OR NOT M8_AOT_CODEGEN_V2 OR
            NOT M8_NATIVE_AOT_CODE_GUARD OR M8_NATIVE_PERFORMANCE_SOURCE_COPY OR M8_AOT_AUDIO_POOL)
        message(STATUS "User engine builder: unsupported native profile; not staged")
        return()
    endif()
    # Bundle the exact pinned wheel (ctypes, any Python 3), not whatever
    # capstone the host interpreter carries. Offline: the builder installs it.
    set(capstone_dir "${CMAKE_BINARY_DIR}/engine-recipe/capstone")
    set(capstone "${capstone_dir}/capstone|${capstone_dir}/capstone-5.0.7.dist-info")
    if(NOT EXISTS "${capstone_dir}/capstone-5.0.7.dist-info/LICENSE.TXT")
        list(GET CMAKE_OSX_ARCHITECTURES 0 arch)
        if(NOT arch)
            set(arch "${CMAKE_HOST_SYSTEM_PROCESSOR}")
        endif()
        file(REMOVE_RECURSE "${capstone_dir}")
        execute_process(COMMAND "${M8_AOT_PYTHON}" -m pip download --disable-pip-version-check --no-input
                --no-deps --only-binary=:all: --platform "macosx_11_0_${arch}" --implementation py --abi none
                --timeout 45 --retries 2 -d "${capstone_dir}/wheel" capstone==5.0.7
            RESULT_VARIABLE status OUTPUT_QUIET ERROR_VARIABLE error TIMEOUT 300)
        file(GLOB wheel "${capstone_dir}/wheel/capstone-5.0.7-*.whl")
        if(status EQUAL 0 AND wheel)
            file(ARCHIVE_EXTRACT INPUT "${wheel}" DESTINATION "${capstone_dir}")
        endif()
        if(NOT EXISTS "${capstone_dir}/capstone/__init__.py")
            set(capstone "")
            message(STATUS "User engine builder: capstone 5.0.7 not bundled (${error}); the builder installs it")
        endif()
    endif()
    string(TOUPPER "${CMAKE_BUILD_TYPE}" config)
    get_directory_property(directory_definitions COMPILE_DEFINITIONS)
    m8_engine_json_list(directory_definitions ${directory_definitions})
    m8_engine_json_list(translate_flags ${aot_generator_flags})
    m8_engine_json_list(corpus_args ${m8_corpus_scope_args})
    set(q "\"")
    set(json_targets "")
    foreach(target IN LISTS targets)
        string(MAKE_C_IDENTIFIER "${target}_EXPORTS" define_symbol)
        set(fields "")
        foreach(field IN ITEMS definitions:COMPILE_DEFINITIONS includes:INCLUDE_DIRECTORIES
                options:COMPILE_OPTIONS link_options:LINK_OPTIONS libraries:LINK_LIBRARIES sources:SOURCES)
            string(REPLACE ":" ";" field "${field}")
            list(GET field 0 key)
            list(GET field 1 property)
            string(APPEND fields ",\n      ${q}${key}${q}: [${q}$<JOIN:$<TARGET_PROPERTY:${target},${property}>,${q}$<COMMA> ${q}>${q}]")
        endforeach()
        list(APPEND json_targets "    ${q}${target}${q}: {\n      ${q}file${q}: ${q}$<TARGET_FILE_NAME:${target}>${q},\n      ${q}define_symbol${q}: ${q}${define_symbol}${q}${fields}\n    }")
    endforeach()
    list(JOIN json_targets ",\n" json_targets)
    set(json_sources "")
    get_target_property(region_sources m8native-regions SOURCES)
    foreach(source IN LISTS region_sources)
        get_source_file_property(definitions "${source}" COMPILE_DEFINITIONS)
        get_source_file_property(options "${source}" COMPILE_OPTIONS)
        if(NOT definitions)
            set(definitions "")
        endif()
        if(NOT options)
            set(options "")
        endif()
        m8_engine_json_list(definitions ${definitions})
        m8_engine_json_list(options ${options})
        list(APPEND json_sources "    ${q}${source}${q}: {${q}definitions${q}: ${definitions}, ${q}options${q}: ${options}}")
    endforeach()
    list(JOIN json_sources ",\n" json_sources)
    if(CMAKE_C_EXTENSIONS OR NOT DEFINED CMAKE_C_EXTENSIONS)
        set(standard "${CMAKE_C${CMAKE_C_STANDARD}_EXTENSION_COMPILE_OPTION}")
    else()
        set(standard "${CMAKE_C${CMAKE_C_STANDARD}_STANDARD_COMPILE_OPTION}")
    endif()
    set(recipe "${CMAKE_BINARY_DIR}/engine-recipe/recipe.json")
    file(GENERATE OUTPUT "${recipe}" CONTENT "{
  ${q}schema${q}: ${q}m8-engine-recipe/1${q},
  ${q}version${q}: ${q}${PROJECT_VERSION}${q},
  ${q}source_dir${q}: ${q}${CMAKE_SOURCE_DIR}${q},
  ${q}binary_dir${q}: ${q}${CMAKE_BINARY_DIR}${q},
  ${q}firmware_sha256${q}: ${q}${M8_FIRMWARE_BIN_SHA256}${q},
  ${q}compiler${q}: ${q}${CMAKE_C_COMPILER}${q},
  ${q}c_flags${q}: ${q}${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${config}}${q},
  ${q}standard${q}: ${q}${standard}${q},
  ${q}pic${q}: ${q}${CMAKE_C_COMPILE_OPTIONS_PIC}${q},
  ${q}architectures${q}: ${q}${CMAKE_OSX_ARCHITECTURES}${q},
  ${q}sysroot${q}: ${q}${CMAKE_OSX_SYSROOT}${q},
  ${q}deployment${q}: ${q}${CMAKE_C_OSX_DEPLOYMENT_TARGET_FLAG}${CMAKE_OSX_DEPLOYMENT_TARGET}${q},
  ${q}shared_flags${q}: ${q}${CMAKE_SHARED_LIBRARY_CREATE_C_FLAGS}${q},
  ${q}soname_flag${q}: ${q}${CMAKE_SHARED_LIBRARY_SONAME_C_FLAG}${q},
  ${q}rpath_flag${q}: ${q}${CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG}${q},
  ${q}rpath${q}: ${q}${M8_RPATH_ORIGIN}${q},
  ${q}directory_definitions${q}: ${directory_definitions},
  ${q}translate_flags${q}: ${translate_flags},
  ${q}corpus_args${q}: ${corpus_args},
  ${q}targets${q}: {
${json_targets}
  },
  ${q}source_properties${q}: {
${json_sources}
  }
}
")
    set(driver "${CMAKE_SOURCE_DIR}/hosts/sdl/common/engine_build.py")
    file(GLOB_RECURSE inputs CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/aot/*")
    add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/engine-src/ENGINE_ID"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${CMAKE_BINARY_DIR}/engine-src"
        COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${CMAKE_SOURCE_DIR}" "-DRECIPE=${recipe}"
            "-DDRIVER=${driver}" "-DCAPSTONE=${capstone}" "-DOUTPUT=${CMAKE_BINARY_DIR}/engine-src"
            -P "${CMAKE_SOURCE_DIR}/cmake/M8EngineSource.cmake"
        DEPENDS ${inputs} "${recipe}" "${driver}" "${CMAKE_SOURCE_DIR}/cmake/M8EngineSource.cmake"
        COMMENT "Stage the user engine builder sources" VERBATIM)
    add_custom_target(m8emu-engine-src ALL DEPENDS "${CMAKE_BINARY_DIR}/engine-src/ENGINE_ID")
    if(TARGET m8emu-app)
        add_dependencies(m8emu-app m8emu-engine-src)
    endif()
endfunction()
