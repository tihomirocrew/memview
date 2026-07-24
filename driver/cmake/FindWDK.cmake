# FindWDK.cmake - locate the Windows Driver Kit and build kernel drivers with plain
# CMake (Ninja + cl or clang-cl), no Visual Studio WDK integration required.
#
# Adapted from Sergius Nyah's FindWDK (MIT). Trimmed to what memview.sys needs: a
# non-KMDF (WDM) driver. Provides:
#   WDK_ROOT / WDK_VERSION    - discovered kit location and version
#   WDK::<LIB>                - imported target per km library (e.g. WDK::NTOSKRNL)
#   wdk_add_driver(<target> <sources...>)  - a target that links into a .sys
#
# Requires an MSVC-compatible compiler (cl.exe or clang-cl); the flags below are
# MSVC-style. Configure the driver on its own - it does not share the app toolchain.

# --- Locate the kit ---------------------------------------------------------

if(DEFINED ENV{WDKContentRoot})
    file(GLOB WDK_NTDDK_FILES "$ENV{WDKContentRoot}/Include/*/km/ntddk.h")
else()
    file(GLOB WDK_NTDDK_FILES
        "C:/Program Files (x86)/Windows Kits/10/Include/*/km/ntddk.h"
        "C:/Program Files/Windows Kits/10/Include/*/km/ntddk.h")
endif()

if(WDK_NTDDK_FILES)
    list(SORT WDK_NTDDK_FILES COMPARE NATURAL)
    list(GET WDK_NTDDK_FILES -1 WDK_LATEST_NTDDK_FILE)
endif()

if(NOT WDK_LATEST_NTDDK_FILE)
    if(WDK_FIND_REQUIRED)
        message(FATAL_ERROR "WDK (Windows Driver Kit) not found. Install it from "
            "https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk")
    endif()
    return()
endif()

# .../Include/<version>/km/ntddk.h  ->  WDK_ROOT and WDK_VERSION
get_filename_component(WDK_ROOT    "${WDK_LATEST_NTDDK_FILE}" DIRECTORY) # .../km
get_filename_component(WDK_ROOT    "${WDK_ROOT}" DIRECTORY)              # .../<version>
get_filename_component(WDK_VERSION "${WDK_ROOT}" NAME)                   # <version>
get_filename_component(WDK_ROOT    "${WDK_ROOT}" DIRECTORY)              # .../Include
get_filename_component(WDK_ROOT    "${WDK_ROOT}" DIRECTORY)              # kit root

message(STATUS "WDK_ROOT:    ${WDK_ROOT}")
message(STATUS "WDK_VERSION: ${WDK_VERSION}")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(WDK_PLATFORM "x64")
else()
    set(WDK_PLATFORM "x86")
endif()

# --- Flags ------------------------------------------------------------------

set(WDK_COMPILE_FLAGS
    /Zp8    # 8-byte struct packing, matching the kernel ABI
    /GF     # pool identical strings
    /GR-    # no RTTI
    /Gz     # __stdcall by default
    /kernel # emit a kernel-mode object
)

set(WDK_COMPILE_DEFINITIONS WINNT=1 _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000000)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    list(APPEND WDK_COMPILE_DEFINITIONS _WIN64 _AMD64_ AMD64)
else()
    list(APPEND WDK_COMPILE_DEFINITIONS _X86_=1 i386=1 STD_CALL)
endif()

# Driver link: native subsystem, /DRIVER, no default CRT, standard section merges.
string(CONCAT WDK_LINK_FLAGS
    "/MANIFEST:NO "
    "/DRIVER "
    "/OPT:REF "
    "/OPT:ICF "
    "/INCREMENTAL:NO "
    "/SUBSYSTEM:NATIVE "
    "/MERGE:_TEXT=.text;_PAGE=PAGE "
    "/NODEFAULTLIB "
    "/SECTION:INIT,d "
    "/VERSION:10.0 ")

# --- Imported targets for the km libraries ----------------------------------

file(GLOB WDK_LIBRARIES "${WDK_ROOT}/Lib/${WDK_VERSION}/km/${WDK_PLATFORM}/*.lib")
foreach(LIBRARY IN LISTS WDK_LIBRARIES)
    get_filename_component(LIBRARY_NAME "${LIBRARY}" NAME_WE)
    string(TOUPPER "${LIBRARY_NAME}" LIBRARY_NAME)
    add_library(WDK::${LIBRARY_NAME} INTERFACE IMPORTED)
    set_property(TARGET WDK::${LIBRARY_NAME}
        PROPERTY INTERFACE_LINK_LIBRARIES "${LIBRARY}")
endforeach()

# --- The driver helper ------------------------------------------------------

function(wdk_add_driver _target)
    add_executable(${_target} ${ARGN})

    set_target_properties(${_target} PROPERTIES
        SUFFIX ".sys"
        COMPILE_OPTIONS     "${WDK_COMPILE_FLAGS}"
        COMPILE_DEFINITIONS "${WDK_COMPILE_DEFINITIONS}"
        LINK_FLAGS          "${WDK_LINK_FLAGS}"
        MSVC_RUNTIME_LIBRARY "")   # no /MD|/MT - the kernel has no user CRT

    target_include_directories(${_target} SYSTEM PRIVATE
        "${WDK_ROOT}/Include/${WDK_VERSION}/shared"
        "${WDK_ROOT}/Include/${WDK_VERSION}/km"
        "${WDK_ROOT}/Include/${WDK_VERSION}/km/crt")

    target_link_libraries(${_target}
        WDK::NTOSKRNL WDK::HAL WDK::WMILIB)

    # The /GS security cookie + GsDriverEntry stub come from BufferOverflow*K.lib;
    # its name varies across WDK versions, so link whichever is present.
    if(TARGET WDK::BUFFEROVERFLOWFASTFAILK)
        target_link_libraries(${_target} WDK::BUFFEROVERFLOWFASTFAILK)
    elseif(TARGET WDK::BUFFEROVERFLOWK)
        target_link_libraries(${_target} WDK::BUFFEROVERFLOWK)
    endif()

    # WDM entry: the CRT stub GsDriverEntry sets up /GS then calls our DriverEntry.
    set_property(TARGET ${_target} APPEND_STRING
        PROPERTY LINK_FLAGS " /ENTRY:GsDriverEntry")
endfunction()
