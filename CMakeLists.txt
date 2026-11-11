cmake_minimum_required(VERSION 3.24)
project(SkyrimYacht VERSION 0.9.9 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- CommonLibSSE-NG (git submodule: alandtse/CommonLibSSE-NG v15.7.3, as Hearthcards): one DLL for SE 1.5.97, AE and
# 1.7.99+ (the new Address Library format)
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/CommonLibSSE-NG EXCLUDE_FROM_ALL)
include(extern/CommonLibSSE-NG/cmake/CommonLibSSE.cmake)

find_package(imgui CONFIG REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME}
    AUTHOR "evodube"
    USE_ADDRESS_LIBRARY
    SOURCES src/plugin/main.cpp src/plugin/Config.cpp src/plugin/Overlay.cpp src/plugin/Scene.cpp src/plugin/Game.cpp src/plugin/Audio.cpp src/plugin/Menu.cpp src/plugin/Render3D.cpp src/plugin/StbVorbis.cpp
            src/sim/Dice.cpp src/sim/Yacht.cpp src/sim/Ai.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE src/plugin extern/SKSEMenuFramework)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/plugin/PCH.h)
target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(${PROJECT_NAME} PRIVATE imgui::imgui d3d11 dxgi d3dcompiler ole32)

if (MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8 /permissive- /Zc:preprocessor)
    target_link_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:Release>:/DEBUG:FULL;/OPT:REF;/OPT:ICF>")
    # the physics is plain C++ with no PCH of its own
    set_source_files_properties(src/sim/Dice.cpp src/sim/Yacht.cpp src/sim/Ai.cpp src/plugin/StbVorbis.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()
