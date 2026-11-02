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
    HostSyncTempoTests.cpp
    NoteGainCurveTests.cpp
    NonAraLifecycleTests.cpp
    OriginalWaveformRestoreTests.cpp
    PitchDrawingTests.cpp
    PitchDriftTests.cpp
    PluginStateArchiveTests.cpp
    ProjectModificationRangeTests.cpp
    TimelineCoordinateTests.cpp
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

# The capture state machine belongs to the plug-in rather than pitchnet_core.
target_sources(NonAraLifecycleTests PRIVATE
    ../Source/Plugin/NonAraCaptureController.cpp)

# Polyphonic note editing (Source/Audio/Polyphonic). The engine has no
# JUCE dependency; the test links pitchnet_core like the others because the
# sources are compiled into it. tools/polyphonic/CMakeLists.txt builds the same
# test without JUCE.
add_executable(PolyphonicTests PolyphonicTests.cpp)
target_link_libraries(PolyphonicTests PRIVATE pitchnet_core)
target_compile_features(PolyphonicTests PRIVATE cxx_std_17)
add_test(NAME PolyphonicTests COMMAND PolyphonicTests)

add_executable(PitchMacroScopeTests PitchMacroScopeTests.cpp)
target_link_libraries(PitchMacroScopeTests PRIVATE pitchnet_core)
target_compile_features(PitchMacroScopeTests PRIVATE cxx_std_17)
add_test(NAME PitchMacroScopeTests COMMAND PitchMacroScopeTests)

add_executable(TrackUndoTests TrackUndoTests.cpp)
target_link_libraries(TrackUndoTests PRIVATE pitchnet_core)
target_compile_features(TrackUndoTests PRIVATE cxx_std_17)
add_test(NAME TrackUndoTests COMMAND TrackUndoTests)

add_executable(TrackGestureTests TrackGestureTests.cpp)
target_link_libraries(TrackGestureTests PRIVATE pitchnet_ui)
target_compile_features(TrackGestureTests PRIVATE cxx_std_17)
add_test(NAME TrackGestureTests COMMAND TrackGestureTests)

add_executable(PitchMacroRenderTests PitchMacroRenderTests.cpp)
target_link_libraries(PitchMacroRenderTests PRIVATE pitchnet_ui)
target_compile_features(PitchMacroRenderTests PRIVATE cxx_std_17)
add_test(NAME PitchMacroRenderTests COMMAND PitchMacroRenderTests)
