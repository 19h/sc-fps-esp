cmake_minimum_required(VERSION 3.10)
project(DataCoreDumper VERSION 1.0 LANGUAGES CXX C)

# Set C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Configure for Windows DLL
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
set(BUILD_SHARED_LIBS ON)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui) # Adjust if you put imgui elsewhere
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)
# ImGui needs access to its own headers and the backend headers
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
# Ensure ImGui uses the same C++ standard
set_target_properties(imgui PROPERTIES CXX_STANDARD ${CMAKE_CXX_STANDARD})

# Add MinHook library
set(MINHOOK_DIR minhook)
add_library(minhook STATIC
    ${MINHOOK_DIR}/hde/hde32.c
    ${MINHOOK_DIR}/hde/hde64.c
    ${MINHOOK_DIR}/buffer.c
    ${MINHOOK_DIR}/hook.c
    ${MINHOOK_DIR}/trampoline.c
)
target_include_directories(minhook PUBLIC ${MINHOOK_DIR})

## Configure Zydis options before adding it
#set(ZYDIS_DIR zydis)
#set(ZYAN_SYSTEM "WINDOWS" CACHE STRING "Target system for Zydis/Zycore" FORCE)
#set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "Don't build Zydis tools" FORCE)
#set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "Don't build Zydis examples" FORCE)
#set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "Don't build Zydis tests" FORCE)
#set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "Don't build Zydis documentation" FORCE)
#
## Add Zydis as a subdirectory - it will build it for us
#add_subdirectory(${ZYDIS_DIR} ${CMAKE_CURRENT_BINARY_DIR}/zydis)

add_compile_options(-O2 -Wall -static -s)

# Embed frida-gadget.dll as a resource (MinGW-w64)
if(MINGW)
    set(RESOURCE_RC ${CMAKE_CURRENT_SOURCE_DIR}/resources.rc)
    set(RESOURCE_OBJ ${CMAKE_CURRENT_BINARY_DIR}/resources.o)
    add_custom_command(
        OUTPUT ${RESOURCE_OBJ}
        COMMAND ${CMAKE_RC_COMPILER} -O coff ${RESOURCE_RC} ${RESOURCE_OBJ}
        DEPENDS ${RESOURCE_RC}
        COMMENT "Compiling resources.rc to resources.o"
    )
    set(EMBEDDED_RESOURCES ${RESOURCE_OBJ})
else()
    set(EMBEDDED_RESOURCES)
endif()

# Add our DLL
add_library(datacore SHARED main.cpp ${EMBEDDED_RESOURCES})
target_include_directories(datacore PRIVATE
    ${MINHOOK_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

# Find the libraries explicitly using their known paths
find_library(D3D11_LIBRARY_PATH NAMES d3d11 PATHS "${CMAKE_FIND_ROOT_PATH}/lib" NO_DEFAULT_PATH)
find_library(DXGI_LIBRARY_PATH NAMES dxgi PATHS "${CMAKE_FIND_ROOT_PATH}/lib" NO_DEFAULT_PATH)

# Add debug messages to confirm CMake finds them
if(NOT D3D11_LIBRARY_PATH)
    message(FATAL_ERROR "CMake could not find libd3d11.a in ${CMAKE_FIND_ROOT_PATH}/lib")
else()
    message(STATUS "Found d3d11 library at: ${D3D11_LIBRARY_PATH}")
endif()
if(NOT DXGI_LIBRARY_PATH)
    message(FATAL_ERROR "CMake could not find libdxgi.a in ${CMAKE_FIND_ROOT_PATH}/lib")
else()
    message(STATUS "Found dxgi library at: ${DXGI_LIBRARY_PATH}")
endif()

# Order is important for linking static libraries
target_link_libraries(datacore PRIVATE
    minhook
    imgui
#    Zydis::Zydis # Use the exported target - this handles include dirs too
    dbghelp
    psapi
    shlwapi
    # Link using the full paths found above
    "${D3D11_LIBRARY_PATH}"
    "${DXGI_LIBRARY_PATH}"
    dwmapi
    d3dcompiler
)

# Set Windows-specific compile options
target_compile_definitions(datacore PRIVATE
    WIN32
    _WINDOWS
    _USRDLL
    _WIN32_WINNT=0x0601
    WIN32_LEAN_AND_MEAN
    UNICODE
)
target_compile_definitions(minhook PRIVATE WIN32 _WINDOWS UNICODE)

# Set output properties
set_target_properties(datacore PROPERTIES
    OUTPUT_NAME "datacore"
    PREFIX ""
    SUFFIX ".dll"
)

# Set output directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Add injector executable
add_executable(injector injector.c)

# Set console application for Windows
if(MINGW)
    target_link_options(injector PRIVATE -mconsole)
endif()

# Set output properties
set_target_properties(injector PROPERTIES
    OUTPUT_NAME "injector"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# No need for manual link options for Zydis since we're using target_link_libraries
