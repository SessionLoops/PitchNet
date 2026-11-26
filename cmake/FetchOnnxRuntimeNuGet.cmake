# Download ONNX Runtime from NuGet and lay it out like the old GitHub release
# archives:
#
#   <dest>/include/onnxruntime_cxx_api.h ...
#   <dest>/lib/<platform libraries>
#
# The rest of CMakeLists.txt (find_library, DLL copying, dylib embedding) only
# knows that layout, so keeping it means nothing downstream has to change.
#
# Why NuGet: github.com/microsoft/onnxruntime, including every release asset,
# has returned 404 since 2026-11-18. The NuGet packages are still published.
#
# Packages used:
#   Microsoft.ML.OnnxRuntime          CPU libraries for every platform, plus headers
#   Microsoft.ML.OnnxRuntime.Gpu.Linux CUDA libraries for Linux (no headers)

set(ONNXRUNTIME_NUGET_BASE_URL "https://www.nuget.org/api/v2/package"
    CACHE STRING "Base URL of the NuGet v2 feed (or a mirror) that serves ONNX Runtime packages")

function(_pitchnet_ort_fetch_nupkg package version out_dir)
    set(download_root "${CMAKE_BINARY_DIR}/_deps/nuget")
    set(archive "${download_root}/${package}.${version}.zip")
    set(extract_dir "${download_root}/${package}.${version}")

    if(NOT EXISTS "${extract_dir}/.extracted")
        set(url "${ONNXRUNTIME_NUGET_BASE_URL}/${package}/${version}")
        message(STATUS "Downloading ${package} ${version} from ${url}")
        file(MAKE_DIRECTORY "${download_root}")
        file(DOWNLOAD "${url}" "${archive}" STATUS download_status SHOW_PROGRESS)
        list(GET download_status 0 download_code)
        if(NOT download_code EQUAL 0)
            list(GET download_status 1 download_message)
            file(REMOVE "${archive}")
            message(FATAL_ERROR
                "Failed to download ${package} ${version} from ${url}: ${download_message}")
        endif()

        file(REMOVE_RECURSE "${extract_dir}")
        file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${extract_dir}")
        file(REMOVE "${archive}")
        file(TOUCH "${extract_dir}/.extracted")
    endif()

    set(${out_dir} "${extract_dir}" PARENT_SCOPE)
endfunction()

function(_pitchnet_ort_copy_native nupkg_dir rid lib_dir)
    set(native_dir "${nupkg_dir}/runtimes/${rid}/native")
    if(NOT IS_DIRECTORY "${native_dir}")
        message(FATAL_ERROR "ONNX Runtime NuGet package ${nupkg_dir} has no ${rid} libraries")
    endif()
    file(GLOB native_files "${native_dir}/*")
    file(COPY ${native_files} DESTINATION "${lib_dir}")
endfunction()

