cmake_minimum_required(VERSION 3.22)
project(PitchNet VERSION 1.0.0)

include(FetchContent)
include(cmake/Asio.cmake)

# Ensure static libs are built with PIC for shared plugin targets (Linux).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ONNX Runtime configuration
option(USE_GPU "Enable GPU acceleration for ONNX Runtime (not supported on macOS)" OFF)
option(USE_CUDA "Enable CUDA execution provider" OFF)
option(USE_DIRECTML "Enable DirectML execution provider (Windows only)" OFF)
option(USE_BUNDLED_CUDA_RUNTIME "Bundle minimal CUDA runtime DLLs (Windows only)" OFF)
option(USE_BUNDLED_DIRECTML_RUNTIME "Bundle DirectML runtime DLL (Windows only)" OFF)
option(USE_ASIO "Enable ASIO support (Windows only)" OFF)
option(ENABLE_NOTE_STRETCH "Enable note timing stretch feature (experimental, has known issues)" ON)
set(CUDA_REDIST_URL "" CACHE STRING "Optional URL to download CUDA runtime redistributable zip")
set(DIRECTML_REDIST_URL "" CACHE STRING "Optional URL to download DirectML redistributable zip")
set(ONNXRUNTIME_VERSION "1.24.3" CACHE STRING "ONNX Runtime version")
set(GAME_MODELS_VERSION "v1.0.3" CACHE STRING "GAME release tag used to download model assets")
set(GAME_MODELS_URL "" CACHE STRING "Optional URL to download GAME model zip")

# Check that CUDA and DirectML are not both enabled
if(USE_CUDA AND USE_DIRECTML)
    message(FATAL_ERROR "ERROR: USE_CUDA and USE_DIRECTML cannot be enabled at the same time. "
                        "DirectML requires a separate version of ONNX Runtime. "
                        "Please enable only one of them.")
endif()

# If CUDA is requested, force GPU pipeline on
if(USE_CUDA)
    set(USE_GPU ON)
    message(STATUS "USE_CUDA=ON -> enabling USE_GPU automatically")
endif()

# If DirectML is requested, force GPU pipeline on
if(USE_DIRECTML)
    set(USE_GPU ON)
    message(STATUS "USE_DIRECTML=ON -> enabling USE_GPU automatically")
endif()

set(PITCHNET_ASIO_ENABLED OFF)
if(USE_ASIO AND WIN32)
    pitchnet_setup_asio(ASIO_INCLUDE_DIR)
    set(PITCHNET_ASIO_ENABLED ON)
    message(STATUS "ASIO support: ENABLED")
elseif(USE_ASIO)
    message(WARNING "USE_ASIO is enabled but ASIO is only supported on Windows")
endif()

set(PITCHNET_JUCE_ASIO 0)
set(PITCHNET_JUCE_ASIO_EXTERNAL 0)
if(PITCHNET_ASIO_ENABLED)
    set(PITCHNET_JUCE_ASIO 1)
    set(PITCHNET_JUCE_ASIO_EXTERNAL 1)
endif()

# Check if ONNX Runtime already exists in build directory
set(ONNXRUNTIME_CACHE_DIR "${CMAKE_BINARY_DIR}/_deps/onnxruntime-src")
if(EXISTS "${ONNXRUNTIME_CACHE_DIR}/include/onnxruntime_cxx_api.h")
    message(STATUS "ONNX Runtime already downloaded, skipping download")
    set(ONNXRUNTIME_ROOT ${ONNXRUNTIME_CACHE_DIR})
