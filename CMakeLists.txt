# svr2008 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
# The release version (shown in the in-game settings menu; package_release.ps1 uses it too).
project(svr2008 VERSION 1.0.8 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(SVR2008_SOURCES
    src/main.cpp
    src/fps_hooks.cpp
    src/game_locator.cpp
    src/input_script.cpp
    src/language.cpp
    src/updater.cpp
)

if(WIN32)
    add_executable(svr2008 WIN32 ${SVR2008_SOURCES})
else()
    add_executable(svr2008 ${SVR2008_SOURCES})
endif()

rexglue_setup_target(svr2008 GPU_PLUGINS xenos)
target_compile_definitions(svr2008 PRIVATE SVR_VERSION="${PROJECT_VERSION}")
if(WIN32)
    # Game file picker (src/game_locator.cpp).
    target_link_libraries(svr2008 PRIVATE comdlg32)
    # Update check and download (src/updater.cpp).
    target_link_libraries(svr2008 PRIVATE winhttp)
endif()
# The updater reads GitHub's release info with nlohmann/json and unpacks the release zip with
# stb_image's zlib decoder, both header-only and already in the SDK's third-party tree.
target_include_directories(svr2008 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rexglue-sdk/thirdparty/inja/third_party/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rexglue-sdk/thirdparty/stb)

# Developer call census of the D3D library functions, for native
# renderer reverse engineering. Off by default; never ship a build with it on.
option(SVR_D3D_CENSUS "Count calls to the game's D3D library functions" OFF)
# Native renderer (cmake/native_renderer.cmake). Its hooks override the
# same D3D functions the census wraps, so the two are exclusive.
option(SVR_NATIVE_RENDERER "Draw through plume instead of emulating the Xenos GPU" OFF)
if(SVR_NATIVE_RENDERER AND SVR_D3D_CENSUS)
    message(FATAL_ERROR "SVR_NATIVE_RENDERER and SVR_D3D_CENSUS hook the same functions; pick one")
endif()
if(SVR_D3D_CENSUS)
    target_sources(svr2008 PRIVATE src/dev/d3d_census.cpp src/dev/shader_dump.cpp)
endif()
if(SVR_NATIVE_RENDERER)
    include(cmake/native_renderer.cmake)
    svr_add_native_renderer(svr2008)
endif()

# Overlay fonts (Press Start 2P for the FPS counter, SIL OFL 1.1) next to the executable.
add_custom_command(TARGET svr2008 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/res/fonts" "$<TARGET_FILE_DIR:svr2008>/fonts")
