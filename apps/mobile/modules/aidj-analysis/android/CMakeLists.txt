cmake_minimum_required(VERSION 3.22)
project(aidj_analysis_jni CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(AIDJ_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../packages/engine")

if(NOT EXISTS "${AIDJ_ENGINE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
      "Cannot find the aidj engine at ${AIDJ_ENGINE_DIR}. "
      "The mobile app must be built from inside the monorepo.")
endif()

# The engine target is already added by the aidj-audio module in the same
# Gradle build. Guarding here keeps this module independently configurable
# without CMake complaining about a duplicate target.
if(NOT TARGET aidj_engine)
  add_subdirectory("${AIDJ_ENGINE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/aidj_engine")
endif()

add_library(aidj_analysis_jni SHARED src/main/cpp/AnalysisJni.cpp)

target_link_libraries(aidj_analysis_jni PRIVATE aidj_engine log)
