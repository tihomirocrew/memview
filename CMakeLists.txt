cmake_minimum_required(VERSION 3.25)
project(memview VERSION 0.1.0 LANGUAGES CXX)

configure_file(src/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h @ONLY)

# Static CRT (/MT equivalent; clang maps this to -fms-runtime-lib=static),
# so the exe has no vcruntime/msvcp/ucrt api-ms-* DLL dependencies.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(imgui CONFIG REQUIRED)
find_package(Zydis CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# asmjit + asmtk (text -> bytes assembler) aren't taken from vcpkg: their APIs
# move in lockstep and vcpkg's asmjit lags asmtk's, so both are vendored as
# submodules pinned to matching commits. asmjit is built from source here;
# asmtk's two sources are compiled straight into the target (they inherit our
# static-CRT flags and link against the asmjit target below).
set(ASMJIT_STATIC ON CACHE BOOL "" FORCE)
set(ASMJIT_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(external/asmjit)
set(ASMTK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/asmtk)

# Zydis/Zycore bake in MSVC flag /wd4201, which clang++ (GNU driver)
# doesn't understand. Leaving it only for MSVC frontend.
foreach(_zy_target Zydis::Zydis Zycore::Zycore)
    if(TARGET ${_zy_target})
        set_property(TARGET ${_zy_target} PROPERTY INTERFACE_COMPILE_OPTIONS
            "$<$<CXX_COMPILER_ID:MSVC>:/wd4201>")
    endif()
endforeach()

add_executable(${PROJECT_NAME} WIN32
    src/main.cpp
    src/app.rc
    src/app/window/render.cpp
    src/app/window/window.cpp
    src/memory/value_format.cpp
    src/memory/scan_session.cpp
    src/memory/signature.cpp
    src/app/app.cpp
    src/app/config.cpp
    src/ui/toolbar.cpp
    src/ui/process_picker.cpp
    src/ui/scan_panel.cpp
    src/ui/addy_list.cpp
    src/ui/memory_view.cpp
    src/ui/disasm.cpp
    src/ui/hex_view.cpp
    src/ui/regions.cpp
    ${ASMTK_DIR}/src/asmtk/asmparser.cpp
    ${ASMTK_DIR}/src/asmtk/asmtokenizer.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE src ${ASMTK_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/generated)

# Compiling asmtk's sources into this target rather than a separate DLL, so
# neutralize its dllimport/dllexport decoration. ASMJIT_STATIC matches how the
# vendored asmjit is built above (its headers gate their own decoration on it).
target_compile_definitions(${PROJECT_NAME} PRIVATE ASMTK_STATIC ASMJIT_STATIC)

target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall -Wextra
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    imgui::imgui
    Zydis::Zydis
    asmjit::asmjit
    nlohmann_json::nlohmann_json
    d3d11
    dxgi
    d3dcompiler
    shell32
    dwmapi
)
