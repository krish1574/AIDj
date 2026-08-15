cmake_minimum_required(VERSION 3.22)
project(aidj_audio_jni CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The engine lives outside the app on purpose: it is a portable library that
# builds and is tested on a desktop host, and the JNI layer below is a thin
# marshalling shim over it. Nothing here contains audio logic.
set(AIDJ_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../packages/engine")

if(NOT EXISTS "${AIDJ_ENGINE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
      "Cannot find the aidj engine at ${AIDJ_ENGINE_DIR}. "
      "The mobile app must be built from inside the monorepo.")
endif()

add_subdirectory("${AIDJ_ENGINE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/aidj_engine")

add_library(aidj_audio_jni SHARED src/main/cpp/JniBridge.cpp)

target_link_libraries(aidj_audio_jni PRIVATE aidj_engine log)
