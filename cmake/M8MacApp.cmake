# Assemble a self-contained macOS application from the portable bundle.
# Script mode: cmake -DAPP=... -DBUNDLE=... -DPLIST=... -DVERSION=... [-DIDENTITY=...]
#   [-DFIRMWARE_FREE=ON -DFIRMWARE_SHA256=...] -P M8MacApp.cmake
# FIRMWARE_FREE copies only the program, the runtime, Unicorn and SDL3: no
# firmware and no library generated from it (libm8aot, libm8native-*). The
# app then asks for the user's firmware and runs the reference engine.
foreach(required APP BUNDLE PLIST VERSION)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "M8MacApp: ${required} is required")
    endif()
endforeach()
if(NOT IDENTITY)
    set(IDENTITY "-")
endif()
set(contents "${APP}/Contents")
file(REMOVE_RECURSE "${APP}")
file(MAKE_DIRECTORY "${contents}/MacOS" "${contents}/Frameworks" "${contents}/Resources")
configure_file("${PLIST}" "${contents}/Info.plist" @ONLY)
file(COPY "${BUNDLE}/m8emu" DESTINATION "${contents}/MacOS" FOLLOW_SYMLINK_CHAIN)
if(FIRMWARE_FREE)
    if(NOT FIRMWARE_SHA256 MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "M8MacApp: FIRMWARE_FREE needs FIRMWARE_SHA256")
    endif()
    file(COPY "${BUNDLE}/gamecontrollerdb.txt" DESTINATION "${contents}/Resources")
    # An allowlist, so a new generated library cannot slip in.
    file(GLOB libraries "${BUNDLE}/lib/libm8rt.dylib" "${BUNDLE}/lib/libunicorn*.dylib" "${BUNDLE}/lib/libSDL3*.dylib")
else()
    file(COPY "${BUNDLE}/firmware" "${BUNDLE}/gamecontrollerdb.txt" DESTINATION "${contents}/Resources")
    file(GLOB libraries "${BUNDLE}/lib/*.dylib")
endif()
file(COPY ${libraries} DESTINATION "${contents}/Frameworks" FOLLOW_SYMLINK_CHAIN)
# Sources, recipe and capstone of the on-device engine builder: no firmware
# bytes or generated code, which the firmware-free check below still enforces.
if(IS_DIRECTORY "${BUNDLE}/engine-src")
    file(COPY "${BUNDLE}/engine-src" DESTINATION "${contents}/Resources")
endif()
if(NOT FIRMWARE_FREE)
    # SDL_GetBasePath() names Resources; runtime loaders look in its lib folder.
    file(CREATE_LINK "../Frameworks" "${contents}/Resources/lib" SYMBOLIC)
endif()
set(executable "${contents}/MacOS/m8emu")
execute_process(COMMAND install_name_tool -add_rpath "@executable_path/../Frameworks" "${executable}"
    RESULT_VARIABLE result ERROR_VARIABLE output)
if(result)
    message(FATAL_ERROR "install_name_tool -add_rpath failed: ${output}")
endif()
# An installed SDL3 keeps its absolute install name; bundle it behind @rpath.
execute_process(COMMAND otool -L "${executable}" OUTPUT_VARIABLE linked)
string(REGEX MATCH "\n[ \t]*(/[^ \t\n]*/libSDL3[^ \t\n/]*\\.dylib)" sdl "${linked}")
if(sdl)
    set(sdl "${CMAKE_MATCH_1}")
    get_filename_component(sdl_name "${sdl}" NAME)
    file(COPY "${sdl}" DESTINATION "${contents}/Frameworks" FOLLOW_SYMLINK_CHAIN)
    execute_process(COMMAND install_name_tool -id "@rpath/${sdl_name}" "${contents}/Frameworks/${sdl_name}")
    execute_process(COMMAND install_name_tool -change "${sdl}" "@rpath/${sdl_name}" "${executable}"
        RESULT_VARIABLE result ERROR_VARIABLE output)
    if(result)
        message(FATAL_ERROR "install_name_tool -change failed: ${output}")
    endif()
endif()
if(FIRMWARE_FREE)
    # Refuse to sign a bundle holding the firmware or code generated from it.
    file(GLOB_RECURSE shipped LIST_DIRECTORIES false "${APP}/*")
    foreach(path IN LISTS shipped)
        get_filename_component(name "${path}" NAME)
        file(SHA256 "${path}" digest)
        if(name MATCHES "^(libm8aot|libm8native|M8_V)" OR name MATCHES "\\.(bin|hex)$" OR digest STREQUAL FIRMWARE_SHA256)
            message(FATAL_ERROR "M8MacApp: firmware-derived file in a firmware-free app: ${path}")
        endif()
    endforeach()
endif()
# Editing load commands invalidates signatures; sign libraries, then the app.
# "-" signs ad hoc; a keychain identity name signs without notarization.
file(GLOB signed "${contents}/Frameworks/*.dylib" "${contents}/Resources/engine-src/python/capstone/lib/*.dylib")
foreach(path IN LISTS signed ITEMS "${APP}")
    execute_process(COMMAND codesign --force --sign "${IDENTITY}" --timestamp=none "${path}"
        RESULT_VARIABLE result ERROR_VARIABLE output)
    if(result)
        message(FATAL_ERROR "codesign failed for ${path}: ${output}")
    endif()
endforeach()
