cmake_minimum_required(VERSION 3.18)

# Match the active Apple toolchain's SDK before CMake probes the compiler.
if(CMAKE_HOST_APPLE AND NOT CMAKE_TOOLCHAIN_FILE AND NOT CMAKE_OSX_SYSROOT
        AND (NOT DEFINED CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        AND "$ENV{CMAKE_TOOLCHAIN_FILE}" STREQUAL "" AND "$ENV{SDKROOT}" STREQUAL "")
    execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
        RESULT_VARIABLE labyrinth_sdk_result
        OUTPUT_VARIABLE labyrinth_sdk
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(labyrinth_sdk_result STREQUAL "0" AND IS_DIRECTORY "${labyrinth_sdk}")
        set(CMAKE_OSX_SYSROOT "${labyrinth_sdk}" CACHE PATH "macOS SDK selected by xcrun" FORCE)
    endif()
endif()

project(labyrinth_single LANGUAGES CXX)

set(labyrinth_icon_dir "${CMAKE_CURRENT_SOURCE_DIR}/gui/assets")
if(WIN32)
    enable_language(RC)
    configure_file(cmake/LabyrinthIcon.rc.in labyrinth-icon.rc @ONLY)
    set(labyrinth_icon_resource "${CMAKE_CURRENT_BINARY_DIR}/labyrinth-icon.rc")
    set_source_files_properties("${labyrinth_icon_resource}" PROPERTIES
        OBJECT_DEPENDS "${labyrinth_icon_dir}/labyrinth.ico")
endif()

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    add_compile_options(/utf-8 $<$<CONFIG:Release>:/O2>)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options($<$<CONFIG:Release>:-O3>)
endif()
option(LABYRINTH_WITH_GUI "Build the Slint desktop console" OFF)
set(LABYRINTH_SLINT_SOURCE_DIR "" CACHE PATH "Local Slint source tree (optional alternative to an installed Slint CMake package)")
option(LABYRINTH_WITH_ONNX "Build the native ONNX Runtime team model" ON)
set(ONNXRUNTIME_ROOT "" CACHE PATH "Path to an unpacked local ONNX Runtime distribution")
set(LABYRINTH_MODELS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/models" CACHE PATH "Model bundle for the build output and install")
set(LABYRINTH_VC_RUNTIME_DIR "" CACHE PATH "Optional app-local Microsoft VC++ runtime DLL directory")

find_package(Threads REQUIRED)
add_library(labyrinth_core STATIC src/protocol.cpp src/config.cpp src/decisions.cpp src/opening.cpp src/score.cpp src/shop.cpp src/team.cpp src/recruitment.cpp)
target_include_directories(labyrinth_core PUBLIC include)
target_include_directories(labyrinth_core SYSTEM PUBLIC third_party)
if(WIN32)
    target_sources(labyrinth_core PRIVATE src/windows_gpu.cpp)
    target_compile_definitions(labyrinth_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(labyrinth_core PRIVATE dxgi d3d12)
endif()

add_library(labyrinth_server_runtime STATIC src/server.cpp src/simulator.cpp)
target_link_libraries(labyrinth_server_runtime PUBLIC labyrinth_core PRIVATE Threads::Threads)
add_executable(labyrinth_server src/server_main.cpp ${labyrinth_icon_resource})
target_link_libraries(labyrinth_server PRIVATE labyrinth_server_runtime Threads::Threads)
if(WIN32)
    target_compile_definitions(labyrinth_server_runtime PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_compile_definitions(labyrinth_server PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(labyrinth_server_runtime PRIVATE ws2_32)
    if(MINGW)
        target_link_options(labyrinth_server PRIVATE -municode)
    endif()
endif()

if(LABYRINTH_WITH_ONNX)
    if(NOT ONNXRUNTIME_ROOT AND CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64"
            AND (NOT CMAKE_OSX_ARCHITECTURES OR CMAKE_OSX_ARCHITECTURES STREQUAL "arm64"))
        set(labyrinth_local_ort "${CMAKE_CURRENT_SOURCE_DIR}/native/build/onnxruntime-osx-arm64-1.23.2")
        if(EXISTS "${labyrinth_local_ort}/include/onnxruntime_cxx_api.h")
            set(ONNXRUNTIME_ROOT "${labyrinth_local_ort}" CACHE PATH "Path to an unpacked local ONNX Runtime distribution" FORCE)
        endif()
    endif()
    if(NOT EXISTS "${ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
        message(FATAL_ERROR "Set ONNXRUNTIME_ROOT to a local ONNX Runtime distribution (no automatic downloads)")
    endif()
    add_library(onnxruntime SHARED IMPORTED)
    if(WIN32)
        set(ort_runtime "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll")
        set(ort_import "${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib")
        if(NOT EXISTS "${ort_import}")
            message(FATAL_ERROR "Missing ONNX Runtime import library: ${ort_import}")
        endif()
        set_target_properties(onnxruntime PROPERTIES IMPORTED_IMPLIB "${ort_import}")
        set(ort_directml "${ONNXRUNTIME_ROOT}/lib/DirectML.dll")
        if(EXISTS "${ort_directml}")
            install(FILES "${ort_directml}" DESTINATION .)
            add_custom_command(TARGET labyrinth_server POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${ort_directml}" "$<TARGET_FILE_DIR:labyrinth_server>")
        endif()
        set(ort_shared "${ONNXRUNTIME_ROOT}/lib/onnxruntime_providers_shared.dll")
        if(EXISTS "${ort_shared}")
            install(FILES "${ort_shared}" DESTINATION .)
        endif()
        if(LABYRINTH_VC_RUNTIME_DIR)
            install(FILES
                "${LABYRINTH_VC_RUNTIME_DIR}/msvcp140.dll"
                "${LABYRINTH_VC_RUNTIME_DIR}/msvcp140_1.dll"
                "${LABYRINTH_VC_RUNTIME_DIR}/vcruntime140.dll"
                "${LABYRINTH_VC_RUNTIME_DIR}/vcruntime140_1.dll"
                third_party/MICROSOFT-RUNTIME-SOURCE.txt DESTINATION .)
        endif()
    elseif(APPLE)
        get_filename_component(ort_runtime "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.dylib" REALPATH)
        set_target_properties(labyrinth_server PROPERTIES INSTALL_RPATH "@loader_path" BUILD_WITH_INSTALL_RPATH TRUE)
    else()
        get_filename_component(ort_runtime "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.so" REALPATH)
        set_target_properties(labyrinth_server PROPERTIES INSTALL_RPATH "$ORIGIN")
    endif()
    if(NOT EXISTS "${ort_runtime}")
        message(FATAL_ERROR "Missing ONNX Runtime shared library: ${ort_runtime}")
    endif()
    set_target_properties(onnxruntime PROPERTIES IMPORTED_LOCATION "${ort_runtime}")
    target_include_directories(onnxruntime SYSTEM INTERFACE "${ONNXRUNTIME_ROOT}/include")
    add_library(labyrinth_inference STATIC src/inference.cpp)
    target_link_libraries(labyrinth_inference PUBLIC labyrinth_core PRIVATE onnxruntime)
    target_compile_definitions(labyrinth_server_runtime PRIVATE LABYRINTH_WITH_ONNX=1)
    if(WIN32)
        target_compile_definitions(labyrinth_inference PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    if(WIN32 OR APPLE)
        add_custom_command(TARGET labyrinth_server POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${ort_runtime}" "$<TARGET_FILE_DIR:labyrinth_server>")
    endif()
    target_link_libraries(labyrinth_server_runtime PRIVATE labyrinth_inference)
    install(FILES "${ort_runtime}" DESTINATION .)
    install(FILES "${ONNXRUNTIME_ROOT}/LICENSE" DESTINATION . RENAME ONNXRUNTIME-LICENSE)
    install(FILES "${ONNXRUNTIME_ROOT}/ThirdPartyNotices.txt" DESTINATION . RENAME ONNXRUNTIME-THIRD-PARTY-NOTICES OPTIONAL)
    set(labyrinth_model_names context.onnx decoder.onnx critic.onnx metadata.json manifest.json export_verification.json)
    set(labyrinth_model_files ${labyrinth_model_names})
    list(TRANSFORM labyrinth_model_files PREPEND "${LABYRINTH_MODELS_DIR}/")
    install(FILES ${labyrinth_model_files} DESTINATION models)
    set(labyrinth_model2_dir "${CMAKE_CURRENT_SOURCE_DIR}/model2")
    if(EXISTS "${labyrinth_model2_dir}/manifest.json")
        set(labyrinth_model2_files ${labyrinth_model_names})
        list(TRANSFORM labyrinth_model2_files PREPEND "${labyrinth_model2_dir}/")
        install(FILES ${labyrinth_model2_files} DESTINATION model2)
    endif()
endif()

if(LABYRINTH_WITH_GUI)
    if(CMAKE_VERSION VERSION_LESS 3.21)
        message(FATAL_ERROR "The Slint console requires CMake 3.21 or newer")
    endif()
    if(LABYRINTH_SLINT_SOURCE_DIR)
        add_subdirectory("${LABYRINTH_SLINT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/slint" EXCLUDE_FROM_ALL)
        set(slint_notices "${LABYRINTH_SLINT_SOURCE_DIR}/LICENSES")
    else()
        find_package(Slint 1.16 CONFIG REQUIRED)
        set(slint_notices "${Slint_DIR}/../../../licenses")
    endif()
    if(IS_DIRECTORY "${slint_notices}")
        install(DIRECTORY "${slint_notices}/" DESTINATION Slint-Licenses)
    endif()
    set(labyrinth_gui_name "黎明界决策系统")
    add_executable(labyrinth_gui gui/console.cpp gui/settings.cpp ${labyrinth_icon_resource})
    set_target_properties(labyrinth_gui PROPERTIES
        OUTPUT_NAME "${labyrinth_gui_name}"
        SLINT_EMBED_RESOURCES embed-files)
    target_link_libraries(labyrinth_gui PRIVATE labyrinth_server_runtime Slint::Slint Threads::Threads)
    slint_target_sources(labyrinth_gui gui/console.slint)
    if(WIN32)
        target_compile_definitions(labyrinth_gui PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
        if(MINGW)
            target_link_options(labyrinth_gui PRIVATE -municode)
        endif()
        if(LABYRINTH_WITH_ONNX OR TARGET slint_cpp-shared)
            add_custom_command(TARGET labyrinth_gui POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_RUNTIME_DLLS:labyrinth_gui>" "$<TARGET_FILE_DIR:labyrinth_gui>"
                COMMAND_EXPAND_LISTS VERBATIM)
            install(FILES $<TARGET_RUNTIME_DLLS:labyrinth_gui> DESTINATION .)
        endif()
    elseif(APPLE)
        set(labyrinth_mac_icon "${labyrinth_icon_dir}/labyrinth.icns")
        set_source_files_properties("${labyrinth_mac_icon}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        target_sources(labyrinth_gui PRIVATE "${labyrinth_mac_icon}")
        set_target_properties(labyrinth_gui PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME "${labyrinth_gui_name}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "org.autopcr.labyrinth.console"
            MACOSX_BUNDLE_ICON_FILE "labyrinth.icns"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
            MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
            INSTALL_RPATH "@loader_path;@loader_path/../../.."
            BUILD_WITH_INSTALL_RPATH TRUE)
        set(labyrinth_mac_executable "$<TARGET_FILE_DIR:labyrinth_server>/$<TARGET_FILE_NAME:labyrinth_gui>")
        add_custom_command(TARGET labyrinth_gui POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:labyrinth_gui>" "${labyrinth_mac_executable}"
            COMMAND /usr/bin/codesign --force --sign - "$<TARGET_BUNDLE_DIR:labyrinth_gui>"
            VERBATIM)
        install(PROGRAMS "${labyrinth_mac_executable}" DESTINATION .)
    else()
        set_target_properties(labyrinth_gui PROPERTIES INSTALL_RPATH "$ORIGIN")
    endif()
    if(NOT WIN32 AND TARGET slint_cpp-shared)
        if(APPLE)
            add_custom_command(TARGET labyrinth_gui POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:slint_cpp-shared>" "$<TARGET_FILE_DIR:labyrinth_server>"
                VERBATIM)
        else()
            set_property(TARGET labyrinth_gui APPEND PROPERTY BUILD_RPATH "$<TARGET_FILE_DIR:slint_cpp-shared>")
        endif()
        install(FILES "$<TARGET_FILE:slint_cpp-shared>" DESTINATION .)
    endif()
    install(TARGETS labyrinth_gui RUNTIME DESTINATION . BUNDLE DESTINATION .)
    if(WIN32)
        install(FILES
            tools/adb_reverse.cmd
            third_party/adb/windows/adb.exe
            third_party/adb/windows/AdbWinApi.dll
            third_party/adb/windows/AdbWinUsbApi.dll
            DESTINATION .)
    endif()
    install(DIRECTORY data/unit_portraits/ DESTINATION data/unit_portraits
        OPTIONAL FILES_MATCHING PATTERN "*.png")
endif()

set(labyrinth_runtime_dir "$<TARGET_FILE_DIR:labyrinth_server>")
set(labyrinth_runtime_commands
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${labyrinth_runtime_dir}/config" "${labyrinth_runtime_dir}/data"
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/config/labyrinth.json"
        "-DDESTINATION=${labyrinth_runtime_dir}/config/labyrinth.json"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SeedConfig.cmake"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/data/game_catalog.json" "${labyrinth_runtime_dir}/data/game_catalog.json")
if(LABYRINTH_WITH_ONNX)
    list(APPEND labyrinth_runtime_commands
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${labyrinth_runtime_dir}/models"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${labyrinth_model_files}
            "${labyrinth_runtime_dir}/models")
    if(labyrinth_model2_files)
        list(APPEND labyrinth_runtime_commands
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${labyrinth_runtime_dir}/model2"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                ${labyrinth_model2_files} "${labyrinth_runtime_dir}/model2")
    endif()
endif()
if(LABYRINTH_WITH_GUI)
    list(APPEND labyrinth_runtime_commands
        COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/unit_portraits"
            "-DDESTINATION=${labyrinth_runtime_dir}/data/unit_portraits"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyUnitPortraits.cmake")
endif()
add_custom_target(labyrinth_runtime_files ALL
    ${labyrinth_runtime_commands}
    DEPENDS labyrinth_server
    COMMENT "Copying Labyrinth server runtime files"
    VERBATIM)

if(LABYRINTH_WITH_GUI)
    add_dependencies(labyrinth_gui labyrinth_runtime_files)
endif()

add_executable(labyrinth_launcher EXCLUDE_FROM_ALL src/launcher.cpp ${labyrinth_icon_resource})
target_include_directories(labyrinth_launcher SYSTEM PRIVATE third_party)
set_target_properties(labyrinth_launcher PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MINGW)
    target_link_options(labyrinth_launcher PRIVATE -municode -static)
endif()

install(TARGETS labyrinth_server RUNTIME DESTINATION .)
install(FILES LICENSE third_party/THIRD_PARTY_NOTICES.txt DESTINATION .)
install(DIRECTORY docs/ DESTINATION docs FILES_MATCHING PATTERN "*.md")
install(FILES third_party/THIRD_PARTY_NOTICES.txt DESTINATION third_party)
if(WIN32 AND LABYRINTH_WITH_GUI)
    install(FILES docs/README-windows.md DESTINATION . RENAME README.md)
else()
    install(FILES README.md DESTINATION .)
endif()
install(CODE "
    set(SOURCE \"${CMAKE_CURRENT_SOURCE_DIR}/config/labyrinth.json\")
    set(DESTINATION \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/config/labyrinth.json\")
    include(\"${CMAKE_CURRENT_SOURCE_DIR}/cmake/SeedConfig.cmake\")
")
install(FILES data/game_catalog.json DESTINATION data)