# pitchnet_fetch_onnxruntime_nuget(
#     DEST <dir>            output directory (include/ and lib/ are created)
#     VERSION <version>
#     [GPU]                 Linux only: use the CUDA package
#     [MACOS_PACKAGE <arm64|x86_64|universal2>])
function(pitchnet_fetch_onnxruntime_nuget)
    cmake_parse_arguments(ORT "GPU" "DEST;VERSION;MACOS_PACKAGE" "" ${ARGN})

    # Assemble in a staging directory and move it into place at the end, so a
    # failed or interrupted configure never leaves a half-filled DEST behind
    # (CMakeLists.txt treats DEST/include/onnxruntime_cxx_api.h as "done").
    set(stage "${ORT_DEST}.staging")
    file(REMOVE_RECURSE "${stage}")
    file(MAKE_DIRECTORY "${stage}/lib")

    # Headers and CPU libraries always come from the CPU package.
    _pitchnet_ort_fetch_nupkg(Microsoft.ML.OnnxRuntime "${ORT_VERSION}" cpu_pkg)
    file(COPY "${cpu_pkg}/build/native/include/" DESTINATION "${stage}/include")

    if(WIN32)
        _pitchnet_ort_copy_native("${cpu_pkg}" win-x64 "${stage}/lib")

    elseif(APPLE)
        set(versioned_name "libonnxruntime.${ORT_VERSION}.dylib")
        if(ORT_MACOS_PACKAGE STREQUAL "universal2")
            foreach(rid osx-arm64 osx-x64)
                if(NOT EXISTS "${cpu_pkg}/runtimes/${rid}/native/libonnxruntime.dylib")
                    message(FATAL_ERROR
                        "ONNX Runtime ${ORT_VERSION} on NuGet has no ${rid} library, "
                        "so a universal2 build is not possible with this version")
                endif()
            endforeach()
            find_program(LIPO_EXECUTABLE lipo REQUIRED)
            execute_process(
                COMMAND "${LIPO_EXECUTABLE}" -create
                    "${cpu_pkg}/runtimes/osx-arm64/native/libonnxruntime.dylib"
                    "${cpu_pkg}/runtimes/osx-x64/native/libonnxruntime.dylib"
                    -output "${stage}/lib/${versioned_name}"
                RESULT_VARIABLE lipo_result)
            if(NOT lipo_result EQUAL 0)
                message(FATAL_ERROR "lipo failed to create a universal2 ONNX Runtime dylib")
            endif()
        else()
            if(ORT_MACOS_PACKAGE STREQUAL "x86_64")
                set(rid osx-x64)
            else()
                set(rid osx-arm64)
            endif()
            set(dylib "${cpu_pkg}/runtimes/${rid}/native/libonnxruntime.dylib")
            if(NOT EXISTS "${dylib}")
                message(FATAL_ERROR "ONNX Runtime ${ORT_VERSION} on NuGet has no ${rid} library")
            endif()
            file(COPY_FILE "${dylib}" "${stage}/lib/${versioned_name}")
        endif()
        # The dylib's install name is @rpath/libonnxruntime.<version>.dylib, so
        # the real file keeps that name; libonnxruntime.dylib is the link name.
        file(CREATE_LINK "${versioned_name}" "${stage}/lib/libonnxruntime.dylib" SYMBOLIC)

    else()
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
            set(rid linux-arm64)
        else()
            set(rid linux-x64)
        endif()

        if(ORT_GPU)
            if(NOT rid STREQUAL "linux-x64")
                message(FATAL_ERROR "The ONNX Runtime CUDA package is only published for linux-x64")
            endif()
            # This package contains its own libonnxruntime.so built with CUDA,
            # plus the CUDA/TensorRT providers. It has no headers.
            _pitchnet_ort_fetch_nupkg(Microsoft.ML.OnnxRuntime.Gpu.Linux "${ORT_VERSION}" gpu_pkg)
            _pitchnet_ort_copy_native("${gpu_pkg}" ${rid} "${stage}/lib")
        else()
            _pitchnet_ort_copy_native("${cpu_pkg}" ${rid} "${stage}/lib")
        endif()

        # NuGet ships only libonnxruntime.so, but its SONAME is
        # libonnxruntime.so.1, which is the name binaries look for at runtime.
        # Recreate the chain the GitHub archive had:
        #   libonnxruntime.so -> libonnxruntime.so.1 -> libonnxruntime.so.<version>
        file(RENAME "${stage}/lib/libonnxruntime.so" "${stage}/lib/libonnxruntime.so.${ORT_VERSION}")
        file(CREATE_LINK "libonnxruntime.so.${ORT_VERSION}" "${stage}/lib/libonnxruntime.so.1" SYMBOLIC)
        file(CREATE_LINK "libonnxruntime.so.1" "${stage}/lib/libonnxruntime.so" SYMBOLIC)
    endif()

    file(REMOVE_RECURSE "${ORT_DEST}")
    file(RENAME "${stage}" "${ORT_DEST}")
endfunction()
