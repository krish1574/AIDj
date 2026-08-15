cmake_minimum_required(VERSION 3.22)
project(aidj_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Platform-independent core. Everything here builds on a desktop host, which is
# the whole point: the DSP and mixing logic must be testable without a device.
set(AIDJ_CORE_SOURCES
    src/mix/Mixer.cpp
    src/dsp/Resampler.cpp
    src/DecodePump.cpp
    src/Engine.cpp
)

add_library(aidj_engine STATIC ${AIDJ_CORE_SOURCES})
target_include_directories(aidj_engine PUBLIC src)

if(NOT MSVC)
  target_compile_options(aidj_engine PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(ANDROID)
  target_sources(aidj_engine PRIVATE
      src/platform/android/OboeOutput.cpp
      src/platform/android/MediaCodecDecoder.cpp
  )

  # Oboe arrives as a prefab package from the Gradle dependency rather than a
  # vendored source copy, so it stays on its own release cadence.
  find_package(oboe REQUIRED CONFIG)

  target_link_libraries(aidj_engine PUBLIC
      oboe::oboe
      mediandk
      log
      android
  )

  # Denormals cost real cycles in fade tails on ARM; flushing them to zero is
  # standard practice in audio callbacks and inaudible.
  target_compile_options(aidj_engine PRIVATE -ffast-math -fno-finite-math-only)
endif()

option(AIDJ_BUILD_TESTS "Build host-side engine tests" OFF)

if(AIDJ_BUILD_TESTS AND NOT ANDROID)
  enable_testing()
  add_subdirectory(tests)
endif()
