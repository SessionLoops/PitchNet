# Unit tests.
#
# Each test is a standalone executable with its own main(), matching the style
# the files were already written in. They link pitchnet_core, which exposes the
# JUCE modules PUBLIC, so a test gets every include path it needs by linking
# that one target.
#
# Built only when PITCHNET_BUILD_TESTS=ON, so a normal build and CI are
# unaffected:
#
#   cmake -B build -DPITCHNET_BUILD_TESTS=ON
#   cmake --build build
#   ctest --test-dir build --output-on-failure

set(PITCHNET_TEST_SOURCES
    AraRegionMergeTests.cpp
    AraRegionSliceTests.cpp
    FormantShiftTests.cpp
    NoteGainCurveTests.cpp
    PitchDriftTests.cpp
    ProjectModificationRangeTests.cpp
    VisualWaveformEnvelopeTests.cpp)

foreach(test_source IN LISTS PITCHNET_TEST_SOURCES)
    get_filename_component(test_name ${test_source} NAME_WE)

    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE pitchnet_core)
    target_compile_features(${test_name} PRIVATE cxx_std_17)

    # The tests assert on results rather than returning a failure code, so a
    # failed assertion aborts the process and CTest sees a non-zero exit.
    add_test(NAME ${test_name} COMMAND ${test_name})

    # NOTE: assert() compiles to nothing under NDEBUG, which release
    # configurations set - so a test written with bare assert() passes
    # vacuously there. Undefining NDEBUG for the target is not the fix: it
    # changes the compile flags for every JUCE source the target pulls in,
    # forcing a full rebuild of JUCE per test. Tests should use a check macro
    # that does not depend on NDEBUG instead; see TestAssert.h.
endforeach()
