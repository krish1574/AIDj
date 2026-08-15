include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
)
FetchContent_MakeAvailable(Catch2)

add_executable(aidj_engine_tests
    test_ring_buffer.cpp
    test_mixer.cpp
    test_resampler.cpp
    test_engine.cpp
    test_analysis_rhythm.cpp
    test_analysis_tonal.cpp
    support/TestDecoder.cpp
    support/NullOutput.cpp
)

target_link_libraries(aidj_engine_tests PRIVATE aidj_engine Catch2::Catch2WithMain)
target_include_directories(aidj_engine_tests PRIVATE ../src .)

# catch_discover_tests runs the built binary at configure time to enumerate
# test cases. That is impossible when cross-compiling for a device, so
# registration is skipped there - the adb runner executes the binary directly
# and Catch2 reports its own results.
if(NOT AIDJ_TESTS_ON_DEVICE)
  include(Catch)
  catch_discover_tests(aidj_engine_tests)
endif()
