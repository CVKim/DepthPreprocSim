cmake_minimum_required(VERSION 3.20)
project(DepthPreprocSim LANGUAGES CXX)

# Toolset must be v142 (pass -T v142 to the VS 2022/2026 generators); C++17, /fp:precise like the DLL vcxproj.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)

set(OPENCV_ROOT "C:/Program Files/AIV/ThirdParty/opencv/opencv_440" CACHE PATH "Prebuilt OpenCV 4.4.0 root (include/, lib/x64_*, bin/x64_*)")
set(OPENCV_LIB_RELEASE "${OPENCV_ROOT}/lib/x64_Release/opencv_world440.lib")
set(OPENCV_LIB_DEBUG   "${OPENCV_ROOT}/lib/x64_Debug/opencv_world440d.lib")
set(OPENCV_DLL_RELEASE "${OPENCV_ROOT}/bin/x64_Release/opencv_world440.dll")
set(OPENCV_DLL_DEBUG   "${OPENCV_ROOT}/bin/x64_Debug/opencv_world440d.dll")
if(NOT EXISTS "${OPENCV_LIB_RELEASE}")
  message(FATAL_ERROR "OpenCV not found at ${OPENCV_ROOT}; pass -DOPENCV_ROOT=<path>")
endif()

if(MSVC)
  # /utf-8: sources carry Korean comments copied from the site build; without it the CP949 code page
  # can swallow the newline after a multi-byte character and comment out the next line.
  add_compile_options(/W3 /EHsc /fp:precise /wd4819 /MP /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
  # Release: /O2 /Oi /GL + /LTCG (WholeProgramOptimization in the DLL project)
  set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Ob2 /Oi /GL /DNDEBUG")
  set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG /OPT:REF /OPT:ICF")
  set(CMAKE_STATIC_LINKER_FLAGS_RELEASE "${CMAKE_STATIC_LINKER_FLAGS_RELEASE} /LTCG")
  set(CMAKE_CXX_FLAGS_DEBUG "/Od /Zi /RTC1 /D_DEBUG")
endif()

add_library(depth_core STATIC
  core/DepthPreprocCore.cpp
  core/MimReader.cpp
  core/IniReader.cpp
  core/Stats.cpp)
target_include_directories(depth_core PUBLIC "${CMAKE_SOURCE_DIR}/core" "${OPENCV_ROOT}/include")
target_link_libraries(depth_core PUBLIC "$<IF:$<CONFIG:Debug>,${OPENCV_LIB_DEBUG},${OPENCV_LIB_RELEASE}>")

add_executable(depth_sim cli/main.cpp)
target_link_libraries(depth_sim PRIVATE depth_core)
# Relative --out paths resolve under <source>/Result/runs (override at runtime with DEPTH_SIM_OUT_ROOT).
target_compile_definitions(depth_sim PRIVATE "DEPTH_SIM_DEFAULT_OUT_ROOT=L\"${CMAKE_SOURCE_DIR}/Result/runs\"")
# Output: <source>/build/<Config>/depth_sim.exe regardless of the binary dir.
set_target_properties(depth_sim PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/build/Release"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/build/Debug")
add_custom_command(TARGET depth_sim POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<IF:$<CONFIG:Debug>,${OPENCV_DLL_DEBUG},${OPENCV_DLL_RELEASE}>"
          "$<TARGET_FILE_DIR:depth_sim>"
  COMMENT "Copying opencv_world440 DLL next to depth_sim.exe")
