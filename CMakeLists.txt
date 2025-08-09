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

add_executable(wanderforge
  src/main.cpp
)

target_link_libraries(wanderforge PRIVATE Vulkan::Vulkan)

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

