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
# the analysis runs its blocks on all cores (parallelFor in PolyInternal.h)
find_package(Threads REQUIRED)
target_link_libraries(polyphonic PUBLIC Threads::Threads)

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

# A separate default makes evaluator/training executable hashes identify the guided strategy.
# Both executables also accept --detector to select a strategy explicitly.
add_executable(polyphonic_guided_cli polyphonic_cli.cpp WavIO.h)
target_compile_definitions(polyphonic_guided_cli PRIVATE POLYPHONIC_GUIDED_DEFAULT=1)
target_link_libraries(polyphonic_guided_cli PRIVATE polyphonic)

enable_testing()
add_executable(PolyphonicTests "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/PolyphonicTests.cpp")
target_link_libraries(PolyphonicTests PRIVATE polyphonic)
add_test(NAME PolyphonicTests COMMAND PolyphonicTests)

add_executable(PolyTempoTests "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/PolyTempoTests.cpp")
target_link_libraries(PolyTempoTests PRIVATE polyphonic)
add_test(NAME PolyTempoTests COMMAND PolyTempoTests)

add_executable(PolyChordTests "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/PolyChordTests.cpp")
target_link_libraries(PolyChordTests PRIVATE polyphonic)
add_test(NAME PolyChordTests COMMAND PolyChordTests)

add_executable(BasicPitchEvidenceTests "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/BasicPitchEvidenceTests.cpp")
target_link_libraries(BasicPitchEvidenceTests PRIVATE polyphonic)
add_test(NAME BasicPitchEvidenceTests COMMAND BasicPitchEvidenceTests)

set(POLYPHONIC_BASIC_PITCH_MODEL "${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/models/basic_pitch_nmp.onnx"
    CACHE FILEPATH "Optional Basic Pitch model for inference regression tests")
if(ONNXRUNTIME_DIR AND EXISTS "${POLYPHONIC_BASIC_PITCH_MODEL}")
    add_test(NAME BasicPitchEvidenceModelTests COMMAND BasicPitchEvidenceTests "${POLYPHONIC_BASIC_PITCH_MODEL}")
endif()

# Input-type classifier (vocal pipeline or polyphonic engine), Source/Audio/Analysis.
file(GLOB INPUTTYPE_SOURCES CONFIGURE_DEPENDS "${POLYPHONIC_DIR}/../Analysis/*.cpp" "${POLYPHONIC_DIR}/../Analysis/*.h")
add_library(inputtype STATIC ${INPUTTYPE_SOURCES})
target_link_libraries(inputtype PUBLIC polyphonic)

add_executable(InputTypeClassifierTests "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/InputTypeClassifierTests.cpp")
target_link_libraries(InputTypeClassifierTests PRIVATE inputtype)
add_test(NAME InputTypeClassifierTests COMMAND InputTypeClassifierTests)

# With ONNX Runtime and the models: YAMNet and Basic Pitch on a few labelled files.
set(INPUTTYPE_YAMNET_MODEL "${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/models/yamnet.onnx"
    CACHE FILEPATH "Optional YAMNet model for input-type classifier tests")
if(ONNXRUNTIME_DIR AND EXISTS "${INPUTTYPE_YAMNET_MODEL}" AND EXISTS "${POLYPHONIC_BASIC_PITCH_MODEL}")
    # Only files with at least 3 s of sound: shorter ones are not classified (minAudibleSeconds).
    set(_audio "${CMAKE_CURRENT_SOURCE_DIR}/test-audio")
    set(_cases
        "polyphonic=${_audio}/01-piano-triad-C-major.wav"
        "polyphonic=${_audio}/03-strings-vibrato.wav"
        "polyphonic=${_audio}/04-woodwind-quartet.wav"
        "polyphonic=${_audio}/08-brahms-hungarian-dance-5-strings.wav"
        # Basic Pitch sees one note at a time here, so the tagger must reject it (3.0 s of sound).
        "polyphonic=${_audio}/missed-notes/01-clarinet-flute-violins-missed-Cs5.wav")
    # Solo singing from vocadito (tools/f0_compare/README.md), unzipped into test-audio/vocadito
    # when available: 37 and 17 have the lowest YAMNet voice scores in the corpus.
    foreach(_n 1 17 37)
        if(EXISTS "${_audio}/vocadito/vocadito_${_n}.wav")
            list(APPEND _cases "vocal=${_audio}/vocadito/vocadito_${_n}.wav")
        endif()
    endforeach()
    add_test(NAME InputTypeClassifierModelTests COMMAND InputTypeClassifierTests
        "${INPUTTYPE_YAMNET_MODEL}" "${POLYPHONIC_BASIC_PITCH_MODEL}" ${_cases})
endif()
