# Polyphonic engine command-line tool and tests, built on their own (no JUCE):
#
#   cmake -S tools/polyphonic -B build-polyphonic -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-polyphonic --config Release
#   ctest --test-dir build-polyphonic -C Release --output-on-failure
#
# For Basic Pitch note evidence (--model), point ONNXRUNTIME_DIR at an ONNX Runtime
# release (the one PitchNet's main build downloads works):
#
#   cmake -S tools/polyphonic -B build-polyphonic -DONNXRUNTIME_DIR=/path/to/onnxruntime-<platform>-<version>

cmake_minimum_required(VERSION 3.22)
project(polyphonic_tools LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(POLYPHONIC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../Source/Audio/Polyphonic")
file(GLOB POLYPHONIC_SOURCES CONFIGURE_DEPENDS "${POLYPHONIC_DIR}/*.cpp" "${POLYPHONIC_DIR}/*.h")

add_library(polyphonic STATIC ${POLYPHONIC_SOURCES})
target_include_directories(polyphonic PUBLIC "${POLYPHONIC_DIR}")

set(ONNXRUNTIME_DIR "" CACHE PATH "ONNX Runtime release directory (include/ and lib/) for Basic Pitch evidence")
if(ONNXRUNTIME_DIR)
    find_library(POLYPHONIC_ORT_LIBRARY NAMES onnxruntime PATHS "${ONNXRUNTIME_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
    target_include_directories(polyphonic PUBLIC "${ONNXRUNTIME_DIR}/include")
    target_link_libraries(polyphonic PUBLIC "${POLYPHONIC_ORT_LIBRARY}")
    target_compile_definitions(polyphonic PUBLIC HAVE_ONNXRUNTIME=1)
    message(STATUS "Polyphonic engine: Basic Pitch evidence enabled (${POLYPHONIC_ORT_LIBRARY})")
endif()

add_executable(polyphonic_cli polyphonic_cli.cpp WavIO.h)
target_link_libraries(polyphonic_cli PRIVATE polyphonic)

enable_testing()
add_executable(PolyphonicTests "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/PolyphonicTests.cpp")
target_link_libraries(PolyphonicTests PRIVATE polyphonic)
add_test(NAME PolyphonicTests COMMAND PolyphonicTests)
