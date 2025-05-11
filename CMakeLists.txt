cmake_minimum_required(VERSION 3.10)

project(DataCoreDumper VERSION 1.0 LANGUAGES CXX C)

# Resource compiler removed as it's not needed anymore

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Configure for Windows DLL
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
set(BUILD_SHARED_LIBS ON)

add_compile_options(-mavx2 -O2 -Wall)
#add_compile_options(-march=haswell -mavx -O3 -ffast-math -flto -Ofast -Wall -static -s)

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

# Add concurrentqueue library
add_subdirectory(concurrentqueue)

## Configure Zydis options before adding it
set(ZYDIS_DIR zydis)
set(ZYAN_SYSTEM "WINDOWS" CACHE STRING "Target system for Zydis/Zycore" FORCE)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "Don't build Zydis tools" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "Don't build Zydis examples" FORCE)
set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "Don't build Zydis tests" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "Don't build Zydis documentation" FORCE)

# Add Zydis as a subdirectory - it will build it for us
add_subdirectory(${ZYDIS_DIR} ${CMAKE_CURRENT_BINARY_DIR}/zydis)

# Resource compilation removed as it's not needed anymore
set(EMBEDDED_RESOURCES)

# Add our DLL
add_library(datacore SHARED main.cpp)
target_include_directories(datacore PRIVATE
        ${MINHOOK_DIR}
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${CMAKE_CURRENT_SOURCE_DIR}/concurrentqueue
        ${CMAKE_CURRENT_SOURCE_DIR}/zydis/include
        ${CMAKE_CURRENT_SOURCE_DIR}/zydis/dependencies/zycore/include
)

# Disable Interprocedural Optimization to avoid gold linker issues
set_property(TARGET datacore PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
set_property(TARGET minhook PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
set_property(TARGET imgui PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)

# Find the libraries explicitly using their known paths
find_library(D3D11_LIBRARY_PATH NAMES d3d11 PATHS "${CMAKE_FIND_ROOT_PATH}/lib" "${CMAKE_FIND_ROOT_PATH}/x86_64-w64-mingw32/lib")
find_library(DXGI_LIBRARY_PATH NAMES dxgi PATHS "${CMAKE_FIND_ROOT_PATH}/lib" "${CMAKE_FIND_ROOT_PATH}/x86_64-w64-mingw32/lib")

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
        concurrentqueue
        Zydis::Zydis # Use the exported target - this handles include dirs too
        dbghelp
        psapi
        shlwapi
        # Link using the full paths found above
        "${D3D11_LIBRARY_PATH}"
        "${DXGI_LIBRARY_PATH}"
        dwmapi
        d3dcompiler
        pthread
        atomic  # For threading atomics
)

# Add linker options to use lld instead of the default linker and avoid gold plugin
# Use static linking for C/C++ standard libraries to avoid dependency issues
target_link_options(datacore PRIVATE
    -static-libgcc
    -static-libstdc++
    -Wl,--export-all-symbols    # Export all symbols to ensure they're visible
    -Wl,--allow-multiple-definition  # Allow duplicate symbols as a fallback solution
)

# Set Windows-specific compile options
target_compile_definitions(datacore PRIVATE
        WIN32
        _WINDOWS
        _USRDLL
        _WIN32_WINNT=0x0601
        NOMINMAX
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
