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
    src/dsp/ThreeBandEq.cpp
    src/dsp/TimeStretch.cpp
    src/DecodePump.cpp
    src/Engine.cpp
    src/analysis/Fft.cpp
    src/analysis/OnsetEnvelope.cpp
    src/analysis/BeatTracker.cpp
    src/analysis/KeyDetector.cpp
    src/analysis/Loudness.cpp
    src/analysis/Structure.cpp
    src/analysis/TrackAnalyzer.cpp
)

add_library(aidj_engine STATIC ${AIDJ_CORE_SOURCES})
target_include_directories(aidj_engine PUBLIC src)

if(NOT MSVC)
  target_compile_options(aidj_engine PRIVATE -Wall -Wextra -Wpedantic)
endif()

# The Android output and decoder depend on Oboe (delivered as a Gradle prefab
# package) and the NDK media APIs. A standalone CMake build - such as
# cross-compiling the test suite to run on a device via adb - has neither, and
# does not need them: the tests supply their own IAudioOutput and IDecoder.
option(AIDJ_ANDROID_PLATFORM_SOURCES "Build the Oboe/MediaCodec backends" ON)

if(ANDROID AND AIDJ_ANDROID_PLATFORM_SOURCES)
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
elseif(ANDROID)
  # Still Android, just without the platform backends. `log` is used by the
  # shared code and is always available.
  target_link_libraries(aidj_engine PUBLIC log)
endif()

option(AIDJ_BUILD_TESTS "Build engine tests" OFF)
# Set when cross-compiling the tests to run on a device via adb, which is the
# fallback when the machine has an Android toolchain but no host compiler.
option(AIDJ_TESTS_ON_DEVICE "Allow building tests for Android" OFF)

if(AIDJ_BUILD_TESTS AND (NOT ANDROID OR AIDJ_TESTS_ON_DEVICE))
  enable_testing()
  add_subdirectory(tests)
endif()