else()
    # Detect platform and set download URL
    if(WIN32)
        if(USE_CUDA)
            set(ONNXRUNTIME_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/onnxruntime-win-x64-gpu_cuda13-${ONNXRUNTIME_VERSION}.zip")
            message(STATUS "Downloading ONNX Runtime GPU CUDA13 package (Windows)")
        elseif(USE_DIRECTML)
            set(ONNXRUNTIME_URL "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/${ONNXRUNTIME_VERSION}")
            message(STATUS "Downloading ONNX Runtime DirectML package from NuGet (Windows)")
        else()
            set(ONNXRUNTIME_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/onnxruntime-win-x64-${ONNXRUNTIME_VERSION}.zip")
            message(STATUS "Downloading ONNX Runtime CPU package (Windows)")
        endif()
    elseif(APPLE)
        if(USE_GPU)
            message(WARNING "GPU acceleration is not supported on macOS, falling back to CPU")
        endif()
        set(ONNXRUNTIME_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/onnxruntime-osx-arm64-${ONNXRUNTIME_VERSION}.tgz")
    else()
        if(USE_CUDA)
            set(ONNXRUNTIME_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/onnxruntime-linux-x64-gpu_cuda13-${ONNXRUNTIME_VERSION}.tgz")
        elseif(USE_GPU)
            set(ONNXRUNTIME_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/onnxruntime-linux-x64-gpu-${ONNXRUNTIME_VERSION}.tgz")
        else()
            set(ONNXRUNTIME_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}.tgz")
        endif()
    endif()

    message(STATUS "Downloading ONNX Runtime from: ${ONNXRUNTIME_URL}")

    FetchContent_Declare(
        onnxruntime
        URL ${ONNXRUNTIME_URL}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(onnxruntime)
    set(ONNXRUNTIME_ROOT ${onnxruntime_SOURCE_DIR})
endif()

# Handle different directory structures for DirectML NuGet package vs regular packages
if(WIN32 AND USE_DIRECTML)
    # DirectML NuGet package has different structure: runtimes/win-x64/native/
    set(ONNXRUNTIME_INCLUDE_DIR ${ONNXRUNTIME_ROOT}/build/native/include)
    set(ONNXRUNTIME_LIBRARY ${ONNXRUNTIME_ROOT}/runtimes/win-x64/native/onnxruntime.lib)
    set(ONNXRUNTIME_DLL_DIR ${ONNXRUNTIME_ROOT}/runtimes/win-x64/native)
else()
    set(ONNXRUNTIME_INCLUDE_DIR ${ONNXRUNTIME_ROOT}/include)
    if(WIN32)
        set(ONNXRUNTIME_LIBRARY ${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib)
        set(ONNXRUNTIME_DLL_DIR ${ONNXRUNTIME_ROOT}/lib)
    else()
        find_library(ONNXRUNTIME_LIBRARY onnxruntime PATHS ${ONNXRUNTIME_ROOT}/lib NO_DEFAULT_PATH)
    endif()
endif()

if(EXISTS ${ONNXRUNTIME_INCLUDE_DIR} AND ONNXRUNTIME_LIBRARY)
    set(ONNXRUNTIME_FOUND TRUE)
    message(STATUS "ONNX Runtime configured:")
    message(STATUS "  Include: ${ONNXRUNTIME_INCLUDE_DIR}")
    message(STATUS "  Library: ${ONNXRUNTIME_LIBRARY}")
    if(USE_GPU AND NOT APPLE)
        message(STATUS "  GPU acceleration: ENABLED")
    else()
        message(STATUS "  GPU acceleration: DISABLED")
    endif()
else()
    set(ONNXRUNTIME_FOUND FALSE)
    message(WARNING "ONNX Runtime configuration failed")
endif()

# Add JUCE as a subdirectory (git submodule in third_party/)
add_subdirectory(third_party/JUCE)

# Create GUI application
juce_add_gui_app(PitchNet
    PRODUCT_NAME "PitchNet"
    COMPANY_NAME "Session Loops"
    BUNDLE_ID "com.sessionloops.pitchnet"
    COMPANY_COPYRIGHT "Copyright 2026"
    MICROPHONE_PERMISSION_ENABLED TRUE
    FILE_SHARING_ENABLED TRUE
    DOCUMENT_EXTENSIONS "wav;mp3;flac;ogg")

# AAX SDK configuration (user must set AAX_SDK_PATH environment variable)
if(DEFINED ENV{AAX_SDK_PATH} AND EXISTS "$ENV{AAX_SDK_PATH}")
    juce_set_aax_sdk_path($ENV{AAX_SDK_PATH})
    set(AAX_AVAILABLE TRUE)
    message(STATUS "AAX SDK found at: $ENV{AAX_SDK_PATH}")
else()
    set(AAX_AVAILABLE FALSE)
    message(STATUS "AAX SDK not found - AAX format will not be built")
endif()

# ARA SDK configuration (bundled as submodule)
set(ARA_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ARA_SDK")
if(EXISTS "${ARA_SDK_PATH}/ARA_API")
    juce_set_ara_sdk_path(${ARA_SDK_PATH})
    set(ARA_AVAILABLE TRUE)
    message(STATUS "ARA SDK found at: ${ARA_SDK_PATH}")
else()
    set(ARA_AVAILABLE FALSE)
    message(STATUS "ARA SDK not found - run: git submodule update --init --recursive")
endif()

# Create VST3 plugin
juce_add_plugin(PitchNetPlugin
    PRODUCT_NAME "PitchNet"
    COMPANY_NAME "Session Loops"
    BUNDLE_ID "com.sessionloops.pitchnet"
    PLUGIN_MANUFACTURER_CODE SLPS
    PLUGIN_CODE PTE1
    FORMATS VST3 AU $<$<BOOL:${AAX_AVAILABLE}>:AAX>
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_ARA_EFFECT $<BOOL:${ARA_AVAILABLE}>
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    MICROPHONE_PERMISSION_ENABLED TRUE
    FILE_SHARING_ENABLED TRUE)

# ARA compile definitions
if(ARA_AVAILABLE)
    target_compile_definitions(PitchNetPlugin PRIVATE
        JucePlugin_Enable_ARA=1)
endif()

# Add source files using GLOB_RECURSE
file(GLOB_RECURSE PITCHNET_CORE_SOURCES
    "Source/Audio/*.cpp" "Source/Audio/*.h"
    "Source/Models/*.cpp" "Source/Models/*.h"
    "Source/Undo/*.cpp" "Source/Undo/*.h"
    "Source/Utils/*.cpp" "Source/Utils/*.h" "Source/Utils/*.mm"
    "Source/JuceHeader.h")

list(FILTER PITCHNET_CORE_SOURCES EXCLUDE REGEX "Source/Utils/UI/")
list(APPEND PITCHNET_CORE_SOURCES
    "Source/Utils/SHA256Utils.cpp"
    "Source/Utils/SHA256Utils.h")

file(GLOB_RECURSE PITCHNET_UI_SOURCES
    "Source/UI/*.cpp" "Source/UI/*.h"
    "Source/Utils/UI/*.cpp" "Source/Utils/UI/*.h")
list(APPEND PITCHNET_UI_SOURCES
    "Source/UI/PianoRoll/PianoRollInteractionContext.cpp"
    "Source/UI/PianoRoll/PianoRollInteractionContext.h"
    "Source/UI/PianoRoll/PianoRollViewState.h")

file(GLOB PLUGIN_SOURCES
    "Source/Plugin/PluginProcessor.cpp" "Source/Plugin/PluginProcessor.h"
    "Source/Plugin/PluginEditor.cpp" "Source/Plugin/PluginEditor.h"
    "Source/Plugin/ARADocumentController.cpp" "Source/Plugin/ARADocumentController.h"
    "Source/Plugin/NonAraCaptureController.cpp" "Source/Plugin/NonAraCaptureController.h"
    "Source/Plugin/HostCompatibility.cpp" "Source/Plugin/HostCompatibility.h")

target_sources(PitchNet PRIVATE
    Source/Main.cpp
    )

target_sources(PitchNetPlugin PRIVATE
    ${PLUGIN_SOURCES}
    )

# Core/UI libraries (stage 1 modularization)
add_library(pitchnet_core STATIC ${PITCHNET_CORE_SOURCES})
add_library(pitchnet_ui STATIC ${PITCHNET_UI_SOURCES})

# Binary data for SVG icons
file(GLOB SVG_FILES "${CMAKE_CURRENT_SOURCE_DIR}/Resources/svg/*.svg")
set(BINARY_DATA_FILES ${SVG_FILES})
set(EMBED_EN_LANG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/Resources/lang/en.json")
if(EXISTS "${EMBED_EN_LANG_FILE}")
    list(APPEND BINARY_DATA_FILES "${EMBED_EN_LANG_FILE}")
endif()
juce_add_binary_data(BinaryData SOURCES ${BINARY_DATA_FILES})

target_link_libraries(pitchnet_core PUBLIC
    BinaryData
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags)

target_link_libraries(pitchnet_ui PUBLIC
    pitchnet_core
    BinaryData
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_audio_basics
    juce::juce_audio_formats
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags)

# Enable fast-math for release builds on non-MSVC compilers
if(NOT MSVC)
    target_compile_options(pitchnet_core PRIVATE $<$<CONFIG:Release>:-ffast-math>)
    target_compile_options(pitchnet_ui PRIVATE $<$<CONFIG:Release>:-ffast-math>)
endif()

# Link JUCE modules
target_link_libraries(PitchNet PRIVATE
    pitchnet_ui
    pitchnet_core
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

target_link_libraries(PitchNetPlugin PRIVATE
    pitchnet_ui
    pitchnet_core
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_audio_plugin_client
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

if(PITCHNET_ASIO_ENABLED)
    target_include_directories(pitchnet_core PUBLIC ${ASIO_INCLUDE_DIR})
endif()

# Link ONNX Runtime if found
if(ONNXRUNTIME_FOUND)
    target_include_directories(pitchnet_core PUBLIC ${ONNXRUNTIME_INCLUDE_DIR})
    target_link_libraries(pitchnet_core PUBLIC ${ONNXRUNTIME_LIBRARY})
    target_compile_definitions(pitchnet_core PUBLIC HAVE_ONNXRUNTIME=1)

    # Pass GPU option to code (macOS always uses CPU)
    if(USE_GPU AND NOT APPLE)
        target_compile_definitions(pitchnet_core PUBLIC ONNXRUNTIME_USE_GPU=1)
    endif()

    # Enable CUDA if requested
    if(USE_CUDA)
        target_compile_definitions(pitchnet_core PUBLIC USE_CUDA=1)
        message(STATUS "CUDA execution provider: ENABLED")
    endif()

    # Enable DirectML if requested (Windows only)
    if(USE_DIRECTML AND WIN32)
        target_compile_definitions(pitchnet_core PUBLIC USE_DIRECTML=1)
        message(STATUS "DirectML execution provider: ENABLED")
    endif()

    # Copy ONNX Runtime DLLs to output directory on Windows
    if(WIN32)
        get_filename_component(ONNXRUNTIME_LIB_DIR ${ONNXRUNTIME_LIBRARY} DIRECTORY)
        # Try lib directory first (most common), then bin directory
        file(GLOB ONNXRUNTIME_DLLS "${ONNXRUNTIME_LIB_DIR}/*.dll")
        if(NOT ONNXRUNTIME_DLLS)
            file(GLOB ONNXRUNTIME_DLLS "${ONNXRUNTIME_LIB_DIR}/../bin/*.dll")
        endif()
        if(ONNXRUNTIME_DLLS)
            foreach(DLL ${ONNXRUNTIME_DLLS})
                message(STATUS "Will copy ONNX Runtime DLL: ${DLL}")
                add_custom_command(TARGET PitchNet POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DLL}"
                    "$<TARGET_FILE_DIR:PitchNet>")

                add_custom_command(TARGET PitchNetPlugin_VST3 POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DLL}"
                    "$<TARGET_FILE_DIR:PitchNetPlugin_VST3>")
            endforeach()
        else()
            message(WARNING "No ONNX Runtime DLLs found to copy")
        endif()
        
        # Copy CUDA runtime DLLs if CUDA is enabled
        if(USE_CUDA)
            # Optional bundled CUDA runtime (redistributable) to avoid system dependency
            # If USE_BUNDLED_CUDA_RUNTIME is ON, we first look for a local redistributable dir,
            # otherwise try to download from CUDA_REDIST_URL (if provided).
            if(USE_BUNDLED_CUDA_RUNTIME)
                set(CUDA_REDIST_DIR "${CMAKE_SOURCE_DIR}/vendor/cuda/bin")
                if(NOT EXISTS "${CUDA_REDIST_DIR}")
                    set(CUDA_REDIST_DIR "${CMAKE_BINARY_DIR}/cuda_redist/bin")
                endif()

                # Download redistributable if URL is provided and dir not present
                if(NOT EXISTS "${CUDA_REDIST_DIR}" AND CUDA_REDIST_URL)
                    message(STATUS "Downloading CUDA runtime redistributable from ${CUDA_REDIST_URL}")
                    file(DOWNLOAD "${CUDA_REDIST_URL}"
                        "${CMAKE_BINARY_DIR}/cuda_redist.zip"
                        SHOW_PROGRESS)
                    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/cuda_redist")
                    execute_process(COMMAND ${CMAKE_COMMAND} -E tar xzf "${CMAKE_BINARY_DIR}/cuda_redist.zip"
                                    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/cuda_redist")
                    # Try common layout: cuda_redist/bin
                    if(EXISTS "${CMAKE_BINARY_DIR}/cuda_redist/bin")
                        set(CUDA_REDIST_DIR "${CMAKE_BINARY_DIR}/cuda_redist/bin")
                    endif()
                endif()
            endif()

            # Common CUDA installation paths
            set(CUDA_PATHS
                "$ENV{CUDA_PATH}/bin"
                "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.0/bin"
                "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.1/bin"
                "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.2/bin"
                "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v11.8/bin"
                "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v11.9/bin"
            )
            
            # CUDA runtime DLLs needed by ONNX Runtime
            set(CUDA_DLLS
                "cublasLt64_12.dll"
                "cublas64_12.dll"
                "cudnn64_8.dll"
                "cufft64_12.dll"
                "curand64_12.dll"
                "cusolver64_12.dll"
                "cusparse64_12.dll"
                "cudart64_12.dll"
            )
            
            # Also try version 11 DLLs
            list(APPEND CUDA_DLLS
                "cublasLt64_11.dll"
                "cublas64_11.dll"
                "cufft64_11.dll"
                "curand64_11.dll"
                "cusolver64_11.dll"
                "cusparse64_11.dll"
                "cudart64_11.dll"
            )
            
            # Find CUDA bin directory (prefer bundled redistributable if present)
            set(CUDA_BIN_DIR "")
            if(USE_BUNDLED_CUDA_RUNTIME AND EXISTS "${CUDA_REDIST_DIR}")
                set(CUDA_BIN_DIR "${CUDA_REDIST_DIR}")
                message(STATUS "Using bundled CUDA runtime from: ${CUDA_BIN_DIR}")
            endif()

            foreach(CUDA_PATH ${CUDA_PATHS})
                if(NOT CUDA_BIN_DIR AND EXISTS "${CUDA_PATH}")
                    set(CUDA_BIN_DIR "${CUDA_PATH}")
                    message(STATUS "Found CUDA bin directory: ${CUDA_BIN_DIR}")
                    break()
                endif()
            endforeach()
            
            if(CUDA_BIN_DIR)
                foreach(CUDA_DLL ${CUDA_DLLS})
                    set(CUDA_DLL_PATH "${CUDA_BIN_DIR}/${CUDA_DLL}")
                    if(EXISTS "${CUDA_DLL_PATH}")
                        message(STATUS "Will copy CUDA DLL: ${CUDA_DLL}")
                        add_custom_command(TARGET PitchNet POST_BUILD
                            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${CUDA_DLL_PATH}"
                            "$<TARGET_FILE_DIR:PitchNet>")
                        
                        add_custom_command(TARGET PitchNetPlugin_VST3 POST_BUILD
                            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${CUDA_DLL_PATH}"
                            "$<TARGET_FILE_DIR:PitchNetPlugin_VST3>")
                    endif()
                endforeach()
            else()
                message(WARNING "CUDA bin directory not found. CUDA runtime DLLs will not be copied.")
                message(WARNING "Please ensure CUDA Toolkit is installed and CUDA_PATH environment variable is set.")
                message(WARNING "Or manually copy CUDA DLLs from CUDA installation to the output directory.")
            endif()
        endif()

        # Copy DirectML runtime DLL if explicitly requested (Windows only)
        # Note: DirectML is typically included in Windows 10/11 and will be loaded from the system
        if(USE_DIRECTML AND USE_BUNDLED_DIRECTML_RUNTIME)
            set(DML_DLLS "DirectML.dll")
            set(DML_BIN_DIR "")

            # DirectML version for auto-download
            if(NOT DIRECTML_VERSION)
                set(DIRECTML_VERSION "1.15.4")
            endif()

            # Check for pre-existing DirectML in vendor directory
            set(DML_REDIST_DIR "${CMAKE_SOURCE_DIR}/vendor/directml/bin")

            # If not found, check/download to build directory
            if(NOT EXISTS "${DML_REDIST_DIR}/DirectML.dll")
                set(DML_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_deps/directml-src")
                set(DML_REDIST_DIR "${DML_DOWNLOAD_DIR}/bin/x64-win")

                # Auto-download from NuGet if not present
                if(NOT EXISTS "${DML_REDIST_DIR}/DirectML.dll")
                    set(DML_NUGET_URL "https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/${DIRECTML_VERSION}")
                    set(DML_NUPKG_PATH "${CMAKE_BINARY_DIR}/_deps/directml.nupkg")

                    message(STATUS "Downloading DirectML ${DIRECTML_VERSION} from NuGet...")
                    file(DOWNLOAD "${DML_NUGET_URL}" "${DML_NUPKG_PATH}"
                        SHOW_PROGRESS
                        STATUS DML_DOWNLOAD_STATUS)

                    list(GET DML_DOWNLOAD_STATUS 0 DML_DOWNLOAD_RESULT)
                    if(DML_DOWNLOAD_RESULT EQUAL 0)
                        message(STATUS "Extracting DirectML package...")
                        file(MAKE_DIRECTORY "${DML_DOWNLOAD_DIR}")
                        execute_process(
                            COMMAND ${CMAKE_COMMAND} -E tar xf "${DML_NUPKG_PATH}"
                            WORKING_DIRECTORY "${DML_DOWNLOAD_DIR}"
                            RESULT_VARIABLE DML_EXTRACT_RESULT)

                        if(NOT DML_EXTRACT_RESULT EQUAL 0)
                            message(WARNING "Failed to extract DirectML package")
                        endif()
                    else()
                        message(WARNING "Failed to download DirectML: ${DML_DOWNLOAD_STATUS}")
                    endif()
                endif()
            endif()

            if(EXISTS "${DML_REDIST_DIR}/DirectML.dll")
                set(DML_BIN_DIR "${DML_REDIST_DIR}")
                message(STATUS "Using bundled DirectML ${DIRECTML_VERSION} from: ${DML_BIN_DIR}")

                foreach(DML_DLL ${DML_DLLS})
                    set(DML_DLL_PATH "${DML_BIN_DIR}/${DML_DLL}")
                    if(EXISTS "${DML_DLL_PATH}")
                        message(STATUS "Will copy DirectML DLL: ${DML_DLL_PATH}")
                        add_custom_command(TARGET PitchNet POST_BUILD
                            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${DML_DLL_PATH}"
                            "$<TARGET_FILE_DIR:PitchNet>")

                        add_custom_command(TARGET PitchNetPlugin_VST3 POST_BUILD
                            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${DML_DLL_PATH}"
                            "$<TARGET_FILE_DIR:PitchNetPlugin_VST3>")
                    endif()
                endforeach()
            else()
                message(WARNING "USE_BUNDLED_DIRECTML_RUNTIME is ON but DirectML.dll not found.")
                message(WARNING "Check network connection or place DirectML.dll in vendor/directml/bin/")
            endif()
        elseif(USE_DIRECTML)
            message(STATUS "DirectML will be loaded from the system (included in Windows 10/11)")
        endif()
    elseif(APPLE)
        # macOS: Embed dylib in app bundle and plugin bundles
        get_filename_component(ONNXRUNTIME_LIB_DIR ${ONNXRUNTIME_LIBRARY} DIRECTORY)
        file(GLOB ONNXRUNTIME_DYLIBS "${ONNXRUNTIME_LIB_DIR}/*.dylib")
        if(ONNXRUNTIME_DYLIBS)
            foreach(DYLIB ${ONNXRUNTIME_DYLIBS})
                message(STATUS "Will embed ONNX Runtime dylib: ${DYLIB}")
                # Standalone app
                add_custom_command(TARGET PitchNet POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:PitchNet>/Contents/Frameworks"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DYLIB}"
                    "$<TARGET_BUNDLE_DIR:PitchNet>/Contents/Frameworks/")
                # VST3 plugin
                add_custom_command(TARGET PitchNetPlugin_VST3 POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:PitchNetPlugin_VST3>/Contents/Frameworks"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DYLIB}"
                    "$<TARGET_BUNDLE_DIR:PitchNetPlugin_VST3>/Contents/Frameworks/")
                # AU plugin
                add_custom_command(TARGET PitchNetPlugin_AU POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:PitchNetPlugin_AU>/Contents/Frameworks"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DYLIB}"
                    "$<TARGET_BUNDLE_DIR:PitchNetPlugin_AU>/Contents/Frameworks/")
            endforeach()
            # Fix rpath for all targets using install_name_tool
            add_custom_command(TARGET PitchNet POST_BUILD
                COMMAND install_name_tool -add_rpath "@executable_path/../Frameworks" "$<TARGET_FILE:PitchNet>" 2>/dev/null || true)
            add_custom_command(TARGET PitchNetPlugin_VST3 POST_BUILD
                COMMAND install_name_tool -add_rpath "@loader_path/../Frameworks" "$<TARGET_BUNDLE_DIR:PitchNetPlugin_VST3>/Contents/MacOS/PitchNet" 2>/dev/null || true)
            add_custom_command(TARGET PitchNetPlugin_AU POST_BUILD
                COMMAND install_name_tool -add_rpath "@loader_path/../Frameworks" "$<TARGET_BUNDLE_DIR:PitchNetPlugin_AU>/Contents/MacOS/PitchNet" 2>/dev/null || true)
        endif()
    endif()
endif()

# Set C++ standard
target_compile_features(PitchNet PRIVATE cxx_std_17)
target_compile_features(PitchNetPlugin PRIVATE cxx_std_17)

# Platform-specific settings
if(WIN32)
    # NOMINMAX prevents Windows.h from defining min/max macros that conflict with std::min/std::max
    target_compile_definitions(pitchnet_core PUBLIC
        NOMINMAX
        JUCE_WASAPI=1
        JUCE_DIRECTSOUND=1
        JUCE_ASIO=${PITCHNET_JUCE_ASIO}
        JUCE_ASIO_USE_EXTERNAL_SDK=${PITCHNET_JUCE_ASIO_EXTERNAL})
elseif(APPLE)
    target_compile_definitions(pitchnet_core PUBLIC
        JUCE_COREAUDIO=1)
else()
    target_compile_definitions(pitchnet_core PUBLIC
        JUCE_ALSA=1
        JUCE_JACK=1)
endif()

# Feature flags
if(ENABLE_NOTE_STRETCH)
    target_compile_definitions(pitchnet_core PUBLIC
        PITCHNET_ENABLE_STRETCH=1
        HACHITUNE_ENABLE_STRETCH=1)
    message(STATUS "Note stretch feature: ENABLED")
else()
    message(STATUS "Note stretch feature: DISABLED")
endif()

# Common definitions
target_compile_definitions(pitchnet_core PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1)

target_compile_definitions(pitchnet_ui PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1)

target_compile_definitions(PitchNet PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:PitchNet,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:PitchNet,JUCE_VERSION>")

target_compile_definitions(PitchNetPlugin PRIVATE
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:PitchNetPlugin,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:PitchNetPlugin,JUCE_VERSION>")

# Check required models
set(MODELS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Resources/models")
set(REQUIRED_MODELS
    pc_nsf_hifigan.onnx
    fcpe.onnx
    rmvpe.onnx
    cent_table.bin
    mel_filterbank.bin
)

# Check required GAME models
set(GAME_DIR "${MODELS_DIR}/GAME")
set(REQUIRED_GAME_MODELS
    encoder.onnx
    segmenter.onnx
    estimator.onnx
    bd2dur.onnx
    dur2bd.onnx
    config.json
)

# Auto-download GAME models if missing
set(GAME_MODELS_MISSING FALSE)
foreach(GMODEL ${REQUIRED_GAME_MODELS})
    if(NOT EXISTS "${GAME_DIR}/${GMODEL}")
        set(GAME_MODELS_MISSING TRUE)
        break()
    endif()
endforeach()

if(GAME_MODELS_MISSING)
    message(STATUS "GAME models not found, downloading...")
    if(NOT GAME_MODELS_URL)
        string(REGEX REPLACE "^v" "" GAME_MODELS_VERSION_NAME "${GAME_MODELS_VERSION}")
        set(GAME_MODELS_URL "https://github.com/openvpi/GAME/releases/download/${GAME_MODELS_VERSION}/GAME-${GAME_MODELS_VERSION_NAME}-small-onnx.zip")
    endif()
    set(GAME_ZIP_URL "${GAME_MODELS_URL}")
    set(GAME_ZIP_PATH "${CMAKE_BINARY_DIR}/GAME-1.0-small-onnx.zip")
    set(GAME_EXTRACT_DIR "${CMAKE_BINARY_DIR}/_game_extract")

    file(DOWNLOAD "${GAME_ZIP_URL}" "${GAME_ZIP_PATH}"
         STATUS GAME_DL_STATUS
         SHOW_PROGRESS)
    list(GET GAME_DL_STATUS 0 GAME_DL_CODE)
    if(NOT GAME_DL_CODE EQUAL 0)
        message(FATAL_ERROR "Failed to download GAME models: ${GAME_DL_STATUS}")
    endif()

    file(MAKE_DIRECTORY "${GAME_EXTRACT_DIR}")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf "${GAME_ZIP_PATH}"
        WORKING_DIRECTORY "${GAME_EXTRACT_DIR}"
        RESULT_VARIABLE GAME_EXTRACT_RESULT)
    if(NOT GAME_EXTRACT_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to extract GAME models")
    endif()

    # Find and copy extracted files into Resources/models/GAME
    file(MAKE_DIRECTORY "${GAME_DIR}")
    file(GLOB_RECURSE GAME_EXTRACTED_FILES "${GAME_EXTRACT_DIR}/*.onnx" "${GAME_EXTRACT_DIR}/config.json")
    foreach(GFILE ${GAME_EXTRACTED_FILES})
        get_filename_component(GNAME "${GFILE}" NAME)
        file(COPY "${GFILE}" DESTINATION "${GAME_DIR}")
    endforeach()

    # Clean up
    file(REMOVE "${GAME_ZIP_PATH}")
    file(REMOVE_RECURSE "${GAME_EXTRACT_DIR}")
    message(STATUS "GAME models downloaded and extracted to ${GAME_DIR}")
endif()

# Check required models
foreach(MODEL ${REQUIRED_MODELS})
    if(NOT EXISTS "${MODELS_DIR}/${MODEL}")
        message(FATAL_ERROR "Required model not found: ${MODELS_DIR}/${MODEL}")
    endif()
endforeach()
foreach(GMODEL ${REQUIRED_GAME_MODELS})
    if(NOT EXISTS "${GAME_DIR}/${GMODEL}")
        message(FATAL_ERROR "Required GAME model not found: ${GAME_DIR}/${GMODEL}")
    endif()
endforeach()
message(STATUS "All required models found")

# Copy all files from models directory to targets
file(GLOB ALL_MODEL_FILES "${MODELS_DIR}/*.onnx" "${MODELS_DIR}/*.bin")
foreach(MODEL_PATH ${ALL_MODEL_FILES})
    get_filename_component(MODEL_NAME "${MODEL_PATH}" NAME)
    if(APPLE)
        add_custom_command(TARGET PitchNet POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:PitchNet>/Contents/Resources/models"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MODEL_PATH}" "$<TARGET_BUNDLE_DIR:PitchNet>/Contents/Resources/models/${MODEL_NAME}")
        add_custom_command(TARGET PitchNetPlugin_VST3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:PitchNetPlugin_VST3>/Contents/Resources/models"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MODEL_PATH}" "$<TARGET_BUNDLE_DIR:PitchNetPlugin_VST3>/Contents/Resources/models/${MODEL_NAME}")
        add_custom_command(TARGET PitchNetPlugin_AU POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:PitchNetPlugin_AU>/Contents/Resources/models"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MODEL_PATH}" "$<TARGET_BUNDLE_DIR:PitchNetPlugin_AU>/Contents/Resources/models/${MODEL_NAME}")
    else()
        add_custom_command(TARGET PitchNet POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:PitchNet>/models"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MODEL_PATH}" "$<TARGET_FILE_DIR:PitchNet>/models/${MODEL_NAME}")
        add_custom_command(TARGET PitchNetPlugin_VST3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:PitchNetPlugin_VST3>/models"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MODEL_PATH}" "$<TARGET_FILE_DIR:PitchNetPlugin_VST3>/models/${MODEL_NAME}")
    endif()
endforeach()

# Copy GAME model subdirectory
file(GLOB GAME_MODEL_FILES "${GAME_DIR}/*")
foreach(GFILE_PATH ${GAME_MODEL_FILES})
    get_filename_component(GFILE_NAME "${GFILE_PATH}" NAME)
    if(APPLE)
        add_custom_command(TARGET PitchNet POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:PitchNet>/Contents/Resources/models/GAME"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GFILE_PATH}" "$<TARGET_BUNDLE_DIR:PitchNet>/Contents/Resources/models/GAME/${GFILE_NAME}")
        add_custom_command(TARGET PitchNetPlugin_VST3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:PitchNetPlugin_VST3>/Contents/Resources/models/GAME"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GFILE_PATH}" "$<TARGET_BUNDLE_DIR:PitchNetPlugin_VST3>/Contents/Resources/models/GAME/${GFILE_NAME}")
        add_custom_command(TARGET PitchNetPlugin_AU POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:PitchNetPlugin_AU>/Contents/Resources/models/GAME"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GFILE_PATH}" "$<TARGET_BUNDLE_DIR:PitchNetPlugin_AU>/Contents/Resources/models/GAME/${GFILE_NAME}")
    else()
        add_custom_command(TARGET PitchNet POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:PitchNet>/models/GAME"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GFILE_PATH}" "$<TARGET_FILE_DIR:PitchNet>/models/GAME/${GFILE_NAME}")
        add_custom_command(TARGET PitchNetPlugin_VST3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:PitchNetPlugin_VST3>/models/GAME"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GFILE_PATH}" "$<TARGET_FILE_DIR:PitchNetPlugin_VST3>/models/GAME/${GFILE_NAME}")
    endif()
endforeach()

# Copy language files
set(LANG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Resources/lang")
file(GLOB LANG_FILES "${LANG_DIR}/*.json")
if(LANG_FILES)
    message(STATUS "Found language files: ${LANG_FILES}")
    if(APPLE)
        add_custom_command(TARGET PitchNet POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:PitchNet>/Contents/Resources/lang"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LANG_FILES} "$<TARGET_BUNDLE_DIR:PitchNet>/Contents/Resources/lang/")
        add_custom_command(TARGET PitchNetPlugin_VST3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:PitchNetPlugin_VST3>/Contents/Resources/lang"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LANG_FILES} "$<TARGET_BUNDLE_DIR:PitchNetPlugin_VST3>/Contents/Resources/lang/")
        add_custom_command(TARGET PitchNetPlugin_AU POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:PitchNetPlugin_AU>/Contents/Resources/lang"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LANG_FILES} "$<TARGET_BUNDLE_DIR:PitchNetPlugin_AU>/Contents/Resources/lang/")
    else()
        add_custom_command(TARGET PitchNet POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:PitchNet>/lang"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LANG_FILES} "$<TARGET_FILE_DIR:PitchNet>/lang/")
    endif()
endif()

# Copy font files
set(FONTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Resources/fonts")
file(GLOB FONT_FILES "${FONTS_DIR}/*.otf" "${FONTS_DIR}/*.ttf")
if(FONT_FILES)
    message(STATUS "Found font files: ${FONT_FILES}")
    if(APPLE)
        add_custom_command(TARGET PitchNet POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:PitchNet>/Contents/Resources/fonts"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FONT_FILES} "$<TARGET_BUNDLE_DIR:PitchNet>/Contents/Resources/fonts/")
        add_custom_command(TARGET PitchNetPlugin_VST3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:PitchNetPlugin_VST3>/Contents/Resources/fonts"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FONT_FILES} "$<TARGET_BUNDLE_DIR:PitchNetPlugin_VST3>/Contents/Resources/fonts/")
        add_custom_command(TARGET PitchNetPlugin_AU POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:PitchNetPlugin_AU>/Contents/Resources/fonts"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FONT_FILES} "$<TARGET_BUNDLE_DIR:PitchNetPlugin_AU>/Contents/Resources/fonts/")
    else()
        add_custom_command(TARGET PitchNet POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:PitchNet>/fonts"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FONT_FILES} "$<TARGET_FILE_DIR:PitchNet>/fonts/")
        add_custom_command(TARGET PitchNetPlugin_VST3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:PitchNetPlugin_VST3>/fonts"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FONT_FILES} "$<TARGET_FILE_DIR:PitchNetPlugin_VST3>/fonts/")
    endif()
endif()

# Re-sign bundles on macOS after all resources are copied
if(APPLE)
    add_custom_command(TARGET PitchNet POST_BUILD
        COMMAND codesign --force --deep -s - "$<TARGET_BUNDLE_DIR:PitchNet>" 2>/dev/null || true
        COMMENT "Re-signing PitchNet bundle")
    add_custom_command(TARGET PitchNetPlugin_VST3 POST_BUILD
        COMMAND codesign --force --deep -s - "$<TARGET_BUNDLE_DIR:PitchNetPlugin_VST3>" 2>/dev/null || true
        COMMENT "Re-signing VST3 bundle")
    add_custom_command(TARGET PitchNetPlugin_AU POST_BUILD
        COMMAND codesign --force --deep -s - "$<TARGET_BUNDLE_DIR:PitchNetPlugin_AU>" 2>/dev/null || true
        COMMENT "Re-signing AU bundle")
endif()
