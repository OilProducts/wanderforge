cmake_minimum_required(VERSION 3.16)

project(wanderforge
  VERSION 0.1.0
  DESCRIPTION "Wanderforge — experimental Vulkan game scaffold"
  LANGUAGES CXX)

# Require C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Warnings (conservative cross-platform set)
if (MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Find Vulkan SDK or system Vulkan
find_package(Vulkan REQUIRED)

# Try to find GLFW (CONFIG mode preferred). Fallback to PkgConfig.
set(WF_HAVE_GLFW OFF)
find_package(glfw3 CONFIG QUIET)
if (glfw3_FOUND)
  message(STATUS "Found glfw3 via CONFIG: ${glfw3_DIR}")
  set(WF_HAVE_GLFW ON)
  set(WF_GLFW_TARGET glfw)
else()
  find_package(GLFW3 QUIET)
  if (GLFW3_FOUND)
    message(STATUS "Found GLFW3 via module")
    set(WF_HAVE_GLFW ON)
    set(WF_GLFW_INCLUDE_DIR ${GLFW3_INCLUDE_DIR})
    set(WF_GLFW_LIBRARIES ${GLFW3_LIBRARY})
  else()
    include(FindPkgConfig)
    pkg_check_modules(GLFW3 QUIET glfw3)
    if (GLFW3_FOUND)
      message(STATUS "Found glfw3 via pkg-config")
      set(WF_HAVE_GLFW ON)
      set(WF_GLFW_INCLUDE_DIR ${GLFW3_INCLUDE_DIRS})
      set(WF_GLFW_LIBRARIES ${GLFW3_LINK_LIBRARIES})
    endif()
  endif()
endif()

add_executable(wanderforge
  src/main.cpp
  src/vk_app.cpp
)

target_link_libraries(wanderforge PRIVATE Vulkan::Vulkan)
if (WF_HAVE_GLFW)
  if (TARGET glfw)
    target_link_libraries(wanderforge PRIVATE glfw)
  else()
    target_include_directories(wanderforge PRIVATE ${WF_GLFW_INCLUDE_DIR})
    target_link_libraries(wanderforge PRIVATE ${WF_GLFW_LIBRARIES})
  endif()
else()
  message(FATAL_ERROR "GLFW not found. Install glfw development package (e.g., libglfw3-dev on Ubuntu, glfw-devel on Fedora). See README Prerequisites.")
endif()

target_compile_definitions(wanderforge PRIVATE
  $<$<CONFIG:Debug>:WF_DEBUG>
)

target_include_directories(wanderforge PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# On macOS with MoltenVK via Vulkan SDK, ensure rpath can find frameworks if needed.
if (APPLE)
  set_target_properties(wanderforge PROPERTIES
    MACOSX_RPATH ON
  )
endif()

# Optional: Vulkan Memory Allocator (header-only). Define WF_HAVE_VMA if header found.
find_path(VMA_INCLUDE_DIR NAMES vk_mem_alloc.h PATH_SUFFIXES include)
if (VMA_INCLUDE_DIR)
  message(STATUS "Found VMA header at: ${VMA_INCLUDE_DIR}")
  target_include_directories(wanderforge PRIVATE ${VMA_INCLUDE_DIR})
  target_compile_definitions(wanderforge PRIVATE WF_HAVE_VMA)
endif()

# Optional: shader tools (used for development workflows)
find_program(WF_GLSLANG_VALIDATOR NAMES glslangValidator QUIET)
find_program(WF_GLSLC NAMES glslc QUIET)
if (WF_GLSLANG_VALIDATOR)
  message(STATUS "Found glslangValidator: ${WF_GLSLANG_VALIDATOR}")
endif()
if (WF_GLSLC)
  message(STATUS "Found glslc: ${WF_GLSLC}")
endif()
